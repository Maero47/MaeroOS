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

/* No-execute (PAE + EFER.NXE only): bit 63 of a 64-bit entry.  A legacy
 * 32-bit entry has no room for it; pte_set() drops it there, and on a PAE CPU
 * without NX (where bit 63 is reserved) as well, so callers can always pass
 * it. */
#define PAGE_NX         0x8000000000000000ULL

/*
 * Page-table entries are 64-bit values everywhere in the kernel: PAE entries
 * are 8 bytes, legacy 32-bit ones are zero-extended on read and truncated on
 * write.  The mode is fixed at boot (boot.asm: CPUID PAE → 3-level paging).
 */
typedef uint64_t pte_t;

extern uint32_t paging_pae;   /* 1: PAE (3-level, 64-bit entries), set by boot.asm */
extern uint32_t paging_nx;    /* 1: EFER.NXE is on, PAGE_NX is honoured */

/* Frame address of an entry (physical memory below 4 GiB). */
static inline uint32_t pte_frame(pte_t e) { return (uint32_t)e & ~0xFFFU; }

/*
 * One page table covers PT_SPAN bytes: 4 MiB (1024 4-byte entries) legacy,
 * 2 MiB (512 8-byte entries) under PAE.  pt_index(va) is the global index of
 * the page table (= the PDE) that maps va, 0..1023 or 0..2047.
 */
#define PT_SHIFT        (paging_pae ? 21U : 22U)
#define PT_SPAN         (1U << PT_SHIFT)
#define PT_ENTRIES      (paging_pae ? 512U : 1024U)
#define USER_PT_COUNT   (0xC0000000U >> PT_SHIFT)   /* PDEs below KERNEL_VMA */
static inline uint32_t pt_index(uint32_t va) { return va >> PT_SHIFT; }
/* First address of the next page table's span; 0 when that wraps past 4 GiB. */
static inline uint32_t pt_next(uint32_t va) { return (va & ~(PT_SPAN - 1)) + PT_SPAN; }

/*
 * Recursive mapping.
 *   legacy: PDE[1023] = the page directory itself.
 *           0xFFC00000 + n*4096 = page table n, 0xFFFFF000 = the directory.
 *   PAE:    PD3[508..511] = page directories 0..3 (PDPT order).
 *           0xFF800000 + n*4096 = page table n (n = va >> 21, 0..2047);
 *           0xFFFFC000 + i*4096 = page directory i, so the four directories
 *           read as one array of 2048 PDEs indexed by va >> 21.
 * Either way the PTE of va is entry (va >> 12) of the table window and its PDE
 * entry pt_index(va) of the directory window.
 */
#define PAGE_TABLES_BASE  (paging_pae ? 0xFF800000U : 0xFFC00000U)
#define PAGE_DIR_VIRT     (paging_pae ? 0xFFFFC000U : 0xFFFFF000U)
#define PAGE_TABLES_LOW   0xFF800000U   /* lowest address either mode uses */

/* Entry i of a table (page table or directory) at kernel address t. */
static inline pte_t tbl_get(const volatile void *t, uint32_t i) {
    if (!paging_pae) return ((const volatile uint32_t *)t)[i];
    const volatile uint32_t *w = (const volatile uint32_t *)t + 2 * i;
    uint32_t lo, hi;
    /* Not atomic; good enough for every reader here: entries this CPU did not
     * write change only under the page-table locks the writers hold (IF=0
     * critical sections), and frames are below 4 GiB, so the high half holds
     * nothing but NX. */
    do { hi = w[1]; lo = w[0]; } while (hi != w[1]);
    return ((pte_t)hi << 32) | lo;
}

/* Store entry i of a table.  A PAE entry is written with one cmpxchg8b so no
 * other CPU's page walk can see half of it. */
static inline void tbl_set(volatile void *t, uint32_t i, pte_t v) {
    if (!paging_pae) { ((volatile uint32_t *)t)[i] = (uint32_t)v; return; }
    if (!paging_nx) v &= ~PAGE_NX;
    volatile uint64_t *p = (volatile uint64_t *)t + i;
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    uint64_t old = *p;
    __asm__ volatile("1: lock cmpxchg8b %0\n\tjnz 1b"
                     : "+m"(*p), "+A"(old) : "b"(lo), "c"(hi) : "memory", "cc");
}

