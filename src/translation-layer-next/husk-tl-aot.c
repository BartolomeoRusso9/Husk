/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Android libraries as Mach-O dylibs, made ahead of time. See husk-tl-aot.h. */
#include "husk-tl-aot.h"

#include <errno.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE ((uint64_t)16384)
#define ROUND_UP(x) (((x) + PAGE - 1) & ~(PAGE - 1))
#define MANIFEST_AT (PAGE - 256)          /* the manifest's offset in the header page */

typedef struct { uint8_t *b; size_t n, cap; } buf;
static bool put(buf *B, const void *p, size_t n)
{
    if (B->n + n > B->cap) {
        size_t cap = (B->n + n) * 2 + 4096;
        uint8_t *nb = realloc(B->b, cap);
        if (!nb) return false;
        B->b = nb; B->cap = cap;
    }
    memcpy(B->b + B->n, p, n); B->n += n;
    return true;
}
static void pad(buf *B, size_t to) { static const uint8_t z[64]; while (B->n < to) put(B, z, to - B->n < 64 ? to - B->n : 64); }

/* ELF, as much as the data copy needs: the PT_LOADs' file bytes at their vaddrs */
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } elf_ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } elf_phdr;

static bool write_zeros(FILE *f, uint64_t n)
{
    static const uint8_t z[16384];
    while (n) { size_t k = n < sizeof(z) ? (size_t)n : sizeof(z); if (fwrite(z, 1, k, f) != k) return false; n -= k; }
    return true;
}

/* The data copy, streamed: `size` bytes from base_vaddr, the file's PT_LOAD bytes where they lie and zero elsewhere. */
static bool write_data(FILE *f, const uint8_t *file, size_t flen, uint64_t base, uint64_t size)
{
    const elf_ehdr *eh = (const elf_ehdr *)file;
    uint64_t at = base, end = base + size;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        elf_phdr p; memcpy(&p, file + eh->e_phoff + (size_t)i * eh->e_phentsize, sizeof(p));
        if (p.p_type != 1 || !p.p_filesz) continue;
        if (p.p_offset + p.p_filesz > flen || p.p_vaddr < at || p.p_vaddr + p.p_filesz > end) return false;   /* PT_LOADs ascend */
        if (!write_zeros(f, p.p_vaddr - at) || fwrite(file + p.p_offset, 1, (size_t)p.p_filesz, f) != p.p_filesz) return false;
        at = p.p_vaddr + p.p_filesz;
    }
    return write_zeros(f, end - at);
}

