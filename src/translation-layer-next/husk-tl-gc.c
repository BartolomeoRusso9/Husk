/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A garbage collector for the Java runtime's objects: mark and sweep, the world stopped while it marks.
 *
 * Every jobj comes from an arena of fixed slots (tl_gc_obj_alloc), so "is this word an object?" is a table lookup and a slot check,
 * and sweeping is a walk over the slots. Marking is conservative: any word that holds an object's address keeps it, wherever the
 * word is. The roots are
 *   - every other thread's registers and stack, the threads suspended (thread_suspend) while the collector reads them; a word there
 *     that is not an object but points somewhere readable is followed one step (256 bytes are read through vm_read_overwrite, which
 *     cannot fault), so a C struct a thread holds -- a queued message it is dispatching -- keeps what it points to;
 *   - the writable data of every image outside the shared cache: Husk's own globals, and the Android libraries made into dylibs;
 *   - what the runtime keeps in its own heap structures, enumerated by the files that own them (classes' statics, the dex string
 *     caches, interned strings, loopers and their queues), and each Java thread's thread-locals (tl_gc_thread_attach);
 *   - every object native code ever took a reference to (jobj.refs other than 1 and 1 << 30): JNI references are counted, and
 *     DeleteLocalRef does not uncount while Java runs, so such an object is kept, as before.
 * An object's slots, an object array's elements and the first 256 bytes its native state points at are scanned as words; strings
 * and primitive arrays hold no references.
 *
 * An unreachable object whose class overrides finalize() is not freed at once: it (and what it reaches) is kept, and its finalize()
 * runs on the collector's finalizer thread; it is freed the next time it is found unreachable. Bitmaps give back their pixels that
 * way, as on Android.
 *
 * Nothing is allocated, logged or locked while the world is stopped: a suspended thread may hold malloc's lock. The lists the
 * collector needs are mmap'd; the locks the root structures change under are taken (by trylock, so a thread waiting on one of them
 * cannot deadlock against the collector) before any thread is suspended. Freeing happens after the world runs again: nothing can
 * reach a garbage object any more.
 *
 * Modes: dry (the default for now) does everything but free and finalize, and logs what it would have freed; on frees. An object
 * made outside the arena (its address range ran out) keeps the collector from freeing for good: it would not be traversed.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-gc.h"

#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/proc.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-jni.h"
#include "husk-tl-jni-internal.h"

/* ------------------------------------------------------------------ the arena */

#define CHUNK_SHIFT 25                              /* 32 MiB chunks */
#define CHUNK_SIZE (1ul << CHUNK_SHIFT)
#define MAP_BITS 15                                 /* chunks are looked up for addresses below 2^40 */
#define SLOT_SIZE ((sizeof(jobj) + 15) & ~(size_t)15)
#define SLOTS_PER_CHUNK (CHUNK_SIZE / SLOT_SIZE)

static uint8_t g_is_chunk[1u << MAP_BITS];
static uintptr_t g_chunks[1u << MAP_BITS];
static uint32_t g_nchunks;
static pthread_mutex_t g_arena = PTHREAD_MUTEX_INITIALIZER;
static jobj *g_free;                                /* free slots, linked through cls */
static uint8_t *g_bump, *g_bump_end;
static _Atomic uint64_t g_allocated;                /* live slots */
static _Atomic uint64_t g_since;                    /* slots handed out since the last collection */
static _Atomic bool g_outside;                      /* an object came from calloc: never free again */

