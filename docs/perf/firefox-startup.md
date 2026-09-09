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
slot array is now allocated at mount alongside the buffer slab, so the size is
decided by the machine rather than reserved in BSS.

Blocks fetched **216 314 -> 94 715**, of which 92 702 distinct: with the working
set resident, essentially nothing is read twice. `ata` **24.7 s -> 11.1 s**.

#### How big, and out of what

Three things bound the cache and they run out independently, so the sizing
consults all three. The first version consulted only the first, which is how it
came to ask for half of the kernel's address space on the strength of a number
that never looked at it.

**A share of free physical memory** (one eighth, measured at mount). The cache is
never handed back, so this is what keeps a small machine honest — and it is what
governs there.

**Heap address space, minus a fixed margin.** The kernel heap is a fixed 256 MiB
span (`HEAP_START`..`HEAP_MAX`) that every other kernel allocation also comes out
of and that nothing ever releases. On the 2 GiB machine it, not physical memory,
is the resource that binds: an eighth of free RAM is 255 MiB, which is the whole
window. The margin is **absolute, not a share**, because what matters to the rest
of the kernel is how many bytes it can still get, not what fraction some other
subsystem took. It is set from measurement: a `[kprof] heap` line reports
headroom at every dump, and everything in the kernel other than this cache uses
**5.5 MiB** of heap to first paint (138 624 KiB used at the mark, of which
133 120 KiB is the cache). The heap only grows, so that is a peak. The margin is
64 MiB — an order of magnitude above the measured demand.

**A ceiling: what the workload is worth.** Measured at six sizes, one
`make smoke-firefox` each, everything else held constant:

| Buffers | First paint | Blocks fetched | `ata` |
|---:|---:|---:|---:|
| 4 MiB | 41.3 s | 218 598 | 13.8 s |
| 8 MiB | 41.3 s | 217 104 | 13.4 s |
| 16 MiB | 41.3 s | 215 310 | 13.4 s |
| 32 MiB | 40.8 s | 205 626 | 12.8 s |
| 64 MiB | 38.0 s | 135 312 | 12.8 s |
| 128 MiB | 35.4 s | 94 370 | 6.2 s |

**There is no knee below 128 MiB.** That is the whole shape of the result and it
is worth stating plainly: a cache that holds *part* of this working set holds
almost none of the value. A startup touches ~92 700 distinct 1 KiB blocks and
cycles over them, so at 16 MiB — a sixth of the working set — LRU evicts every
block before it is wanted again and the cache is worth 1.5 % of the traffic
against a 4 MiB one. The value appears all at once when the cache finally
exceeds the working set: 128 MiB fetches 94 370 blocks against 92 640 distinct,
so 98 % of the fetches are first-time reads and there is nothing left to win.
The ceiling is therefore 128 MiB of buffers, expressed as a 160 MiB budget
because that is what yields exactly those 32 768 sets once the slot descriptors
are counted; the next power of two would need 273 MiB and buy nothing.

What each bound actually decides, booted and read off the console:

| Machine | Free at mount | Binds | Buffers | Heap left after mount |
|---|---:|---|---:|---:|
| 2048 MiB | 2040 MiB | the ceiling | 128 MiB | 125.9 MiB |
| 512 MiB | 504 MiB | physical share | 32 MiB | 223.4 MiB |
| 128 MiB | 120 MiB | physical share | 4 MiB | 251.8 MiB |

#### The fallback has to be able to run

The halving loop that gives up sets when they do not fit was, as first written,
unreachable. `kmalloc()` cannot fail for a request the free list does not
already hold: it grows the heap, and both `heap_expand()` (out of heap address
space) and `vmm_alloc_page()` (out of physical frames) print and enter a `hlt`
loop rather than return. The loop read as a safety valve while the only two
outcomes were success and a wedged kernel — which is worse than no loop, because
the next reader trusts it.

`kmalloc_try()` checks both resources before it can reach either halt and returns
NULL instead. Demonstrated by removing the heap clamp — recreating exactly the
sizing that had no clamp — and asking for a 1 GiB budget against the 256 MiB
window:

