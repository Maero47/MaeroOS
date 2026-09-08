# Where the time to Firefox's first paint goes

This is the method and the measurements behind the startup work on
`yonet/perf-startup`. It replaces guessing with an accounting that adds up: the
question "why does a browser that starts in a second on Linux take three minutes
here" turned out to have two answers, neither of them the scheduler.

## The instrument

`kernel/kprof.c` (interface in `include/kernel/kprof.h`) partitions **every**
cycle the machine executes into one bucket at a time. A bucket change charges
the TSC delta since the last change to the bucket that was current and starts
counting for the new one, so the buckets sum to the elapsed TSC by construction.
`kprof_dump` prints both totals, and the closure is checkable in every line of
output — every measurement below has `accounted == elapsed` exactly.

The buckets:

| Bucket | What it is | Where it is entered |
|---|---|---|
| `user` | ring 3 | the bucket in effect between traps |
| `idle` | halted, nothing runnable | `scheduler_start` (`proc/scheduler.c`) |
| `sched` | the dispatch loop itself | `kprof_park` around every `swtch` |
| `irq` | hardware interrupt handlers | `irq_handler` (`arch/i686/cpu/irq.c`) |
| `pgfault` | `#PF` handler | `isr_handler` (`arch/i686/cpu/isr_handlers.c`) |
| `ata` | PIO transfers, interrupts off | `ata_read`/`ata_write` (`drivers/ata.c`) |
| `fb` | the memcpy into the framebuffer | `framebuffer_write` (`drivers/framebuffer.c`) |
| `exc` | other CPU exceptions | `isr_handler` |
| one per syscall number | kernel time inside that syscall | `isr_handler` on `int 0x80` |

Two properties make the result trustworthy:

* **Exclusive.** A nested bucket replaces its parent rather than adding to it, so
  `ata` is not also counted in the `read` that caused it, and the syscall buckets
  do not contain the page faults taken inside them.
* **Per thread.** The current bucket is parked in `struct proc` on a context
  switch and restored on dispatch, so a thread that blocks in a syscall stops
  charging that syscall while another thread runs. Time actually spent *blocked*
  is measured separately by `kprof_sleep_*` and attributed to the syscall that
  blocked; that figure is thread-seconds and legitimately exceeds wall time.

Exact counters ride along: syscalls, context switches, page faults split by
cause, ATA transactions and sectors, ext2 block reads with hits and misses,
framebuffer KiB, wake-ups and reschedules.

### Taking a measurement

```sh
make smoke-firefox                     # KVM, -smp 1, 2 GiB; artifacts under build/ff-smoke/
grep '^\[kprof\]' build/ff-smoke/<run>/serial.log
```

A dump is emitted every 10 s of tick time and on demand through syscall 503
(504 resets the counters). `/disk/ff` issues 503 the instant the paint marker
appears, so the line tagged `mark` covers exactly the startup being measured
rather than the nearest periodic dump. All figures below are from `mark` lines.

Measure one QEMU at a time: the numbers are wall-clock and the host has 15 GiB.

## The breakdown

Before any fix, on a Ryzen 5 5600X under KVM with `-smp 1` and 2 GiB, first paint
at **185 s** (`build/ff-smoke/20260908-221923-cpuhost`):

| Bucket | Time | Share | What it actually is |
|---|---:|---:|---|
| `write` syscall | 79.7 s | 43.6 % | the desktop blitting the whole screen to `/dev/fb0` |
| `ata` | 71.6 s | 39.2 % | 367 k PIO transactions, 733 k sectors |
| other syscalls | 21.0 s | 11.5 % | `mmap2` 6.9, `sched_yield` 4.8, `clock_gettime64` 2.8, `exec` 1.2, rest 5.3 |
| `pgfault` | 5.3 s | 2.9 % | 10 k COW, 38 k anonymous, 67 k file-backed |
| `user` | 2.6 s | 1.4 % | Firefox's own code |
| `sched` | 2.3 s | 1.2 % | 232 k context switches |
| `irq` | 0.5 s | 0.2 % | |
| `idle` | **0.0 s** | 0.0 % | |
| **total** | **182.8 s** | **100 %** | `accounted == elapsed`, exactly |

The single most informative number is `idle = 0`. The machine was never once
short of runnable work, so none of the latency-shaped hypotheses — the 100 Hz
tick granularity, wake-to-run delay, the `MAX_PROCS` scans, BKL contention (there
is none at `-smp 1`) — can account for any of the 185 s. This was never a
latency problem. It was throughput, in two places.

## Why `write` was 80 s

The desktop composites the whole screen into a back buffer and then writes all of
it to `/dev/fb0` on every frame, whether or not anything changed. The framebuffer
is device memory, so that copy runs at MMIO speed rather than RAM speed: 4 MiB at
1280x800 measured **10.4 ms**, i.e. about 394 MB/s. A startup did ~3500 of them —
**14 GiB** pushed across the bus to show a window opening.