static bool new_chunk(void)
{
    if (g_nchunks >= (1u << MAP_BITS)) return false;
    uint8_t *p = mmap(NULL, CHUNK_SIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return false;
    uintptr_t base = ((uintptr_t)p + CHUNK_SIZE - 1) & ~(CHUNK_SIZE - 1);
    if ((uintptr_t)p < base) munmap(p, base - (uintptr_t)p);
    uintptr_t end = (uintptr_t)p + CHUNK_SIZE * 2;
    if (end > base + CHUNK_SIZE) munmap((void *)(base + CHUNK_SIZE), end - (base + CHUNK_SIZE));
    if ((base >> CHUNK_SHIFT) >= (1u << MAP_BITS)) { munmap((void *)base, CHUNK_SIZE); return false; }
    g_chunks[g_nchunks++] = base;
    g_is_chunk[base >> CHUNK_SHIFT] = 1;
    g_bump = (uint8_t *)base;
    g_bump_end = (uint8_t *)base + SLOTS_PER_CHUNK * SLOT_SIZE;
    return true;
}

jobj *tl_gc_obj_alloc(void)
{
    pthread_mutex_lock(&g_arena);
    jobj *o = g_free;
    if (o) g_free = (jobj *)o->cls;
    else {
        if (g_bump == g_bump_end && !new_chunk()) {
            pthread_mutex_unlock(&g_arena);
            tl_gc_forbid_freeing("the object arena is full, and objects come from malloc now");
            return calloc(1, sizeof(jobj));
        }
        o = (jobj *)g_bump;
        g_bump += SLOT_SIZE;
    }
    memset(o, 0, sizeof(*o));
    o->gc = TL_GC_ALLOCATED;
    pthread_mutex_unlock(&g_arena);
    atomic_fetch_add_explicit(&g_allocated, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_since, 1, memory_order_relaxed);
    return o;
}

void tl_gc_forbid_freeing(const char *why)
{
    if (!atomic_exchange(&g_outside, true)) tl_log_line("gc: freeing turned off for good: %s", why);
}

static bool in_arena(const void *p)
{
    uintptr_t idx = (uintptr_t)p >> CHUNK_SHIFT;
    return idx < (1u << MAP_BITS) && g_is_chunk[idx];
}

/* Give a slot back. What the object owned has been freed. */
static void slot_free_locked(jobj *o)
{
    memset(o, 0, sizeof(*o));
    o->cls = (tl_jclass *)g_free;
    g_free = o;
    atomic_fetch_sub_explicit(&g_allocated, 1, memory_order_relaxed);
}

void tl_gc_obj_free(jobj *o)
{
    if (!o) return;
    if (!in_arena(o)) { free(o); return; }
    pthread_mutex_lock(&g_arena);
    slot_free_locked(o);
    pthread_mutex_unlock(&g_arena);
}

/* The object at this address, or NULL: an arena slot's start, allocated. */
static inline jobj *object_at(uintptr_t w)
{
    if (w & 15) return NULL;
    uintptr_t idx = w >> CHUNK_SHIFT;
    if (idx >= (1u << MAP_BITS) || !g_is_chunk[idx]) return NULL;
    uintptr_t off = w & (CHUNK_SIZE - 1);
    if (off % SLOT_SIZE || off / SLOT_SIZE >= SLOTS_PER_CHUNK) return NULL;
    jobj *o = (jobj *)w;
    return (o->gc & TL_GC_ALLOCATED) ? o : NULL;
}

/* ------------------------------------------------------------------ threads */

#define MAX_THREADS 1024
typedef struct {
    _Atomic bool live;
    jobj **pending, **thread;
    tl_gc_regs **regchain;
    uintptr_t stack_lo, stack_hi;
} gthread;
static gthread g_threads[MAX_THREADS];
static pthread_mutex_t g_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t g_thread_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static __thread gthread *t_me;

static void thread_gone(void *p) { gthread *g = p; if (g) atomic_store(&g->live, false); }
static void key_init(void) { pthread_key_create(&g_thread_key, thread_gone); }

void tl_gc_thread_attach(void)
{
    if (t_me) return;
    pthread_once(&g_key_once, key_init);
    pthread_mutex_lock(&g_threads_lock);
    gthread *g = NULL;
    for (int i = 0; i < MAX_THREADS && !g; i++) if (!atomic_load(&g_threads[i].live)) g = &g_threads[i];
    if (g) {
        pthread_t me = pthread_self();
        uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(me);
        g->pending = tl_jni_tls_pending_addr();
        g->thread = dvm_tls_thread_addr();
        g->regchain = dvm_tls_regchain_addr();
        g->stack_hi = top;
        g->stack_lo = top - pthread_get_stacksize_np(me);
        atomic_store(&g->live, true);
    }
    pthread_mutex_unlock(&g_threads_lock);
    t_me = g ? g : (gthread *)&g_threads[0];       /* no slot left: not attached, and not asked again */
    if (g) pthread_setspecific(g_thread_key, g);
}

/* ------------------------------------------------------------------ marking */

static uint64_t *g_stack;                           /* the mark stack: mmap'd, committed as it is touched */
static size_t g_stack_cap, g_sp;
static bool g_overflow;
static uint8_t g_epoch = 1;                         /* 1 or 2: the mark this collection gives */

static inline bool marked(const jobj *o) { return ((o->gc >> TL_GC_MARK_SHIFT) & 3) == g_epoch; }
static inline void set_mark(jobj *o) { o->gc = (uint8_t)((o->gc & ~(3u << TL_GC_MARK_SHIFT)) | (g_epoch << TL_GC_MARK_SHIFT)); }

static inline void push(jobj *o)
{
    if (marked(o)) return;
    set_mark(o);
    if (g_sp == g_stack_cap) { g_overflow = true; return; }
    g_stack[g_sp++] = (uint64_t)(uintptr_t)o;
}

void tl_gc_mark_word(uintptr_t w)
{
    jobj *o = object_at(w);
    if (o) push(o);
}

void tl_gc_mark_range(const void *p, size_t bytes)
{
    if (!p) return;
    const uintptr_t *w = p;
    for (size_t i = 0, n = bytes / sizeof(uintptr_t); i < n; i++) tl_gc_mark_word(w[i]);
}

/* A word that is not an object but may point at a C struct holding objects: read 256 bytes there, if they are readable. */
/* The readable, non-executable memory of the process, sorted, made while the world is stopped (nothing maps or unmaps then): a word
 * is followed by reading where it points directly, after a binary search here, instead of one vm_read_overwrite call per word -- which
 * was most of a collection's pause. */
#define MAX_REGIONS 65536
static struct { uintptr_t lo, hi; } *g_regions;
static int g_nregions;

static void collect_regions(void)
{
    g_nregions = 0;
    if (!g_regions) {
        void *p = mmap(NULL, MAX_REGIONS * sizeof(*g_regions), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return;
        g_regions = p;
    }
    vm_address_t addr = 0x100000000ull;
    while (g_nregions < MAX_REGIONS && addr < (1ull << 40)) {
        vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t ic = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &ic, &obj) != KERN_SUCCESS) break;
        if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
        /* writable too: heaps, stacks and data, where the structs that hold objects live (a read-only mapping may be a file that
           faults when read past its end) */
        if ((info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) && !(info.protection & VM_PROT_EXECUTE)) {
            if (g_nregions && g_regions[g_nregions - 1].hi == addr) g_regions[g_nregions - 1].hi = addr + size;
            else { g_regions[g_nregions].lo = addr; g_regions[g_nregions].hi = addr + size; g_nregions++; }
        }
        addr += size;
    }
}

