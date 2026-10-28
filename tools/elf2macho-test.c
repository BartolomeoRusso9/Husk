/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * elf2macho-test: an arm64 Android library (.so) as a Mach-O dylib, for running Android code with no JIT (Husk 3.2; see
 * Husk_3.2.MD and 3.2-progress.md).
 *
 * The translation layer runs a library from memory it copies the image into and rewrites (svc, x18, tpidr_el0, ctr_el0, adr/adrp
 * to data), which needs an executable region iOS only grants with a debugger attached. A dylib is code iOS runs once it is signed,
 * so the loader's own mapping and rewriting (tl_ld_aot_build in husk-tl-ld.c) run here, ahead of time, and the result goes into one:
 *
 *   __TEXT  r-x   the Mach-O header (with the manifest, __husk_manifest) padded to 16 KiB, then __husk_image: the image at its
 *                 own vaddr layout with every code rewrite applied, its stub pages, and a thread-block page
 *   __DATA  rw-   __husk_data: the image as the file has it, the rest zero -- relocated at run time; also the stubs' data slots
 *                 and the thread block, which the loader fills
 *   __LINKEDIT    empty fixups, export trie and symbol table (dyld wants them), then the code signature
 *
 * The distance from __husk_image to __husk_data is the image's size (image + stubs + thread block), the delta the rewrites were
 * made for; dyld keeps segment distances, so the patched code finds its data wherever the dylib is loaded.
 *
 *   tools/regress/build.sh tools/elf2macho-test.c <out>           (it links the translation layer)
 *   elf2macho-test <lib.so> <out.dylib> [--ios]       write it (macOS: also ad-hoc signed; --ios marks it for iOS, unsigned)
 *   elf2macho-test --load <out.dylib> <lib.so> [fn [arg]]   dlopen it, check it against a fresh build, call fn(arg) from the
 *                                                     dylib's code (a leaf that needs no relocation: mpg123_plain_strerror -1)
 */
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "husk-tl-ld.h"

#define PAGE ((uint64_t)16384)
#define MANIFEST_AT (PAGE - 256)          /* the manifest's offset in the header page */
#define ROUND_DOWN(x) ((x) & ~(PAGE - 1))
#define ROUND_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))

void tl_log_line(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ ELF */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } elf_ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } elf_phdr;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } elf_sym;
typedef struct { int64_t d_tag; uint64_t d_val; } elf_dyn;
enum { PT_LOAD_ = 1, PT_DYNAMIC_ = 2, EM_AARCH64_ = 183, DT_NULL_ = 0, DT_STRTAB_ = 5, DT_SYMTAB_ = 6, DT_GNU_HASH_ = 0x6ffffef5,
       DT_HASH_ = 4 };

typedef struct {
    uint8_t *file; size_t flen;
    const elf_ehdr *eh; const elf_phdr *ph;
    uint64_t lo, hi;                  /* the image: lowest PT_LOAD rounded down, highest end rounded up (16 KiB) */
    uint64_t max_align;
} elf;

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: %s\n", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n ? (size_t)n : 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); *len = (size_t)n; return b;
}

static bool elf_open(elf *E, const char *path)
{
    memset(E, 0, sizeof(*E));
    if (!(E->file = slurp(path, &E->flen))) return false;
    if (E->flen < sizeof(elf_ehdr) || memcmp(E->file, "\x7f" "ELF", 4) || E->file[4] != 2) {
        fprintf(stderr, "%s: not a 64-bit ELF file\n", path); return false;
    }
    E->eh = (const elf_ehdr *)E->file;
    if (E->eh->e_machine != EM_AARCH64_) { fprintf(stderr, "%s: not arm64 (machine %u)\n", path, E->eh->e_machine); return false; }
    if (E->eh->e_phoff + (uint64_t)E->eh->e_phnum * sizeof(elf_phdr) > E->flen) { fprintf(stderr, "%s: bad program headers\n", path); return false; }
    E->ph = (const elf_phdr *)(E->file + E->eh->e_phoff);
    E->lo = UINT64_MAX;
    for (int i = 0; i < E->eh->e_phnum; i++) {
        const elf_phdr *p = &E->ph[i];
        if (p->p_type != PT_LOAD_) continue;
        if (p->p_offset + p->p_filesz > E->flen) { fprintf(stderr, "%s: PT_LOAD %d runs past the file\n", path, i); return false; }
        if (p->p_vaddr < E->lo) E->lo = p->p_vaddr;
        if (p->p_vaddr + p->p_memsz > E->hi) E->hi = p->p_vaddr + p->p_memsz;
        if (p->p_align > E->max_align) E->max_align = p->p_align;
    }
    if (E->lo == UINT64_MAX) { fprintf(stderr, "%s: no PT_LOAD\n", path); return false; }
    E->lo = ROUND_DOWN(E->lo); E->hi = ROUND_UP(E->hi);
    return true;
}