`present()` (`userspace/desktop/desktop.c`) now keeps a shadow of the frame it
last put on the screen, in ordinary cached memory, and writes only the runs of
scanlines that differ; contiguous dirty rows are coalesced into one `write`.
Comparing 4 MiB of cached RAM is roughly an order of magnitude cheaper than
writing it to the framebuffer, and during a startup nearly every row is identical
from frame to frame. A fullscreen app owns the screen directly, so the handoff
(`blank_framebuffer`) and the app's exit both invalidate the shadow and the next
present is a full blit; if the shadow cannot be allocated, every present is a
full blit exactly as before.

Framebuffer traffic to first paint: **14 GiB -> 10 MiB**. The `write` syscall plus
the `fb` bucket: **79.7 s -> 1.8 s**.

## Why `ata` was 72 s

ext2 read one 1 KiB block per ATA transaction and cached **eight** of them. That
is less than a single page fault's worth: a 4 KiB fault on `libxul.so` needs the
group descriptor, the inode block, up to three indirect blocks and four data
blocks, so the metadata a fault needed was evicted by that same fault's data and
re-read from the platter next time. The hit rate was 34 %.

Three changes:

* The cache is set-associative — bucket by block number, LRU within the bucket —
  so a large cache costs no more per lookup than the old linear scan over eight
  slots. Buffers are allocated on first use, and the total is capped at a share
  of the free physical memory measured at mount, so the cache is large on the
  2 GiB machine that runs the browser and stays small on a 128 MiB one.
* A whole-block read that misses extends over as many physically consecutive file
  blocks as the request still needs, so a 4 KiB page fault on a contiguous file
  is one ATA transaction instead of four. Clustering is restricted to kernel
  destinations: `sys_read` hands the filesystem the user pointer unbounced, and
  `ata_read` transfers with interrupts disabled, so a longer transaction into
  user memory would widen the window in which a demand-paged destination page
  faults in the middle of a live transfer.
* Between the sectors of one transfer the driver read the alternate status
  register four times where the interface needs one. Under KVM each of those is
  a VM exit, so three quarters of that cost was overhead on every 512 bytes.

Transactions **367 k -> 63 k**, hit rate **34 % -> 55 %**, `ata` **71.6 s -> 34.4 s**.

### A measured wrong turn worth recording

The obvious next step — make the cache much bigger — made the system *slower*.
A 32 MiB cache whose buffers were allocated one `kmalloc` per slot cut `ata` from
34.4 s to 28.2 s but pushed `pgfault` from 5.6 s to **20.5 s**, and first paint
went from 82 s to **112 s**. `kmalloc` is a first-fit walk of one free list
(`mm/heap.c`), and both ext2 and the page-fault path allocate on every call, so
32 k extra 1 KiB blocks on that list make every later allocation walk past them.

Allocating all the buffers as a single slab and carving the slots out of it
gives the heap one entry no matter how big the cache is, and gets both: `ata`
26.0 s and `pgfault` 4.5 s. The cache is sized at mount from an eighth of free
physical memory (16 MiB on the 2 GiB machine), rounded to a power-of-two set
count so the bucket index stays a mask.

## Results

Five runs per configuration, `make smoke-firefox` (KVM, `-smp 1`, 2 GiB):

| Configuration | First paint (s) | Mean | Spread |
|---|---|---:|---:|
| baseline (`5a88eee` + kprof) | 217.8 200.5 224.0 232.4 224.7 | 219.9 s | 31.9 s |
| + dirty-row present, + ext2 clustering | 81.6 81.3 82.8 82.7 82.0 | 82.1 s | 1.5 s |
| (rejected) per-slot 32 MiB cache | 112.5 (one run, abandoned) | - | - |
| + slab-backed, memory-sized cache | 67.3 65.5 67.1 67.9 65.8 | **66.7 s** | 2.4 s |

The run-to-run spread collapsing from ~32 s to ~1.5 s is itself a result: the
variance was the variable amount of screen blitting and repeated disk reading,
not scheduling nondeterminism.

## What is left

At ~70 s the remaining profile is `ata` 26.0 s, other syscalls 20.5 s, `pgfault`
4.5 s, `sched` 3.5 s, `user` 2.2 s, `fb` 1.3 s. Disk is still the largest single
cost. Two more leads, both measured but not acted on:

* `sched_yield` — ~315 k calls, ~6.9 s. Worth finding out who spins.
* `mmap2` — ~1900 calls at ~3.6 ms each. `vma_gap_find` calls
  `first_mapped_page`, which walks a candidate range page by page, per candidate.