/* How many bytes from w on are readable (0: none). */
static size_t readable_from(uintptr_t w)
{
    int lo = 0, hi = g_nregions - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (w < g_regions[mid].lo) hi = mid - 1;
        else if (w >= g_regions[mid].hi) lo = mid + 1;
        else return g_regions[mid].hi - w;
    }
    return 0;
}

/* A read that faults while the collector follows a word: memory the map said was readable can be gone by the time it is read
 * (iOS takes back purgeable and compressed memory when little is left; the collector's thread faulted with 650 MiB to spare). The
 * fault handler jumps back here and the word is passed over, rather than the process ending. */
static __thread sigjmp_buf *t_probe;
static struct sigaction g_gc_prev_segv, g_gc_prev_bus;
static void gc_fault(int sig, siginfo_t *info, void *uc)
{
    if (t_probe) { sigjmp_buf *j = t_probe; t_probe = NULL; siglongjmp(*j, 1); }
    struct sigaction *prev = sig == SIGBUS ? &g_gc_prev_bus : &g_gc_prev_segv;
    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) { prev->sa_sigaction(sig, info, uc); return; }
    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN && prev->sa_handler) { prev->sa_handler(sig); return; }
    signal(sig, SIG_DFL);
}
static void gc_fault_install(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gc_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_gc_prev_segv);
    sigaction(SIGBUS, &sa, &g_gc_prev_bus);
}