/* A defined dynamic symbol's vaddr, by a linear walk of .dynsym (the count from the hash table). */
static uint64_t elf_lookup(const elf *E, const char *name)
{
    const elf_dyn *dyn = NULL; size_t ndyn = 0;
    for (int i = 0; i < E->eh->e_phnum; i++)
        if (E->ph[i].p_type == PT_DYNAMIC_) { dyn = (const elf_dyn *)(E->file + E->ph[i].p_offset); ndyn = E->ph[i].p_filesz / sizeof(elf_dyn); }
    if (!dyn) return 0;
    uint64_t strtab = 0, symtab = 0, hash = 0, gnu = 0;
    for (size_t i = 0; i < ndyn && dyn[i].d_tag != DT_NULL_; i++) {
        if (dyn[i].d_tag == DT_STRTAB_) strtab = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_SYMTAB_) symtab = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_HASH_) hash = dyn[i].d_val;
        else if (dyn[i].d_tag == DT_GNU_HASH_) gnu = dyn[i].d_val;
    }
    /* vaddr -> file offset */
    #define OFF(v) ({ uint64_t _v = (v), _o = 0; for (int _i = 0; _i < E->eh->e_phnum; _i++) { const elf_phdr *_p = &E->ph[_i]; \
        if (_p->p_type == PT_LOAD_ && _v >= _p->p_vaddr && _v < _p->p_vaddr + _p->p_filesz) _o = _p->p_offset + (_v - _p->p_vaddr); } _o; })
    if (!strtab || !symtab) return 0;
    uint32_t n = 0;
    if (hash) n = ((const uint32_t *)(E->file + OFF(hash)))[1];
    else if (gnu) {
        const uint32_t *g = (const uint32_t *)(E->file + OFF(gnu));
        uint32_t nb = g[0], base = g[1], bloom = g[2];
        const uint32_t *buckets = g + 4 + bloom * 2, *chain = buckets + nb;
        uint32_t last = 0;
        for (uint32_t b = 0; b < nb; b++) if (buckets[b] > last) last = buckets[b];
        if (last) { while (!(chain[last - base] & 1)) last++; n = last + 1; }
    }
    const elf_sym *syms = (const elf_sym *)(E->file + OFF(symtab));
    const char *strs = (const char *)(E->file + OFF(strtab));
    for (uint32_t i = 1; i < n; i++)
        if (syms[i].st_shndx && syms[i].st_value && !strcmp(strs + syms[i].st_name, name)) return syms[i].st_value;
    #undef OFF
    return 0;
}

/* ------------------------------------------------------------------ Mach-O */

typedef struct { uint8_t *b; size_t n, cap; } buf;
static void put(buf *B, const void *p, size_t n)
{
    if (B->n + n > B->cap) { B->cap = (B->n + n) * 2 + 4096; B->b = realloc(B->b, B->cap); }
    memcpy(B->b + B->n, p, n); B->n += n;
}
static void pad(buf *B, size_t to) { static const uint8_t z[64]; while (B->n < to) put(B, z, to - B->n < 64 ? to - B->n : 64); }

