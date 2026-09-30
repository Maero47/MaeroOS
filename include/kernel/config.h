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

#define USER_STACK_TOP    0xC0000000UL
#define USER_STACK_PAGES  64
#define USER_STACK_BASE   (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