static void mark_one_step(uintptr_t w)
{
    if (w < 0x100000000ull || w >= (1ull << 40) || (w & 7) || object_at(w)) return;
    size_t n = readable_from(w);
    if (n > 256) n = 256;
    if (!n) return;
    const volatile uintptr_t *p = (const volatile uintptr_t *)w;
    sigjmp_buf jb;
    if (sigsetjmp(jb, 0)) return;          /* the read faulted: passed over */
    t_probe = &jb;
    for (size_t i = 0; i < n / sizeof(uintptr_t); i++) tl_gc_mark_word(p[i]);
    t_probe = NULL;
}

/* Every thread's stack, from pthread, before the world stops (asking pthread takes its list lock, which a stopped thread may hold).
 * A thread made after this falls back to the VM region its sp is in, at most 1 MiB of it. */
#define MAX_STACKS 4096
static struct { thread_act_t port; uintptr_t lo, hi; } g_stacks[MAX_STACKS];
static int g_nstacks;

static void collect_stacks(void)
{
    g_nstacks = 0;
    thread_act_array_t list; mach_msg_type_number_t n;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t p = pthread_from_mach_thread_np(list[i]);
        if (p && g_nstacks < MAX_STACKS) {
            uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(p);
            size_t size = pthread_get_stacksize_np(p);
            if (top && size) { g_stacks[g_nstacks].port = list[i]; g_stacks[g_nstacks].hi = top; g_stacks[g_nstacks].lo = top - size; g_nstacks++; }
        }
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
}

static void mark_word_deep(uintptr_t w)
{
    jobj *o = object_at(w);
    if (o) push(o);
    else mark_one_step(w);
}

static void scan_object(jobj *o)
{
    switch (o->kind) {
    case TL_K_OBJ_ARRAY:
        if (o->oarr.v) tl_gc_mark_range(o->oarr.v, (size_t)o->oarr.len * sizeof(jobj *));
        break;
    case TL_K_CLASS:
        if (o->klass.jc && o->klass.jc->mirror) tl_gc_mark_word((uintptr_t)o->klass.jc->mirror);
        break;
    default: break;
    }
    jvalue *f = o->fields;
    if (f && o->nfields) tl_gc_mark_range(f, (size_t)o->nfields * sizeof(jvalue));
    if (o->native) mark_one_step((uintptr_t)o->native);
}

static void drain(void)
{
    while (g_sp && !g_overflow) scan_object((jobj *)(uintptr_t)g_stack[--g_sp]);
}

/* ------------------------------------------------------------------ the world */

#define MAX_RANGES 4096
static struct { uintptr_t lo, hi; } g_data[MAX_RANGES];
static int g_ndata;

/* The writable data of every image outside the shared cache: read with dyld's help, so before the world stops. */
static void collect_data_ranges(void)
{
    g_ndata = 0;
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n && g_ndata < MAX_RANGES; i++) {
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!h || h->magic != MH_MAGIC_64 || (h->flags & MH_DYLIB_IN_CACHE)) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        const uint8_t *lc = (const uint8_t *)(h + 1);
        for (uint32_t k = 0; k < h->ncmds && g_ndata < MAX_RANGES; k++) {
            const struct load_command *c = (const struct load_command *)lc;
            if (c->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *s = (const struct segment_command_64 *)c;
                if (!strncmp(s->segname, "__DATA", 6) && s->vmsize) {
                    g_data[g_ndata].lo = (uintptr_t)s->vmaddr + (uintptr_t)slide;
                    g_data[g_ndata].hi = g_data[g_ndata].lo + (uintptr_t)s->vmsize;
                    g_ndata++;
                }
            }
            lc += c->cmdsize;
        }
    }
}

#define MAX_SUSPENDED 4096
static thread_act_t g_suspended[MAX_SUSPENDED];
static int g_nsuspended;

static void resume_all(void)
{
    for (int i = 0; i < g_nsuspended; i++) {
        thread_resume(g_suspended[i]);
        mach_port_deallocate(mach_task_self(), g_suspended[i]);
    }
    g_nsuspended = 0;
}