static bool write_dylib(const elf *E, const tl_ld_aot_image *A, const char *out, const char *install_name, bool ios)
{
    const uint64_t S = A->size;              /* image + stubs + thread block = the delta from the image to its writable copy */
    const uint64_t HDR = PAGE;               /* header and load commands, then the image at the next page */
    const uint64_t text_size = HDR + S, data_vm = text_size, link_vm = data_vm + S;

    /* __LINKEDIT: chained-fixups header with no fixups, an empty export trie, an empty string table */
    buf L = {0};
    struct dyld_chained_fixups_header fh = {0};
    fh.fixups_version = 0;
    fh.starts_offset = sizeof(fh);
    uint32_t starts[1 + 3] = { 3, 0, 0, 0 };            /* seg_count, then no fixups in __TEXT, __DATA, __LINKEDIT */
    fh.imports_offset = fh.starts_offset + sizeof(starts);
    fh.symbols_offset = fh.imports_offset;
    fh.imports_count = 0;
    fh.imports_format = DYLD_CHAINED_IMPORT;
    fh.symbols_format = 0;
    put(&L, &fh, sizeof(fh)); put(&L, starts, sizeof(starts)); pad(&L, (L.n + 7) & ~7ul);
    const uint32_t fix_off = 0, fix_size = (uint32_t)L.n;
    const uint32_t trie_off = (uint32_t)L.n; const uint8_t trie[8] = { 0, 0 }; put(&L, trie, 8);
    const uint32_t str_off = (uint32_t)L.n; const uint8_t strs[8] = { ' ', 0 }; put(&L, strs, 8);
    const uint64_t link_file = link_vm;      /* file offsets equal vm addresses for every segment */

    /* load commands */
    buf C = {0};
    struct segment_command_64 text = { LC_SEGMENT_64, sizeof(struct segment_command_64) + sizeof(struct section_64), "__TEXT",
        0, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE, 1, 0 };
    struct section_64 img = { "__husk_image", "__TEXT", HDR, S, (uint32_t)HDR, 14, 0, 0,
        S_REGULAR | S_ATTR_SOME_INSTRUCTIONS, 0, 0, 0 };
    struct section_64 man = { "__husk_manifest", "__TEXT", MANIFEST_AT, sizeof(tl_aot_manifest), (uint32_t)MANIFEST_AT, 3, 0, 0,
        S_REGULAR, 0, 0, 0 };
    text.cmdsize += sizeof(struct section_64); text.nsects = 2;
    put(&C, &text, sizeof(text)); put(&C, &img, sizeof(img)); put(&C, &man, sizeof(man));
    struct segment_command_64 data = { LC_SEGMENT_64, sizeof(struct segment_command_64) + sizeof(struct section_64), "__DATA",
        data_vm, S, data_vm, S, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 1, 0 };
    struct section_64 dat = { "__husk_data", "__DATA", data_vm, S, (uint32_t)data_vm, 14, 0, 0, S_REGULAR, 0, 0, 0 };
    put(&C, &data, sizeof(data)); put(&C, &dat, sizeof(dat));
    struct segment_command_64 link = { LC_SEGMENT_64, sizeof(struct segment_command_64), "__LINKEDIT",
        link_vm, ROUND_UP(L.n), link_file, L.n, VM_PROT_READ, VM_PROT_READ, 0, 0 };
    put(&C, &link, sizeof(link));

    size_t nl = strlen(install_name) + 1, idsize = (sizeof(struct dylib_command) + nl + 7) & ~7ul;
    struct dylib_command id = { LC_ID_DYLIB, (uint32_t)idsize, { { sizeof(struct dylib_command) }, 2, 0x10000, 0x10000 } };
    size_t at = C.n; put(&C, &id, sizeof(id)); put(&C, install_name, nl); pad(&C, at + idsize);

    /* dyld refuses a dylib that links nothing: libSystem, as every dylib has */
    static const char libsystem[] = "/usr/lib/libSystem.B.dylib";
    size_t lssize = (sizeof(struct dylib_command) + sizeof(libsystem) + 7) & ~7ul;
    struct dylib_command ls = { LC_LOAD_DYLIB, (uint32_t)lssize, { { sizeof(struct dylib_command) }, 2, 0x05470000, 0x00010000 } };
    at = C.n; put(&C, &ls, sizeof(ls)); put(&C, libsystem, sizeof(libsystem)); pad(&C, at + lssize);

    struct linkedit_data_command fix = { LC_DYLD_CHAINED_FIXUPS, sizeof(struct linkedit_data_command), (uint32_t)link_file + fix_off, fix_size };
    put(&C, &fix, sizeof(fix));
    struct linkedit_data_command exp = { LC_DYLD_EXPORTS_TRIE, sizeof(struct linkedit_data_command), (uint32_t)link_file + trie_off, 8 };
    put(&C, &exp, sizeof(exp));
    struct symtab_command st = { LC_SYMTAB, sizeof(struct symtab_command), 0, 0, (uint32_t)link_file + str_off, 8 };
    put(&C, &st, sizeof(st));
    struct dysymtab_command dst = { LC_DYSYMTAB, sizeof(struct dysymtab_command) };
    put(&C, &dst, sizeof(dst));
    struct build_version_command bv = { LC_BUILD_VERSION, sizeof(struct build_version_command),
        ios ? PLATFORM_IOS : PLATFORM_MACOS, ios ? 0x000F0000u : 0x000B0000u, ios ? 0x001A0000u : 0x001A0000u, 0 };
    put(&C, &bv, sizeof(bv));
    struct uuid_command uu = { LC_UUID, sizeof(struct uuid_command) };
    uint64_t h = 1469598103934665603ull;                 /* the same library always gets the same UUID */
    for (size_t i = 0; i < E->flen; i += 4096) { h ^= E->file[i]; h *= 1099511628211ull; }
    memcpy(uu.uuid, &h, 8); h = h * 6364136223846793005ull + S; memcpy(uu.uuid + 8, &h, 8);
    put(&C, &uu, sizeof(uu));

    struct mach_header_64 mh = { MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_DYLIB, 11, (uint32_t)C.n,
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS, 0 };
    if (sizeof(mh) + C.n > MANIFEST_AT) { fprintf(stderr, "load commands do not fit\n"); return false; }
    tl_aot_manifest M = { TL_AOT_MAGIC, TL_AOT_VERSION, (uint32_t)PAGE, tl_aot_hash(E->file, E->flen), E->flen, A->base_vaddr,
                          A->npages, A->nstub, A->size };
    memcpy(M.magic, TL_AOT_MAGIC, 8);

    /* the file: header, padding, image, its copy, linkedit */
    buf F = {0};
    put(&F, &mh, sizeof(mh)); put(&F, C.b, C.n); pad(&F, MANIFEST_AT); put(&F, &M, sizeof(M)); pad(&F, HDR);
    put(&F, A->text, S);                  /* __TEXT: the patched image, stubs, thread-block page */
    put(&F, A->data, S);                  /* __DATA: the unpatched image */
    put(&F, L.b, L.n);

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(F.b, 1, F.n, f) != F.n) { fprintf(stderr, "%s: %s\n", out, strerror(errno)); if (f) fclose(f); return false; }
    fclose(f);
    printf("%s: image at vaddr 0x%llx, %zu + %zu stub + 1 pages (%llu KiB, PT_LOAD align up to 0x%llx), delta 0x%llx, %s\n"
           "  rewritten: %zu x18 (%zu not), %zu svc (%zu out of range), %zu tpidr, %zu adrp, %zu adr (%zu not); %zu pages mix code and data\n",
           out, (unsigned long long)A->base_vaddr, A->npages, A->nstub, (unsigned long long)(S >> 10), (unsigned long long)E->max_align,
           (unsigned long long)S, ios ? "iOS" : "macOS", A->n_x18, A->n_x18_failed, A->n_svc, A->n_svc_far, A->n_tpidr, A->n_adrp,
           A->n_adr, A->n_adr_failed, A->n_mixed);
    free(F.b); free(C.b); free(L.b);
    return true;
}

