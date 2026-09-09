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

### A correctness note on the clustered read

The clustered read must hold the block cache's guard across **both** the disk
transfer and the inserts that publish it, not just the inserts — otherwise a
writer running in between makes the disk newer than what the reader is about to
publish, and the cache is left disagreeing with the disk until those blocks are
evicted. `ext2_read_block` has always held the guard across both halves; the
clustered path initially did not, and now does.

Holding it across the transfer is nearly free: `ata_read` already runs the whole
transfer with interrupts disabled (a cluster is at most 128 sectors, one ATA
command), so preemption was impossible for the expensive part anyway.

Measured, the window could not in fact be entered on this kernel:
`scheduler_tick` only preempts a thread it interrupts in **user** mode, the
window contains no voluntary yield or sleep, and every trap holds the big kernel
lock. Instrumenting the window to count context switches across it recorded
**51 626 clustered reads during a Firefox startup with 40+ threads and zero
crossings**. The defect was latent, not live — but the code was relying on a
global scheduling property instead of a local invariant, which an
interrupt-driven disk driver or preemptible kernel sections would silently
invalidate.

`tools/race_probe.py` (probe in `userspace/raceprobe/`) runs a reader that mmaps
a file several times larger than the block cache and faults every page, against
a thread rewriting whole blocks of that file, then verifies every block through
a fresh open.

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

### Two ext2 defects found and fixed alongside

Neither is a performance problem, but both were found by this work and both are
in `fs/ext2.c`.

**Blocks were lost on delete.** `ext2_free_inode_blocks` walked the direct blocks
and the singly-indirect chain and stopped, although the driver reads all three
indirect levels and allocates two of them. Everything a file held past ~268 KiB
(1 KiB blocks) was lost the moment it was deleted — the bitmap bits stayed set
with nothing referencing them. One create-and-delete cycle of a 12 MiB file cost
**12 074 blocks**, and repeated cycles filled the disk. Truncate leaked the same
way when shrinking, and its size ceiling was applied to every call rather than
only to growth, so `ftruncate` on a large file returned `-1` instead of releasing
the tail. Both now go through `ext2_free_blocks_from(inode, from)`, which walks
all three levels and drops indirect blocks that stop being needed.

**Unlink released an inode that was still open.** `ext2_unlink` freed the blocks
and the inode immediately, so the allocator could hand a live file's blocks to
another file — the same defect tmpfs had, and worse than a leak. ext2 makes a
fresh `vfs_node_t` per lookup, so the reference count cannot live on the node as
tmpfs's does; it is keyed on the inode number instead, fed by the
`retain_fn`/`close_fn` hooks `vfs_retain`/`vfs_close` already drive.

`statfs` reported fixed constants, so nothing could observe any of this; it now
reports the superblock's live counters when an ext2 volume is mounted.

`ports/abiprobes/p26_unlink_frees_space` checks all three properties, is verified
against real Linux, and fails on the unfixed kernel with *"10 create/unlink
cycles of a 2048 KiB file lost 17880 KiB"*.

## Results

Five runs per configuration, `make smoke-firefox` (KVM, `-smp 1`, 2 GiB):

| Configuration | First paint (s) | Mean | Spread |
|---|---|---:|---:|
| baseline (`5a88eee` + kprof) | 217.8 200.5 224.0 232.4 224.7 | 219.9 s | 31.9 s |
| + dirty-row present, + ext2 clustering | 81.6 81.3 82.8 82.7 82.0 | 82.1 s | 1.5 s |
| (rejected) per-slot 32 MiB cache | 112.5 (one run, abandoned) | - | - |
| + slab-backed, memory-sized cache | 67.3 65.5 67.1 67.9 65.8 | **66.7 s** | 2.4 s |
| the same, with `-cpu host` | 66.9 67.7 65.7 66.8 67.3 | 66.9 s | 2.0 s |
| + the clustered-read cache guard | 67.9 67.5 66.5 67.4 66.7 | 67.2 s | 1.4 s |
| + the ext2 space-accounting fixes | 68.5 66.3 66.6 68.9 68.7 | 67.8 s | 2.6 s |

The run-to-run spread collapsing from ~32 s to ~2 s is itself a result: the
variance was the variable amount of screen blitting and repeated disk reading,
not scheduling nondeterminism.

`-cpu host` neither breaks the guest nor helps it: 66.9 s against 66.7 s is a
tenth of the spread. That follows from the breakdown — user code is under 4 % of
a startup, so a wider instruction set has almost nothing to speed up.
`tools/smoke_firefox.py --cpu <model>` re-checks this in one command.

## Round three: 67.7 s -> 34.7 s

Five runs of the round-two tree (`d88019c`) measured **67.7 s** (70.3 67.4 67.0
67.0 66.8). At the mark of the 67.0 s run (`build/ff-smoke/20260909-120012-base-3`,
54.9 s of guest time):

