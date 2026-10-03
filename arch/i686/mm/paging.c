#include "paging.h"
#include "tlb.h"
#include "../../../mm/pmm.h"
#include "../../../kernel/printk.h"
#include "../../../proc/process.h"
#include "../../../proc/scheduler.h"
#include "../../../proc/signal.h"
#include "../../../proc/shm.h"
#include "../cpu/isr.h"
#include "../cpu/gdt.h"
#include "../cpu/pit.h"
#include "../cpu/tss.h"
#include "../cpu/dfault.h"
#include "../../../mm/kstack.h"
#include <kernel/config.h>
#include <stdint.h>
#include <kernel/kprof.h>

extern void panic(const char *msg, registers_t *regs) __attribute__((noreturn));
extern int vma_handle_fault(uint32_t addr);   /* demand-paged anonymous VMAs */
extern int vma_prot_lookup(uint32_t addr);    /* mmap prot of the VMA at addr, -1 if none */

/*
 * paging.c — x86 page table management, legacy two-level (4-byte entries) or
 * PAE three-level (8-byte entries, NX), chosen by boot.asm from CPUID.
 *
 * Recursive mapping (paging.h has the details):
 *   legacy: PDE[1023] = the page directory itself; tables at 0xFFC00000,
 *           the directory at 0xFFFFF000.
 *   PAE:    PD3[508..511] = page directories 0..3; tables at 0xFF800000,
 *           the four directories at 0xFFFFC000 (one 2048-entry array).
 *
 * An address space is named by what CR3 holds: the page directory (legacy) or
 * the PDPT (PAE) — "pgdir_phys" everywhere.  A PAE address space owns five
 * frames: the PDPT and its four directories, allocated together, so PDPT
 * entries never change after creation (the CPU caches them at each CR3 load).
 * Directory 3's entries 0..507 map the kernel and are copied from the kernel
 * address space, sharing its page tables, exactly like legacy PDEs 768..1022.
 */

/* Provided by boot.asm — the boot page directory (virtual address) */
extern uint32_t boot_page_directory[];
extern uint64_t boot_pae_pd[];           /* PAE: PD0..PD3, 512 entries each */

uint32_t paging_nx;

/* Kernel-half entries of directory 3 (PAE) / of the directory (legacy) that
 * every address space shares: below the recursive slot(s). */
#define PAE_KERNEL_PDES   508U           /* PD3[0..507]; 508..511 recursive */
#define PAE_REC_INDEX     (3U * 512U + PAE_KERNEL_PDES)   /* global PDE 2044 */

/* Linker symbols for kernel section boundaries */
extern char _kernel_phys_start[], _kernel_phys_end[];
extern char _text_start[], _text_end[];
extern char _rodata_start[], _rodata_end[];

/* Exception table (extable), filled by copy_from/to_user's inline asm.  Each
 * entry is two u32s: the faulting-instruction address and its fixup address. */
extern uint32_t __start___ex_table[], __stop___ex_table[];

/* If `eip` is a registered faulting instruction, redirect regs->eip to its fixup
 * (which returns -EFAULT) and return 1; else 0.  Linux-style recovery so a fault
 * during a user-memory access in kernel mode doesn't panic the kernel. */
static int fixup_exception(registers_t *regs) {
    for (uint32_t *e = __start___ex_table; e < __stop___ex_table; e += 2) {
        if (e[0] == regs->eip) { regs->eip = e[1]; return 1; }
    }
    return 0;
}

/* Physical address of the kernel page directory (exported for scheduler CR3 switching) */
uint32_t kernel_pgdir_phys;

static void page_fault_handler(registers_t *regs);
static void map_higher_half_physical_memory(void);

void *paging_temp_map(phys_t phys);
void  paging_temp_unmap(void);

/*
 * Allocate empty page tables for every PDE covering [start, end) in the current
 * kernel page directory.  Used so kernel regions that grow lazily (the heap)
 * have their PDEs in place before any process pgdir snapshots PDE 768-1022 —
 * the page tables are then shared and growth is globally visible.
 */
static void reserve_kernel_pagetables(uint32_t start, uint32_t end) {
    for (uint32_t va = start & ~(PT_SPAN - 1); va < end; va += PT_SPAN) {
        if (pde_present(va)) continue;
        uint32_t pt_phys = pmm_alloc_frame();
        /* FATAL by design (audit category (c)): called from paging_init before
         * the heap exists, to put the kernel's PDEs in place so every later
         * process pgdir snapshots them.  Nothing has allocated from the PMM
         * yet, so failing means the machine has less RAM than the kernel
         * image, and there is no caller to tell. */
        if (!pt_phys)
            panic("paging_init: no memory for a kernel page table", 0);
        pmm_frame_incref(pt_phys);          /* permanent — never freed */
        pde_set(va, pt_phys | PAGE_PRESENT | PAGE_WRITABLE);   /* kernel-only PDE */
        /* recursive window to the new PT */
        __builtin_memset((void *)pt_window(pt_index(va)), 0, PAGE_SIZE);
    }
}

/* Turn on EFER.NXE when the CPU has NX (CPUID 0x80000001:EDX bit 20).  Only
 * meaningful with PAE: legacy entries have no bit 63.  "nonx" on the kernel
 * command line keeps it off (paging_nx_disable). */
static int nx_refused;
void paging_nx_disable(void) { nx_refused = 1; }

static void enable_nx(void) {
    if (!paging_pae || nx_refused) return;
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000U));
    if (a < 0x80000001U) return;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001U));
    if (!(d & (1U << 20))) return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080U));
    lo |= 0x800U;                                       /* EFER.NXE */
    __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(0xC0000080U));
    paging_nx = 1;
}