bool tl_aot_make_dylib(const char *name, const uint8_t *file, size_t flen, const char *out_path, bool ios, tl_ld_aot_image *info,
                       char *err, size_t errlen)
{
    tl_ld_aot_image A;
    if (!tl_ld_aot_build(name, file, flen, &A)) { snprintf(err, errlen, "%s", A.err); return false; }
    const uint64_t S = A.size;               /* image + stubs + thread block = the delta from the image to its writable copy */
    const uint64_t HDR = PAGE;               /* header and load commands, then the image at the next page */
    const uint64_t text_size = HDR + S, data_vm = text_size, link_vm = data_vm + S;
    bool ok = false;
    buf L = {0}, C = {0};
    FILE *f = NULL;
    char tmp[1100]; snprintf(tmp, sizeof(tmp), "%s.part", out_path);

    /* __LINKEDIT: chained-fixups header with no fixups, an empty export trie, an empty string table */
    struct dyld_chained_fixups_header fh = {0};
    fh.starts_offset = sizeof(fh);
    uint32_t starts[1 + 3] = { 3, 0, 0, 0 };            /* seg_count, then no fixups in __TEXT, __DATA, __LINKEDIT */
    fh.imports_offset = fh.starts_offset + sizeof(starts);
    fh.symbols_offset = fh.imports_offset;
    fh.imports_format = DYLD_CHAINED_IMPORT;
    put(&L, &fh, sizeof(fh)); put(&L, starts, sizeof(starts)); pad(&L, (L.n + 7) & ~7ul);
    const uint32_t fix_size = (uint32_t)L.n;
    const uint32_t trie_off = (uint32_t)L.n; const uint8_t trie[8] = { 0, 0 }; put(&L, trie, 8);
    const uint32_t str_off = (uint32_t)L.n; const uint8_t strs[8] = { ' ', 0 }; put(&L, strs, 8);
    const uint64_t link_file = link_vm;      /* file offsets equal vm addresses for every segment */

    /* load commands */
    struct segment_command_64 text = { LC_SEGMENT_64, sizeof(struct segment_command_64) + 2 * sizeof(struct section_64), "__TEXT",
        0, text_size, 0, text_size, VM_PROT_READ | VM_PROT_EXECUTE, VM_PROT_READ | VM_PROT_EXECUTE, 2, 0 };
    struct section_64 img = { "__husk_image", "__TEXT", HDR, S, (uint32_t)HDR, 14, 0, 0, S_REGULAR | S_ATTR_SOME_INSTRUCTIONS, 0, 0, 0 };
    struct section_64 man = { "__husk_manifest", "__TEXT", MANIFEST_AT, sizeof(tl_aot_manifest), (uint32_t)MANIFEST_AT, 3, 0, 0,
        S_REGULAR, 0, 0, 0 };
    put(&C, &text, sizeof(text)); put(&C, &img, sizeof(img)); put(&C, &man, sizeof(man));
    struct segment_command_64 data = { LC_SEGMENT_64, sizeof(struct segment_command_64) + sizeof(struct section_64), "__DATA",
        data_vm, S, data_vm, S, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, 1, 0 };
    struct section_64 dat = { "__husk_data", "__DATA", data_vm, S, (uint32_t)data_vm, 14, 0, 0, S_REGULAR, 0, 0, 0 };
    put(&C, &data, sizeof(data)); put(&C, &dat, sizeof(dat));
    struct segment_command_64 link = { LC_SEGMENT_64, sizeof(struct segment_command_64), "__LINKEDIT",
        link_vm, ROUND_UP(L.n), link_file, L.n, VM_PROT_READ, VM_PROT_READ, 0, 0 };
    put(&C, &link, sizeof(link));

    const char *slash = strrchr(out_path, '/');
    char install[300]; snprintf(install, sizeof(install), "@rpath/%s", slash ? slash + 1 : out_path);
    size_t nl = strlen(install) + 1, idsize = (sizeof(struct dylib_command) + nl + 7) & ~7ul;
    struct dylib_command id = { LC_ID_DYLIB, (uint32_t)idsize, { { sizeof(struct dylib_command) }, 2, 0x10000, 0x10000 } };
    size_t at = C.n; put(&C, &id, sizeof(id)); put(&C, install, nl); pad(&C, at + idsize);

    /* dyld refuses a dylib that links nothing: libSystem, as every dylib has */
    static const char libsystem[] = "/usr/lib/libSystem.B.dylib";
    size_t lssize = (sizeof(struct dylib_command) + sizeof(libsystem) + 7) & ~7ul;
    struct dylib_command ls = { LC_LOAD_DYLIB, (uint32_t)lssize, { { sizeof(struct dylib_command) }, 2, 0x05470000, 0x00010000 } };
    at = C.n; put(&C, &ls, sizeof(ls)); put(&C, libsystem, sizeof(libsystem)); pad(&C, at + lssize);

    struct linkedit_data_command fix = { LC_DYLD_CHAINED_FIXUPS, sizeof(struct linkedit_data_command), (uint32_t)link_file, fix_size };
    put(&C, &fix, sizeof(fix));
    struct linkedit_data_command exp = { LC_DYLD_EXPORTS_TRIE, sizeof(struct linkedit_data_command), (uint32_t)link_file + trie_off, 8 };
    put(&C, &exp, sizeof(exp));
    struct symtab_command st = { LC_SYMTAB, sizeof(struct symtab_command), 0, 0, (uint32_t)link_file + str_off, 8 };
    put(&C, &st, sizeof(st));
    struct dysymtab_command dst = { LC_DYSYMTAB, sizeof(struct dysymtab_command) };
    put(&C, &dst, sizeof(dst));
    struct build_version_command bv = { LC_BUILD_VERSION, sizeof(struct build_version_command),
        ios ? PLATFORM_IOS : PLATFORM_MACOS, ios ? 0x000F0000u : 0x000B0000u, 0x001A0000u, 0 };
    put(&C, &bv, sizeof(bv));
    tl_aot_manifest M = { "", TL_AOT_VERSION, (uint32_t)PAGE, tl_aot_hash(file, flen), flen, A.base_vaddr, A.npages, A.nstub, A.size };
    memcpy(M.magic, TL_AOT_MAGIC, 8);
    struct uuid_command uu = { LC_UUID, sizeof(struct uuid_command) };
    uint64_t h = M.so_hash ^ ((uint64_t)TL_AOT_VERSION << 56);   /* the same library and loader always give the same UUID */
    memcpy(uu.uuid, &h, 8); h = h * 6364136223846793005ull + S; memcpy(uu.uuid + 8, &h, 8);
    put(&C, &uu, sizeof(uu));

    struct mach_header_64 mh = { MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_DYLIB, 11, (uint32_t)C.n,
        MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_NO_REEXPORTED_DYLIBS, 0 };
    if (!C.b || !L.b || sizeof(mh) + C.n > MANIFEST_AT) { snprintf(err, errlen, "the load commands do not fit"); goto out; }

    /* the file, streamed: header page (with the manifest), image, its copy, linkedit */
    buf H = {0};
    put(&H, &mh, sizeof(mh)); put(&H, C.b, C.n); pad(&H, MANIFEST_AT); put(&H, &M, sizeof(M)); pad(&H, HDR);
    if (!H.b || H.n != HDR) { free(H.b); snprintf(err, errlen, "out of memory"); goto out; }
    if (!(f = fopen(tmp, "wb"))) { free(H.b); snprintf(err, errlen, "%s: %s", tmp, strerror(errno)); goto out; }
    bool w = fwrite(H.b, 1, H.n, f) == H.n
          && fwrite(A.text, 1, (size_t)S, f) == S
          && write_data(f, file, flen, A.base_vaddr, S)
          && fwrite(L.b, 1, L.n, f) == L.n;
    free(H.b);
    if (fclose(f) != 0) w = false;
    f = NULL;
    if (!w) { snprintf(err, errlen, "%s: writing failed (%s)", tmp, strerror(errno)); unlink(tmp); goto out; }
    if (rename(tmp, out_path) != 0) { snprintf(err, errlen, "%s: %s", out_path, strerror(errno)); unlink(tmp); goto out; }
    ok = true;
out:
    if (info) { *info = A; info->text = NULL; info->region = NULL; }
    tl_ld_aot_free(&A);
    free(L.b); free(C.b);
    return ok;
}