```
[EXT2] block cache: 131072 sets did not fit, halving
[EXT2] block cache: 65536 sets did not fit, halving
[EXT2] block cache 131072 KiB (32768 sets x 4 ways of 1024 B) + 2048 KiB slots
[EXT2]  Mounted: block_size=1024  inodes=65536  inode_size=256 cache=on
```

The same request through plain `kmalloc()` ends the boot:

```
[HEAP] FATAL: heap exhausted (at 0xe0000000)
```

— 25 serial lines and no further output, against a system that boots and runs.

`kmalloc()` halting the machine on exhaustion is still the root problem, and it
is not confined to this cache: **any allocation a workload can drive should be
able to fail.** `kmalloc_try()` is the primitive for callers that have a smaller
size they could take; converting the rest of the kernel to handle failure is a
much larger change and is not attempted here.

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
| the same, before the sizing fix | 34.7 34.7 34.7 34.7 34.7 | 34.7 s | 0.0 s |
| + the cache sizing fixed | 34.9 35.1 35.3 34.8 35.0 | 35.0 s | 0.5 s |
| final tree, 20 boots (19 passes) | 34.4 .. 37.3 | **34.9 s** | 2.9 s |

The last two rows are hours apart and the host drifts between them, so the
comparison to make is a same-session one: re-measuring the pre-fix commit
immediately before the final five runs gave 35.8 35.8 34.9, mean **35.5 s**,
against the fixed tree's 35.0 s. The sizing fix changes nothing about the cache
the 2 GiB machine ends up with — 32 768 sets either way — and the numbers agree
that it does not.

**67.7 s -> 34.7 s, a 48.7 % cut.** With the original 220 s baseline, first paint
is now **6.3x** faster than where this work started.

The spread collapsing to nothing is itself a result: what remained variable was
the disk, and the disk is now mostly cache.

## The clock was measuring the wrong thing

Found in review, not by this work: the TSC's one-second refinement reported
**8.3 GHz on a 3.7 GHz machine**, in five boots out of six.

### What the raw inputs said

The refinement computes cycles-per-tick as (TSC span) / (ticks delivered).
Printing both ends of the window rather than guessing at the ratio:

```
[TSC] 36705961 cycles per 10 ms tick (3670 MHz, initial)
[TSCDBG] win=100 span=8105196kcyc avg=82997207 pit0=4 pit=100
         n=96 min=17353 max=4294967295 over2x=11
[TSC] 82997207 cycles per 10 ms tick (8299 MHz, 1 s average)
[TSCDBG] win=1000 span=37286035kcyc avg=38180899 pit0=4 pit=1000
         n=900 min=17278 max=1333633401 over2x=7
```

The arithmetic is right and the tick accounting is right: the tick counter moved
from 4 to 100 while 96 intervals were sampled, and 4 more had been sampled
before, so the divisor of 100 is exactly the number of intervals in the span.
What is wrong is the span. 8.1e9 cycles at the true rate is **2.17 seconds** of
wall time for 100 ticks — and `max=4294967295` is the saturation value, so at
least one "tick interval" exceeded 2^32 cycles: **a single interrupt standing in
for about 113 ticks.**

The guest runs long stretches with interrupts disabled — a PIO disk transfer
holds IF=0 for the whole transfer, and early boot is nothing but disk — and the
PIC collapses every tick missed during one such stretch into one pending
interrupt. QEMU then repays the backlog at faster than 100 Hz: ticks 100..1000
took 7.83 s, i.e. 115 Hz. So the ten-second average was right only because the
catch-up had finished by then, and the one-second average was taken while the
deficit was at its maximum.

The comment in the code asserted that "over 1 s and 10 s the tick count is
faithful to wall time". Over 1 s it is not.

### Fixed by measuring against something that cannot be coalesced

PIT channel 2 is the one channel whose gate is software-controlled and whose
output is readable, both through port 0x61, so a calibration against it is a
busy-wait with no interrupt anywhere in it — which is the whole point. This is
what a PC has always done (Linux: `pit_calibrate_tsc`). `tsc_init()` runs it
once, after `pit_init()` and before `sti`, and the passive tick-interval path is
then never used.

