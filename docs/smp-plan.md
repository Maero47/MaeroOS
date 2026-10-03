# Removing the Big Kernel Lock: a staged plan

MaeroOS boots up to 8 CPUs, but every kernel path (syscalls, faults, interrupts, the
scheduler loop and kernel threads) runs under one recursive spinlock, the Big Kernel
Lock (BKL). This document covers four things:

- an inventory of what that lock protects today;
- measurements of where it hurts;
- what other kernels did to get rid of theirs;
- a staged, testable plan to replace it.

Stage 0 (lock primitives, a lock-order checker and a torture test) is implemented and
changes no behaviour. Every later stage is a proposal.

Numbers come from `make bench-bkl` (below) under KVM, on a 16-thread host that was also
running other QEMU guests (load average about 10). Expect ±30% run-to-run noise. Every
table gives the median and the individual runs.

---

## 1. Summary

- **SMP makes the kernel slower today.** Every kernel-bound workload runs faster with
  `-smp 1` than with `-smp 4`. This holds even with a single process, where nothing
  contends with it:

  | Workload | `-smp 1` | `-smp 4` |
  |---|---|---|
  | pipe ping-pong | 1.19 M ops/s | 0.14 M ops/s |
  | mmap+munmap | 565 k ops/s | 102 k ops/s |
  | stat | 1.26 M ops/s | 0.55 M ops/s |

  Only CPU-bound user code scales: 4 shell loops finish in 256 ms instead of 643 ms.
  Kernel throughput cannot rise above one CPU's worth, and under contention it falls
  below it: getpid from 4 processes runs at 0.67 M ops/s on 4 CPUs against 2.8 M ops/s
  on 1.
- **There are three causes, and only one of them is lock granularity:**
  1. **The BKL is a test-and-set spinlock taken by every interrupt, including the
     timer tick on idle CPUs.** While one CPU executes, the idle APs spend 70-80% of
     wall time spinning with interrupts off inside their own LAPIC-timer interrupt. In
     `forks`, 81% of all spinning was `lapic-timer` waiting for `execve`.
  2. **Some holds are very long, and the longest are console output.** `execve` held
     the lock ~10 ms per call (99% of lock time in fork+exec loads; single holds up to
     90 ms). Almost all of that is one unconditional `printk("[SYSCALL] exec …")` line
     (`proc/syscall.c`, `sys_exec`), which busy-waits on QEMU's 115200-baud UART
     (~87 µs per character, `include/kernel/config.h`) while holding the BKL.

     Turning that line into `ktrace()` in a scratch build took the 200-exec loop
     from 1990 ms to **54 ms** on `-smp 1` (37×) and from 4955 ms to **175 ms** on
     `-smp 4` (28×). Every other CPU's interrupts wait out such a hold. Any printk
     under the lock costs milliseconds.
  3. **SMP-only costs are paid inside the lock:**
     - Every `munmap` broadcasts a synchronous TLB shootdown to all CPUs. mmap+munmap
       holds grow 8×.
     - Every pipe read/write wakes one global wait channel (`io_activity`), which drags
       the network daemon `knetd` and an idle CPU's scheduler scan into the lock.
       Pipe ping-pong drops 8-17×.
- **The lock hides real races.** Removing it is not mechanical. Each of these is safe
  only because the BKL serialises the CPUs:
  - Check-then-sleep without a lock handshake (pipes, futex, wait, flock, unix
    sockets, ptys).
  - Threads that are visible as RUNNABLE before their context is saved.
  - Global temp-map PTEs.
  - Unlocked printk/klog.
  - Per-CPU `bkl_depth` instead of per-thread depth.
  - `preempt_disable()`, which is not exclusion at all (section 5).
- **Order of work** (section 8):
  1. Cheap wins that keep the BKL. Measured or easy: a TTAS spin plus a cheap
     per-CPU id (2-3.4× on SMP4 kernel throughput in a scratch build), an
     interrupt-free idle path, per-object wait queues, no UART busy-wait under the lock (the exec
     printk alone is 28-37× on fork+exec), targeted shootdowns.
  2. Foundations: wait queues with `sleep(chan, lock)`, an `on_cpu` flag, per-CPU
     kmap slots, atomic refcounts, a locked printk.
  3. Make the BKL per-thread and drop it across `swtch`, as Linux did in `schedule()`.
  4. Then split, in order: page allocator → scheduler → timers → fd tables → pipes →
     VFS caches → block layer → filesystems → network (lwIP stays under one lock) →
     drivers.
  5. Push the BKL from trap entry down into a per-syscall "needs Giant" flag, so
     unconverted code keeps running under it for a long time.

---

## 2. How the BKL works today

| What | Where |
|---|---|
| The lock: `g_bkl`, a test-and-set `spinlock_t` | `arch/i686/cpu/bkl.c` (`bkl_enter`, `bkl_leave`); `arch/i686/cpu/spinlock.h` |
| Recursion: per **CPU**, `cpus[].bkl_depth` | `arch/i686/cpu/percpu.h` |
| Taken at every exception, syscall (`int 0x80`) and hardware interrupt | `isr_common_stub`, `irq_common_stub` in `arch/i686/cpu/isr.asm` |
| Not taken by the TLB-shootdown IPI and the NMI | `tlb_ipi_isr`, `nmi_isr` (isr.asm) |
| Held across `swtch()`; the scheduler loop runs holding it | `scheduler_start` in `proc/scheduler.c` |
| Released only at iret-to-user, around the idle wait, and in `forkret` | `scheduler_start` (idle path), `forkret` in `proc/process.c` |
| Kernel threads run holding it the whole time | `proc_create_kthread` (process.c) |
| A spinner serves TLB-shootdown requests | `bkl_enter` → `tlb_serve_pending` (`arch/i686/cpu/smp.c`) |

How it behaves:

- **It spins with interrupts off.** An interrupt that arrives on CPU B while CPU A is
  in the kernel spins on B until A leaves the kernel. The timer tick of an *idle* AP
  is included.
- **The kernel is not preemptible.** A thread switches only at a return to user mode
  (`sched_irq_exit`, `resched_on_return`).
- **`preempt_disable()` only stops that switch.** It does not exclude other CPUs (see
  `proc/scheduler.c`).

---

## 3. Measurements

### 3.1 Method

`make bench-bkl` builds the kernel with `BKLSTAT=1` (`arch/i686/cpu/bklstat.h`; the
code sits in `bkl.c` under `#ifdef BKLSTAT`, so a normal build contains none of it). It
then boots `-smp 4` under KVM and runs each workload between `schedlat bkl reset` and
`schedlat bkl dump` (syscalls 507/508; ENOSYS in a normal build).

Each CPU charges its lock hold time to a **reason**:

- the syscall number or vector that entered the kernel;
- `sched` (the scheduler loop and the idle re-acquire);
- `kthread`.

A spinning CPU charges its spin twice:

- to its own reason ("waiter");
- to the reason the holder had when the spin began ("blocked_by").

Hold and spin histograms are kept as well. The scheduler hooks (`bklstat_sched_in`/`out`) carry a thread's reason across
`swtch`. The overhead is a few `rdtsc` per lock operation. Each window also contains the exec of the
`schedlat` process itself, which is the `execve` share in the scale rows.

Workloads (`userspace/schedlat`):