/*
 * A new PAE address space: PDPT + four zeroed directories, directory 3's
 * kernel half copied from `kpd3` (a kernel address of 508 8-byte PDEs) and
 * its last four entries pointing back at the directories.  Returns the PDPT
 * frame, 0 on OOM.  Uses the temp maps, with interrupts off.
 */
static uint32_t pae_pgdir_fill(const volatile void *kpd3);
static uint32_t pae_pgdir_new(const volatile void *kpd3) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t pdpt = pae_pgdir_fill(kpd3);
    if (fl & 0x200) __asm__ volatile("sti");
    return pdpt;
}

static uint32_t pae_pgdir_fill(const volatile void *kpd3) {
    uint32_t pdpt = pmm_alloc_frame();
    if (!pdpt) return 0;
    uint32_t pd[4];
    for (int k = 0; k < 4; k++) {
        pd[k] = pmm_alloc_frame();
        if (!pd[k]) {
            while (--k >= 0) pmm_free_frame(pd[k]);
            pmm_free_frame(pdpt);
            return 0;
        }
    }
    for (int k = 0; k < 4; k++) {
        volatile void *t = paging_temp_map(pd[k]);
        __builtin_memset((void *)t, 0, PAGE_SIZE);
        if (k == 3) {
            for (uint32_t i = 0; i < PAE_KERNEL_PDES; i++)
                tbl_set(t, i, tbl_get(kpd3, i));
            for (uint32_t j = 0; j < 4; j++)
                tbl_set(t, PAE_KERNEL_PDES + j, pd[j] | PAGE_PRESENT | PAGE_WRITABLE);
        }
        paging_temp_unmap();
    }
    volatile void *t = paging_temp_map(pdpt);
    __builtin_memset((void *)t, 0, PAGE_SIZE);
    for (uint32_t k = 0; k < 4; k++)
        tbl_set(t, k, pd[k] | PAGE_PRESENT);   /* a PDPTE takes P/PWT/PCD only */
    paging_temp_unmap();
    return pdpt;
}

void paging_init(void) {
    enable_nx();

    uint32_t pd_phys;
    if (paging_pae) {
        pd_phys = pae_pgdir_new(&boot_pae_pd[3 * 512]);
        /* FATAL by design (audit category (c)): the kernel page directory,
         * the very first allocation the kernel makes. */
        if (!pd_phys)
            panic("paging_init: no memory for the kernel page directory", 0);
        goto loaded;
    }
    pd_phys = pmm_alloc_frame();
    /* FATAL by design (audit category (c)): the kernel page directory, the
     * very first allocation the kernel makes. */
    if (!pd_phys)
        panic("paging_init: no memory for the kernel page directory", 0);

    /*
     * The allocator may return a frame above the boot-time 4 MiB mapping.
     * Initialize it through the temporary mapping instead of assuming
     * pd_phys + KERNEL_VMA is currently mapped.
     */
    uint32_t *pd_virt = (uint32_t *)paging_temp_map(pd_phys);
    for (int i = 0; i < 1024; i++)
        pd_virt[i] = 0;

    /* Copy kernel higher-half mappings from boot_page_directory (PDE 768-1022) */
    for (int i = 768; i < 1023; i++)
        pd_virt[i] = boot_page_directory[i];

    /* Install recursive self-mapping at PDE[1023] */
    pd_virt[1023] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;

    paging_temp_unmap();

loaded:
    /* Switch to the new page directory */
    __asm__ volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");

    /* Save for scheduler CR3 switching */
    kernel_pgdir_phys = pd_phys;

    /* Register the page fault handler + the debug (#DB) watchpoint handler */
    isr_install_handler(14, page_fault_handler);

    map_higher_half_physical_memory();

    /*
     * Pre-create the page tables for the kernel heap (and any lazily-grown
     * kernel region) in THIS kernel page directory, before any process pgdir
     * is created.  pgdir_create() snapshots kernel PDEs 768-1022 from here, so
     * the page tables become shared across every address space — heap growth
     * then only fills PTEs in those shared tables and is visible to all
     * processes.  (The old all-RAM direct map happened to pre-create these as a
     * side effect; capping it to 256 MiB removed that, so we do it explicitly.)
     */
    reserve_kernel_pagetables(HEAP_START, HEAP_MAX);
    /* Same for the guarded kernel-stack window (mm/kstack.c). */
    reserve_kernel_pagetables(KSTACK_REGION_START, KSTACK_REGION_END);

    /* The double-fault task switches CR3; give it this directory, which maps
     * every kernel region (arch/i686/cpu/tss.c). */
    tss_set_df_cr3(pd_phys);

    printk("[VMM]  New PD at phys 0x%08x. Recursive mapping active.\n",
           (unsigned)pd_phys);
    printk("[VMM]  Paging: %s, NX %s.\n",
           paging_pae ? "PAE (3-level, 64-bit entries)" : "legacy 2-level (no PAE)",
           paging_nx ? "on" : "off");
}

/*
 * Exhaustion must stay visible in the log without a failing loop drowning
 * every other line: one line per second of PIT time, then a count of what was
 * suppressed when the next one gets through.  Shared by the paging, heap and
 * page-fault OOM paths so a single burst produces a single readable trace.
 */
void kmem_oom_report(const char *what, unsigned detail) {
    static uint32_t next_tick;
    static uint32_t suppressed;
    uint32_t now = pit_ticks();

    /* pit_ticks() is 0 until pit_init(); before that every call prints, which
     * is what early boot wants. */
    if (now && now < next_tick) { suppressed++; return; }
    next_tick = now + 100;                       /* 100 Hz → one line/second */
    if (suppressed) {
        printk("[OOM] out of %s (0x%08x); %u further failures suppressed\n",
               what, detail, (unsigned)suppressed);
        suppressed = 0;
    } else {
        printk("[OOM] out of %s (0x%08x)\n", what, detail);
    }
}