| Bucket | Time | Share |
|---|---:|---:|
| `ata` | 24.70 s | 45.0 % |
| other syscalls | 19.19 s | 34.9 % |
| `pgfault` | 4.56 s | 8.3 % |
| `sched` | 3.01 s | 5.5 % |
| `user` | 2.03 s | 3.7 % |
| `fb` | 1.26 s | 2.3 % |
| `irq` | 0.18 s | 0.3 % |
| `idle` | **0.00 s** | 0.0 % |
| **total** | **54.92 s** | `accounted == elapsed` |

The syscall half: `sched_yield` 6.96 s over 299 613 calls, `mmap2` 5.46 s over
1943, `exit_group` 1.50 s over 8, `clock_gettime64` 1.14 s over 80 977, `execve`
1.11 s over 16, then nothing above 0.5 s.

### Probes

`kprof_probe_begin`/`kprof_probe_end` are named (cycles, count) accumulators for
one code span. They are **additive** and deliberately outside the exclusive
bucket accounting — a probe may nest inside another probe or inside any bucket —
so probe totals sum to nothing and must never be added to the bucket table. They
answer "how much of this syscall is that function", which buckets cannot. A
calibration probe times 256 empty spans per dump, so the instrument's own cost
is a printed number rather than an assumption: it is under 0.5 ms per 1024
spans, i.e. below the resolution of everything measured with it.

Two exact counters were added for this round's central question. One bit per
block of the volume, set the first time the block comes off the platter, splits
disk traffic into `disk_blk` (blocks fetched) and `distinct` (blocks fetched for
the first time). A third counts file faults that land on the page after the same
thread's previous file fault.

### Fifty-seven per cent of the disk traffic was re-reading

A startup fetched **216 314 blocks and only 92 800 distinct ones**. The reads
were well batched but the same blocks kept coming back.

The cache had a memory-based sizing already, and a compile-time set count that
overrode it: the 2 GiB machine wanted 255 MiB and got 4096 sets, 16 MiB. The
slot array is now allocated at mount alongside the buffer slab, so the ceiling
can be expressed in bytes rather than reserved in BSS, and it is 128 MiB — what
the measured working set asks for with room to spare, and still 94 % of the
machine left. A 128 MiB guest is unaffected: the one-eighth-of-free-memory share
still binds there and still lands on 8 MiB.

Blocks fetched **216 314 -> 94 715**, of which 92 702 distinct: with the working
set resident, essentially nothing is read twice. `ata` **24.7 s -> 11.1 s**.

### Fault-around: measured, and not worth doing

Of 67 000 file-backed faults, **6805** land on the page after that thread's
previous file-backed fault. Ninety per cent are scattered, so populating a run
of 8 or 16 pages per fault would mostly fetch pages nobody asked for — and with
the cache now holding the whole working set, a page fetched early is not a page
saved later, it is only a page fetched. The lead is closed with a number rather
than acted on.

### Copying a whole block to read a few bytes out of it

ext2 reached its metadata through `ext2_read_block`, which copies a whole block
out of the cache into a buffer the caller `kmalloc`'d. A block group descriptor
is 32 bytes, an inode 128, an indirect pointer 4: each of those reads allocated
1 KiB, copied 1 KiB, used a fraction and freed the buffer. Over a startup that
was 3.0 s in `ext2_read_inode`, 3.0 s in the indirect walk and 0.9 s in the
`kmalloc`/`kfree` pairs.

`ext2_cache_get` hands out the cache slot itself with the cache's preempt guard
held, and `ext2_read_block_part` copies out only the bytes asked for. The slot
pointer never escapes the guard and **only one slot is ever held at a time** —
holding two would be a use-after-free, because the second fetch can evict the
first when both land in the same set. That is why the indirect walk reads one
32-bit word at a time rather than borrowing whole indirect blocks. With reaching
an indirect block reduced to a lookup, the per-call cache of whole indirect
blocks cost more to fill than it saved and is gone.

`ext2_read_inode` 3.0 s -> 1.06 s, the allocation pairs 0.90 s -> 0.02 s.

### The ATA handshake was two thirds of the transfer

Splitting `ata_read` by transaction size gives a cost model from the totals
alone: a 2-sector transaction cost 160.6 us and an 8-sector one 434.2 us, so
**45.6 us per sector and 69 us fixed per command**. Single-sector PIO spends
three port accesses on every 512 bytes — poll the status for DRQ, `rep insw` the
data, read the alternate status to let the drive settle — and under KVM each of
those is an exit into QEMU. Two of the three are handshake, not data.

READ MULTIPLE is the ATA feature for exactly that: the drive asserts DRQ once
per block of N sectors and the host transfers the whole block. The drive states
its largest block in IDENTIFY word 47, the host selects one with SET MULTIPLE
MODE, and both are checked — a drive that does not offer multiple mode, or
rejects the block size, leaves `ata_multi` at 0 and the original single-sector
path. A transfer that is not a whole number of blocks ends with a short block.

Per sector **45.6 us -> 17.0 us**, `ata` **11.1 s -> 6.5 s**.

### Every reference to the running thread read the Local APIC

This is the one the profiler found by elimination rather than by suspicion, and
it was worth more than everything above it.

Guarding a single block-cache lookup measured **8.6 us**. Probing inside it, the
set scan was 0.01 us and the 1 KiB copy 0.18 us. The time was in the two lines
around them: `preempt_disable()`, and `preempt_enable()`. An empty probe pair in
the same place measured 0.005 us, so it was not the instrument.

