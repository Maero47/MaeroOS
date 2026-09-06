#pragma once
#include <stdint.h>
#include "../fs/vfs.h"

/* ELF32 file header */
typedef struct {
    uint8_t  e_ident[16];   /* [0-3]="\x7fELF", [4]=class (1=32-bit), [5]=endian */
    uint16_t e_type;        /* 2 = ET_EXEC */
    uint16_t e_machine;     /* 3 = EM_386 */
    uint32_t e_version;
    uint32_t e_entry;       /* Entry point virtual address */
    uint32_t e_phoff;       /* Program header table offset (bytes from file start) */
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;      /* ELF header size (52 for ELF32) */
    uint16_t e_phentsize;   /* Program header entry size (32 for ELF32) */
    uint16_t e_phnum;       /* Number of program headers */
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

/* ELF32 program header */
typedef struct {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_offset;      /* Offset in file */
    uint32_t p_vaddr;       /* Virtual address to load at */
    uint32_t p_paddr;
    uint32_t p_filesz;      /* Bytes in file */
    uint32_t p_memsz;       /* Bytes in memory (>= p_filesz; BSS = memsz - filesz) */
    uint32_t p_flags;       /* 1=X, 2=W, 4=R */
    uint32_t p_align;
} __attribute__((packed)) Elf32_Phdr;

#define PT_LOAD   1
#define PT_INTERP 3
#define ET_EXEC   2
#define ET_DYN    3
#define EM_386    3

/* Extra ELF facts the exec auxv needs (Linux AT_PHDR family + dynamic). */
typedef struct {
    uint32_t entry;         /* biased entry point (e_entry + load_bias) */
    uint32_t heap_end;      /* page-aligned top of all segments (biased) */
    uint32_t load_bias;     /* base the object was actually loaded at */
    uint32_t phdr_vaddr;    /* user VA of the program header table (0=unknown) */
    uint16_t phent;
    uint16_t phnum;
    int      has_interp;    /* 1 if a PT_INTERP was present */
    char     interp[80];    /* interpreter path ("" if none) */
} elf_info_t;

/*
 * elf_load — parse an ELF32 executable from a VFS node and map its PT_LOAD
 * segments into the given page directory.
 *
 * On success: fills *out_entry with e_entry, *out_heap_end with the first
 * page-aligned address above all loaded segments, and returns 0.
 * On failure: returns -1 (bad ELF magic / OOM).
 *
 * Each mapped physical frame has pmm_frame_incref called on it; the caller
 * is responsible for freeing via pgdir_free_user() when done.
 */
int elf_load(vfs_node_t *node, uint32_t pgdir_phys,
             uint32_t *out_entry, uint32_t *out_heap_end);

/* As elf_load, also reporting phdr location facts (info may be NULL). */
int elf_load_ex(vfs_node_t *node, uint32_t pgdir_phys,
                uint32_t *out_entry, uint32_t *out_heap_end,
                elf_info_t *info);

/*
 * elf_load_bias — core loader that also accepts ET_DYN (PIE / ld.so) objects
 * and applies a load bias.  For ET_EXEC the bias is forced to 0 (fixed VAs).
 * For ET_DYN the object is loaded at `want_bias`.  PT_INTERP is recorded in
 * info->interp (not rejected).  All address facts in *info are bias-adjusted.
 * info must be non-NULL.
 */
int elf_load_bias(vfs_node_t *node, uint32_t pgdir_phys, uint32_t want_bias,
                  elf_info_t *info);
