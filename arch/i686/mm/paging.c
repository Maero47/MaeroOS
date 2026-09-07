#include "paging.h"
#include "tlb.h"
#include "../../../mm/pmm.h"
#include "../../../kernel/printk.h"
#include "../../../proc/process.h"
#include "../../../proc/scheduler.h"
#include "../../../proc/signal.h"
#include "../cpu/isr.h"
#include "../cpu/gdt.h"
#include <kernel/config.h>
#include <stdint.h>

extern void panic(const char *msg, registers_t *regs) __attribute__((noreturn));
extern int vma_handle_fault(uint32_t addr);   /* demand-paged anonymous VMAs */

/*
 * paging.c — x86 two-level page table management.
 *
 * Recursive mapping (PDE[1023] = page directory itself):
 *   0xFFC00000 + i*4096  → page table i (all 1024 page tables accessible)
 *   0xFFFFF000           → the page directory itself
 */

/* Provided by boot.asm — the boot page directory (virtual address) */
extern uint32_t boot_page_directory[];

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

#define KPGDIR ((uint32_t *)PAGE_DIR_VIRT)

static void page_fault_handler(registers_t *regs);
static void debug_watch_handler(registers_t *regs);
void watchpoint_arm(uint32_t addr, int pid, uint32_t target_value);
static void map_higher_half_physical_memory(void);

void *paging_temp_map(uint32_t phys);
void  paging_temp_unmap(void);

/*
 * Allocate empty page tables for every PDE covering [start, end) in the current
 * kernel page directory.  Used so kernel regions that grow lazily (the heap)
 * have their PDEs in place before any process pgdir snapshots PDE 768-1022 —
 * the page tables are then shared and growth is globally visible.
 */
static void reserve_kernel_pagetables(uint32_t start, uint32_t end) {
    for (uint32_t va = start & ~0x3FFFFFU; va < end; va += 0x400000U) {
        uint32_t *pde = paging_get_pde(va);
        if (*pde & PAGE_PRESENT) continue;
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            printk("[VMM]  FATAL: OOM reserving kernel page table for 0x%08x\n",
                   (unsigned)va);
            for (;;) __asm__ volatile("hlt");
        }
        pmm_frame_incref(pt_phys);          /* permanent — never freed */
        *pde = pt_phys | PAGE_PRESENT | PAGE_WRITABLE;   /* kernel-only PDE */
        uint32_t *pt = paging_get_pte(va);  /* recursive window to the new PT */
        for (int i = 0; i < 1024; i++) pt[i] = 0;
    }
}

void paging_init(void) {
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) {
        printk("[PAGING] FATAL: could not allocate page directory frame\n");
        for (;;) __asm__ volatile("hlt");
    }

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

    /* Switch to the new page directory */
    __asm__ volatile("mov %0, %%cr3" :: "r"(pd_phys) : "memory");

    /* Save for scheduler CR3 switching */
    kernel_pgdir_phys = pd_phys;

    /* Register the page fault handler + the debug (#DB) watchpoint handler */
    isr_install_handler(14, page_fault_handler);
    isr_install_handler(1, debug_watch_handler);

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

    printk("[VMM]  New PD at phys 0x%08x. Recursive mapping active.\n",
           (unsigned)pd_phys);
}

void paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    virt &= ~0xFFFU;
    phys &= ~0xFFFU;

    uint32_t *pde = paging_get_pde(virt);
    if (!(*pde & PAGE_PRESENT)) {
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
        if (!(*pde & PAGE_PRESENT)) {        /* re-check inside the critical section */
            uint32_t pt_phys = pmm_alloc_frame();
            if (!pt_phys) {
                if (eflags & 0x200) __asm__ volatile("sti");
                printk("[VMM]  FATAL: OOM allocating page table for 0x%08x\n",
                       (unsigned)virt);
                for (;;) __asm__ volatile("hlt");
            }
            *pde = pt_phys | PAGE_PRESENT | PAGE_WRITABLE | (flags & PAGE_USER);
            /* Zero the new page table via the recursive window */
            uint32_t *pt = paging_get_pte(virt & ~0x3FFFFFU);
            for (int i = 0; i < 1024; i++)
                pt[i] = 0;
        }
        if (eflags & 0x200) __asm__ volatile("sti");
    }

    *paging_get_pte(virt) = phys | flags;
    tlb_flush_single(virt);
}