`current_proc` is `cpus[this_cpu_id()].proc`, and `this_cpu_id()` answered by
reading the Local APIC ID register. That register lives in the LAPIC's MMIO
page, so a guest read of it exits to the hypervisor — about **1.6 us**.
`preempt_disable()` names `current_proc` twice and `preempt_enable()` three
times: five exits, 8 us, for 0.2 us of work. The same tax was on `sys_pro`
(2 exits), `disp_tss` (2), `yield_pre` (1) and `signal_return_to_user` (~5), and
on every other line of the kernel that mentions the running thread.

While only one CPU executes, the answer cannot change, so it is read once and
remembered; `smp_percpu_go_multi()` turns the cache off before the first AP is
started and every call reads the register again from then on. The fast path is
not an assumption about the machine but a fact about how many CPUs are running.
Verified with `--smp 2`: the AP comes online and Firefox paints.

| | before | after |
|---|---:|---:|
| `pgfault` bucket | 3.94 s | **0.26 s** |
| syscall buckets | 17.90 s | **7.04 s** |
| indirect-block walk | 3.06 s | **0.12 s** |
| `ext2_read_inode` | 1.10 s | **0.014 s** |
| signal delivery on syscall return | 3.28 s | **0.22 s** |
| first paint | 46.2 s | **34.6 s** |

## Results

Five runs per configuration, `make smoke-firefox` (KVM, `-smp 1`, 2 GiB). Every
run's `screen-paint.png` scores ~134 810 light pixels, i.e. real browser chrome
(see `tools/smoke_firefox.py`; below ~40 000 means no evidence).

| Configuration | First paint (s) | Mean | Spread |
|---|---|---:|---:|
| round-two tree (`d88019c`) | 70.3 67.4 67.0 67.0 66.8 | 67.7 s | 3.5 s |
| + 128 MiB block cache | 54.2 53.6 53.5 54.1 54.2 | 53.9 s | 0.7 s |
| + ext2 metadata without the block copy | 52.5 51.1 51.4 52.1 50.7 | 51.6 s | 1.8 s |
| + ATA READ MULTIPLE | 46.0 45.6 45.9 45.9 45.5 | 45.8 s | 0.5 s |
| + the cached CPU id | 35.1 34.8 34.9 35.0 34.9 | 34.9 s | 0.3 s |
| final tree | 34.7 34.7 34.7 34.7 34.7 | **34.7 s** | 0.0 s |

**67.7 s -> 34.7 s, a 48.7 % cut.** With the original 220 s baseline, first paint
is now **6.3x** faster than where this work started.

The spread collapsing to nothing is itself a result: what remained variable was
the disk, and the disk is now mostly cache.

## What is left

At the mark of a 34.7 s run (`build/ff-smoke/20260909-132926-final-t`, 23.0 s of
guest time):

| Bucket | Time | Share |
|---|---:|---:|
| `sched` | 7.39 s | 32.1 % |
| other syscalls | 6.77 s | 29.4 % |
| `ata` | 6.36 s | 27.6 % |
| `fb` | 1.24 s | 5.4 % |
| `user` | 0.92 s | 4.0 % |
| `pgfault` | 0.22 s | 0.9 % |
| `irq` | 0.11 s | 0.5 % |
| `idle` | **0.00 s** | 0.0 % |

The shape has changed completely. Disk was 45 % and is now 28 %; the page-fault
handler was 4.6 s and is now 0.22 s; and the scheduler, which was 5 % of a
startup, is now the largest single bucket — not because it got slower but
because of what it now has to absorb:

* **The yield storm.** Firefox spins on `sched_yield`, and now that a yield is
  cheap it spins far faster: **299 613 calls before, 14.5 million after**. The
  spin is bounded by what it waits for, not by how fast we serve it, so the
  count rose to fill the same wall time. Those 14.5 M context switches are the
  7.4 s of `sched` and 2.2 s of the `sched_yield` bucket — roughly **40 % of
  what a startup now costs**, spent making no progress.
* **Two `cr3` reloads per context switch, 2.49 s.** The dispatch loads the
  incoming thread's page directory and reloads the kernel's on the way back out,
  and each write flushes the TLB. Threads of one process share a page directory,
  so for a switch inside Firefox both writes are avoidable — but only by not
  returning to the kernel page directory between threads, which is what makes
  tearing an address space down safe. Sized, not attempted.
* **`fxsave`/`fxrstor` on every switch, 1.22 s.** Lazy FPU (CR0.TS) is the
  standard answer.
* **`ata` 6.36 s.** 189 184 sectors at 17.0 us and 29 636 commands at ~94 us
  fixed. The sector count is now the distinct working set (92 649 blocks) and
  cannot fall without Firefox touching less; the fixed half, 2.8 s, would come
  down with larger transactions, which needs readahead — and readahead against a
  fault distribution that is 90 % scattered is a bet, not a win.
* **`exit_group` 2.37 s over 9 calls.** Tearing down an address space costs
  260 ms. Nothing else in the profile is that concentrated.
