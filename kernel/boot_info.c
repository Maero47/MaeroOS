/*
 * Boot information: Multiboot 1 as-is, Multiboot 2 translated into the
 * Multiboot 1 view the rest of the kernel reads.  See include/kernel/boot_info.h.
 *
 * Multiboot 2 layout (spec 2.0, https://www.gnu.org/software/grub/manual/multiboot2/):
 * u32 total_size, u32 reserved, then 8-byte aligned tags {u32 type, u32 size,
 * payload}, ending with a type 0 tag.  boot.asm copied the block into
 * mb2_info_copy (clamped to its size) before paging, so every tag is read from
 * kernel .bss here and nothing below needs the loader's memory mapped.
 */
#include <kernel/boot_info.h>
#include <kernel/config.h>
#include "printk.h"
#include "../lib/string.h"
#include <stdint.h>

/* boot.asm: the early mapping covers physical [0, boot_mapped_bytes). */
extern char boot_mapped_bytes[];

#define MB2_TAG_END             0
#define MB2_TAG_CMDLINE         1
#define MB2_TAG_LOADER_NAME     2
#define MB2_TAG_MODULE          3
#define MB2_TAG_BASIC_MEMINFO   4
#define MB2_TAG_MMAP            6
#define MB2_TAG_FRAMEBUFFER     8
#define MB2_TAG_EFI32           11
#define MB2_TAG_EFI64           12
#define MB2_TAG_ACPI_OLD        14
#define MB2_TAG_ACPI_NEW        15
#define MB2_TAG_EFI_MMAP        17
#define MB2_TAG_EFI_BS          18
#define MB2_TAG_EFI32_IH        19
#define MB2_TAG_EFI64_IH        20

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
} __attribute__((packed)) mb2_mmap_entry_t;

/* UEFI EFI_MEMORY_DESCRIPTOR (descriptor_size may be larger; use the tag's). */
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
} __attribute__((packed)) efi_mem_desc_t;

#define MAX_MMAP     256
#define MAX_MODS     8
#define STR_MAX      256
#define RSDP_MAX     36

/* framebuffer.c reads the RGB field positions/sizes that Multiboot 1 places
 * right after framebuffer_type, past the end of multiboot_info_t. */
static struct {
    multiboot_info_t mb;
    uint8_t color[6];
} __attribute__((packed)) mb1_view;

static multiboot_mmap_entry_t mmap_view[MAX_MMAP];
static multiboot_module_t     mods_view[MAX_MODS];
static char     cmdline_buf[STR_MAX];
static char     loader_buf[STR_MAX];
static uint8_t  rsdp_buf[RSDP_MAX];
static uint32_t rsdp_len;
static int      protocol;
static int      is_efi;

static uint32_t phys_of(const void *p) {
    return (uint32_t)(uintptr_t)p - KERNEL_VMA;
}