static void map_higher_half_physical_memory(void) {
    uint32_t total_phys = pmm_total_frames() * PAGE_SIZE;

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
    uint32_t map_limit  = total_phys < direct_max ? total_phys : direct_max;

    paging_map(KERNEL_VMA + 0x3FF000U, 0x3FF000U,
               PAGE_PRESENT | PAGE_WRITABLE);

    /* boot.asm maps 0-8 MiB; extend from there up to the window limit. */
    for (uint32_t phys = 0x800000U; phys < map_limit; phys += PAGE_SIZE) {
        paging_map(KERNEL_VMA + phys, phys, PAGE_PRESENT | PAGE_WRITABLE);
    }

    printk("[VMM]  RAM: %u MiB total; higher-half direct map covers %u MiB "
           "(rest is high memory via temp maps).\n",
           (unsigned)(total_phys / (1024U * 1024U)),
           (unsigned)(map_limit / (1024U * 1024U)));
}

void paging_unmap(uint32_t virt) {
    virt &= ~0xFFFU;
    if (*paging_get_pde(virt) & PAGE_PRESENT) {
        *paging_get_pte(virt) = 0;
        tlb_flush_single(virt);
    }
}

uint32_t paging_get_physical(uint32_t virt) {
    if (!(*paging_get_pde(virt) & PAGE_PRESENT)) return 0;
    uint32_t pte = *paging_get_pte(virt);
    if (!(pte & PAGE_PRESENT)) return 0;
    return (pte & ~0xFFFU) | (virt & 0xFFFU);
}

void paging_set_kernel_permissions(void) {
    uint32_t addr = (uint32_t)(uintptr_t)_text_start;
    while (addr < (uint32_t)(uintptr_t)_text_end) {
        uint32_t *pte = paging_get_pte(addr);
        if (*pte & PAGE_PRESENT)
            *pte &= ~PAGE_WRITABLE;
        tlb_flush_single(addr);
        addr += 4096;
    }
    addr = (uint32_t)(uintptr_t)_rodata_start;
    while (addr < (uint32_t)(uintptr_t)_rodata_end) {
        uint32_t *pte = paging_get_pte(addr);
        if (*pte & PAGE_PRESENT)
            *pte &= ~PAGE_WRITABLE;
        tlb_flush_single(addr);
        addr += 4096;
    }
    printk("[VMM]  Kernel .text/.rodata marked read-only (W^X).\n");
}

/* ─── Temporary page mapping ─────────────────────────────────────────────── */
/*
 * TEMP_MAP_VIRT and TEMP_MAP_VIRT2 use PTEs 1 and 2 of the shared
 * boot_page_table1 (PDE[768] in every pgdir points to the same physical
 * frame for boot_page_table1).  Modifying these PTEs is globally visible
 * across all page directories.  Must only be used with IF=0.
 */

void *paging_temp_map(uint32_t phys) {
    *paging_get_pte(TEMP_MAP_VIRT) = (phys & ~0xFFFU) | PAGE_PRESENT | PAGE_WRITABLE;
    tlb_flush_single(TEMP_MAP_VIRT);
    return (void *)TEMP_MAP_VIRT;
}

void paging_temp_unmap(void) {
    *paging_get_pte(TEMP_MAP_VIRT) = 0;
    tlb_flush_single(TEMP_MAP_VIRT);
}

void *paging_temp_map2(uint32_t phys) {
    *paging_get_pte(TEMP_MAP_VIRT2) = (phys & ~0xFFFU) | PAGE_PRESENT | PAGE_WRITABLE;
    tlb_flush_single(TEMP_MAP_VIRT2);
    return (void *)TEMP_MAP_VIRT2;
}