/* Suspend every thread but this one. Threads made while doing it are found by asking again, until no new one turns up. */
static bool suspend_all(void)
{
    thread_act_t self = mach_thread_self();
    bool ok = false;
    for (int round = 0; round < 16 && !ok; round++) {
        thread_act_array_t list; mach_msg_type_number_t n;
        if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) break;
        bool fresh = false;
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            bool known = list[i] == self;
            for (int k = 0; !known && k < g_nsuspended; k++) known = g_suspended[k] == list[i];
            if (!known && g_nsuspended < MAX_SUSPENDED && thread_suspend(list[i]) == KERN_SUCCESS) {
                g_suspended[g_nsuspended++] = list[i];      /* the port's reference is kept until resume_all */
                fresh = true;
            } else mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
        if (!fresh) ok = true;
    }
    mach_port_deallocate(mach_task_self(), self);
    return ok;
}

/* A stopped thread's registers and stack. The stack runs from below sp (the 128-byte red zone) to the top of its region. */
static void scan_thread(thread_act_t th)
{
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS) return;
    for (int i = 0; i < 29; i++) mark_word_deep((uintptr_t)st.__x[i]);
    mark_word_deep((uintptr_t)arm_thread_state64_get_fp(st));
    mark_word_deep((uintptr_t)arm_thread_state64_get_lr(st));
    /* the SIMD registers too: compiled code moves pointers through them (a struct copied with ldp/stp q) */
    {
        arm_neon_state64_t ns;
        mach_msg_type_number_t nc = ARM_NEON_STATE64_COUNT;
        if (thread_get_state(th, ARM_NEON_STATE64, (thread_state_t)&ns, &nc) == KERN_SUCCESS)
            for (int i = 0; i < 32; i++) {
                uint64_t w[2]; memcpy(w, &ns.__v[i], sizeof(w));
                mark_word_deep((uintptr_t)w[0]); mark_word_deep((uintptr_t)w[1]);
            }
    }
    uintptr_t sp = (uintptr_t)arm_thread_state64_get_sp(st);
    if (!sp) return;
    /* the top: from pthread (collect_stacks); a thread made since, from the VM region sp is in */
    uintptr_t lo = sp > 128 ? sp - 128 : sp, hi = 0;
    for (int i = 0; i < g_nstacks; i++)
        if (g_stacks[i].port == th && sp >= g_stacks[i].lo && sp < g_stacks[i].hi) { hi = g_stacks[i].hi; if (lo < g_stacks[i].lo) lo = g_stacks[i].lo; break; }
    if (!hi) {
        vm_address_t addr = sp; vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t ic = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
        if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &ic, &obj) != KERN_SUCCESS) return;
        if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
        if (addr > sp || !(info.protection & VM_PROT_READ)) return;
        hi = addr + size;
        if (lo < addr) lo = addr;
        if (hi - lo > (1u << 20)) hi = lo + (1u << 20);
    }
    if (hi - lo > (64u << 20)) hi = lo + (64u << 20);
    lo &= ~(uintptr_t)7;
    for (uintptr_t p = lo; p + 8 <= hi; p += 8) mark_word_deep(*(const uintptr_t *)p);
}

static void scan_registered_threads(void)
{
    for (int i = 0; i < MAX_THREADS; i++) {
        gthread *g = &g_threads[i];
        if (!atomic_load(&g->live)) continue;
        if (g->pending) tl_gc_mark_word((uintptr_t)*g->pending);
        if (g->thread) tl_gc_mark_word((uintptr_t)*g->thread);
        for (tl_gc_regs *r = g->regchain ? *g->regchain : NULL; r; r = r->prev) tl_gc_mark_range(r->regs, (size_t)r->n * sizeof(uint64_t));
    }
}

/* ------------------------------------------------------------------ a collection */

typedef struct {
    uint64_t objects, reachable, garbage, garbage_bytes, finalizing, pinned, condemned;
    double pause_ms, total_ms;
    double t_threads, t_data, t_roots, t_pinned, t_fin, t_sweep;   /* where the pause went */
    bool aborted, freed;
} gstats;

static _Atomic int g_mode = TL_GC_DRY;
static jobj **g_finq;                               /* unreachable, finalize() still to run: mmap'd */
static size_t g_finq_cap, g_nfinq;
static pthread_mutex_t g_fin_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fin_cv = PTHREAD_COND_INITIALIZER;

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }

