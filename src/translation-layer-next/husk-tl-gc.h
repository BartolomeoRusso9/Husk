// SPDX-License-Identifier: GPL-2.0-or-later
/* A garbage collector for the Java runtime's objects (husk-tl-gc.c).
 *
 * Until it, an object the interpreter made lived for ever: an app that kept making objects (every screen it opened, every list it
 * scrolled, every response it parsed) grew until iOS ended Husk, minutes in. */
#ifndef HUSK_TL_GC_H
#define HUSK_TL_GC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct jobj jobj;

/* jobj.gc */
enum {
    TL_GC_ALLOCATED = 1u << 0,      /* a live slot of the arena (a free slot has 0) */
    TL_GC_MARK_SHIFT = 1,           /* bits 1-2: the epoch of the collection that last reached it */
    TL_GC_FINALIZED = 1u << 3,      /* its finalize() has run (or was queued): freed when next found unreachable */
    TL_GC_CONDEMNED = 1u << 4,      /* found unreachable by the last collection: freed if this one finds it unreachable again */
    TL_GC_FIN_WAIT = 1u << 5,       /* to finalize, found unreachable once: condemned when this collection ends, finalized if the next agrees */
};

/* The arena every jobj comes from (husk-tl-jni.c's obj_alloc): a zeroed object, and one given back. */
jobj *tl_gc_obj_alloc(void);
void tl_gc_obj_free(jobj *o);

/* Off, measuring what it would free without freeing (the first runs on a new app), or collecting. */
enum { TL_GC_OFF = 0, TL_GC_DRY = 1, TL_GC_ON = 2 };
/* Start the collector's thread (once; later calls only change the mode). TL_GC in the environment ("off", "dry", "on") wins. */
void tl_gc_start(int mode);
/* Something the collector cannot see holds objects: it measures from now on, and frees nothing. */
void tl_gc_forbid_freeing(const char *why);
/* A thread that runs Java: its thread-locals are roots. Cheap after the first call on a thread (dvm_call makes it). */
void tl_gc_thread_attach(void);

/* For the root enumerators below, only while the world is stopped: an object (or anything: words that are not objects are
 * ignored), and a range of words that may hold objects. */
void tl_gc_mark_word(uintptr_t w);
void tl_gc_mark_range(const void *p, size_t bytes);

/* The roots other files keep, each enumerated by its owner. None may allocate or take a lock: the world is stopped. */
void tl_jni_gc_roots(void);         /* classes: mirrors, statics, detached fields */
void dvm_gc_roots(void);            /* dex string caches and loaders, proxy methods */
void dvm_natives_gc_roots(void);    /* the interned strings */
void tl_loop_gc_roots(void);        /* loopers and their queues */
/* Locks held across a collection, so no structure above is half-changed while it is read: try to take them (false: none taken,
 * try again later), and let them go. */
bool dvm_natives_gc_trylock(void);
void dvm_natives_gc_unlock(void);
bool tl_loop_gc_trylock(void);
void tl_loop_gc_unlock(void);

/* Thread-locals, by address on the calling thread (tl_gc_thread_attach records them). */
void *tl_jni_tls_pending_addr(void);
void *dvm_tls_thread_addr(void);
void *dvm_tls_regchain_addr(void);

/* An interpreter frame's registers, when they are on the heap (methods with more than 64): chained through the thread so the
 * collector sees them (dvm_call_inner). */
typedef struct tl_gc_regs {
    struct tl_gc_regs *prev;
    const uint64_t *regs;
    uint32_t n;
} tl_gc_regs;

/* For each class: call fn (outside a collection). */
void tl_jni_each_class(void (*fn)(void *jclass));
/* Whether instances of the class run finalize(): 1 yes, -1 no (husk-tl-dvm.c; called outside a collection). */
int dvm_gc_finalizable(void *jclass);
/* Run o.finalize() on this thread, clearing what it throws. */
void dvm_gc_run_finalizer(jobj *o);

#endif