void paging_temp_unmap2(void) {
    *paging_get_pte(TEMP_MAP_VIRT2) = 0;
    tlb_flush_single(TEMP_MAP_VIRT2);
}

/* ─── Page directory management ──────────────────────────────────────────── */

uint32_t pgdir_create(void) {
    uint32_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) return 0;

    /* Temp-map the new PD to initialize it */
    uint32_t *pd = (uint32_t *)paging_temp_map(pd_phys);

    /* Zero all entries */
    for (int i = 0; i < 1024; i++)
        pd[i] = 0;

    /* Copy kernel higher-half mappings from the current kernel pgdir */
    uint32_t *cur_pd = KPGDIR;
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
void pgdir_map(uint32_t pgdir_phys, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t prev_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");
    paging_map(virt, phys, flags);
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");
}

/* Return the physical frame backing `virt` in pgdir_phys, or 0 if unmapped. */
uint32_t pgdir_virt_to_phys(uint32_t pgdir_phys, uint32_t virt) {
    uint32_t prev_cr3, phys = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");
    if (*paging_get_pde(virt) & PAGE_PRESENT) {
        uint32_t pte = *paging_get_pte(virt);
        if (pte & PAGE_PRESENT) phys = pte & ~0xFFFU;
    }
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");
    return phys;
}

/*
 * pgdir_free_user — free all user-space pages (PDE 0-767) in a page directory,
 * then free the page directory frame itself.
 *
 * Temporarily switches CR3 to pgdir_phys so the recursive mapping addresses
 * the target page directory.  IF must be 0 at call time.
 */
void pgdir_free_user(uint32_t pgdir_phys) {
    uint32_t prev_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(prev_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(pgdir_phys) : "memory");

    uint32_t *pd = KPGDIR;  /* now addresses pgdir_phys via recursive mapping */

    for (int i = 0; i < 768; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        uint32_t pt_phys = pd[i] & ~0xFFFU;

        /* Access page table entries via recursive mapping */
        uint32_t *pt = (uint32_t *)(PAGE_TABLES_BASE + (uint32_t)i * PAGE_SIZE);
        for (int j = 0; j < 1024; j++) {
            if (!(pt[j] & PAGE_PRESENT)) continue;
            uint32_t frame_phys = pt[j] & ~0xFFFU;
            pmm_frame_decref(frame_phys);
        }

        pd[i] = 0;
        pmm_free_frame(pt_phys);  /* PT frame itself is not ref-counted */
    }

    /* Restore previous pgdir (also flushes TLB) */
    __asm__ volatile("mov %0, %%cr3" :: "r"(prev_cr3) : "memory");

    /* Free the pgdir frame (not ref-counted) */
    pmm_free_frame(pgdir_phys);
}

/* ─── Page fault handler ─────────────────────────────────────────────────── */

/* Read a 4-byte word from the faulting process's user space, present-checked so
 * a wild pointer in a crashed thread can't fault us in ring 0.  The fault keeps
 * the process's CR3 loaded, so user VAs resolve directly.  Returns 0 if the
 * address is out of range or unmapped. */
static uint32_t user_read_word(uint32_t va) {
    if (va < 0x08000000U || va >= 0xC0000000U) return 0;
    if (!(*paging_get_pde(va) & PAGE_PRESENT)) return 0;
    if (!(*paging_get_pte(va) & PAGE_PRESENT)) return 0;
    return *(uint32_t *)(uintptr_t)va;
}

/* ── Hardware watchpoint (DR0/#DB) ───────────────────────────────────────────
 * Catch the exact instruction that writes the corrupting value into a watched
 * user heap address.  DR0 holds the linear address; DR7 enables a 4-byte WRITE
 * breakpoint; #DB (vector 1) traps AFTER the write so we can read the value.
 * Only meaningful for one process (the watched pid), since the linear address
 * exists in every address space. */
