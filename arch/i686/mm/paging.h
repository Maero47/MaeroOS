#pragma once
#include <stdint.h>

/* PTE/PDE flag bits */
#define PAGE_PRESENT    0x001
#define PAGE_WRITABLE   0x002
#define PAGE_USER       0x004
#define PAGE_WRITETHRU  0x008
#define PAGE_NOCACHE    0x010
#define PAGE_ACCESSED   0x020
#define PAGE_DIRTY      0x040
#define PAGE_HUGE       0x080  /* PDE: 4 MiB page */
/* Bit 8 is the hardware Global bit, which the CPU only honours when CR4.PGE
 * is set.  This kernel never enables PGE (fpu.c sets only OSFXSR/OSXMMEXCPT),
 * so the bit is ignored by the MMU on every entry and is used as software
 * storage: "write permission on this page was removed by mprotect()".  It
 * survives fork's COW marking, so the copy-on-write handler can tell a page
 * that is read-only because it is shared (break it) from one the process
 * asked to be read-only (SIGSEGV).  Enabling CR4.PGE would require moving
 * this flag. */
#define PAGE_WRPROT     0x100
#define PAGE_COW        0x200  /* AVL bit 9: copy-on-write */
#define PAGE_SHARED     0x400  /* AVL bit 10: shared memory — never COW'd */
#define PAGE_PROTNONE   0x800  /* AVL bit 11: PROT_NONE page — NOT present, but the
                                * entry still owns its frame so the data survives
                                * mprotect(PROT_NONE) → mprotect(RW) (Linux
                                * _PAGE_PROTNONE).  Any access faults. */

/* Recursive page table addresses (PDE[1023] = page directory itself) */
#define PAGE_TABLES_BASE  0xFFC00000U  /* Start of all page tables (virtual) */
#define PAGE_DIR_VIRT     0xFFFFF000U  /* Page directory itself (virtual) */

/* Get pointer to the PTE for a virtual address */
static inline uint32_t *paging_get_pte(uint32_t virt) {
    return (uint32_t *)(PAGE_TABLES_BASE + ((virt >> 12) * 4));
}

/* Get pointer to the PDE for a virtual address */
static inline uint32_t *paging_get_pde(uint32_t virt) {
    return (uint32_t *)(PAGE_DIR_VIRT + ((virt >> 22) * 4));
}

void paging_init(void);
void paging_map(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_unmap(uint32_t virt);
uint32_t paging_get_physical(uint32_t virt);

/* Make kernel .text and .rodata read-only (W^X enforcement) */
void paging_set_kernel_permissions(void);

/* Physical address of the kernel page directory (set by paging_init) */
extern uint32_t kernel_pgdir_phys;

/*
 * Temporary kernel virtual addresses for accessing arbitrary physical frames.
 * These use PTEs 1 and 2 of the shared boot_page_table1.  Must only be used
 * with interrupts disabled (IF=0) to avoid races.
 */
#define TEMP_MAP_VIRT  0xC0001000U
#define TEMP_MAP_VIRT2 0xC0002000U

void *paging_temp_map(uint32_t phys);   /* maps phys at TEMP_MAP_VIRT */
void  paging_temp_unmap(void);
void *paging_temp_map2(uint32_t phys);  /* maps phys at TEMP_MAP_VIRT2 */
void  paging_temp_unmap2(void);

/* Allocate a new page directory with kernel mappings copied from the current one */
uint32_t pgdir_create(void);

/* Map a page in a specific page directory (not necessarily the current CR3).
 * Caller must ensure IF=0 around this call. */
void pgdir_map(uint32_t pgdir_phys, uint32_t virt, uint32_t phys, uint32_t flags);

/* Return the physical frame backing `virt` in pgdir_phys, or 0 if unmapped.
 * Caller must ensure IF=0 (switches CR3 transiently). */
uint32_t pgdir_virt_to_phys(uint32_t pgdir_phys, uint32_t virt);

/* Free all user-space pages (PDE 0-767) in a page directory, then free the
 * pgdir frame itself.  Caller must ensure IF=0. */
void pgdir_free_user(uint32_t pgdir_phys);

/*
 * Page-directory sharing (threads / CLONE_VM).  pgdir_retain marks one more
 * user of a pgdir; pgdir_release drops one and returns non-zero while other
 * users remain (the caller must NOT free).  An unshared pgdir is not in the
 * table: release returns 0 and the caller frees as usual.
 */
void pgdir_retain(uint32_t pgdir_phys);
int pgdir_release(uint32_t pgdir_phys);

/* Debug: arm a 4-byte hardware write-watchpoint (DR0/#DB) on a linear address
 * for one pid, reporting the instruction that writes `target_value` there. */