/* ------------------------------------------------------------------ check */

static const struct mach_header_64 *header_of(const char *path)
{
    char want[PATH_MAX]; if (!realpath(path, want)) return NULL;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        char got[PATH_MAX];
        if (realpath(_dyld_get_image_name(i), got) && !strcmp(got, want)) return (const struct mach_header_64 *)_dyld_get_image_header(i);
    }
    return NULL;
}

static vm_prot_t prot_at(const void *p)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p; mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    extern kern_return_t mach_vm_region(vm_map_t, mach_vm_address_t *, mach_vm_size_t *, vm_region_flavor_t, vm_region_info_t,
                                        mach_msg_type_number_t *, mach_port_t *);
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;
    return info.protection;
}

static bool build(const elf *E, const char *so, tl_ld_aot_image *A)
{
    const char *slash = strrchr(so, '/');
    if (!tl_ld_aot_build(slash ? slash + 1 : so, E->file, E->flen, A)) { fprintf(stderr, "%s: %s\n", so, A->err); return false; }
    return true;
}

static int check(const char *dylib, const char *so, const char *fn, long arg)
{
    elf E; if (!elf_open(&E, so)) return 1;
    tl_ld_aot_image A; if (!build(&E, so, &A)) return 1;
    void *h = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const struct mach_header_64 *mh = header_of(dylib);
    if (!mh) { fprintf(stderr, "loaded, but dyld does not list it\n"); return 1; }
    unsigned long isz = 0, dsz = 0, msz = 0;
    uint8_t *rx = getsectiondata(mh, "__TEXT", "__husk_image", &isz);
    uint8_t *rw = getsectiondata(mh, "__DATA", "__husk_data", &dsz);
    const tl_aot_manifest *M = (const tl_aot_manifest *)getsectiondata(mh, "__TEXT", "__husk_manifest", &msz);
    const uint64_t S = A.size;
    printf("dlopen ok: header %p, image %p (%lu KiB), copy %p, delta 0x%lx\n", (void *)mh, (void *)rx, isz >> 10, (void *)rw, (long)(rw - rx));
    if (!rx || !rw || isz != S || dsz != S || (uint64_t)(rw - rx) != S) { fprintf(stderr, "FAIL: sections are not where they belong\n"); return 1; }
    if ((uintptr_t)rx % PAGE || (uintptr_t)rw % PAGE) { fprintf(stderr, "FAIL: not 16 KiB aligned\n"); return 1; }
    if (!M || msz != sizeof(*M) || memcmp(M->magic, TL_AOT_MAGIC, 8) || M->version != TL_AOT_VERSION || M->so_hash != tl_aot_hash(E.file, E.flen)
        || M->size != S || M->base_vaddr != A.base_vaddr) { fprintf(stderr, "FAIL: the manifest does not describe this library\n"); return 1; }
    /* The same library patched again, in other memory at another address, must give the same bytes: nothing in the image depends
     * on where it was built. */
    if (memcmp(rx, A.text, S)) {
        size_t k = 0; while (k < S && rx[k] == A.text[k]) k++;
        fprintf(stderr, "FAIL: the dylib's code differs from a fresh build at +%#zx\n", k); return 1;
    }
    if (memcmp(rw, A.data, S)) { fprintf(stderr, "FAIL: the dylib's data differs from the .so\n"); return 1; }
    vm_prot_t px = prot_at(rx), pw = prot_at(rw);
    printf("matches a fresh build; image %c%c%c, copy %c%c%c\n", px & 1 ? 'r' : '-', px & 2 ? 'w' : '-', px & 4 ? 'x' : '-',
           pw & 1 ? 'r' : '-', pw & 2 ? 'w' : '-', pw & 4 ? 'x' : '-');
    if ((px & (VM_PROT_WRITE | VM_PROT_EXECUTE)) != VM_PROT_EXECUTE || (pw & (VM_PROT_WRITE | VM_PROT_EXECUTE)) != VM_PROT_WRITE) {
        fprintf(stderr, "FAIL: protections\n"); return 1;
    }
    rw[S - 1] = 0x5a;                     /* the copy takes writes */
    if (fn) {
        uint64_t v = elf_lookup(&E, fn);
        if (!v) { fprintf(stderr, "FAIL: %s is not a defined symbol of %s\n", fn, so); return 1; }
        const char *(*f)(long) = (const char *(*)(long))(rx + (v - A.base_vaddr));
        const char *r = f(arg);
        /* a string in read-only data is read through the image; one in writable data through the copy */
        bool in = r && (((uint8_t *)r >= rx && (uint8_t *)r < rx + S) || ((uint8_t *)r >= rw && (uint8_t *)r < rw + S));
        printf("%s(%ld) from the dylib's code at %p returned %p: \"%s\"\n", fn, arg, (void *)f, (void *)r, in ? r : "(outside the image)");
    }
    tl_ld_aot_free(&A);
    printf("OK\n");
    return 0;
}