static void copy_str(char *dst, const char *src, uint32_t max_src) {
    uint32_t n = 0;
    while (n < max_src && n < STR_MAX - 1 && src[n]) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

static int checksum_ok(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += p[i];
    return sum == 0;
}

/* Keep the RSDP from tag 14/15 if its checksums hold; ACPI 2.0+ wins. */
static void take_rsdp(const uint8_t *p, uint32_t len, int is_new) {
    if (len < 20 || memcmp(p, "RSD PTR ", 8) != 0 || !checksum_ok(p, 20)) {
        printk("[BOOT] ignoring an invalid ACPI RSDP tag\n");
        return;
    }
    uint32_t want = 20;
    if (is_new && p[15] >= 2 && len >= RSDP_MAX && checksum_ok(p, RSDP_MAX))
        want = RSDP_MAX;
    if (want < rsdp_len) return;        /* already have a 2.0 one */
    memcpy(rsdp_buf, p, want);
    rsdp_len = want;
}

static void add_mmap(uint64_t addr, uint64_t len, uint32_t type,
                     uint32_t *count, uint64_t *top) {
    if (*count >= MAX_MMAP || len == 0) return;
    multiboot_mmap_entry_t *e = &mmap_view[(*count)++];
    e->size = sizeof(*e) - sizeof(e->size);
    e->addr = addr;
    e->len  = len;
    e->type = type;
    if (type == MULTIBOOT_MEMORY_AVAILABLE && addr + len > *top)
        *top = addr + len;
}

/* EFI memory types -> Multiboot: after ExitBootServices, loader and boot
 * services memory is free RAM (the modules in it are reserved by kernel_main;
 * the info block was copied into .bss). */
static uint32_t efi_to_mb_type(uint32_t t) {
    switch (t) {
    case 1: case 2: case 3: case 4: case 7: return MULTIBOOT_MEMORY_AVAILABLE;
    case 9:  return 3;      /* ACPI reclaimable */
    case 10: return 4;      /* ACPI NVS */
    case 8:  return 5;      /* unusable */
    default: return MULTIBOOT_MEMORY_RESERVED;
    }
}

static multiboot_info_t *mb2_translate(uint32_t info_phys) {
    multiboot_info_t *mb = &mb1_view.mb;
    const uint8_t *base = (const uint8_t *)(uintptr_t)(info_phys + KERNEL_VMA);
    uint32_t total = *(const uint32_t *)base;
    uint32_t nmmap = 0, nmods = 0;
    uint64_t top = 0;
    const mb2_tag_t *efi_mmap = NULL;
    int have_mmap = 0;

    memset(&mb1_view, 0, sizeof(mb1_view));

    for (uint32_t off = 8; off + sizeof(mb2_tag_t) <= total; ) {
        const mb2_tag_t *tag = (const mb2_tag_t *)(base + off);
        if (tag->type == MB2_TAG_END) break;
        if (tag->size < sizeof(mb2_tag_t) || tag->size > total - off) {
            printk("[BOOT] multiboot2: truncated tag %u at offset %u\n",
                   (unsigned)tag->type, (unsigned)off);
            break;
        }
        const uint8_t *pl = (const uint8_t *)(tag + 1);
        uint32_t plen = tag->size - sizeof(mb2_tag_t);

        switch (tag->type) {
        case MB2_TAG_CMDLINE:
            copy_str(cmdline_buf, (const char *)pl, plen);
            mb->cmdline = phys_of(cmdline_buf);
            mb->flags |= MULTIBOOT_FLAG_CMDLINE;
            break;
        case MB2_TAG_LOADER_NAME:
            copy_str(loader_buf, (const char *)pl, plen);
            mb->boot_loader_name = phys_of(loader_buf);
            break;
        case MB2_TAG_MODULE:
            if (plen >= 8 && nmods < MAX_MODS) {
                const uint32_t *m = (const uint32_t *)pl;
                mods_view[nmods].mod_start = m[0];
                mods_view[nmods].mod_end   = m[1];
                mods_view[nmods].cmdline   = phys_of(pl + 8);
                nmods++;
            }
            break;
        case MB2_TAG_BASIC_MEMINFO:
            if (plen >= 8) {
                mb->mem_lower = ((const uint32_t *)pl)[0];
                mb->mem_upper = ((const uint32_t *)pl)[1];
                mb->flags |= MULTIBOOT_FLAG_MEM;
            }
            break;
        case MB2_TAG_MMAP: {
            if (plen < 8) break;
            uint32_t esz = ((const uint32_t *)pl)[0];
            if (esz < sizeof(mb2_mmap_entry_t)) break;
            for (uint32_t o = 8; o + esz <= plen; o += esz) {
                const mb2_mmap_entry_t *e = (const mb2_mmap_entry_t *)(pl + o);
                add_mmap(e->addr, e->len, e->type, &nmmap, &top);
            }
            have_mmap = 1;
            break;
        }
        case MB2_TAG_FRAMEBUFFER:
            /* u64 addr, u32 pitch, u32 width, u32 height, u8 bpp, u8 type,
             * u16 reserved, then (type 1, RGB) six field position/size bytes:
             * the same order Multiboot 1 uses. */
            if (plen >= 24) {
                memcpy(&mb->framebuffer_addr, pl, 8);
                memcpy(&mb->framebuffer_pitch, pl + 8, 4);
                memcpy(&mb->framebuffer_width, pl + 12, 4);
                memcpy(&mb->framebuffer_height, pl + 16, 4);
                mb->framebuffer_bpp  = pl[20];
                mb->framebuffer_type = pl[21];
                if (plen >= 30) memcpy(mb1_view.color, pl + 24, 6);
                mb->flags |= MULTIBOOT_FLAG_FB;
            }
            break;
        case MB2_TAG_EFI32: case MB2_TAG_EFI64:
        case MB2_TAG_EFI_BS: case MB2_TAG_EFI32_IH: case MB2_TAG_EFI64_IH:
            is_efi = 1;
            break;
        case MB2_TAG_EFI_MMAP:
            is_efi = 1;
            efi_mmap = tag;
            break;
        case MB2_TAG_ACPI_OLD:
            take_rsdp(pl, plen, 0);
            break;
        case MB2_TAG_ACPI_NEW:
            take_rsdp(pl, plen, 1);
            break;
        default:
            break;
        }
        off += (tag->size + 7) & ~7u;
    }

    /* No Multiboot memory map: derive one from the EFI map. */
    if (!have_mmap && efi_mmap) {
        const uint8_t *pl = (const uint8_t *)(efi_mmap + 1);
        uint32_t plen = efi_mmap->size - sizeof(mb2_tag_t);
        uint32_t dsz = plen >= 8 ? ((const uint32_t *)pl)[0] : 0;
        if (dsz >= sizeof(efi_mem_desc_t)) {
            for (uint32_t o = 8; o + dsz <= plen; o += dsz) {
                const efi_mem_desc_t *d = (const efi_mem_desc_t *)(pl + o);
                add_mmap(d->phys_start, d->num_pages * 4096ULL,
                         efi_to_mb_type(d->type), &nmmap, &top);
            }
            have_mmap = 1;
        }
    }

    if (nmmap) {
        mb->mmap_addr   = phys_of(mmap_view);
        mb->mmap_length = nmmap * sizeof(mmap_view[0]);
        mb->flags |= MULTIBOOT_FLAG_MMAP;
        /* pmm_init sizes its bitmap from mem_upper.  Under UEFI the basic
         * meminfo tag (if any) stops at the first firmware hole, far below
         * the end of RAM, so size it from the highest available byte. */
        if (top > 0x100000000ULL) top = 0x100000000ULL;
        uint32_t upper_kb = top > 0x100000 ? (uint32_t)((top - 0x100000) / 1024) : 0;
        if (!(mb->flags & MULTIBOOT_FLAG_MEM) || upper_kb > mb->mem_upper) {
            if (!(mb->flags & MULTIBOOT_FLAG_MEM)) mb->mem_lower = 640;
            mb->mem_upper = upper_kb;
            mb->flags |= MULTIBOOT_FLAG_MEM;
        }
    }
    if (nmods) {
        mb->mods_addr  = phys_of(mods_view);
        mb->mods_count = nmods;
        mb->flags |= MULTIBOOT_FLAG_MODS;
    }

    printk("[BOOT] multiboot2: loader \"%s\"%s, %u mmap entries, %u module(s), "
           "fb %s, ACPI RSDP %s\n",
           loader_buf, is_efi ? " (UEFI)" : " (BIOS)",
           (unsigned)nmmap, (unsigned)nmods,
           (mb->flags & MULTIBOOT_FLAG_FB) ? "yes" : "no",
           rsdp_len == RSDP_MAX ? "v2" : rsdp_len ? "v1" : "none");
    return mb;
}

/* The Multiboot 1 block is wherever the loader put it; read it in place
 * (GRUB and QEMU keep it low) and copy out the strings while the boot
 * mapping still reaches them. */
static multiboot_info_t *mb1_take(uint32_t info_phys) {
    multiboot_info_t *mb = (multiboot_info_t *)(uintptr_t)(info_phys + KERNEL_VMA);
    uint32_t mapped = (uint32_t)(uintptr_t)boot_mapped_bytes;
    if ((mb->flags & MULTIBOOT_FLAG_CMDLINE) && mb->cmdline < mapped - STR_MAX)
        copy_str(cmdline_buf, (const char *)(uintptr_t)(mb->cmdline + KERNEL_VMA), STR_MAX);
    if ((mb->flags & (1u << 9)) && mb->boot_loader_name &&
        mb->boot_loader_name < mapped - STR_MAX)
        copy_str(loader_buf, (const char *)(uintptr_t)(mb->boot_loader_name + KERNEL_VMA), STR_MAX);
    return mb;
}

multiboot_info_t *boot_info_init(uint32_t magic, uint32_t info_phys) {
    if (magic == MULTIBOOT1_BOOTLOADER_MAGIC) {
        protocol = 1;
        return mb1_take(info_phys);
    }
    if (magic == MULTIBOOT2_BOOTLOADER_MAGIC) {
        protocol = 2;
        return mb2_translate(info_phys);
    }
    return NULL;
}

int boot_info_protocol(void) { return protocol; }
int boot_info_is_efi(void)   { return is_efi; }

const void *boot_info_rsdp(uint32_t *len) {
    if (len) *len = rsdp_len;
    return rsdp_len ? rsdp_buf : NULL;
}

const char *boot_info_cmdline(void)     { return cmdline_buf; }
const char *boot_info_loader_name(void) { return loader_buf; }