/*
 * Ensure a page table exists for the 4 MiB region containing `virt`.
 *
 * Returns 0 if the PDE is present on return, -1 if the table frame could not
 * be allocated.  Split out of paging_map so a caller that must not fail
 * halfway (mremap moving PTEs, shmat mapping a whole segment) can reserve
 * every table it will need BEFORE it starts mutating page tables, and fail
 * cleanly with -ENOMEM while the address space is still untouched.
 *
 * `user` selects whether the table is reachable from ring 3; a table shared by
 * user and kernel pages keeps PAGE_USER on the PDE (the PTE still gates the
 * individual page), which is what paging_map has always done.
 */
int paging_reserve_table(uint32_t virt, int user) {
    if (pde_present(virt)) return 0;

    /*
     * Allocating a page table for a not-present PDE must be atomic w.r.t.
     * preemption.  Threads of one process share the page directory, and a
     * large mmap (e.g. an 8 MiB thread stack) memsets pages preemptibly —
     * so two threads whose distinct VAs land in the same 4 MiB PDE could
     * both see "not present" and each allocate a page table, the second
     * overwriting the first's PDE and orphaning its PTEs (lost mappings →
     * zeroed TLS, non-deterministic crashes).  Guard with saved-IF cli.
     */
    uint32_t eflags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags) :: "memory");
    int rc = 0;
    if (!pde_present(virt)) {            /* re-check inside the critical section */
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            rc = -1;
        } else {
            pde_set(virt, pt_phys | PAGE_PRESENT | PAGE_WRITABLE |
                          (user ? PAGE_USER : 0U));
            /* Zero the new page table via the recursive window */
            __builtin_memset((void *)pt_window(pt_index(virt)), 0, PAGE_SIZE);
        }
    }
    if (eflags & 0x200) __asm__ volatile("sti");
    if (rc != 0)
        kmem_oom_report("page tables", virt);
    return rc;
}

/*
 * Reserve every page table [start, end) will need.  0 on success, -1 on OOM
 * (some tables may have been allocated; they are empty and simply stay in the
 * directory, which costs a frame, not correctness).
 *
 * For a caller whose mapping loop must not fail halfway.
 */
int paging_reserve_range(uint32_t start, uint32_t end, int user) {
    for (uint32_t v = start & ~(PT_SPAN - 1); v < end; v += PT_SPAN) {
        if (paging_reserve_table(v, user) != 0) return -1;
        if (v + PT_SPAN < v) break;                /* 4 GiB wrap */
    }
    return 0;
}

/*
 * Map one page.  Returns 0, or -1 if the page table it needs cannot be
 * allocated — it used to print and hlt forever, which made every user-driven
 * mapping (mmap, brk, stack growth, COW) a machine halt.  On failure NOTHING
 * has been changed, so a caller can propagate -ENOMEM without unwinding.
 */
int paging_map(uint32_t virt, phys_t phys, pte_t flags) {
    virt &= ~0xFFFU;
    phys &= ~(phys_t)0xFFFU;

    if (paging_reserve_table(virt, (flags & PAGE_USER) != 0) != 0)
        return -1;

    pte_set(virt, phys | flags);
    tlb_flush_single(virt);
    return 0;
}

static void map_higher_half_physical_memory(void) {
    /* 64-bit: 4 GiB of frames * PAGE_SIZE would wrap a uint32_t to 0. */
    uint64_t total_phys = (uint64_t)pmm_total_frames() * PAGE_SIZE;

    /*
     * boot.asm already maps the first 4 MiB at KERNEL_VMA.  Extend that direct
     * "phys + KERNEL_VMA" map — but ONLY up to the kernel heap base.  The
     * kernel virtual layout above 0xC0000000 is shared: HEAP_START (0xD0000000)
     * and the recursive page tables (0xFFC00000) live there, so direct-mapping
     * all of RAM would (a) collide with the heap past 256 MiB and (b) for >1 GiB
     * wrap past 4 GiB and corrupt the recursive mapping — which is exactly why
     * booting with >512 MiB used to hang.
     *
     * Physical frames ABOVE this window ("high memory") are still fully usable:
     * the PMM allocates them, paging_map() maps them into any page directory
     * (user pages, the heap), and the kernel touches them via paging_temp_map().
     * Nothing relies on phys+KERNEL_VMA for high frames (only the low PMM bitmap
     * and the GRUB-loaded initrd, both well under this limit).
     *
     * PTEs 1 and 2 in the first page table remain reserved for the temporary
     * mapping helpers.
     */
    uint32_t direct_max = HEAP_START - KERNEL_VMA;   /* 256 MiB window */
    uint32_t map_limit  = total_phys < direct_max ? (uint32_t)total_phys : direct_max;

    /* FATAL by design (audit category (c)): the direct map is built once from
     * paging_init, before the heap and before any process; the kernel cannot
     * address its own RAM without it.  The page tables it needs come out of
     * memory nothing has allocated from yet, so failing here means the machine
     * has less RAM than the kernel image. */
    if (paging_map(KERNEL_VMA + 0x3FF000U, 0x3FF000U,
                   PAGE_PRESENT | PAGE_WRITABLE) != 0)
        panic("paging_init: cannot map the VGA/BIOS page", 0);

    /* boot.asm maps 0-8 MiB; extend from there up to the window limit. */
    for (uint32_t phys = 0x800000U; phys < map_limit; phys += PAGE_SIZE) {
        if (paging_map(KERNEL_VMA + phys, phys,
                       PAGE_PRESENT | PAGE_WRITABLE) != 0)
            panic("paging_init: cannot build the higher-half direct map", 0);
    }

    printk("[VMM]  RAM: %u MiB total; higher-half direct map covers %u MiB "
           "(rest is high memory via temp maps).\n",
           (unsigned)(pmm_ram_frames() / 256),
           (unsigned)(map_limit / (1024U * 1024U)));
}

