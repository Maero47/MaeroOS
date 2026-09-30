#pragma once

/* Hot-path debug tracing: per-exec/per-signal/per-fault printks (ktrace()) and
 * kprof's probe spans and periodic dump.  Off by default so normal runs are
 * neither slowed nor spammed; `make KTRACE=1` builds them in.  Boot-time and
 * one-shot messages stay plain printk. */
#ifndef KTRACE
#define KTRACE 0
#endif
/* printk() only in a KTRACE build; the arguments are still type-checked. */
#define ktrace(...) do { if (KTRACE) printk(__VA_ARGS__); } while (0)

#define KERNEL_VMA      0xC0000000UL
#define KERNEL_PHYS     0x00100000UL
#define KSTACKSIZE      32768
#define PAGE_SIZE       4096
#define MAX_PROCS       128   /* Firefox w/ dom.ipc.processPrelaunch needs ~60+
                               * threads (parent ~40 + prelaunched content ~20)
                               * on top of ~10 system procs — 64 was the ceiling
                               * ([proc] live procs peak=63/64) and allocation
                               * failure mid-launch stalls Gecko in opaque ways. */
#define MAX_FD          128   /* Firefox multiprocess + SCM_RIGHTS opens many fds */
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
#define KSTACK_SLOTS        256            /* MAX_PROCS + one per AP, with room */
#define KSTACK_REGION_END   (KSTACK_REGION_START + KSTACK_SLOTS * KSTACK_SLOT_SIZE)

#define USER_STACK_TOP    0xC0000000UL
#define USER_STACK_PAGES  64
#define USER_STACK_BASE   (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