int main(int argc, char **argv)
{
    tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 0);
    if (argc >= 4 && !strcmp(argv[1], "--load")) return check(argv[2], argv[3], argc > 4 ? argv[4] : NULL, argc > 5 ? strtol(argv[5], NULL, 0) : 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <lib.so> <out.dylib> [--ios]\n       %s --load <out.dylib> <lib.so> [leaf function [argument]]\n", argv[0], argv[0]);
        return 2;
    }
    bool ios = argc > 3 && !strcmp(argv[3], "--ios");
    elf E; if (!elf_open(&E, argv[1])) return 1;
    tl_ld_aot_image A; if (!build(&E, argv[1], &A)) return 1;
    const char *slash = strrchr(argv[2], '/');
    char install[256]; snprintf(install, sizeof(install), "@rpath/%s", slash ? slash + 1 : argv[2]);
    bool ok = write_dylib(&E, &A, argv[2], install, ios);
    tl_ld_aot_free(&A);
    if (ok && !ios) {
        char cmd[2048]; snprintf(cmd, sizeof(cmd), "codesign -s - -f '%s' 2>/dev/null", argv[2]);
        if (system(cmd) != 0) { fprintf(stderr, "%s: codesign failed\n", argv[2]); return 1; }
    }
    return ok ? 0 : 1;
}