void paging_unmap(uint32_t virt) {
    virt &= ~0xFFFU;
    if (pde_present(virt)) {
        pte_set(virt, 0);
        tlb_flush_single(virt);
    }
}

/* Kernel addresses only: every kernel page is below 4 GiB. */
uint32_t paging_get_physical(uint32_t virt) {
    pte_t pte = pte_read(virt);
    if (!(pte & PAGE_PRESENT)) return 0;
    return (uint32_t)pte_frame(pte) | (virt & 0xFFFU);
}

void paging_set_kernel_permissions(void) {
    uint32_t addr = (uint32_t)(uintptr_t)_text_start;
    while (addr < (uint32_t)(uintptr_t)_text_end) {
        pte_t pte = pte_get(addr);
        if (pte & PAGE_PRESENT)
            pte_set(addr, pte & ~(pte_t)PAGE_WRITABLE);
        tlb_flush_single(addr);
        addr += 4096;
    }
    addr = (uint32_t)(uintptr_t)_rodata_start;
    while (addr < (uint32_t)(uintptr_t)_rodata_end) {
        pte_t pte = pte_get(addr);
        if (pte & PAGE_PRESENT)
            pte_set(addr, pte & ~(pte_t)PAGE_WRITABLE);
        tlb_flush_single(addr);
        addr += 4096;
    }
    /* NX on the whole direct map except kernel .text: .rodata, .data, .bss,
     * the boot code that is done with, and every other RAM page reached
     * through phys + KERNEL_VMA.  The page tables are shared by every address
     * space, so this is done once. */
    uint32_t nx_pages = 0;
    if (paging_nx) {
        uint32_t ts = (uint32_t)(uintptr_t)_text_start & ~0xFFFU;
        uint32_t te = ((uint32_t)(uintptr_t)_text_end + 0xFFFU) & ~0xFFFU;
        for (addr = KERNEL_VMA; addr < HEAP_START; addr = pt_next(addr)) {
            if (!pde_present(addr)) continue;
            for (uint32_t va = addr; va < pt_next(addr); va += PAGE_SIZE) {
                if (va >= ts && va < te) continue;
                pte_t pte = pte_get(va);
                if (!(pte & PAGE_PRESENT) || (pte & PAGE_NX)) continue;
                pte_set(va, pte | PAGE_NX);
                nx_pages++;
            }
        }
        tlb_flush_all();
    }
    printk("[VMM]  Kernel .text/.rodata marked read-only (W^X)%s.\n",
           paging_nx ? "; direct map outside .text no-execute" : "");
    (void)nx_pages;
}

/* ─── Temporary page mapping ─────────────────────────────────────────────── */
/*
 * TEMP_MAP_VIRT and TEMP_MAP_VIRT2 use PTEs 1 and 2 of the shared
 * boot_page_table1 (PDE[768] in every pgdir points to the same physical
 * frame for boot_page_table1).  Modifying these PTEs is globally visible
 * across all page directories.  Must only be used with IF=0.
 */

void *paging_temp_map(phys_t phys) {
    pte_set(TEMP_MAP_VIRT, (phys & ~(phys_t)0xFFFU) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
    tlb_flush_single(TEMP_MAP_VIRT);
    return (void *)TEMP_MAP_VIRT;
}

void paging_temp_unmap(void) {
    pte_set(TEMP_MAP_VIRT, 0);
    tlb_flush_single(TEMP_MAP_VIRT);
}

void *paging_temp_map2(phys_t phys) {
    pte_set(TEMP_MAP_VIRT2, (phys & ~(phys_t)0xFFFU) | PAGE_PRESENT | PAGE_WRITABLE | PAGE_NX);
    tlb_flush_single(TEMP_MAP_VIRT2);
    return (void *)TEMP_MAP_VIRT2;
}

void paging_temp_unmap2(void) {
    pte_set(TEMP_MAP_VIRT2, 0);
    tlb_flush_single(TEMP_MAP_VIRT2);
}

/* ─── Page directory management ──────────────────────────────────────────── */

uint32_t pgdir_create(void) {
    /* Directory 3 of the current address space, whose kernel half is the one
     * every address space shares. */
    if (paging_pae)
        return pae_pgdir_new((const volatile void *)(uintptr_t)(PAGE_DIR_VIRT +
                                                                3U * 4096U));

    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) return 0;

    /* Temp-map the new PD to initialize it */
    uint32_t *pd = (uint32_t *)paging_temp_map(pd_phys);

    /* Zero all entries */
    for (int i = 0; i < 1024; i++)
        pd[i] = 0;

    /* Copy kernel higher-half mappings from the current kernel pgdir */
    uint32_t *cur_pd = (uint32_t *)PAGE_DIR_VIRT;
    for (int i = 768; i < 1023; i++)
        pd[i] = cur_pd[i];

    /* Set recursive self-mapping at PDE[1023] */
    pd[1023] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;

    paging_temp_unmap();
    return pd_phys;
}

/*
 * pgdir_map — map a page in an arbitrary page directory.
 *
 * Temporarily switches CR3 to pgdir_phys so that the recursive mapping
 * works relative to the target pgdir.  IF must be 0 at call time.
 */
int pgdir_map(uint32_t pgdir_phys, uint32_t virt, phys_t phys, pte_t flags) {
    uint32_t prev_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");
    int rc = paging_map(virt, phys, flags);
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");
    return rc;
}