| Workload | What it runs |
|---|---|
| `forks` | busybox sh: 200 × fork+exec `/true` |
| `forks4` | 4 of those at once (100 each) |
| `targz` | `tar cf - /lib /bin /usr \| gzip` |
| `targz4` | 4 of those at once |
| `par4` | 4 CPU-bound sh loops (almost no kernel time) |
| `scale K P` | P processes each run a tight syscall loop for 1 s and report total ops/s |

The `scale` loops (`schedlat scale`, new) are:

- **getpid**: trap, lock, return;
- **pipe**: 64-byte write+read on a private pipe;
- **stat**: path walk;
- **mmap**: map, touch and unmap one anonymous page (PMM, page tables, TLB).

### 3.2 SMP1 against SMP4 (same kernel, `bklstat` build)

Median [runs]. Lower is better for `time-*` (ms). Higher is better for the scale
rows (k ops/s).

| Workload | `-smp 1` | `-smp 4` | Effect |
|---|---|---|---|
| time forks (200 exec) | 1990 [2040,1940] | 4955 [5880,4030] | 2.5× slower |
| time forks4 (4×100) | 3770 [3850,3690] | 9605 [8940,10270] | 2.5× slower |
| time targz | 1040 [1080,1000] | 1205 [1080,1330] | ≈ |
| time targz4 | 4095 [4280,3910] | 1485 [1360,1610] | 2.8× faster (gzip is user time) |
| time par4 | 643 [708,578] | 256 [266,246] | 2.5× faster (pure user) |
| getpid ×1 | 2877 | 1297 [860,1735] | 0.45× |
| getpid ×4 | 2784 | 665 [614,716] | 0.24× |
| pipe ×1 | 1187 | 139 [110,167] | 0.12× |
| pipe ×4 | 1302 | 265 [274,257] | 0.20× |
| stat ×1 | 1259 | 549 [375,723] | 0.44× |
| stat ×4 | 1125 | 474 [499,450] | 0.42× |
| mmap ×1 | 565 | 102 [112,93] | 0.18× |
| mmap ×4 | 601 | 129 [133,125] | 0.21× |

### 3.3 Where the time goes (`-smp 4`, run base4-2; build/bench-bkl/results.txt)

- **busy**: % of wall time the lock was held.
- **spin**: per CPU, % of wall time spent spinning with interrupts off.

| Workload | busy | Spin per CPU | Top holders (share of hold time) | Top waiters (share of spin time) |
|---|---|---|---|---|
| forks | 99.8% | 1 / 76 / 72 / 81% | execve 98.8% | **lapic-timer 81%**, sched 19% |
| forks4 | 99.9% | 67 / 61 / 90 / 81% | execve 98.8% | #PF 44%, rt_sigprocmask 12%, lapic-timer 8% |
| targz4 | 39% | 15 / 12 / 21 / 19% | execve 52%, read 21%, write 15% | lapic-timer 27%, sched 12%, write 12% |
| getpid ×4 | 85% | 61 / 75 / 67 / 68% | getpid 78% | getpid 80% |
| pipe ×1 | 85% | 42 / 26 / 14 / 14% | **sched 32%, kthread 25%**, write+read 21% | lapic-timer 36%, read/write 52% |
| pipe ×4 | 89% | 62 / 79 / 70 / 67% | write 44%, read 44% | read/write 91% |
| stat ×1 | 70% | 0 / 10 / 9 / 10% | statx 80% | lapic-timer 74%, sched 25% |
| mmap ×1 | 91% | 11 / 9 / 6 / 6% | munmap 44%, mmap2 42% | lapic-timer 47%, sched 43% |
| mmap ×4 | 94% | 61 / 72 / 69 / 72% | munmap 50%, mmap2 36% | mmap2 43%, munmap 43% |

Holds and spins:

| Workload | Longest single hold | Longest spin |
|---|---|---|
| execve-heavy runs | 90-170 ms | 90-220 ms |
| getpid | – | about 1 µs (the median hold is 2-8 µs) |

Further findings:

- **execve held the lock ~10 ms per exec of a 34 KB static binary, even on one
  CPU.** 203 execs took 2.05 s / 1.96 s in the two `-smp 1` runs; a busybox exec
  took ~17 ms. The cause is the per-exec console line, not exec itself:
  - `printk` → `serial_putc` spins on the UART at 115200 baud, about 5 ms per
    60-character line.
  - On top of that comes a full VGA text-mode scroll (`drivers/vga.c`), all under
    the BKL.
  - With the line demoted to `ktrace()` (scratch build `noexec`, 2 runs on
    `-smp 4`, 1 on `-smp 1`):

    | Workload | `-smp 1` before → after | `-smp 4` before → after |
    |---|---|---|
    | forks | 1990 → 54 ms | 4955 → 175 ms |
    | forks4 | 3770 → 84 ms | 9605 → 374 ms |
    | targz4 | 4095 → 3600 ms | 1485 → 1263 ms |

  - The rest of the exec gap (busybox ~7 ms more than `/true`) is eager ELF loading:
    every page goes through `pgdir_map` and `paging_temp_map`, with several CR3
    reloads per page (`proc/elf.c`, `arch/i686/mm/paging.c`). Next comes the eager
    64-page user stack (`sys_exec`).
  - `[ELF] Loaded …` adds a second line for every PIE exec (`proc/elf.c`).
- **Idle CPUs pay for the lock.**
  - An AP whose LAPIC timer fires (100 Hz) while another CPU is in the kernel spins
    until that CPU leaves, with interrupts off.
  - The idle loop re-takes the lock every ~200k `pause` iterations to rescan the
    ptable (`sched` waiters).
  - On a busy single-process workload the three idle CPUs alone spend 6-14% of wall
    time each spinning.
- **Hold times stretch under SMP even without contention:**

  | Op | `-smp 1` | `-smp 4`, one process |
  |---|---|---|
  | statx | 0.5 µs | 0.9 µs |
  | mmap+munmap | 1.1 µs | 9.3 µs |

  - **mmap+munmap**: `munmap` → `tlb_shootdown()` (`arch/i686/cpu/smp.c`) sends an
    IPI to *every* online CPU, whether or not it runs this address space, and
    busy-waits for all of them to ack.
  - **statx**: the extra cost is cache-line traffic on shared kernel data and the lock
    word.
- **The global `io_activity` channel.** `io_wake()` (`proc/scheduler.c`) is called by:
  - every pipe read and write (`proc/pipe.c`);
  - eventfd;
  - pty;
  - input;
  - NIC and audio IRQs.

  It wakes *every* sleeper on that one channel, `knetd` included (`net/net.c:knetd`,
  which then runs `net_poll_all` under the lock). It also wakes khdad/ksoundd, poll and
  select. In "pipe ×1", 25% of lock time was `knetd` and 32% was idle CPUs' scheduler
  scans (`sched_pick` is a linear ptable scan) kicked by those wakes. That makes a
  private pipe 8.5× slower than on one CPU.
- **`this_cpu_id()`** (a Local APIC MMIO read once the APs run) cost 2-150 ns per call
  here: median 6 ns, with outliers under contention. That is cheap on this host's KVM
  (APIC virtualisation), but the in-tree comment measured 1.6 µs elsewhere. Every
  `current_proc` reference pays it.
- `bad_switch` (a context switch with `bkl_depth != 1`) was 0 in every run. So the
  per-CPU depth (section 5) is a latent bug, not a live one, on these workloads.