static void *map_array(size_t entries, size_t *cap)
{
    size_t bytes = (entries * sizeof(void *) + 0x3FFF) & ~(size_t)0x3FFF;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { *cap = 0; return NULL; }
    *cap = bytes / sizeof(void *);
    return p;
}
static void unmap_array(void *p, size_t cap) { if (p) munmap(p, (cap * sizeof(void *) + 0x3FFF) & ~(size_t)0x3FFF); }

static bool is_root_by_refs(const jobj *o) { return o->kind == TL_K_CLASS || (o->refs != 1 && o->refs != (1u << 30)); }

static size_t owned_bytes(const jobj *o)
{
    size_t b = SLOT_SIZE + (size_t)o->nfields * sizeof(jvalue);
    switch (o->kind) {
    case TL_K_STRING: b += (o->str.utf8 ? strlen(o->str.utf8) + 1 : 0) + (o->str.u16 ? (size_t)o->str.len16 * 2 : 0); break;
    case TL_K_PRIM_ARRAY: b += (size_t)o->arr.len * o->arr.esz; break;
    case TL_K_OBJ_ARRAY: b += (size_t)o->oarr.len * sizeof(jobj *); break;
    default: break;
    }
    return b;
}

static void free_owned(jobj *o)
{
    switch (o->kind) {
    case TL_K_STRING: free(o->str.utf8); free(o->str.u16); break;
    case TL_K_PRIM_ARRAY: free(o->arr.data); break;
    case TL_K_OBJ_ARRAY: free(o->oarr.v); break;
    default: break;
    }
    free(o->fields);
    if (o->monitor) {
        /* the interpreter's monitor (husk-tl-dvm.c's mon): a mutex, then a condition */
        pthread_mutex_destroy((pthread_mutex_t *)o->monitor);
        pthread_cond_destroy((pthread_cond_t *)((uint8_t *)o->monitor + sizeof(pthread_mutex_t)));
        free(o->monitor);
    }
}

/* What finalize() says for each class, asked before the world stops (the answer is kept in the class). */
static void note_finalizable(void *jclass)
{
    tl_jclass *c = jclass;
    if (c->gc_fin || atomic_load(&c->linking)) return;
    /* a class only C implements has no finalize() to run */
    c->gc_fin = c->dvm ? (int8_t)dvm_gc_finalizable(c) : -1;
}

#define EACH_SLOT(o) \
    for (uint32_t c_ = 0; c_ < g_nchunks; c_++) \
        for (uint8_t *b_ = (uint8_t *)g_chunks[c_], *p_ = b_, \
                     *e_ = (b_ <= g_bump && g_bump <= b_ + SLOTS_PER_CHUNK * SLOT_SIZE) ? g_bump : b_ + SLOTS_PER_CHUNK * SLOT_SIZE; \
             p_ < e_; p_ += SLOT_SIZE) \
            for (jobj *o = (jobj *)p_; o; o = NULL)