void pgdir_install_pt(uint32_t pgdir_phys, uint32_t idx, pte_t pde) {
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    uint32_t dir = pgdir_phys;
    if (paging_pae) {                     /* the PDPT names directory idx/512 */
        dir = (uint32_t)pte_frame(tbl_get(paging_temp_map2(pgdir_phys), idx / 512U));
        paging_temp_unmap2();
        idx %= 512U;
    }
    tbl_set(paging_temp_map2(dir), idx, pde);
    paging_temp_unmap2();
    if (fl & 0x200) __asm__ volatile("sti");
}

/* Return the whole PTE of `virt` in pgdir_phys, 0 if there is none. */
pte_t pgdir_virt_to_pte(uint32_t pgdir_phys, uint32_t virt) {
    uint32_t prev_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");
    pte_t pte = pte_read(virt);
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");
    return pte;
}

/* Return the physical frame backing `virt` in pgdir_phys, or 0 if unmapped. */
phys_t pgdir_virt_to_phys(uint32_t pgdir_phys, uint32_t virt) {
    pte_t pte = pgdir_virt_to_pte(pgdir_phys, virt);
    return (pte & PAGE_PRESENT) ? pte_frame(pte) : 0;
}

/* ─── Signal-return page ─────────────────────────────────────────────────── */
/*
 * The sigreturn trampoline, at SIGPAGE_VA in every address space.  A handler
 * returns here with esp just above the frame's argument block; the frame
 * builder (proc/signal.c) put the restore-frame address and its type marker
 * right after the four argument slots, at [esp+12] and [esp+16]:
 *     mov  eax, 0x77          ; sigreturn
 *     mov  ecx, [esp+12]      ; restore-frame address
 *     mov  edx, [esp+16]      ; marker
 *     int  0x80
 * One frame, shared read-only by everyone; its permanent reference keeps it.
 */
static uint32_t sigpage_phys;

int paging_map_sigpage(uint32_t pgdir_phys) {
    static const uint8_t code[] = {
        0xB8, 0x77, 0x00, 0x00, 0x00,
        0x8B, 0x4C, 0x24, 0x0C,
        0x8B, 0x54, 0x24, 0x10,
        0xCD, 0x80,
        0x0F, 0x0B,                     /* ud2: sigreturn does not return */
    };
    if (!sigpage_phys) {
        uint32_t f = pmm_alloc_frame();
        if (!f) return -1;
        pmm_frame_incref(f);                   /* the permanent reference */
        uint32_t fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
        uint8_t *k = (uint8_t *)paging_temp_map(f);
        __builtin_memset(k, 0xCC, PAGE_SIZE);
        __builtin_memcpy(k, code, sizeof(code));
        paging_temp_unmap();
        if (fl & 0x200) __asm__ volatile("sti");
        sigpage_phys = f;
    }
    /* Read-only and executable.  COW like any private page, so a process
     * that mprotect()s it writable gets its own copy on the first store and
     * never writes the one every process runs. */
    pmm_frame_incref(sigpage_phys);
    uint32_t fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    int rc = pgdir_map(pgdir_phys, SIGPAGE_VA, sigpage_phys,
                       PAGE_PRESENT | PAGE_USER | PAGE_COW | PAGE_WRPROT);
    if (fl & 0x200) __asm__ volatile("sti");
    if (rc != 0) pmm_frame_decref(sigpage_phys);
    return rc;
}

/*
 * pgdir_free_user — free all user-space pages (PDE 0-767) in a page directory,
 * then free the page directory frame itself.
 *
 * Temporarily switches CR3 to pgdir_phys so the recursive mapping addresses
 * the target page directory.  IF must be 0 at call time.
 */
void pgdir_free_user(uint32_t pgdir_phys) {
    /* The address space dies here, whoever frees it (reap, exec, a failed
     * fork): its shm attachments and reservations go with it, before the
     * frame can be handed to a new page directory. */
    shm_mm_release(pgdir_phys);

    uint32_t prev_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");

    /* now addresses pgdir_phys via recursive mapping */
    volatile void *pd = (volatile void *)(uintptr_t)PAGE_DIR_VIRT;

    for (uint32_t i = 0; i < USER_PT_COUNT; i++) {
        pte_t pde = tbl_get(pd, i);
        if (!(pde & PAGE_PRESENT)) continue;
        uint32_t pt_phys = (uint32_t)pte_frame(pde);   /* tables are low */

        /* Access page table entries via recursive mapping */
        volatile void *pt = pt_window(i);
        for (uint32_t j = 0; j < PT_ENTRIES; j++) {
            pte_t e = tbl_get(pt, j);
            /* PAGE_PROTNONE entries are not present but still own a frame. */
            if (!(e & (PAGE_PRESENT | PAGE_PROTNONE))) continue;
            pmm_frame_decref(pte_frame(e));
        }

        tbl_set(pd, i, 0);
        pmm_free_frame(pt_phys);  /* PT frame itself is not ref-counted */
    }

    /* PAE: the four directories, read from the recursive slots before the
     * address space is left. */
    uint32_t dirs[4] = { 0, 0, 0, 0 };
    if (paging_pae)
        for (uint32_t k = 0; k < 4; k++)
            dirs[k] = (uint32_t)pte_frame(tbl_get(pd, PAE_REC_INDEX + k));

    /* Restore previous pgdir (also flushes TLB) */
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");

    /* Free the pgdir frame (not ref-counted) */
    pmm_free_frame(pgdir_phys);
    for (uint32_t k = 0; k < 4; k++)
        if (dirs[k]) pmm_free_frame(dirs[k]);
}

/* ─── Page fault handler ─────────────────────────────────────────────────── */