- **A boot printk interleaving made one run fail its login.** The APs print "LAPIC
  timer calibrated" without the lock while the BSP prints the getty prompt. The
  `Password: ` prompt came out interleaved with that text, so a harness waiting for
  the prompt timed out. printk/klog take no lock (`kernel/klog.c`). This is the same
  interleaving `smoke-klock` shows on a failure.

### 3.4 Two cheap fixes, measured (scratch builds, not committed)

Both experiments are kept out of the commit, which changes no behaviour; they are
stage-1 candidates (section 8):

- **TTAS**: `bkl_enter` spins on a plain read and only tries the locked exchange when
  the lock looks free. Today every spin iteration is a `xchg`, which bounces the lock's
  cache line between all waiters and the holder.
- **sgdt**: `this_cpu_id()` derives the CPU index from the address of the GDT that this
  CPU loaded (`sgdt`; every CPU has its own GDT in `arch/i686/cpu/gdt.c`). It falls back
  to the LAPIC read before the GDT is loaded.

`-smp 4`, median of 2 runs [runs]; `base4` is the unmodified BKLSTAT kernel:

| Metric | base4 | ttas | sgdt | ttas + sgdt | `-smp 1` reference |
|---|---|---|---|---|---|
| `this_cpu_id()` ns (median) | 6 (2-148) | 53 (2-139) | 6 (3-42) | 6 (3-11) | 1 |
| getpid ×1 k ops/s | 1297 | 792 | 1950 | 1383 | 2877 |
| getpid ×4 | 665 | 1314 | 1208 | **1848 (2.8×)** | 2784 |
| pipe ×1 | 139 | 123 | 232 | 226 | 1187 |
| pipe ×4 | 265 | 573 | 547 | **896 (3.4×)** | 1302 |
| stat ×1 | 549 | 556 | 1101 | **1037 (1.9×)** | 1259 |
| stat ×4 | 474 | 540 | 687 | **1020 (2.2×)** | 1125 |
| mmap ×1 | 102 | 135 | 201 | 195 (1.9×) | 565 |
| mmap ×4 | 129 | 149 | 190 | 223 (1.7×) | 601 |
| time forks4 ms | 9605 | 9685 | 7665 | 7410 | 3770 |
| time par4 ms | 256 | 247 | 244 | 245 | 643 |

Notes:

- The LAPIC id read is cheap *on average* on this host, but it costs 100-150 ns once
  CPUs contend. A `stat` touches `current_proc` many times, so `sgdt` alone doubles
  single-process `stat` throughput.
- TTAS matters once several CPUs spin.
- Together they recover 2-3.4× of kernel throughput under SMP4. That is still below
  `-smp 1`: the remaining gap is idle-CPU tick spinning, the global wake channel,
  shootdowns, and the serial printk below.

---

## 4. Inventory

**IRQ** marks state touched from interrupt context: a device IRQ, the PIT/LAPIC tick
(`scheduler_tick` runs on every CPU), or an IPI.

**Today** is the protection that exists apart from the BKL:

- *BKL* means none;
- *cli* means a local `cli`/`sti`, which excludes nothing on another CPU;
- *preempt* means `preempt_disable`, which is the same as the BKL.

### 4.1 Core: scheduler, processes, memory, timers

| State | Definition / mutators | IRQ | Today | Proposed lock |
|---|---|---|---|---|
| `ptable[]`, `ptable_hwm`, `next_pid` | `proc/process.c`; `allocproc`, `proc_release`, `proc_exit`, fork/clone, waitpid | yes (tick scans it; NMI reads it) | BKL | `sched_lock` (irqsave). Later a pid hash plus per-proc lock |
| Per-thread `state`, `sleep_chan`, `wake_tick`, `vruntime`, `vr_*` | `sleep_on`, `wake_up*`, `scheduler_tick`, `sched_pick` (`proc/scheduler.c`) | yes | BKL | `sched_lock` |
| `sched_min_vr`, `g_sleep_seq`, idle and hand-off counters | `proc/scheduler.c` | tick | BKL | `sched_lock` / per-CPU |
| Run selection | `sched_pick`: linear scan of all of `ptable` per pick, by every CPU | – | BKL | per-CPU run queues (stage 4b) |
| `cpus[c].need_resched`, `.proc`, `.run_t0` written or read cross-CPU | `resched_cpu`, `cpu_curr_vr` | IPI | BKL | atomics, plus a seqcount on `run_t0` |
| `io_activity` global wait channel | `io_wake` (scheduler.c), 35 callers | yes | BKL | per-object wait queues (stage 2) |
| `ktimers[]` | `proc/ktimer.c`: `kt_alloc`, `ktimer_tick` (every CPU), `kt_fire` → `signal_send` | yes | BKL | `timer_lock` (irqsave) |
| Signals: `pending_sigs`, `sighand`/`sigshared` refcounts | `proc/signal.c` (`signal_send` does a non-atomic \|=) | yes (tick, tty ^C, faults) | BKL | per-`sighand` `siglock` (irqsave); atomic refs |
| Futex | no table: waiters found by a ptable scan on `futex_phys` (`futex_wake_n`, `proc/syscall.c`) | no | BKL | hashed futex buckets, each with a spinlock |
| fd tables: `fdtable->refcount`, `ofile[]`; fd object refs (pipe, eventfd, epoll) | `proc/syscall.c`: `fdtable_put`, `fd_retain`/`fd_release` | no | BKL | per-fdtable spinlock; atomic file refs |
| Pipes: ring and `count` | `proc/pipe.c` | no | BKL; check-then-sleep | per-pipe spinlock + wait queue |
| Unix sockets: `all_sockets`, GC state | `proc/usocket.c` | no | BKL | per-socket lock + global `unix_gc` mutex |
| shm `objects[]`, `attaches[]`; `shmaps[]` | `proc/shm.c`, `proc/syscall.c` | no | BKL (bare cli in shm) | `shm` mutex |
| flock list | `proc/flock.c` (comment: "Every syscall runs under the BKL") | no | BKL | `flock` mutex |
| VMA lists (per mm) | `vma_insert`… in `proc/syscall.c` | #PF | cli + BKL | per-mm sleeping lock (mmap_lock) |
| PMM bitmap, cursors, quarantine, `frame_refcount[]` | `mm/pmm.c` (comment: "Single CPU … sufficient") | #PF, IRQ allocations | cli | `pmm_lock` (irqsave); atomic refcounts |
| kmalloc heap (TLSF) | `mm/heap.c` (comment relies on the BKL for SMP) | possible | cli | `heap_lock` (irqsave) |
| Kernel stacks `slot_state[]` | `mm/kstack.c` | no | cli; shootdown "under the BKL" | `kstack_lock` |
| `pgdir_shares[]`, PDE allocation, `sigpage` | `arch/i686/mm/paging.c` | #PF | cli + BKL | `pgtable_lock`, or per-mm |
| Temp-map slots `TEMP_MAP_VIRT`/`2` | `paging_temp_map*` (one global PTE each, local `invlpg` only; "IF=0 only") | #PF COW | BKL + cli | **per-CPU slots** (stage 2) |
| TLB shootdown generations | `arch/i686/cpu/smp.c` (`tlb_shootdown`: "none today: callers hold the BKL") | IPI | atomic req, plain ack | concurrent senders; target only CPUs in the mm's cpumask |
| IRQ handler tables | `arch/i686/cpu/irq.c` | read in IRQ | BKL | `irq_lock` on install (rare); RCU-lite read |
| klog ring, printk → serial/VGA | `kernel/klog.c` ("No locking"), `kernel/printk.c` | yes | none | `log_lock` (irqsave), plus a lockless emergency path for panic and NMI |
| kprof (single global "current bucket") | `kernel/kprof.c` | yes | cli | per-CPU (it is only correct with `-smp 1` today) |
| random pool | `kernel/random.c` (mixed from the PIT IRQ) | yes | BKL | irqsave spinlock |
| kwatch stall detector | `kernel/kwatch.c` | tick | BKL | per-CPU |