static bool collect(gstats *s, int mode)
{
    memset(s, 0, sizeof(*s));
    double t0 = now_ms();
    tl_jni_each_class(note_finalizable);
    tl_jni_each_class(note_finalizable);           /* asking may have declared classes */
    collect_data_ranges();
    collect_stacks();
    size_t gcap = 0;
    uint64_t total = atomic_load(&g_allocated) + 65536;
    jobj **garbage = map_array(total + total / 2, &gcap);
    if (!g_stack) g_stack = map_array(64u << 20, &g_stack_cap);
    if (!garbage || !g_stack) { unmap_array(garbage, gcap); s->aborted = true; return false; }
    size_t ngarbage = 0;
    g_sp = 0; g_overflow = false;
    g_epoch = g_epoch == 1 ? 2 : 1;
    bool finalize = mode == TL_GC_ON && g_finq;

    /* the locks the roots change under, then the arena's (no new objects while marking), then everyone else stops */
    bool locked = false;
    for (int tries = 0; tries < 200 && !locked; tries++) {
        if (dvm_natives_gc_trylock()) {
            if (tl_loop_gc_trylock()) {
                if (pthread_mutex_trylock(&g_threads_lock) == 0) {
                    if (pthread_mutex_trylock(&g_arena) == 0) locked = true;
                    else pthread_mutex_unlock(&g_threads_lock);
                }
                if (!locked) tl_loop_gc_unlock();
            }
            if (!locked) dvm_natives_gc_unlock();
        }
        if (!locked) usleep(2000);
    }
    if (!locked) { unmap_array(garbage, gcap); s->aborted = true; return false; }
    bool stopped = suspend_all();
    double t1 = now_ms();

    /* ---- the world is stopped: no malloc, no logging, no locks until resume_all */
    if (stopped) {
        collect_regions();
        double p0 = now_ms();
        for (int i = 0; i < g_nsuspended; i++) { scan_thread(g_suspended[i]); drain(); }
        double p1 = now_ms(); s->t_threads = p1 - p0;
        for (int i = 0; i < g_ndata; i++) { tl_gc_mark_range((const void *)g_data[i].lo, g_data[i].hi - g_data[i].lo); drain(); }
        double p2 = now_ms(); s->t_data = p2 - p1;
        scan_registered_threads(); drain();
        tl_jni_gc_roots(); drain();
        dvm_gc_roots(); drain();
        dvm_natives_gc_roots(); drain();
        tl_loop_gc_roots(); drain();
        double p3 = now_ms(); s->t_roots = p3 - p2;
        /* objects native code holds a reference to, and every class mirror */
        EACH_SLOT(o) {
            if (!(o->gc & TL_GC_ALLOCATED)) continue;
            s->objects++;
            if (is_root_by_refs(o) && !marked(o)) { s->pinned++; push(o); drain(); }
        }
        double p4 = now_ms(); s->t_pinned = p4 - p3;
        /* what is still unmarked cannot be reached. One to finalize first is kept, with all it reaches, and queued; one whose class
           has not been asked about yet is kept until it has been */
        EACH_SLOT(o) {
            if (!(o->gc & TL_GC_ALLOCATED) || marked(o) || o->kind != TL_K_OBJECT || !o->cls) continue;
            int fin = o->cls->gc_fin;
            if (fin == 0 || (fin > 0 && !(o->gc & TL_GC_FINALIZED))) {
                push(o); drain();
                /* finalize() too waits for a second collection to agree: a Bitmap's frees its pixels, and one found unreachable
                   only because its reference was in flight was still being drawn (CoreGraphics faulted on the main thread) */
                if (fin > 0 && !(o->gc & TL_GC_CONDEMNED)) { o->gc |= TL_GC_FIN_WAIT; continue; }
                if (fin > 0 && finalize && g_nfinq < g_finq_cap) { o->gc |= TL_GC_FINALIZED; g_finq[g_nfinq++] = o; s->finalizing++; }
                else if (fin > 0) s->finalizing++;
            }
        }
        double p5 = now_ms(); s->t_fin = p5 - p4;
        if (!g_overflow) {
            /* An object is freed when two collections in a row find it unreachable: a reference only in flight when one
               collection looked (between a register and memory the collector does not read, a thread copying a struct) has
               landed somewhere it reads by the next, and an object freed under a thread still using it is a crash. */
            EACH_SLOT(o) {
                if (!(o->gc & TL_GC_ALLOCATED)) continue;
                if (marked(o)) {
                    if (o->gc & TL_GC_FIN_WAIT) { o->gc = (uint8_t)((o->gc & ~TL_GC_FIN_WAIT) | TL_GC_CONDEMNED); s->condemned++; continue; }
                    o->gc &= (uint8_t)~TL_GC_CONDEMNED; s->reachable++; continue;
                }
                if (!(o->gc & TL_GC_CONDEMNED)) { o->gc |= TL_GC_CONDEMNED; s->condemned++; continue; }
                if (ngarbage < gcap) garbage[ngarbage++] = o; else g_overflow = true;
            }
        }
        s->t_sweep = now_ms() - p5;
    }
    resume_all();
    /* ---- the world runs again */
    s->pause_ms = now_ms() - t1;
    pthread_mutex_unlock(&g_threads_lock);
    tl_loop_gc_unlock();
    dvm_natives_gc_unlock();
    /* The arena stays locked until the garbage is freed: the garbage's slots are still marked allocated, so nothing can hand them
       out, but a collection must not overlap the next one's view of them either. Freeing what they own does not touch the arena. */
    if (!stopped || g_overflow) s->aborted = true;
    else {
        s->garbage = ngarbage;
        for (size_t i = 0; i < ngarbage; i++) s->garbage_bytes += owned_bytes(garbage[i]);
        if (mode == TL_GC_ON && !atomic_load(&g_outside)) {
            for (size_t i = 0; i < ngarbage; i++) free_owned(garbage[i]);
            for (size_t i = 0; i < ngarbage; i++) slot_free_locked(garbage[i]);
            s->freed = true;
        }
    }
    pthread_mutex_unlock(&g_arena);
    unmap_array(garbage, gcap);
    atomic_store(&g_since, 0);
    s->total_ms = now_ms() - t0;
    return !s->aborted;
}

