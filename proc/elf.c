#include "elf.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../arch/i686/mm/paging.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include <kernel/config.h>
#include <stdint.h>

int elf_load(vfs_node_t *node, uint32_t pgdir_phys,
             uint32_t *out_entry, uint32_t *out_heap_end)
{
    return elf_load_ex(node, pgdir_phys, out_entry, out_heap_end, 0);
}

int elf_load_ex(vfs_node_t *node, uint32_t pgdir_phys,
                uint32_t *out_entry, uint32_t *out_heap_end,
                elf_info_t *info)
{
    elf_info_t local;
    elf_info_t *inf = info ? info : &local;
    int rc = elf_load_bias(node, pgdir_phys, 0, inf);
    if (rc < 0) return rc;
    *out_entry    = inf->entry;
    *out_heap_end = inf->heap_end;
    return 0;
}

int elf_load_bias(vfs_node_t *node, uint32_t pgdir_phys, uint32_t want_bias,
                  elf_info_t *info)
{
    if (!node || node->size < sizeof(Elf32_Ehdr) || !info) {
        printk("[ELF] reject '%s': bad node/size=%u\n",
               node ? node->name : "?", node ? (unsigned)node->size : 0);
        return -1;
    }

    /*
     * Linux never slurps the whole executable into a contiguous kernel buffer
     * — it pages segments in from the file.  We used to kmalloc(node->size)
     * for the ENTIRE image (firefox-bin is 686 KB, /disk/ff 143 KB), which
     * fails intermittently once the kernel heap is fragmented → exec returns
     * ENOMEM and the child dies (status 127).  Instead, read only the ELF
     * header + program-header table into a small buffer (`img`), and stream
     * each segment's file data through a one-page bounce buffer (`bounce`).
     * In-memory files (initrd, node->data != NULL) keep the zero-copy path.
     */
    uint8_t *owned_hdr = NULL;
    uint8_t *bounce    = NULL;
    const uint8_t *img = node->data;
    if (!img) {
        /* Read the fixed ELF header first to learn the phdr-table extent. */
        Elf32_Ehdr eh0;
        if (vfs_read(node, 0, sizeof(eh0), (uint8_t *)&eh0) != sizeof(eh0)) {
            printk("[ELF] short read (header) on '%s'\n", node->name);
            return -1;
        }
        uint32_t hdr_span = eh0.e_phoff +
                            (uint32_t)eh0.e_phnum * eh0.e_phentsize;
        if (hdr_span < sizeof(Elf32_Ehdr)) hdr_span = sizeof(Elf32_Ehdr);
        if (hdr_span > 65536 || hdr_span > node->size) {
            printk("[ELF] '%s': implausible phdr span %u\n",
                   node->name, (unsigned)hdr_span);
            return -1;
        }
        owned_hdr = (uint8_t *)kmalloc(hdr_span);
        if (!owned_hdr) {
            printk("[ELF] OOM: kmalloc(%u) for headers of '%s' FAILED\n",
                   (unsigned)hdr_span, node->name);
            return -1;
        }
        if (vfs_read(node, 0, hdr_span, owned_hdr) != hdr_span) {
            printk("[ELF] short read (phdrs) on '%s'\n", node->name);
            kfree(owned_hdr);
            return -1;
        }
        bounce = (uint8_t *)kmalloc(PAGE_SIZE);
        if (!bounce) {
            printk("[ELF] OOM: bounce page for '%s' FAILED\n", node->name);
            kfree(owned_hdr);
            return -1;
        }
        img = owned_hdr;   /* header+phdrs only; segment data via vfs_read */
    }

    /* Validate ELF magic and class */
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)img;
    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F'  ||
        ehdr->e_ident[4] != 1    ||   /* ELFCLASS32 */
        (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) ||
        ehdr->e_machine != EM_386) {
        printk("[ELF] Bad header in '%s'\n", node->name);
        if (owned_hdr) kfree(owned_hdr);
        if (bounce) kfree(bounce);
        return -1;
    }

    /* ET_EXEC has fixed virtual addresses (bias must be 0); ET_DYN (PIE,
     * ld.so) is position-independent and loads at want_bias. */
    uint32_t bias = (ehdr->e_type == ET_DYN) ? want_bias : 0;

    uint32_t heap_end = 0;

    info->entry      = 0;
    info->load_bias  = bias;
    info->phdr_vaddr = 0;
    info->phent      = ehdr->e_phentsize;
    info->phnum      = ehdr->e_phnum;
    info->has_interp = 0;
    info->interp[0]  = '\0';

    /* Walk program headers */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (ehdr->e_phoff + (uint32_t)(i + 1) * ehdr->e_phentsize > node->size)
            break;

        const Elf32_Phdr *phdr =
            (const Elf32_Phdr *)(img + ehdr->e_phoff + (uint32_t)i * ehdr->e_phentsize);

        if (phdr->p_type == PT_INTERP) {
            /* Record the interpreter path instead of rejecting it. */
            uint32_t n = phdr->p_filesz;
            if (n > sizeof(info->interp) - 1) n = sizeof(info->interp) - 1;
            if (phdr->p_offset + n <= node->size) {
                if (node->data) memcpy(info->interp, node->data + phdr->p_offset, n);
                else            vfs_read(node, phdr->p_offset, n, (uint8_t *)info->interp);
                info->interp[n] = '\0';
                info->has_interp = 1;
            }
            continue;
        }
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0)
            continue;

        /* The phdr table lives inside the segment that maps file offset 0
         * (standard for ET_EXEC and PIE) — report its biased user VA. */
        if (phdr->p_offset == 0 &&
            phdr->p_filesz >= ehdr->e_phoff +
                              (uint32_t)ehdr->e_phnum * ehdr->e_phentsize)
            info->phdr_vaddr = bias + phdr->p_vaddr + ehdr->e_phoff;

        /* Guard against p_vaddr + p_memsz integer overflow */
        if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) {
            printk("[ELF] Segment address overflow\n");
            if (owned_hdr) { kfree(owned_hdr); }
            if (bounce)    { kfree(bounce); }
            return -1;
        }

        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
            phdr->p_offset + phdr->p_filesz > node->size) {
            printk("[ELF] Segment exceeds file size\n");
            if (owned_hdr) { kfree(owned_hdr); }
            if (bounce)    { kfree(bounce); }
            return -1;
        }

        /* Page-align the biased virtual address range */
        uint32_t bvaddr = bias + phdr->p_vaddr;
        uint32_t vstart = bvaddr & ~(PAGE_SIZE - 1);
        uint32_t vend   = (bvaddr + phdr->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        /* Reject any segment that overlaps the kernel */
        if (vend > KERNEL_VMA || vend < vstart) {
            printk("[ELF] Segment extends into kernel VA space\n");
            if (owned_hdr) { kfree(owned_hdr); }
            if (bounce)    { kfree(bounce); }
            return -1;
        }

        /* Map and populate each page.  A biased segment may share its first
         * or last page with an adjacent segment (PIE text/data are often only
         * one page apart); reuse an already-mapped frame instead of clobbering. */
        for (uint32_t va = vstart; va < vend; va += PAGE_SIZE) {
            uint8_t *dst;
            uint32_t existing = pgdir_virt_to_phys(pgdir_phys, va);
            if (existing) {
                dst = (uint8_t *)paging_temp_map(existing);
            } else {
                uint32_t phys = pmm_alloc_frame();
                if (!phys) {
                    printk("[ELF] OOM loading segment\n");
                    if (owned_hdr) { kfree(owned_hdr); }
            if (bounce)    { kfree(bounce); }
                    return -1;
                }
                pmm_frame_incref(phys);
                pgdir_map(pgdir_phys, va, phys,
                          PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
                dst = (uint8_t *)paging_temp_map(phys);
                memset(dst, 0, PAGE_SIZE);   /* zero-fill BSS/holes up front */
            }

            /* Offset of this page within the aligned segment */
            uint32_t page_off = va - vstart;

            /* Virtual extent of file data, relative to vstart */
            uint32_t seg_file_start = bvaddr - vstart;
            if (page_off + PAGE_SIZE > seg_file_start &&
                page_off < seg_file_start + phdr->p_filesz) {
                uint32_t copy_start = (page_off > seg_file_start)
                                      ? page_off - seg_file_start : 0;
                uint32_t dst_off    = (seg_file_start > page_off)
                                      ? seg_file_start - page_off : 0;
                uint32_t avail      = phdr->p_filesz - copy_start;
                uint32_t room       = PAGE_SIZE - dst_off;
                uint32_t to_copy    = avail < room ? avail : room;
                /* Source the segment bytes from the file (disk) or the
                 * in-memory image.  vfs_read goes through a stable heap
                 * bounce buffer so it never nests inside the dst temp-map. */
                if (node->data) {
                    memcpy(dst + dst_off,
                           node->data + phdr->p_offset + copy_start, to_copy);
                } else {
                    vfs_read(node, phdr->p_offset + copy_start, to_copy, bounce);
                    memcpy(dst + dst_off, bounce, to_copy);
                }
            }

            paging_temp_unmap();
        }

        uint32_t seg_end = bvaddr + phdr->p_memsz;
        if (seg_end > heap_end)
            heap_end = seg_end;
    }

    /* Round heap start up to a page boundary */
    heap_end = (heap_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    info->entry    = bias + ehdr->e_entry;
    info->heap_end = heap_end;

    printk("[ELF]  Loaded '%s': type=%s bias=0x%08x entry=0x%08x top=0x%08x%s\n",
           node->name, ehdr->e_type == ET_DYN ? "DYN" : "EXEC",
           (unsigned)bias, (unsigned)info->entry, (unsigned)heap_end,
           info->has_interp ? " (needs ld.so)" : "");
    if (owned_hdr) kfree(owned_hdr);
    if (bounce) kfree(bounce);
    return 0;
}