static void page_fault_handler(registers_t *regs) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    uint32_t err = regs->err_code;

    /* A kernel-mode touch of a kernel stack's guard is a stack overflow.  Say
     * so, and stop, before anything here uses more of the little stack that
     * is left.  (A push into the guard never gets this far: the CPU cannot
     * deliver the #PF on that stack either, and it becomes a double fault.) */
    if (!(err & 0x4U) && kstack_guard_stack(cr2))
        kstack_overflow_panic("page fault in kernel stack guard", regs, cr2);

    /*
     * COW fault: protection violation (bit 0) + write (bit 1) + PAGE_COW set.
     * Allocate a new frame, copy the old one, update PTE.
     */
    if ((err & 0x3U) == 0x3U && pde_present(cr2)) {
        pte_t pte = pte_get(cr2);
        int vprot = vma_prot_lookup(cr2);
        if ((pte & PAGE_PRESENT) && (pte & PAGE_COW) &&
            !(pte & PAGE_WRPROT) &&
            (vprot < 0 || (vprot & 0x2))) {
            /* A COW page in a mapping WITHOUT PROT_WRITE (mprotect(PROT_READ)
             * after fork) is a real protection fault, not a COW break: the
             * vprot test above lets it fall through to SIGSEGV (Linux
             * do_wp_page is only reached when the VMA allows writing).
             * PAGE_WRPROT is the same test for a page with no VMA at all (the
             * ELF image, the brk heap, the main stack), where vprot is < 0 and
             * would otherwise be read as "writes allowed".  It covers the
             * copy path and the wp_page_reuse one below equally: a last
             * reference is no reason to re-grant a write the process asked us
             * to refuse. */
            phys_t old_phys = pte_frame(pte);

            /* Last reference (the sharer exited, unmapped or DONTNEED'ed its
             * side): no copy needed, just make the page writable again (Linux
             * wp_page_reuse). */
            if (pmm_frame_refcount(old_phys) == 1) {
                kprof_count(KPE_PF_COW);
                /* Grants write while keeping the entry's other bits, which is
                 * only correct because the test above has already excluded
                 * PAGE_WRPROT pages; do not relax that guard without changing
                 * this line too. */
                pte_set(cr2, (pte & ~(pte_t)PAGE_COW) | PAGE_WRITABLE);
                tlb_flush_single(cr2 & ~0xFFFU);
                tlb_shootdown();
                return;
            }

            kprof_count(KPE_PF_COW);
            phys_t new_phys = pmm_alloc_user_frame();
            if (!new_phys) {
                /* Linux: do_wp_page returns VM_FAULT_OOM and
                 * pagefault_out_of_memory() picks a victim.  We have no OOM
                 * killer to choose with, so the victim is the process that
                 * asked for the page.  Killing one greedy process keeps the
                 * machine alive, which panicking here did not. */
                kmem_oom_report("memory to break a COW page", (unsigned)cr2);
                if ((err & 0x4U) && current_proc)
                    proc_group_exit(SIGKILL);          /* does not return */
                /* Kernel-mode fault (copy_to_user into a COW page): hand the
                 * syscall its -EFAULT fixup rather than dying. */
                if (fixup_exception(regs)) return;
                panic("COW fault: out of physical memory in kernel mode", regs);
            }

            /* Copy old frame → new frame via temp mappings.  The #PF handler is
             * a trap gate (IF preserved), so it runs with interrupts ENABLED.
             * paging_temp_map/temp_map2 modify globally-shared PTEs that MUST be
             * used with IF=0 — an interrupt (or a syscall path on return) that
             * also touches the temp mappings would clobber them mid-copy and the
             * COW page would get garbage (heap-metadata corruption).  Guard it. */
            uint32_t cow_if;
            __asm__ volatile("pushf; pop %0; cli" : "=r"(cow_if) :: "memory");
            const uint32_t *src = (const uint32_t *)paging_temp_map(old_phys);
            uint32_t       *dst = (uint32_t *)paging_temp_map2(new_phys);
            for (int k = 0; k < (int)(PAGE_SIZE / 4); k++)
                dst[k] = src[k];
            paging_temp_unmap();
            paging_temp_unmap2();
            if (cow_if & 0x200) __asm__ volatile("sti");

            /* New frame has refcount=1 (not COW); set it with incref */
            pmm_frame_incref(new_phys);

            /* Update PTE: writable, no COW, executable or not as before */
            pte_set(cr2, new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                         (pte & PAGE_NX));
            tlb_flush_single(cr2 & ~0xFFFU);

            /* SMP: a sibling thread (shared pgdir) may have the old read-only
             * COW entry cached and would keep reading the pre-break frame —
             * force it to flush before we proceed. */
            tlb_shootdown();

            /* Release our share of the old frame */
            pmm_frame_decref(old_phys);
            return;
        }
        /* STALE-TLB write-protection fault (SMP).  The PTE is present and ALREADY
         * WRITABLE (not COW), yet a write faulted "protection".  That means a
         * sibling thread on another CPU broke COW for this shared-pgdir page
         * (changed the PTE read-only→writable) but this CPU's TLB still caches
         * the old read-only entry.  The page is genuinely writable now — just
         * flush this CPU's stale entry and retry.  Without this the write would
         * fall through to SIGSEGV (the spurious crash that surfaced once the
         * fill-before-map fix let true thread parallelism run further). */
        /* Only for an access that the entry actually permits: a USER write to
         * a supervisor page (kernel .data, the recursive page-table window)
         * is present+writable too, but it is refused by the U/S bit, not by a
         * stale entry — retrying it would fault forever.  Let it fall through
         * to SIGSEGV (SEGV_ACCERR). */
        int user_ok = !(err & 0x4U) ||
                      ((pte & PAGE_USER) && (pde_get(cr2) & PAGE_USER));
        if (user_ok && (pte & PAGE_PRESENT) && (pte & PAGE_WRITABLE) &&
            !(pte & PAGE_COW)) {
            tlb_flush_single(cr2 & ~0xFFFU);
            return;
        }
    }

    /* The same for an instruction fetch (error bit 4, NX only): a sibling
     * thread's mprotect(PROT_EXEC) — a JIT flipping a code page from RW to
     * RX — cleared NX on a page this CPU still caches as no-execute. */
    if ((err & 0x11U) == 0x11U && pde_present(cr2)) {
        pte_t pte = pte_get(cr2);
        int user_ok = !(err & 0x4U) ||
                      ((pte & PAGE_USER) && (pde_get(cr2) & PAGE_USER));
        if (user_ok && (pte & PAGE_PRESENT) && !(pte & PAGE_NX)) {
            tlb_flush_single(cr2 & ~0xFFFU);
            return;
        }
    }

    /* A not-present entry carrying PAGE_PROTNONE is a page the process made
     * inaccessible with mprotect(PROT_NONE) (or mapped PROT_NONE): it owns a
     * frame and must NOT be demand-populated or treated as stack growth — the
     * access is a genuine SIGSEGV (Linux: pte_protnone → access_error). */
    int protnone = !(err & 0x1U) && cr2 < 0xC0000000U &&
                   (pte_read(cr2) & PAGE_PROTNONE);

    /* Demand-paged VMA: a not-present fault may be the first touch of a
     * lazily-allocated mmap region (e.g. a thread stack).  This fires for BOTH
     * user-mode faults AND kernel-mode faults — the kernel writes to user mmap
     * buffers during syscalls (copy_to_user, a read() into a fresh buffer), and
     * those faults occur at CPL=0 on a user address.  Populate + retry. */
    if (!(err & 0x1U) && !protnone && cr2 < 0xC0000000U && current_proc &&
        vma_handle_fault(cr2)) {
        return;
    }

    /* Automatic main-thread stack growth.  We advertise an 8 MiB stack
     * (RLIMIT_STACK + /proc/self/maps) but only eagerly map the top 256 KiB;
     * glibc/SpiderMonkey legitimately recurse deeper.  A not-present fault just
     * below the current stack, within the 8 MiB window below USER_STACK_TOP, is
     * a real stack extension — map a fresh zeroed page and retry.  (The guard is
     * narrow: only the stack region, never the mmap/heap area lower down.) */
    {
        uint32_t stack_grow_floor = (uint32_t)USER_STACK_TOP - (8U * 1024 * 1024);
        if (!(err & 0x1U) && !protnone && current_proc &&
            cr2 >= stack_grow_floor && cr2 < (uint32_t)USER_STACK_BASE) {
            uint32_t page = cr2 & ~0xFFFU;
            phys_t phys = pmm_alloc_user_frame();
            if (phys) {
                kprof_count(KPE_PF_STACK);
                pmm_frame_incref(phys);
                if (paging_map(page, phys,
                               PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER |
                               page_nx_unless(proc_stack_exec(current_proc))) != 0) {
                    /* No page table for the new stack page.  Give the frame
                     * back and fall through: the process gets SIGSEGV, which
                     * is what a stack that cannot grow means. */
                    pmm_frame_decref(phys);
                } else {
                    __builtin_memset((void *)page, 0, PAGE_SIZE);
                    tlb_flush_single(page);
                    return;
                }
            }
        }
    }

    /* Exception-table recovery: a fault during a kernel-mode user-memory access
     * (copy_from/to_user) that COW/demand-paging/stack-growth couldn't resolve —
     * e.g. the mapping was torn down by a sibling CPU when the process is killed
     * mid-syscall under -smp 2.  Redirect to the copy's fixup (returns -EFAULT)
     * instead of panicking.  Only for kernel-mode faults (err bit2 == 0). */
    if (!(err & 0x4U) && fixup_exception(regs))
        return;

    /* Not a handled fault */
    const char *present = (err & 0x1U) ? "protection" : "not-present";
    const char *access  = (err & 0x10U) ? "exec" : (err & 0x2U) ? "write" : "read";
    const char *ring    = (err & 0x4U) ? "user"        : "kernel";

    pte_t pte_val = pte_read(cr2);
    printk("[PAGE FAULT] pid=%d addr=0x%08x  eip=0x%08x  (%s %s in %s mode) pte=%08x%s\n",
           current_proc ? current_proc->pid : -1,
           (unsigned)cr2, (unsigned)regs->eip, access, present, ring,
           (unsigned)pte_val, (pte_val & PAGE_NX) ? " NX" : "");
    printk("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
           (unsigned)regs->eax, (unsigned)regs->ebx, (unsigned)regs->ecx,
           (unsigned)regs->edx, (unsigned)regs->esi, (unsigned)regs->edi,
           (unsigned)regs->ebp, (unsigned)regs->useresp);
    if (current_proc && cr2 < 0xC0000000U) {
        /* Is the faulting address inside a known demand-paged VMA?  If yes, the
         * region was mmap'd but our handler failed to back it (kernel bug); if
         * no, it's a wild/corrupted pointer (userspace).  Also show the mmap
         * cursor + image/heap spans to locate cr2 in the address space. */
        uint32_t s, e, pr; int in_vma = -1;
        for (int i = 0; proc_vma_iter(current_proc, i, &s, &e, &pr) == 0; i++)
            if (cr2 >= s && cr2 < e) { in_vma = i; break; }
        if (in_vma >= 0)
            printk("  cr2 in VMA #%d [%08x,%08x) prot=%x  (mmap'd but UNBACKED!)\n",
                   in_vma, (unsigned)s, (unsigned)e, (unsigned)pr);
        else
            printk("  cr2 NOT in any VMA (wild pointer)  mmap_next=%08x heap=%08x..%08x img=%08x..%08x\n",
                   (unsigned)current_proc->mmap_next, (unsigned)current_proc->brk_base,
                   (unsigned)current_proc->heap_end, (unsigned)current_proc->image_start,
                   (unsigned)current_proc->image_end);
    }


    /* User-mode fault: send SIGSEGV and let the process die gracefully.
     *
     * Fault-loop breaker: a process may install a SIGSEGV handler that returns
     * straight back to the faulting instruction (e.g. Firefox's crash handler
     * that expects the re-fault to hit SIG_DFL).  If the same EIP keeps faulting
     * on the same address with no forward progress (sys_mprotect counts as
     * progress and clears the record), the handler is making things worse — force the
     * default action (terminate) instead of re-invoking it endlessly. */
    if ((err & 0x4U) && current_proc) {
        if (regs->eip == current_proc->last_fault_eip &&
            cr2 == current_proc->last_fault_addr) {
            if (++current_proc->fault_repeat >= 3) {
                /* On a call-to-NULL, [esp] is the caller's return address.
                 * Read it only if the page is actually mapped — a crashed
                 * thread may have a wild esp, and faulting here is in ring 0. */
                uint32_t sp = regs->useresp, retaddr = 0;
                if (sp >= 0x08000000U && sp < 0xC0000000U &&
                    (pte_read(sp) & PAGE_PRESENT)) {
                    retaddr = *(uint32_t *)sp;
                }
                printk("[SIG] pid=%d SIGSEGV loop eip=%08x addr=%08x ret=%08x — killed\n",
                       current_proc->pid, (unsigned)regs->eip, (unsigned)cr2,
                       (unsigned)retaddr);
                proc_group_exit(SIGSEGV);   /* does not return */
            }
        } else {
            current_proc->last_fault_eip  = regs->eip;
            current_proc->last_fault_addr = cr2;
            current_proc->fault_repeat    = 0;
        }
        /* Synchronous fault: thread-directed SIGSEGV.  With SIG_DFL the whole
         * thread group exits (Linux force_sig_fault -> get_signal ->
         * do_group_exit), not just the faulting thread.  The faulting address
         * and its cause travel with the signal: a present page means the access
         * was refused, an absent one that nothing is mapped there (arch/x86/mm/
         * fault.c: SEGV_ACCERR vs SEGV_MAPERR). */
        signal_send_fault(current_proc, SIGSEGV,
                          (err & 0x1U) ? SEGV_ACCERR : SEGV_MAPERR, cr2);
        signal_deliver_pending(regs);  /* → handler, or group exit if SIG_DFL */
        return;
    }

    panic("Unhandled page fault", regs);
}