static inline volatile void *pt_window(uint32_t idx) {
    return (volatile void *)(uintptr_t)(PAGE_TABLES_BASE + idx * 4096U);
}

/* The PDE covering va (current address space). */
static inline pte_t pde_get(uint32_t va) {
    return tbl_get((const volatile void *)(uintptr_t)PAGE_DIR_VIRT, pt_index(va));
}
static inline void pde_set(uint32_t va, pte_t v) {
    tbl_set((volatile void *)(uintptr_t)PAGE_DIR_VIRT, pt_index(va), v);
}
/* Is there a page table for va? */
static inline int pde_present(uint32_t va) { return (pde_get(va) & PAGE_PRESENT) != 0; }

/* The PTE of va; its page table must exist (pde_present). */
static inline pte_t pte_get(uint32_t va) {
    return tbl_get((const volatile void *)(uintptr_t)PAGE_TABLES_BASE, va >> 12);
}
static inline void pte_set(uint32_t va, pte_t v) {
    tbl_set((volatile void *)(uintptr_t)PAGE_TABLES_BASE, va >> 12, v);
}
/* The PTE of va, or 0 when there is no page table. */
static inline pte_t pte_read(uint32_t va) { return pde_present(va) ? pte_get(va) : 0; }

/* NX for a user page: PAGE_NX unless `exec`. */
static inline pte_t page_nx_unless(int exec) { return exec ? 0 : PAGE_NX; }

void paging_init(void);
/* Before paging_init: keep EFER.NXE off even if the CPU has NX ("nonx"). */
void paging_nx_disable(void);
/* Map one page.  Returns 0, or -1 if the page table it needs could not be
 * allocated (physical memory exhausted).  On failure nothing was changed. */
int  paging_map(uint32_t virt, uint32_t phys, pte_t flags)
     __attribute__((warn_unused_result));

/* Ensure the page table covering `virt` exists, so a later paging_map() of any
 * address in that 4 MiB cannot fail.  0 on success, -1 on OOM. */
int  paging_reserve_table(uint32_t virt, int user);

/* The same for every table spanning [start, end).  0 on success, -1 on OOM. */
int  paging_reserve_range(uint32_t start, uint32_t end, int user)
     __attribute__((warn_unused_result));

void paging_unmap(uint32_t virt);

/* Rate-limited one-line "out of <what>" diagnostic, shared by every kernel
 * memory-exhaustion path so a burst does not flood the log. */
void kmem_oom_report(const char *what, unsigned detail);
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
/* Map a page in another page directory.  0 on success, -1 on OOM. */
int  pgdir_map(uint32_t pgdir_phys, uint32_t virt, uint32_t phys, pte_t flags)
     __attribute__((warn_unused_result));

/* Return the physical frame backing `virt` in pgdir_phys, or 0 if unmapped.
 * Caller must ensure IF=0 (switches CR3 transiently). */
uint32_t pgdir_virt_to_phys(uint32_t pgdir_phys, uint32_t virt);
/* Set user PDE `idx` (0..USER_PT_COUNT-1) of another address space to `pde`
 * (fork).  Uses TEMP_MAP_VIRT2: IF=0. */
void pgdir_install_pt(uint32_t pgdir_phys, uint32_t idx, pte_t pde);

/* The whole PTE of `virt` in pgdir_phys (0 if there is none).  IF=0. */
pte_t pgdir_virt_to_pte(uint32_t pgdir_phys, uint32_t virt);

/*
 * The signal-return page: one read-only, executable user page at SIGPAGE_VA in
 * every address space, holding the sigreturn trampoline that signal frames
 * return through (proc/signal.c).  The trampoline used to be written onto the
 * user stack, which a no-execute stack cannot run.  0 on success, -1 on OOM.
 */
#define SIGPAGE_VA        0xBF7FF000U   /* just below the 8 MiB stack window */
int paging_map_sigpage(uint32_t pgdir_phys) __attribute__((warn_unused_result));

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