/* ------------------------------------------------------------------ the threads */

static void *finalizer_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("husk-finalizer");
    for (;;) {
        pthread_mutex_lock(&g_fin_lock);
        while (!g_nfinq) pthread_cond_wait(&g_fin_cv, &g_fin_lock);
        jobj *o = g_finq[--g_nfinq];
        pthread_mutex_unlock(&g_fin_lock);
        dvm_gc_run_finalizer(o);
    }
    return NULL;
}

static void *collector_thread(void *arg)
{
    (void)arg;
    pthread_setname_np("husk-gc");
    double last = now_ms();
    for (;;) {
        sleep(2);
        int mode = atomic_load(&g_mode);
        if (mode == TL_GC_OFF) continue;
        size_t avail = os_proc_available_memory();
        uint64_t since = atomic_load(&g_since);
        double t = now_ms();
        /* often enough that memory never runs out, rarely enough that the pauses go unnoticed: once the app has made as many new
           objects as it keeps (at least a million), after a minute with any, or at once when iOS is close to ending the app */
        uint64_t live = atomic_load(&g_allocated) - since;
        bool due = since > (live > 1000000 ? live : 1000000) || (since > 20000 && t - last > 60000)
                || (avail && avail < (768u << 20) && since && t - last > 3000);
        if (!due) continue;
        last = t;
        gstats s;
        pthread_mutex_lock(&g_fin_lock);           /* the finalizer thread does not pop while the queue is being filled */
        bool ok = collect(&s, mode);
        pthread_mutex_unlock(&g_fin_lock);
        if (g_nfinq) pthread_cond_signal(&g_fin_cv);
        tl_log_line("gc: %s: %llu objects, %llu reachable (%llu held by native code), %llu unreachable (%.1f MiB)%s, %llu to finalize, %llu kept for one more collection; "
                    "paused %.1f ms (threads %.1f, data %.1f, roots %.1f, native refs %.1f, finalizers %.1f, sweep %.1f; %.1f ms in all), "
                    "%zu MiB left before jetsam%s",
                    s.freed ? "collected" : "measured, nothing freed (dry run)",
                    (unsigned long long)s.objects, (unsigned long long)s.reachable, (unsigned long long)s.pinned,
                    (unsigned long long)s.garbage, s.garbage_bytes / 1048576.0,
                    atomic_load(&g_outside) ? " [objects outside the arena: freeing off]" : "",
                    (unsigned long long)s.finalizing, (unsigned long long)s.condemned, s.pause_ms, s.t_threads, s.t_data, s.t_roots, s.t_pinned, s.t_fin, s.t_sweep, s.total_ms,
                    os_proc_available_memory() >> 20, ok ? "" : " -- aborted");
    }
    return NULL;
}

void tl_gc_start(int mode)
{
    const char *e = getenv("TL_GC");
    if (e) mode = !strcmp(e, "off") ? TL_GC_OFF : !strcmp(e, "on") ? TL_GC_ON : TL_GC_DRY;
    atomic_store(&g_mode, mode);
    static bool started;
    if (started) return;
    started = true;
    g_finq = map_array(1u << 20, &g_finq_cap);
    gc_fault_install();
    pthread_t t;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 8u << 20);
    pthread_create(&t, &a, collector_thread, NULL);
    pthread_create(&t, &a, finalizer_thread, NULL);
    pthread_attr_destroy(&a);
    tl_log_line("gc: started (%s)", mode == TL_GC_ON ? "collecting" : mode == TL_GC_DRY ? "dry run: measuring, freeing nothing" : "off");
}