static uint32_t wp_addr;     /* watched linear address (0 = disabled) */
static int      wp_pid;      /* only report writes by this pid */
static uint32_t wp_target;   /* the corrupting value we're hunting */

void watchpoint_arm(uint32_t addr, int pid, uint32_t target_value) {
    wp_addr = addr; wp_pid = pid; wp_target = target_value;
    __asm__ volatile("mov %0, %%dr0" :: "r"(addr));
    /* DR7: L0=1 (bit0), R/W0=01 write (bits16-17), LEN0=11 4-byte (bits18-19) */
    uint32_t dr7 = (1U << 0) | (1U << 16) | (3U << 18);
    __asm__ volatile("mov %0, %%dr7" :: "r"(dr7));
    __asm__ volatile("mov %0, %%dr6" :: "r"(0U));   /* clear status */
    printk("[wp] armed DR0=%08x pid=%d hunting val=%08x\n",
           (unsigned)addr, pid, (unsigned)target_value);
}

static unsigned wp_hits;
static void debug_watch_handler(registers_t *regs) {
    uint32_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" :: "r"(0U));      /* clear status */
    if (!(dr6 & 1) || !wp_addr) return;                /* not our DR0 hit */
    if (!current_proc || current_proc->pid != wp_pid) return;
    if (++wp_hits <= 3 || (wp_hits % 2000) == 0)
        printk("[wp] hit #%u eip=%08x cur=%08x\n", wp_hits, (unsigned)regs->eip,
               (unsigned)((*paging_get_pde(wp_addr) & PAGE_PRESENT) &&
                          (*paging_get_pte(wp_addr) & PAGE_PRESENT)
                          ? *(volatile uint32_t *)(uintptr_t)wp_addr : 0xDEADU));
    uint32_t val = 0;
    if ((*paging_get_pde(wp_addr) & PAGE_PRESENT) &&
        (*paging_get_pte(wp_addr) & PAGE_PRESENT))
        val = *(volatile uint32_t *)(uintptr_t)wp_addr;
    /* Report the write that lands the corrupting value (and disarm to stop). */
    if (val == wp_target) {
        printk("[wp] HIT! eip=%08x wrote %08x to %08x (ring %s)\n",
               (unsigned)regs->eip, (unsigned)val, (unsigned)wp_addr,
               (regs->cs & 3) ? "user" : "KERNEL");
        printk("[wp]  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
               (unsigned)regs->eax, (unsigned)regs->ebx, (unsigned)regs->ecx,
               (unsigned)regs->edx, (unsigned)regs->esi, (unsigned)regs->edi,
               (unsigned)regs->ebp);
        wp_addr = 0;                                   /* disarm */
        __asm__ volatile("mov %0, %%dr7" :: "r"(0U));
    }
}

