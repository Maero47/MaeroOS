#pragma once
#include <stdint.h>

/*
 * Multiboot 1 structures — exactly as the spec defines them.
 * All structs are packed to prevent compiler padding.
 * Reference: https://www.gnu.org/software/grub/manual/multiboot/multiboot.html
 */

/* multiboot_info_t.flags bits */
#define MULTIBOOT_FLAG_MEM      (1 << 0)   /* mem_lower/mem_upper valid */
#define MULTIBOOT_FLAG_BOOTDEV  (1 << 1)
#define MULTIBOOT_FLAG_CMDLINE  (1 << 2)
#define MULTIBOOT_FLAG_MODS     (1 << 3)
#define MULTIBOOT_FLAG_AOUT     (1 << 4)
#define MULTIBOOT_FLAG_ELF      (1 << 5)
#define MULTIBOOT_FLAG_MMAP     (1 << 6)   /* mmap_addr / mmap_length valid */
#define MULTIBOOT_FLAG_DRIVES   (1 << 7)
#define MULTIBOOT_FLAG_FB       (1 << 12)  /* framebuffer fields valid */

/* Memory map entry type */
#define MULTIBOOT_MEMORY_AVAILABLE  1
#define MULTIBOOT_MEMORY_RESERVED   2

/* ─── Memory map entry ──────────────────────────────────────────────────── */
typedef struct {
    uint32_t size;      /* Size of this entry NOT including this field itself */
    uint64_t addr;      /* Physical start address */
    uint64_t len;       /* Length in bytes */
    uint32_t type;      /* 1 = available, 2 = reserved, etc. */
} __attribute__((packed)) multiboot_mmap_entry_t;

/* ─── Module descriptor ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t mod_start;  /* Physical start of module */
    uint32_t mod_end;    /* Physical end of module */
    uint32_t cmdline;    /* Physical address of NUL-terminated command string */
    uint32_t pad;
} __attribute__((packed)) multiboot_module_t;

/* ─── Top-level multiboot_info ──────────────────────────────────────────── */
typedef struct {
    uint32_t flags;

    /* Present if MULTIBOOT_FLAG_MEM */
    uint32_t mem_lower;   /* KiB of lower memory (below 1 MiB) */
    uint32_t mem_upper;   /* KiB of upper memory (above 1 MiB) */

    uint32_t boot_device;

    uint32_t cmdline;     /* Physical address of boot command line */

    /* Modules (MULTIBOOT_FLAG_MODS) */
    uint32_t mods_count;
    uint32_t mods_addr;   /* Physical address of first multiboot_module_t */

    /* ELF section table (MULTIBOOT_FLAG_ELF) */
    uint32_t syms[4];

    /* Memory map (MULTIBOOT_FLAG_MMAP) */
    uint32_t mmap_length; /* Total size of mmap buffer in bytes */
    uint32_t mmap_addr;   /* Physical address of first mmap entry */

    uint32_t drives_length;
    uint32_t drives_addr;

    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    /* VBE info */
    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;

    /* Framebuffer (MULTIBOOT_FLAG_FB) */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp, framebuffer_type;
} __attribute__((packed)) multiboot_info_t;