/* ── Shared page directories (threads) ──────────────────────────────────── */

/* Only a pgdir with two or more users has an entry, and every user is a live
 * process, so MAX_PROCS / 2 entries can never all be taken.  Sized from
 * MAX_PROCS so raising the process limit cannot outgrow it; a full table would
 * leave a shared pgdir untracked, and the first of its users to exit would free
 * it under the others. */
#define PGDIR_SHARES (MAX_PROCS / 2 + 1)
static struct { uint32_t phys; int count; } pgdir_shares[PGDIR_SHARES];

void pgdir_retain(uint32_t pgdir_phys) {
    int free_slot = -1;

    for (int i = 0; i < PGDIR_SHARES; i++) {
        if (pgdir_shares[i].phys == pgdir_phys && pgdir_shares[i].count > 0) {
            pgdir_shares[i].count++;
            return;
        }
        if (free_slot < 0 && pgdir_shares[i].count == 0)
            free_slot = i;
    }
    /* Cannot happen (see PGDIR_SHARES); refusing loudly beats a later
     * use-after-free of a page directory that is still loaded somewhere. */
    if (free_slot < 0)
        panic("pgdir_retain: shared page directory table full", 0);
    /* First share: the original owner + the new user. */
    pgdir_shares[free_slot].phys = pgdir_phys;
    pgdir_shares[free_slot].count = 2;
}