static void page_fault_handler(registers_t *regs) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    uint32_t err = regs->err_code;

    /*
     * COW fault: protection violation (bit 0) + write (bit 1) + PAGE_COW set.
     * Allocate a new frame, copy the old one, update PTE.
     */
    if ((err & 0x3U) == 0x3U &&
        (*paging_get_pde(cr2) & PAGE_PRESENT)) {
        uint32_t *pte = paging_get_pte(cr2);
        if ((*pte & PAGE_PRESENT) && (*pte & PAGE_COW)) {
            uint32_t old_phys = *pte & ~0xFFFU;
            uint32_t new_phys = pmm_alloc_frame();
            if (!new_phys)
                panic("COW fault: out of physical memory", regs);

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

            /* Update PTE: writable, no COW */
            *pte = new_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
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
        if ((*pte & PAGE_PRESENT) && (*pte & PAGE_WRITABLE) && !(*pte & PAGE_COW)) {
            tlb_flush_single(cr2 & ~0xFFFU);
            return;
        }
    }

    /* Demand-paged anonymous VMA: a not-present fault may be the first touch of
     * a lazily-allocated mmap region (e.g. a thread stack).  This fires for BOTH
     * user-mode faults AND kernel-mode faults — the kernel writes to user mmap
     * buffers during syscalls (copy_to_user, a read() into a fresh buffer), and
     * those faults occur at CPL=0 on a user address.  Populate + retry. */
    if (!(err & 0x1U) && cr2 < 0xC0000000U && current_proc &&
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
        if (!(err & 0x1U) && current_proc &&
            cr2 >= stack_grow_floor && cr2 < (uint32_t)USER_STACK_BASE) {
            uint32_t page = cr2 & ~0xFFFU;
            uint32_t phys = pmm_alloc_frame();
            if (phys) {
                pmm_frame_incref(phys);
                paging_map(page, phys,
                           PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
                __builtin_memset((void *)page, 0, PAGE_SIZE);
                tlb_flush_single(page);
                return;
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
    const char *access  = (err & 0x2U) ? "write"      : "read";
    const char *ring    = (err & 0x4U) ? "user"        : "kernel";

    uint32_t pte_val = 0;
    if (*paging_get_pde(cr2) & PAGE_PRESENT) pte_val = *paging_get_pte(cr2);
    printk("[PAGE FAULT] pid=%d addr=0x%08x  eip=0x%08x  (%s %s in %s mode) pte=%08x\n",
           current_proc ? current_proc->pid : -1,
           (unsigned)cr2, (unsigned)regs->eip, access, present, ring,
           (unsigned)pte_val);
    printk("  eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
           (unsigned)regs->eax, (unsigned)regs->ebx, (unsigned)regs->ecx,
           (unsigned)regs->edx, (unsigned)regs->esi, (unsigned)regs->edi,
           (unsigned)regs->ebp, (unsigned)regs->useresp);
    /* Aliasing check: is the physical frame holding the corrupt value (at edi)
     * mapped at MORE THAN ONE user virtual address?  If so, a write through the
     * alias VA corrupted edi's frame (a frame-aliasing kernel bug) — which a VA
     * watchpoint could never catch.  Scan all user PTEs for the same frame. */
    if (current_proc && regs->edi >= 0x08000000U && regs->edi < 0xC0000000U) {
        uint32_t want_va = regs->edi & ~0xFFFU;
        uint32_t want_ph = 0;
        if ((*paging_get_pde(want_va) & PAGE_PRESENT) &&
            (*paging_get_pte(want_va) & PAGE_PRESENT))
            want_ph = *paging_get_pte(want_va) & ~0xFFFU;
        if (want_ph) {
            int aliases = 0;
            for (uint32_t pde = 0; pde < 768 && aliases < 6; pde++) {
                if (!(*paging_get_pde(pde << 22) & PAGE_PRESENT)) continue;
                uint32_t *pt = (uint32_t *)(PAGE_TABLES_BASE + pde * PAGE_SIZE);
                for (uint32_t pti = 0; pti < 1024 && aliases < 6; pti++) {
                    if (!(pt[pti] & PAGE_PRESENT)) continue;
                    if ((pt[pti] & ~0xFFFU) != want_ph) continue;
                    uint32_t va = (pde << 22) | (pti << 12);
                    if (va == want_va) continue;
                    printk("  [ALIAS] frame %08x (of edi %08x) ALSO mapped at %08x pte=%08x\n",
                           (unsigned)want_ph, (unsigned)want_va, (unsigned)va,
                           (unsigned)pt[pti]);
                    aliases++;
                }
            }
            if (!aliases)
                printk("  [ALIAS] frame %08x of edi is mapped at exactly one VA (no aliasing)\n",
                       (unsigned)want_ph);
        }
    }
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
        /* Dump memory around the registers that point into mapped user memory —
         * one of them holds the corrupted heap location feeding the bad pointer.
         * Print 0x20 bytes around edi/esi/edx if present (read-checked). */
        uint32_t probes[3] = { regs->edi, regs->esi, regs->edx };
        const char *pn[3] = { "edi", "esi", "edx" };
        for (int pi = 0; pi < 3; pi++) {
            uint32_t a = probes[pi] & ~0xFU;
            if (a < 0x1000 || a >= 0xC0000000U) continue;
            if (!(*paging_get_pde(a) & PAGE_PRESENT) ||
                !(*paging_get_pte(a) & PAGE_PRESENT)) continue;
            const uint32_t *m = (const uint32_t *)a;
            printk("  mem@%s=%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                   pn[pi], (unsigned)a, (unsigned)m[0], (unsigned)m[1],
                   (unsigned)m[2], (unsigned)m[3], (unsigned)m[4], (unsigned)m[5],
                   (unsigned)m[6], (unsigned)m[7]);
        }
    }

    /* User-stack backtrace: the page fault keeps the faulting process's CR3, so
     * its user pages are directly readable here (present-checked).  Walk the EBP
     * chain printing each frame's return address — symbolize offline with
     * tools/ffsym.sh against the fixed library load bases.  Only for faults that
     * involve a user context. */
    if (current_proc && cr2 < 0xC0000000U) {
        uint32_t bp = regs->ebp;
        printk("[bt] eip=%08x esp=%08x tgid=%d gs=%04x tls_base=%08x\n",
               (unsigned)regs->eip, (unsigned)regs->useresp,
               current_proc->tgid,
               (unsigned)(regs->gs & 0xFFFF),
               (unsigned)current_proc->tls_base);
        for (int n = 0; n < 12 && bp >= 0x08000000U && bp < 0xC0000000U; n++) {
            uint32_t ret = user_read_word(bp + 4);
            uint32_t nbp = user_read_word(bp);
            if (ret == 0) break;
            printk("[bt] #%d ebp=%08x ret=%08x\n", n, (unsigned)bp, (unsigned)ret);
            if (nbp <= bp) break;          /* stacks grow down; chain must ascend */
            bp = nbp;
        }
    }

    /* User-mode fault: send SIGSEGV and let the process die gracefully.
     *
     * Fault-loop breaker: a process may install a SIGSEGV handler that returns
     * straight back to the faulting instruction (e.g. Firefox's crash handler
     * that expects the re-fault to hit SIG_DFL).  If the same EIP keeps faulting
     * with no forward progress, the handler is making things worse — force the
     * default action (terminate) instead of re-invoking it endlessly. */
    if ((err & 0x4U) && current_proc) {
        if (regs->eip == current_proc->last_fault_eip) {
            if (++current_proc->fault_repeat >= 3) {
                /* On a call-to-NULL, [esp] is the caller's return address.
                 * Read it only if the page is actually mapped — a crashed
                 * thread may have a wild esp, and faulting here is in ring 0. */
                uint32_t sp = regs->useresp, retaddr = 0;
                if (sp >= 0x08000000U && sp < 0xC0000000U &&
                    (*paging_get_pde(sp) & PAGE_PRESENT) &&
                    (*paging_get_pte(sp) & PAGE_PRESENT)) {
                    retaddr = *(uint32_t *)sp;
                }
                printk("[SIG] pid=%d SIGSEGV loop eip=%08x addr=%08x ret=%08x — killed\n",
                       current_proc->pid, (unsigned)regs->eip, (unsigned)cr2,
                       (unsigned)retaddr);
                proc_group_exit(SIGSEGV);   /* does not return */
            }
        } else {
            current_proc->last_fault_eip = regs->eip;
            current_proc->fault_repeat   = 0;
        }
        /* Synchronous fault: thread-directed SIGSEGV.  With SIG_DFL the whole
         * thread group exits (Linux force_sig_fault -> get_signal ->
         * do_group_exit), not just the faulting thread. */
        signal_send(current_proc, SIGSEGV);
        signal_deliver_pending(regs);  /* → handler, or group exit if SIG_DFL */
        return;
    }

    panic("Unhandled page fault", regs);
}

/* ── Shared page directories (threads) ──────────────────────────────────── */

#define PGDIR_SHARES 64
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
    if (free_slot >= 0) {
        /* First share: the original owner + the new user. */
        pgdir_shares[free_slot].phys = pgdir_phys;
        pgdir_shares[free_slot].count = 2;
    }
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
