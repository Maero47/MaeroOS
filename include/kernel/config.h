#pragma once

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

#define USER_STACK_TOP    0xC0000000UL
#define USER_STACK_PAGES  64
#define USER_STACK_BASE   (USER_STACK_TOP - (USER_STACK_PAGES * PAGE_SIZE))
