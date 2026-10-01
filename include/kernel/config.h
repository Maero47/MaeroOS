#pragma once

/* Hot-path debug tracing: per-exec/per-signal/per-fault printks (ktrace()) and
 * kprof's probe spans and periodic dump.  Off by default so normal runs are
 * neither slowed nor spammed; `make KTRACE=1` builds them in.  Boot-time and
 * one-shot messages stay plain printk. */
/* `make KHEAP_TEST=1`: kernel heap self-test at boot plus free-memory
 * poisoning (mm/heap.c).  Debug builds only. */
#ifndef KHEAP_TEST
#define KHEAP_TEST 0
#endif
#ifndef KTRACE
#define KTRACE 0
#endif
/* Set to 1 to log every AF_INET socket call and its result (proc/syscall.c).
 * Separate from KTRACE because it distorts what KTRACE measures: QEMU's UART
 * paces output at 115200 baud, ~87 us a character, so each ~60-character
 * line costs the calling syscall about 5 ms. */
#ifndef NETTRACE
#define NETTRACE 0
#endif
/* printk() only in a KTRACE build; the arguments are still type-checked. */
#define ktrace(...) do { if (KTRACE) printk(__VA_ARGS__); } while (0)

#define KERNEL_VMA      0xC0000000UL
#define KERNEL_PHYS     0x00100000UL
#define KSTACKSIZE      32768
#define PAGE_SIZE       4096
#define MAX_PROCS       256   /* one slot per thread.  Firefox's parent runs ~60
                               * threads and each content process ~15-25; after
                               * a page load (a fresh web process next to the
                               * prelaunched and privileged ones) 128 ran out
                               * ("[proc] table FULL (128/128)") and Gecko
                               * crashed on the failed clone.  ptable is .bss,
                               * which boot.asm maps within the first 12 MiB. */
#define MAX_FD          512   /* per process.  The Firefox parent holds IPC
                               * sockets plus a memfd per shared-memory segment;
                               * 128 ran out on the first page load ("failed to
                               * create read-only memfd: Too many open files").
                               * Reported as RLIMIT_NOFILE. */
/*
 * Kernel virtual address map — the one place every kernel window is listed.
 * Everything at or above KERNEL_VMA is shared by every page directory:
 * pgdir_create copies the kernel PDEs, so a window's page tables must exist
 * before the first process is created.
 *
 *   0xC0000000-0xD0000000  direct map of the first 256 MiB of RAM (paging.c);
 *                          PTEs 1-2 are the temp-map slots (TEMP_MAP_VIRT*)
 *   0xD0000000-0xE0000000  kernel heap (HEAP_START..HEAP_MAX)
 *   0xE0000000-0xF0000000  framebuffer (drivers/framebuffer.c)
 *   0xF0000000-0xF2000000  kernel stacks (KSTACK_REGION_*)
 *   0xF2000000-0xF8000000  free
 *   0xF8000000-0xFC000000  device registers (MMIO_WINDOW_*): mm/mmio.c hands
 *                          out uncached ranges at boot, in probe order, with
 *                          a guard page between them — xHCI, AHCI, NVMe,
 *                          e1000, HDA register BARs
 *   0xFC000000-0xFD000000  free
 *   0xFD000000-0xFE000000  ACPI tables and operation regions (ACPI_MAP_*),
 *                          drivers/acpi.c's own allocator: uACPI maps at run
 *                          time too, so its page tables are reserved up front
 *   0xFEC00000             I/O APIC (not mapped; MADT only)
 *   0xFEE00000             local APIC, identity-mapped (LAPIC_PHYS_BASE)
 *   0xFEE01000-0xFFC00000  free
 *   0xFFC00000-0xFFFFFFFF  recursive page tables and page directory
 *
 * The _Static_asserts below keep the windows from growing into each other.
 */
#define HEAP_START      0xD0000000UL
#define HEAP_MAX        0xE0000000UL

/* Kernel stacks live in their own window, not the heap (mm/kstack.c).  Each
 * slot is KSTACK_SLOT_SIZE of address space: the stack occupies the top
 * KSTACKSIZE and everything below it stays unmapped, so running off the bottom
 * of a stack faults on the guard instead of scribbling over whatever the heap
 * put next to it.  The guard is as large as the stack itself, so even a frame
 * that moves %esp by several pages in one go cannot step over it into the
 * neighbouring slot.  Sits above the framebuffer window (0xE0000000-0xF0000000)
 * and well below the identity-mapped LAPIC (0xFEE00000). */
#define KSTACK_REGION_START 0xF0000000UL
#define KSTACK_SLOT_SIZE    0x10000UL      /* 64 KiB: 32 KiB guard + stack */
#define KSTACK_SLOTS        512            /* MAX_PROCS + one per AP, with room:
                                            * 0xF0000000-0xF2000000, its 8 page
                                            * tables reserved at boot */
#define KSTACK_REGION_END   (KSTACK_REGION_START + KSTACK_SLOTS * KSTACK_SLOT_SIZE)

/* ACPI mapping window (drivers/acpi.c): uACPI's tables and SystemMemory
 * operation regions, mapped uncached and never reused.  16 MiB just below the
 * I/O APIC (0xFEC00000) and LAPIC (0xFEE00000) pages; its 4 page tables are
 * reserved by acpi_init before init's page directory is built. */
#define ACPI_MAP_START      0xFD000000UL
#define ACPI_MAP_END        0xFE000000UL

/* Device register window (mm/mmio.c, mmio_map()). */
#define MMIO_WINDOW_START   0xF8000000UL
#define MMIO_WINDOW_END     0xFC000000UL

_Static_assert(KSTACK_REGION_END <= MMIO_WINDOW_START,
               "kernel stacks overlap the MMIO window");
_Static_assert(MMIO_WINDOW_END <= ACPI_MAP_START,
               "MMIO window overlaps the ACPI window");
_Static_assert(ACPI_MAP_END <= 0xFEC00000UL,
               "ACPI window overlaps the I/O APIC");

#define USER_STACK_TOP    0xC0000000UL
#define USER_STACK_PAGES  64
#define USER_STACK_BASE   (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