### 4.2 VFS and filesystems

| State | Definition / mutators | IRQ | Today | Proposed lock |
|---|---|---|---|---|
| Mount table `g_mnt[]`, `g_mnt_seq`; lazy-umount `g_detached[]` | `fs/vfs.c` `vfs_mount_add`/`remove`; lookups and walks take no lock | no | preempt | `mount_lock` for writers, plus a seqcount (or RCU-lite) for the lookup walk |
| ext2/3/4 rw per instance: block cache, read-ahead buffer, vnode hash, extent cache | `fs/ext2.c` (`ext2_read_block`, `ext2_cache_get`, …) | no | preempt | per-instance `bcache_lock` (spin) + per-vnode mutex |
| ext2 journal; journaled-instance list | `fs/ext2.c` `fs->lock` (sleeping test-and-set, x4/ext3/ext4 only), `g_jfs_lock` | no | own sleeping lock; **plain ext2 has none** | `fs->lock` becomes a `kmutex_t` for every variant |
| ext4 (ro driver): block cache, nodes | `fs/ext4.c` | no | preempt | per-instance lock |
| vfat / exfat: `fs->lock`, FAT/metadata cache, nodes | `fs/vfat.c`, `fs/exfat.c` `fs_lock` (test-and-set + `yield`) | no | own lock | `kmutex_t` |
| tmpfs tree, `refs`, inode numbers | `fs/tmpfs.c` (also uses temp-map slot 2) | no | **BKL only** | per-instance `kmutex_t` |
| devfs console termios / pgrp; pty rings `ptys[8]` | `fs/devfs.c` (`pty_buf_read`/`write` check-then-sleep) | no | BKL only | per-pty spinlock + wait queues |
| procfs scratch nodes `fd_file_nodes[]`, `proc_pid_*_nodes[]` | `fs/procfs.c`: rebuilt by `memset` on every lookup; walks `ptable` unlocked | no | **BKL only** (two CPUs would scribble on each other's node) | allocate per lookup; read the ptable under `sched_lock` |

### 4.3 Block devices and drivers

| State | Definition / mutators | IRQ | Today | Wait style / proposed lock |
|---|---|---|---|---|
| ATA PIO/DMA | `drivers/ata.c` (`ata_irq_save`: "single CPU") | no handler | cli | **Busy-polls with IF=0 and the BKL held, up to 10 s (30 s for FLUSH).** Every other CPU's interrupts wait. Needs a per-channel lock and later an IRQ-driven path |
| AHCI: global 128 KiB bounce buffer, ports | `drivers/ahci.c` | no | cli + **`ahci_lock`** | busy-polls `port_exec`. Already SMP-locked |
| NVMe: global bounce buffer, queues | `drivers/nvme.c` | no | cli + **`nvme_lock`** | busy-polls. Already SMP-locked |
| blkdev registry; partitions | `drivers/blkdev.c`, `drivers/blkpart.c` (USB hotplug from kusbd) | no | BKL / preempt | `blk_lock` (rwlock-ish: hotplug is rare) |
| USB MSC / xHCI rings, device tables | `drivers/usb/*` | xHCI IRQ: counters + wake | `usb_lock` (sleeping test-and-set) | the only block path that **sleeps** for its IRQ (`wait_event` → `sleep_on`) |
| Keyboard and mouse rings | `drivers/keyboard.c`, `drivers/mouse.c` | yes (IRQ1, IRQ12, kusbd) | cli | `input_lock` (irqsave) |
| Framebuffer | `drivers/framebuffer.c` (`framebuffer_write`: full-screen MMIO memcpy under the BKL) | no | write-once | no lock needed: the user owns the bytes |
| VGA text cursor | `drivers/vga.c` | via printk | none | under `log_lock` |
| PCI config space 0xCF8/0xCFC | `drivers/pci.c` (uACPI's `pci_lock` covers only uACPI) | no | BKL | `pci_cfg_lock` (irqsave), shared with uACPI |
| HDA / AC97 / ALSA | `drivers/hda.c` (`verb_lock`), `drivers/alsa.c` (`lk`, `pcm_busy`); `in_buf`/`out_buf` unlocked across `copy_from_user` | IRQ → `io_wake` | partly locked | per-stream lock; private buffers |
| ACPI / uACPI shims | `drivers/acpi.c` (`map_lock`, `pci_lock`, `work_lock`, uACPI spinlocks/mutex) | SCI | **already SMP-locked** | keep; the uACPI mutex `yield()`s while held → `kmutex_t` |

### 4.4 Network

| State | Definition / mutators | Context | Today | Proposed lock |
|---|---|---|---|---|
| lwIP core: pools, pcbs, timers | `third_party/lwip`, `net/lwip_port/lwipopts.h` (`NO_SYS 1`, `SYS_LIGHTWEIGHT_PROT 0`) | knetd + socket syscalls | preempt + BKL | **one `net_lock` `kmutex_t`** around every lwIP call (it is not re-entrant). Keep it that way |
| `sockets[]`, refs, rx queues | `net/socket.c` (~30 preempt sections) | syscalls + knetd | preempt | under `net_lock`; per-socket wait queues |
| NIC rings: e1000, rtl8139, r8169 | IRQ handlers only ack and `io_wake`; ring work runs in `net_poll_all` | IRQ + knetd | preempt | under `net_lock`; IRQ ack state is per-device irqsave |
| firewall rules, netlink, rawsock lists | `net/firewall.c`, `net/netlink.c`, `net/rawsock.c` | lwIP hooks + syscalls | BKL only | under `net_lock` (or Giant) |

---

## 5. What the BKL hides today

The splitting stages must deal with each of these first. Each one is safe *only*
because no second CPU can run kernel code.

1. **Check-then-sleep with no handshake.** `sleep_on(chan)` (`proc/scheduler.c`)
   marks the thread SLEEPING and switches. It takes no lock that a waker also takes.

   Callers that test a condition and then call `sleep_on`:
   - pipes (`proc/pipe.c`);
   - eventfd;
   - futex WAIT;
   - waitpid;
   - flock;
   - unix sockets;
   - pty rings;
   - poll/select (`io_wait_sleep`).

   Without the BKL, a waker on another CPU can run between the test and the state
   change, and the wakeup is lost. Fix: xv6's `sleep(chan, lk)`. The condition is
   checked under `lk`, and `sleep` releases `lk` only after the thread is marked
   SLEEPING under the scheduler lock. Or use wait queues with a prepare-to-wait /
   check / schedule sequence.
2. **Runnable before switched.** `yield`, `sleep_on` and `sched_preempt` set
   `state = RUNNABLE`/`SLEEPING` *before* `swtch` saves the context. `proc_exit`
   marks ZOMBIE before its last `swtch`, and the waiter may then free the kernel
   stack.

   Without the BKL, another CPU could:
   - dispatch a thread whose registers are not saved yet;
   - free a stack that is still in use.

   Fix: an `on_cpu` flag cleared after the switch (Linux `p->on_cpu`). xv6 avoids it
   by holding `p->lock` across `swtch` and releasing it in the next context.
3. **The BKL nesting depth is per CPU, not per thread** (`cpus[].bkl_depth`). A thread
   that sleeps with depth 2 hands depth 2 to the next thread on that CPU. Example: a
   page fault inside a syscall that blocks on USB-MSC I/O. That thread then returns
   to user mode still holding the lock. `bad_switch` counts such switches in BKLSTAT
   builds and was 0 in every run; the fix is still structural: stage 3 makes the
   depth per thread.
4. **`preempt_disable()` is not exclusion.** About 30 sections in `net/socket.c` and the
   ext2/ext4/blkpart caches rely on it. It only stops the switch at a return to user;
   the BKL is what actually keeps other CPUs out.

   Latent bug today: `ext2_read_block`/`ext2_cache_get` (`fs/ext2.c`) and the ext4
   cache keep `preempt_disable` across the device read. On USB-MSC that read *sleeps*
   (`wait_event` in `drivers/usb/xhci.c`), which drops the CPU and, with it, the BKL's
   protection of the cache. The "voluntary sleeps must not happen while held" rule in
   `proc/scheduler.c` is already broken there.
5. **Global temp-map PTEs** (`paging_temp_map`/`paging_temp_map2`,
   `arch/i686/mm/paging.c`). There is one PTE each in the shared kernel page table,
   with a local `invlpg` only. Two CPUs mapping at once would get each other's page.

   Callers:
   - fork's page-table copy;
   - ELF load;
   - COW;
   - tmpfs;
   - `proc/syscall.c` in several places, including one across a `vfs_read`.

   Fix: per-CPU slots (the kmap_atomic model): `TEMP_MAP_VIRT + cpu*2*PAGE`, used
   with preemption and interrupts off.
6. **Unlocked printk/klog.** These lines interleave across CPUs today (section 3.3):
   - the APs' boot prints;
   - lockdep and torture FAIL lines.

   The klog ring's index read-modify-write races.
7. **Two-write IPI sends.** `ICR_HI` and then `ICR_LO` are written in `resched_ipi`,
   `tlb_ipi_to`/`tlb_shootdown` and `send_ipi`. A nested interrupt that sends its own
   IPI between the two writes retargets the first one. Fix: irqsave around the pair,
   or x2APIC's single 64-bit ICR write.
8. **TLB shootdown liveness.** Shootdown relies on every target either:
   - having interrupts on, or
   - spinning in `bkl_enter`, which calls `tlb_serve_pending`.

   Any new spin with IF=0 must also serve shootdowns. `kspinlock_t` does (stage 0).
   It also assumes a single sender ("none today: callers hold the BKL").
9. **Cross-CPU reads of per-CPU fields.** `cpu_curr_vr` reads another CPU's 64-bit
   `run_t0`, which can tear on i686. `resched_cpu` writes another CPU's
   `need_resched`.

---

## 6. How others did it

| Kernel | Path | What to take from it |
|---|---|---|
| **Linux** | 2.0 (1996) had one `kernel_flag`, taken on entry like ours. 2.2/2.4 moved hot paths under their own spinlocks and left `lock_kernel()` as a recursive lock that **`schedule()` released and re-took around a context switch**. 2.6 made it preemptible for a while, and then removed it caller by caller ("BKL pushdown" into the individual ioctl/open/llseek handlers). It was removed entirely in 2.6.39 (2011). lockdep (2.6.18) found the ordering bugs that the splitting introduced. | (a) Push the lock *down* into callees first, then delete it where it guards nothing. (b) Drop it across sleep. (c) Turn on a lock validator before splitting. (d) It took ~15 years: the residual cases are the long tail. |
| **FreeBSD** SMPng (5.x-8.x) | `Giant` was a recursive sleep mutex taken around unconverted code. Subsystems became `MPSAFE` one at a time: the network stack first, then VFS (`MNTK_MPSAFE`), then drivers (`D_NEEDGIANT`). Giant is dropped during unbounded sleep and must be taken before any other non-sleepable lock. Lock types: spin mutex (disables interrupts, for interrupt-filter data), sleep mutex, rw, sx (may sleep while held), rmlock. WITNESS checks the order. | A per-entry-point "needs Giant" flag is how unconverted code keeps working. Choose lock types by "may I sleep while holding it". |
| **NetBSD** | `KERNEL_LOCK` is recursive and *dropped while sleeping* and reacquired on wake. Subsystems opt out with `*_MPSAFE` flags (callouts, kthreads, interrupt handlers). Most of the kernel went fine-grained in 5.0 (2009, the newlock2 work); the network stack followed over the 7.x-9.x releases. | The same per-thread, released-on-sleep BKL as stage 3 here. MPSAFE flags per handler. |
| **OpenBSD** | Still runs most syscalls under `KERNEL_LOCK`, with a per-syscall NOLOCK marking for converted ones. `NET_LOCK` is an rwlock around the whole network stack. WITNESS was ported from FreeBSD (6.2). | Converting syscalls one by one works with a small team. One lock for the whole network stack is a defensible end state, like lwIP here. |
| **DragonFly** | Replaced the MP lock with LWKT serializing tokens, which are released automatically when the holder blocks. | Confirms that "drop on sleep" is the right semantics for a giant lock. |
| **xv6 / xv6-riscv** (MIT) | Spinlocks with `push_off`/`pop_off`: a per-CPU nesting count that restores IF only at the outermost release. `acquire` panics on recursion. `sleep(chan, lk)` takes `p->lock` before releasing `lk`, so a wake cannot be lost. `p->lock` is held across `swtch` and released by the next context. | The exact sleep/wakeup and context-switch discipline needed in stages 2-4. Small enough to copy the idea, with attribution. |
| **seL4** | Deliberately keeps a big lock, because every kernel path is short and non-preemptible (Peters, Danis, Elphinstone, Heiser, "For a Microkernel, a Big Lock Is Fine", APSys 2015). | A big lock is fine **only if holds are µs-short**. Ours reach 10-170 ms (console printk at 115200 baud inside exec, ATA polling), so the cheap stage-1 work (shortening holds, keeping idle CPUs and interrupts away from the lock) pays off before any split. |
| **Fuchsia / Zircon** | Lock classes and a runtime validator (lockdep) that builds an acquisition-order graph, checks it for cycles, and checks IRQ-safety (no IRQ-unsafe lock taken while an IRQ-safe one is held). The scheduler lock was split per CPU over time. | The model behind KLOCKDEP here: classes, not instances; edges recorded on acquire; cycle check. |

---

## 7. Lock design

### 7.1 Types

The types are in `include/kernel/klock.h` (stage 0). Their implementation is in
`kernel/klock.c`.

| Type | Use | Rules |
|---|---|---|
| `kspinlock_t` + `kspin_lock_irqsave` / `kspin_unlock_irqrestore` | The default for short sections, and for anything an interrupt handler touches: scheduler, timers, PMM, heap, pipes, wait queues | Never held across `sleep_on`/`yield`/`swtch`. Not recursive. Spins test-and-test-and-set and serves TLB shootdowns while spinning. The IRQ state is restored only by the matching unlock. |
| `kspin_lock` / `kspin_unlock` | Data no interrupt handler ever touches | KLOCKDEP reports a class that is taken both in IRQ and with IF=1. |
| `kmutex_t` | Long sections that may sleep: a filesystem instance, lwIP (`net_lock`), the unix-socket GC, shm, flock | Thread context only. Never under a spinlock. Until stage 3 it needs the BKL held, which every kernel path holds; KLOCKDEP reports otherwise. |
| Wait queue (stage 2) | Replaces `sleep_on(chan)` plus the global `io_activity` | `wait_event(wq, cond, lock)`, xv6-style: the condition is checked under the object's lock. |
| seqcount (stage 4+) | Read-mostly data with a single writer: mount-table lookup, `run_t0`, routing | `arch/i686/cpu/tsc.c` already uses this pattern for the clock. |
| RCU-lite (later, optional) | Mount table, IRQ handler table, firewall rules | A writer copies and publishes. The old copy is freed after every CPU passes a quiescent state (a return to user, or idle). This is only needed if the seqcount retries prove costly. |
| atomics | Refcounts: file, fdtable, sighand, pgdir shares, frame refcounts | `__sync_*` / `__atomic_*` builtins, already used in `smp.c` and `usocket.c`. |

Not planned: reader/writer spinlocks (writer starvation; a seqcount or a plain spinlock
is simpler), adaptive mutexes, and priority inheritance.

### 7.2 Lock order

Outermost first. A lock may only be taken while holding locks *above* it. Spinlocks are
always below every sleeping lock.

```
Giant (the BKL, per-thread and released on sleep from stage 3)
  └ per-mm mmap_lock (kmutex)
     └ filesystem instance kmutex (ext2 fs->lock, vfat/exfat fs_lock, tmpfs)
        └ vnode/inode kmutex
           └ net_lock (kmutex; lwIP)            [disjoint from fs, never nested with it]
              └ object spinlocks: fdtable, file, pipe, pty, socket, futex bucket, wait queue
                 └ sighand siglock
                    └ sched_lock (ptable, run queues; later per-CPU rq locks, ordered by CPU id)
                       └ timer_lock
                          └ heap_lock → kstack_lock → pgtable_lock → pmm_lock (heap growth maps pages)
                             └ irq_lock, input_lock, pci_cfg_lock
                                └ log_lock (printk), always last; never call printk with it held
```

Rules:

1. **IRQ-safety.** Any lock an interrupt handler takes is irqsave everywhere.
2. **No sleeping under a spinlock.** `kmalloc` may run under a spinlock that sits
   above `heap_lock`, because heap growth (`heap_expand`) only takes the locks below it
   and never sleeps. A kmalloc that could sleep (none today) may not.
3. **Wakeups are made under the object lock.** `wake_up` takes `sched_lock` below it.
4. **TLB shootdowns are not sent while holding a spinlock** that a target might be
   spinning on. The target serves shootdowns while spinning, so this is safe today,
   but keep shootdowns outside `sched_lock`.
5. **Per-CPU run queue locks are taken in CPU-id order** when two are needed, for
   migration.

### 7.3 Lockdep-lite (stage 0, `make KLOCKDEP=1`)

It lives in `kernel/klock.c` and is compiled out unless the flag is set.

- **Classes.** One class per lock name: every lock initialised from
  `KSPINLOCK_INIT("x")` is class "x". Classes are registered on first use, up to 64.
- **Held locks.** Each CPU keeps a stack of the spinlocks it holds. Each thread keeps
  a list of the kmutexes it holds, indexed by ptable slot.
- **Order graph.** On every acquire the checker records an edge *held → new* in a
  64×64 bitset. It reports an **inversion** the first time the new edge closes a
  cycle (it searches the whole graph, so A→B→C→A is caught).

It also reports:

| Report | Check |
|---|---|
| recursion | the class is already held on this CPU, or the lock's owner is this CPU, which panics, since the alternative is a silent deadlock |
| irq-unsafe | a class seen both in a hardware interrupt handler (`cpus[].in_irq`) and with IF=1 |
| sleeping with a spinlock held | hooks in `sleep_on` and `yield` (`klock_might_sleep`) |
| kmutex misuse | taken in IRQ, under a spinlock, or without the BKL (until stage 3); unlocked by a non-owner |
| foreign unlock | a spinlock unlocked by a CPU that does not hold it |

Reports are `[lockdep] …` console lines, each printed once, and counted by
`klockdep_reports()`. Not checked yet: lock instances (the checker works on classes,
like Linux), hold-time limits, and IRQ-safe → IRQ-unsafe nesting (Zircon's rule; add
it when the first IRQ-safe lock lands).

---

## 8. Stages

Every stage follows these rules:

1. It keeps `make check`, `SMOKE_SMP=4 python3 tools/smoke_cmds.py`, `make smoke-abi`
   and `make smoke-klock` passing.
2. It adds or extends a torture test.
3. It is measured with `make bench-bkl` (`BENCH_BKL_ARGS="--smp 8"` too) before and
   after.
4. It runs a `KLOCKDEP=1` build through `make check` once before merging.

Risk is the chance of shipping a hang or corruption that the tests miss; effort is in
focused engineer-days.

| # | Stage | Scope (files) | Test | Risk | Effort |
|---|---|---|---|---|---|
| **0** ✅ | Primitives, lockdep-lite, torture | `include/kernel/klock.h`, `kernel/klock.c`, `tools/smoke_klock.py`, `BKLSTAT` + `tools/bench_bkl.py` | `make smoke-klock` (4 CPUs in parallel; a broken lock must FAIL: verified) | none (unused by the kernel) | done |
| **1** | **Cheap wins, still one BKL** | | | | |
| 1a | TTAS spin in `bkl_enter` | `arch/i686/cpu/bkl.c` | bench-bkl getpid ×4 / pipe ×4 | very low | 0.5 |
| 1b | Cheap per-CPU id: `sgdt` (each CPU has its own GDT) or a `%fs` per-CPU segment, instead of the LAPIC MMIO read | `bkl.c` `this_cpu_id`, `gdt.c` | bench-bkl; all smokes | low | 1 |
| 1c | Idle APs stay off the lock. (i) The idle poll checks `need_resched` and a lock-free "runnable count" before re-taking the BKL. (ii) The LAPIC tick on an idle CPU does `scheduler_tick` bookkeeping without the BKL, or is stopped while idle (tickless idle) and the resched IPI is relied on. | `proc/scheduler.c`, `arch/i686/cpu/irq.c`, `isr.asm` (a lock-free path for vectors 0xF0/0xFC when the CPU is idle) | bench-bkl `forks`: idle spin should fall from ~75% to ~0 | medium (wake races) | 3 |
| 1d | Replace `io_activity` with per-object wait channels: pipe, pty, eventfd, socket, input, audio, knetd's own NIC channel. poll/select sleeps on the channels of the fds it watches. | `proc/pipe.c`, `fs/devfs.c`, `proc/syscall.c` (poll), `net/*`, drivers' `io_wake` | bench-bkl pipe ×1 (expect close to SMP1); smoke-net, smoke-x, smoke-hda | medium | 3 |
| 1e | **No serial busy-wait under the lock.** (i) Demote the per-exec `[SYSCALL] exec` and `[ELF] Loaded` lines to `ktrace()`: measured 28-37× on fork+exec (section 3.3). Check every smoke script that waits for them first. (ii) Make printk append to klog and leave the UART drain to the THRE interrupt or a kthread, keeping synchronous output for panic only. (iii) Batch the per-page CR3 reloads in ELF load; map the user stack lazily. | `proc/syscall.c` `sys_exec`, `proc/elf.c`, `kernel/printk.c`, `drivers/serial.c`, `arch/i686/mm/paging.c` | bench-bkl forks/forks4; smoke-dyn, smoke-abi, every suite that greps the console | low (i), medium (ii) | 0.5 + 3 + 2 |
| 1f | Targeted TLB shootdown: a per-mm cpumask of CPUs that ran it; skip CPUs that are idle or in another mm (they reload CR3 on the next dispatch) | `arch/i686/cpu/smp.c`, `proc/scheduler.c` | bench-bkl mmap ×1/×4; threadprobe/mtmalloc smokes | medium | 2 |
| 1g | Locked printk/klog (irqsave `log_lock`; a lockless path for panic and NMI) | `kernel/printk.c`, `kernel/klog.c` | boot logs stop interleaving; smoke-klock FAIL lines are readable | low | 1 |
| 1h | ATA: poll with interrupts on and the BKL dropped between status reads, or use the IRQ | `drivers/ata.c` | smoke-disk, smoke-ext2rw/ext4rw | medium | 2 |
| **2** | **Foundations** | | | | |
| 2a | Wait queues + `sleep_locked(chan, kspinlock)` (xv6 `sleep(chan, lk)`). Convert every check-then-sleep site in section 5, item 1 to check under the object lock. Under the BKL this is behaviour-preserving. | `proc/scheduler.c`, `proc/pipe.c`, futex/wait/flock/usocket, `fs/devfs.c` ptys | smoke-cmds, unixprobe, threadprobe; a new `schedlat torture` (N processes: pipes, futexes, fork/exit, signals, checksummed) | medium | 5 |
| 2b | `on_cpu` flag: `sched_pick` skips threads still switching out; exit frees the stack only after `on_cpu` clears | `proc/scheduler.c`, `proc/process.c` | as above | medium | 2 |
| 2c | Per-CPU temp-map slots (`kmap_local`) | `arch/i686/mm/paging.c` and its callers | fork/COW/ELF smokes, smoke-dyn | low | 2 |
| 2d | Atomic refcounts: file, fdtable, sighand, `pgdir_shares`, `frame_refcount` | `proc/syscall.c`, `proc/signal.c`, `mm/pmm.c`, `paging.c` | heap_stress (leaks), smoke-cmds | low | 2 |
| 2e | IPI send under irqsave; `run_t0` under a seqcount | `scheduler.c`, `smp.c` | – | low | 0.5 |
| **3** | **The BKL becomes per-thread and is dropped across `swtch`** (Linux 2.2-2.6 `schedule()`, NetBSD, FreeBSD Giant). Depth is saved in `struct proc`; `swtch` releases the BKL and the resumed thread re-takes it. The scheduler loop runs *without* the BKL under `sched_lock` (stage 4a lands together with this). | `bkl.c`, `percpu.h`, `scheduler.c`, `process.c` (`forkret`), `isr.asm` | everything; smoke-klock extended (mutexes without the BKL); SMP8 stress | **high** (the central change) | 8 |
| **4** | **Scheduler** | | | | |
| 4a | `sched_lock` (irqsave) over ptable state, `sleep_on`/`wake_up`, `sched_pick`, vruntime, `scheduler_tick`; `sleep_locked` takes it before releasing the object lock | `proc/scheduler.c`, `proc/process.c` | bench-sched (latency must not regress), smoke-cmds SMP4/8 | high | 5 |
| 4b | Per-CPU run queues (one lock each) with simple pull balancing; `sched_pick` stops scanning the whole ptable | `proc/scheduler.c` | bench-sched, bench-bkl | medium | 5 |
| 4c | Interrupt and IPI entry no longer take the BKL: handlers converted to own locks (tick, resched IPI, input, NIC ack); unconverted handlers wrap themselves in `bkl_enter` | `isr.asm`, `irq.c`, drivers | smoke-usb, smoke-net*, smoke-hda | medium | 4 |
| **5** | **Page allocator, heap, kstacks, page tables** (`pmm_lock`, `heap_lock`, `kstack_lock`, `pgtable_lock`, per-mm `mmap_lock`). This is the task's "page allocator first". It is cheap here because these already have saved-IF cli sections, but it only buys parallelism after stage 3, so it sits after it. Page faults on anonymous memory can then run without Giant. | `mm/*.c`, `arch/i686/mm/paging.c`, `vma_*` in `proc/syscall.c` | KHEAP_TEST=1 under SMP4, mtmalloc, bench-bkl mmap | medium | 5 |
| **6** | **Timers and signals**: `timer_lock`; a timer list or wheel instead of scanning every thread's `wake_tick` on each tick; per-sighand `siglock` | `proc/ktimer.c`, `proc/scheduler.c`, `proc/signal.c` | schedlat audio/nice, smoke-abi (signals, itimers) | medium | 4 |
| **7** | **Syscall entry takes Giant only for unconverted syscalls**: a per-syscall MPSAFE table (FreeBSD `SYF_MPSAFE`, OpenBSD NOLOCK). First set: getpid/gettid/getppid, clock_gettime, sched_yield, nanosleep, brk/mmap/munmap/mprotect (after 5), futex (hashed buckets), kill (after 6) | `proc/syscall.c` dispatch, `isr.asm` | bench-bkl getpid ×4 should scale ~4× | medium | 3 + per syscall |
| **8** | **fd tables, pipes, eventfd, epoll**: per-fdtable lock, per-pipe lock + wait queues; read/write/close/dup become MPSAFE for these fd types | `proc/syscall.c`, `proc/pipe.c` | bench-bkl pipe ×4, heap_stress, unixprobe | medium | 5 |
| **9** | **VFS caches**: `mount_lock` + seqcount lookup; procfs scratch nodes made per-call; path walk MPSAFE for initrd and tmpfs first | `fs/vfs.c`, `fs/procfs.c`, `fs/tmpfs.c`, `fs/initrd.c` | bench-bkl stat ×4, smoke-cmds | medium-high | 6 |
| **10** | **Block layer**: `blk_lock` for the registry; per-device request lock; drop the global bounce buffers (or make them per device); ATA per-channel lock | `drivers/blkdev.c`, `ata.c`, `ahci.c`, `nvme.c`, `usb_msc.c` | smoke-disk/ahci/nvme/usb, smoke-install | medium | 4 |
| **11** | **Filesystems**: ext2 `fs->lock` → `kmutex_t` for every variant (plain ext2 too), plus a block-cache spinlock and the ext4 ro cache; vfat/exfat `fs_lock` → `kmutex_t`; fix the preempt-across-sleep cache bug | `fs/ext2.c`, `fs/ext4.c`, `fs/vfat.c`, `fs/exfat.c` | smoke-ext2rw/ext4rw/vfat/exfat (with 4 writers in parallel), smoke-alpine | high (on-disk corruption) | 8 |
| **12** | **Network**: `net_lock` kmutex replaces `preempt_disable`+BKL around every lwIP entry, knetd, and NIC ring polling; socket syscalls MPSAFE under it. lwIP itself stays single-threaded. | `net/*.c`, NIC drivers' poll paths | smoke-net/net6/tcpsrv/fw, smoke-alpine-net (sshd) | medium | 4 |
| **13** | **Drivers**: input rings `input_lock`; audio stream locks; PCI cfg lock shared with uACPI; xHCI `usb_lock` → `kmutex_t`; framebuffer write without any lock | `drivers/*` | smoke-usb/pc/hda/audio/gui | low-medium | 4 |
| **14** | Residual Giant, for a long time: tty/pty line discipline and console, procfs/sysfs generators, netlink and rawsock, ACPI evaluation, mount/umount, module-ish rare ioctls, ptrace, USB hotplug, the installer paths. They stay correct because Giant is still taken for every non-MPSAFE syscall and handler. | – | – | – | – |

**Why this order differs a little from the classic list:**

- The task's order (page allocator → scheduler → …) is the right order of
  *conversions*.
- Here the BKL is taken at trap entry and held by the scheduler loop, so nothing
  converted can run in parallel until stage 3 makes the BKL per-thread and stage 4
  lets the scheduler and interrupts run without it.
- The page allocator (stage 5) is still the first *data* subsystem to split. It can
  be done before stage 3 as a no-op under the BKL, and is ready the moment stage 3
  lands.

**What the stages buy:**

| After | Effect |
|---|---|
| Stage 1 | Idle CPUs and timer ticks stop paying for the lock, pipes stop dragging knetd in, and exec is short. Expect SMP4 to stop being slower than SMP1 for single-process loads. |
| Stage 3 + 4 | Scheduling and interrupts scale. |
| Stage 7 + 5 + 8 | The common syscalls of a desktop and Firefox scale (`mmap`/`brk`/`futex`/`read`/`write` on pipes and eventfd). |
| Stage 9-12 | File and network I/O scale, as far as one lwIP lock allows. |

---

## 9. Test strategy

| Level | Test | When |
|---|---|---|
| Unit / lock | `make smoke-klock` (`KLOCK_TEST=1`): 4 kernel threads leave the BKL and run 800k spinlock and ~1.5k kmutex rounds in parallel. Torn pairs, lost updates or two CPUs inside fail it, and lockdep must catch exactly one deliberate inversion. Extend it with every new primitive (wait queues, `sleep_locked`, seqcount, per-CPU run queues). | every stage |
| Order | A `KLOCKDEP=1` build through `make check` and the SMP smokes: zero `[lockdep]` lines. | before merging each stage |
| Contention | `make bench-bkl` (`--smp 4`, `--smp 8`, `--smp 1` as the reference), recorded in `build/bench-bkl/results.txt`. Each stage states which row it expects to move. | each stage |
| System | `make check` (27 suites; most are `-smp 1`), `SMOKE_SMP=4` and `=8` `smoke_cmds`, `make smoke-abi`, `smoke-pc` (q35, 2 CPUs), `smoke-alpine`/`alpinex` and `smoke-firefox` for the heavy multi-threaded load | each stage |
| Stress (new) | `schedlat torture`: N processes × M threads doing pipe ping-pong with checksums, futex mutex/condvar, fork+exec+exit+wait, mmap/munmap/mprotect of shared memory with content checks, signals between threads, run for minutes under SMP4 and SMP8 with a KLOCKDEP kernel. Plus a filesystem variant: 4 writers on one ext4/vfat mount, then fsck on the host. | from stage 2 |
| Hang detection | kwatch (`kernel/kwatch.c`) NMI dump on stalls; `bkl_state` shows a wedged lock | always |

---

## 10. Stage 0, as implemented

New files:

| File | Contents |
|---|---|
| `include/kernel/klock.h`, `kernel/klock.c` | `kspinlock_t` (irqsave and plain; TTAS spin that serves TLB shootdowns), `kmutex_t` (sleeping, built on `sleep_on`; needs the BKL until stage 3), the lockdep-lite checker (`KLOCKDEP=1`), and the torture threads (`KLOCK_TEST=1`). Nothing in the kernel uses them yet. |
| `arch/i686/cpu/bklstat.h` | Header for the `BKLSTAT=1` contention counters. |
| `tools/smoke_klock.py`, `tools/bench_bkl.py` | The two drivers. |

Changes to existing files:

| File | Change |
|---|---|
| `arch/i686/cpu/bkl.c` | The counters, compiled only with `BKLSTAT=1`. |
| `arch/i686/cpu/isr.asm` | `BKLSTAT` passes the trap frame to `bkl_enter_trap`. The default build is unchanged. |
| `proc/scheduler.c` | `klock_might_sleep` hooks (empty inlines by default); the `bklstat_sched_in`/`out` hooks (empty unless `BKLSTAT=1`). |
| `proc/syscall.c` | Syscalls 507/508, `BKLSTAT` only. |
| `kernel/main.c` | Starts the torture threads with `KLOCK_TEST=1` only. |
| `userspace/schedlat` | `scale` and `bkl` modes; `forks4`/`targz4` workloads. |
| `Makefile` | `BKLSTAT`, `KLOCKDEP`, `KLOCK_TEST` (all recorded in the `.ktrace-stamp` rebuild stamp); `make bench-bkl`, `make smoke-klock`. |

Usage:

```
make smoke-klock                      # SMP4 lock torture; "[KLOCK-TEST] PASS"
make bench-bkl                        # BKL contention report, -smp 4
make bench-bkl BENCH_BKL_ARGS="--smp 8 --tag after-1a"
make KLOCKDEP=1 && make check         # run the suites with the order checker on
```

Both `smoke-klock` and `bench-bkl` leave an instrumented `kernel.elf`; the next plain
`make` rebuilds it (the stamp changes).

---

## 11. References

- Linux: BKL removed in 2.6.39 (kernelnewbies.org/Linux_2_6_39: "the BKL has been
  removed completely from the kernel sources, including the functions lock_kernel()
  and unlock_kernel()"). lockdep design: kernel.org/doc/html/latest/locking/lockdep-design.html.
  GPL: reference only, nothing copied.
- FreeBSD locking(9), mutex(9), witness(4): man.freebsd.org (Giant: recursive, dropped
  across unbounded sleep, taken before other non-sleepable locks). SMPng history in the
  FreeBSD 5.x-8.x release notes.
- NetBSD KERNEL_LOCK(9): man.netbsd.org/KERNEL_LOCK.9 ("dropped while sleeping", "New
  code should not use KERNEL_LOCK", `*_MPSAFE` flags). NetBSD 5.0 release notes.
- OpenBSD witness(4): man.openbsd.org/witness.4 (from BSD/OS 5.0 via FreeBSD);
  syscalls.master NOLOCK; NET_LOCK.
- xv6-riscv (MIT): github.com/mit-pdos/xv6-riscv, `kernel/spinlock.c` (push_off/pop_off),
  `kernel/proc.c` (`sleep(chan, lk)`, `sched`, `p->lock` across `swtch`); the xv6 book,
  chapters "Locking" and "Scheduling". The design is used in `kernel/klock.c`; no code
  was copied.
- Fuchsia/Zircon lockdep design: fuchsia.dev/fuchsia-src/concepts/kernel/lockdep-design
  (lock classes, order graph, IRQ-safety rule).
- S. Peters, A. Danis, K. Elphinstone, G. Heiser, "For a Microkernel, a Big Lock Is
  Fine", APSys 2015.
- DragonFly BSD LWKT serializing tokens (dragonflybsd.org, "LWKT tokens" documentation).
