#pragma once

/* Hot-path debug tracing: per-exec/per-signal/per-fault printks (ktrace()) and
 * kprof's probe spans and periodic dump.  Off by default so normal runs are
 * neither slowed nor spammed; `make KTRACE=1` builds them in.  Boot-time and
 * one-shot messages stay plain printk. */
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

#define USER_STACK_TOP    0xC0000000UL
#define USER_STACK_PAGES  64
#define USER_STACK_BASE   (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