Two things fell out of it. It is **stable to 0.05 %** — 3700 to 3702 MHz across
twenty boots — and it is **the same under KVM and TCG**, where the passive path
gave 3670 and 1459 MHz respectively. And the value it reports is 3700 MHz, which
is the 5600X's invariant-TSC base clock: so the ten-second average that looked
correct at 3818 MHz was itself **3.1 % high**, because even at ten seconds the
tick count had not quite caught up.

Every `kprof` millisecond figure in the sections above was converted with that
3818 MHz rate and is therefore about 3 % low in absolute terms. Bucket shares,
ratios and the before/after comparisons are unaffected — both sides used the
same rate — and first paint is measured on the host, not by the guest clock.

### And a bound, so a bad refinement can never be adopted

The passive path stays as a fallback for hardware whose channel 2 does not
behave, and a refinement there is now adopted only if it is within **±50 %** of
the rate it would replace. A real TSC rate does not change — it is invariant on
any CPU this kernel will meet, and under a hypervisor the guest's follows the
host's — so the only legitimate movement is the bias in the estimate being
replaced.

The bound comes from both requirements rather than being picked round. It must
reject the coalescing error, measured at **2.26x**. It must admit the real
correction, bounded by how wrong the initial estimate can be: that estimate is a
*minimum* over consecutive tick intervals, so it can only be biased low, by a
short catch-up interval, never high, and the worst bias observed is **1.24x**
(3067 MHz against a true 3818). Anything between 1.24x and 2.26x works; 1.5x
leaves 21 % of headroom above the largest correction that has to get through and
rejects the error it has to stop by 51 %. ±25 % was tried first and left under
one per cent of headroom on the admit side — it would have worked on these boots
by luck.

Forcing the fallback (channel 2 stubbed out) shows both halves working:

```
[TSC] PIT channel 2 unusable; falling back to tick intervals
[TSC] 30671224 cycles per 10 ms tick (3067 MHz, initial)
[TSC] 1 s average rejected: 8475 MHz against 3067 MHz (outside +-50%)
[TSC] 38181950 cycles per 10 ms tick (3818 MHz, 10 s average)
```

### Twenty boots

`make smoke-firefox`, back to back: **19 PASS, 1 FAIL**, mean first paint over
the passes **34.9 s**. Every one of the twenty printed exactly one `[TSC]` line,
all of them `PIT ch2`: five at 3700 MHz, eleven at 3701, four at 3702. No
refinement ran, so none could be adopted, and nothing landed outside a sane band.

### The wedge is a different bug, and it is still there

One of the twenty hung, and its clock was correct from boot (3701 MHz, PIT ch2)
— so the wedge is not the calibration. What it is:

* First paint never arrives; the harness times out after 360 s. No panic, no
  `[SIG]`, no page fault, no output at all for the last 340 s.
* The last activity is Firefox's X handshake, ~19 s in, right after its first
  window is created.
* **Two independent periodic outputs stop at the same moment**: `kprof`'s dump,
  which is emitted from syscall entry and gated on the tick counter, and
  maeroX's trace histogram, which its main loop emits every 167 iterations. The
  last kprof dump is `t=10.00s`; there is no `t=20.00s`.

The leading hypothesis is that the tick source stops. Every timeout in this
kernel is tick-based (`wake_tick`), so if `pit_ticks()` freezes, every timed
wait becomes indefinite, every thread ends up blocked, no syscalls run, and
neither periodic output can fire — one cause that explains all of it. That is
not proved: a lost futex wakeup that happens to catch every thread would look
the same from the outside. Distinguishing them needs a thread-state dump
triggered from the serial line, which this kernel does not have; that is the
next step, not a guess to act on.

Rate, honestly: the review saw it once in six boots, this round once in twenty,
and roughly thirty earlier `smoke-firefox` runs in the same session did not hit
it at all. It predates this branch.

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