int pgdir_release(uint32_t pgdir_phys) {
    for (int i = 0; i < PGDIR_SHARES; i++) {
        if (pgdir_shares[i].phys == pgdir_phys && pgdir_shares[i].count > 0) {
            if (--pgdir_shares[i].count <= 1) {
                /* Last co-owner left: drop the entry; the final release
                 * (no entry) lets the caller free. */
                pgdir_shares[i].count = 0;
                pgdir_shares[i].phys = 0;
            }
            return 1;   /* still referenced by someone */
        }
    }
    return 0;           /* unshared: caller frees */
}

/* Resident user pages of the address space pgdir_phys (present PTEs below
 * the kernel), and how many of them are shared (PAGE_SHARED) - for
 * /proc/<pid>/statm and status.  Walks through the recursive mapping with
 * that directory loaded, interrupts off, like pgdir_virt_to_pte. */
void pgdir_count_resident(uint32_t pgdir_phys, uint32_t *resident, uint32_t *shared) {
    uint32_t res = 0, sh = 0, fl, prev_cr3;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    if (prev_cr3 != pgdir_phys)
        __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");
    for (uint32_t t = 0; t < USER_PT_COUNT; t++) {
        uint32_t va = t << PT_SHIFT;
        if (!pde_present(va)) continue;
        volatile void *pt = pt_window(t);
        for (uint32_t i = 0; i < PT_ENTRIES; i++) {
            pte_t e = tbl_get(pt, i);
            if (!(e & PAGE_PRESENT) || !(e & PAGE_USER)) continue;
            res++;
            if (e & PAGE_SHARED) sh++;
        }
    }
    if (prev_cr3 != pgdir_phys)
        __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");
    if (fl & 0x200) __asm__ volatile("sti");
    *resident = res;
    *shared = sh;
}
