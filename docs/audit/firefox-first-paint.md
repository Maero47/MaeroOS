# Kernel audit: why Firefox 115 stalls before first paint

Scope: a read-only comparison of the MaeroOS primitives that Firefox 115.15.0esr,
glibc 2.36 (the libc on the disk image), GTK/GLib and libevent depend on against
the semantics of Linux, with a ranked fix plan. No kernel code was changed.

All MaeroOS references are `file:line` in commit `8da3cde` (branch
`yonet/futex-audit`, 2026-09-06). Reference sources read for this audit:

| Source | Version read | Location of the checkout used |
|---|---|---|
| Linux | `torvalds/linux` master, commit `df2908090cda` (2026-09-06) | sparse clone, `kernel/futex/`, `kernel/fork.c`, `kernel/exit.c`, `kernel/signal.c`, `fs/exec.c`, `mm/madvise.c`, `mm/mmap.c`, `net/unix/af_unix.c`, `net/core/scm.c`, `arch/x86/entry/syscalls/syscall_32.tbl` |
| glibc | tag `glibc-2.36` (`c804cd1c00ad`) and tag `glibc-2.41` (`74f59e9271cb`) | `nptl/`, `sysdeps/nptl/`, `sysdeps/unix/sysv/linux/` |
| Firefox | tag `FIREFOX_115_15_0esr_RELEASE` (`09ae5d4354dd`, github.com/mozilla-firefox/firefox) | `ipc/glue/`, `ipc/chromium/src/`, `dom/ipc/`, `widget/gtk/`, `security/sandbox/linux/`, `js/`, `memory/build/`, `toolkit/xre/` |
| GLib | 2.74.6 (`glib/gspawn.c`) | fetched from gitlab.gnome.org; Debian 12's GLib, matching the Debian glibc 2.36-9+deb12u14 in `testfiles/lib/libc.so.6` |

Limitations: the Firefox binaries and the GTK/GLib libraries are not in this
worktree (`testfiles/firefox` and `testfiles/fflib` are git-ignored), so the
"what does the shipped binary import" question is answered from source, not from
`objdump`. Nothing was booted; every "would" below is a code-reading conclusion
and each carries a probe program (section 8) that proves or disproves it in QEMU.

---

## 1. Executive summary

The futex compare-and-sleep itself is not the problem. Every trap into the
kernel takes the big kernel lock (`arch/i686/cpu/isr.asm:169-176`,
`arch/i686/cpu/bkl.c:18-40`) and the lock is held across `swtch()` until the
next iret, so a `FUTEX_WAIT` that read `*uaddr` and called `sleep_on()`
(`proc/syscall.c:5224-5227`, `:5319`) cannot lose a `FUTEX_WAKE` issued from
another CPU. The wake path matches by `(tgid, uaddr)` for private futexes
(`proc/syscall.c:5150-5190`), which is the Linux key. The real gaps are in what
surrounds the futex: signal and thread-group semantics, the vfork model, the
spurious wake-up sources the kernel adds on top, and scheduler latency. Those
gaps (a) turn ordinary crashes in a child thread into a permanent stall of the
parent, (b) let a failed `posix_spawn` kill the whole browser, and (c) create
exactly the conditions under which glibc 2.36's condvar (BZ#25847) loses a
signal.

The five most likely root causes, ranked:

### RC1. A fatal signal kills one thread, not the thread group (confidence: high)

- Firefox: any `MOZ_CRASH`, `abort()`, or SIGSEGV in a worker thread of the
  parent or a content process. Breakpad's handler writes a dump and re-raises
  the signal with the default disposition; glibc's `raise()`/`abort()` is
  `tgkill(getpid(), gettid(), sig)` (`glibc-2.36/nptl/pthread_kill.c:42-44`).
- Linux: a fatal signal with `SIG_DFL` starts a group exit and SIGKILLs every
  other thread (`kernel/signal.c:985-993`, `complete_signal`;
  `kernel/signal.c:1328-1348`, `zap_other_threads`; `kernel/signal.c:3039`,
  `do_group_exit(signr)` in `get_signal`).
- MaeroOS: `signal_deliver_pending` runs `proc_exit(128 + sig)` for the
  current thread only (`proc/signal.c:124-127`); the page-fault path does the
  same (`arch/i686/mm/paging.c:641-666`); `sys_tgkill` targets exactly one
  thread (`proc/syscall.c:4994-5016`). Siblings keep running with the
  crashed thread's locks held and its work never completed.
- Why it fails first paint: a crash in the "IPC Launch" thread, the IPC I/O
  thread, or any thread that owns a `std::call_once`/`Monitor` state leaves
  the main thread parked forever in `WaitForProcessHandle`
  (`ipc/glue/GeckoChildProcessHost.cpp:871-879`) or in a modal spin. The
  kernel's own `[ff-exit]` trace was added for this symptom
  (`proc/scheduler.c:450-454`: "If the thread running a std::call_once static
  init exits mid-init ... the main thread hangs forever"). A crash in a content
  process likewise leaves its remaining threads holding the IPC socket open, so
  the parent never sees EOF and never gets a `SIGCHLD` it can act on (RC3).
- `sys_exit_group` only marks siblings with SIGKILL (`proc/syscall.c:3298-3310`);
  sleeping siblings are woken (`proc/signal.c:58-61`) and die at their next
  syscall exit, but a CPU-bound sibling dies only when it next traps, because
  `scheduler_tick` never delivers signals (`proc/scheduler.c:254-260`).

### RC2. A `CLONE_VM` child without `CLONE_THREAD` joins the parent's thread group (confidence: high for early exits, medium for hangs)

- Firefox launches `glxtest` (and `vaapitest`) through
  `g_spawn_async_with_pipes` with `G_SPAWN_LEAVE_DESCRIPTORS_OPEN |
  G_SPAWN_DO_NOT_REAP_CHILD`, chosen so that GLib "run[s] posix_spawn()
  directly" (`widget/gtk/GfxInfo.cpp:600-609`). GLib 2.74 does take the
  `posix_spawn` path under those flags (`glib/gspawn.c:2200-2207`). glibc's
  `posix_spawn` is `clone3(CLONE_VM | CLONE_VFORK, exit_signal = SIGCHLD)` on a
  private stack (`sysdeps/unix/sysv/linux/spawni.c:383-390`); on exec failure
  the child calls `_exit(127)` (`spawni.c:306`), and `_exit` is the
  `exit_group` syscall (`sysdeps/unix/sysv/linux/_exit.c`).
- Linux: the child is a separate thread group (`kernel/fork.c:2039-2047`,
  `CLONE_THREAD` requires `CLONE_SIGHAND`; without `CLONE_THREAD` a new
  `signal_struct` is created), so `exit_group` in the child kills only the
  child.
- MaeroOS: `sys_clone` puts every `CLONE_VM` child into the parent's group,
  `child->tgid = parent->tgid` (`proc/syscall.c:5751-5757`, the comment admits
  "We can't key on CLONE_THREAD here"). `sys_exit_group` then SIGKILLs every
  thread with that tgid (`proc/syscall.c:3301-3307`): a `posix_spawn` whose
  `execve` fails (missing binary, `ENOEXEC`, transient `ENOMEM` in
  `elf_load_bias`) kills the whole Firefox parent. This matches the launcher's
  "Firefox exited (status=...) before paint" retries (`userspace/ff/ff.c:231-234`).
  The same child also reports the parent's pid from `getpid()`
  (`proc/syscall.c:1172-1176`), and its `parent` pointer is the calling
  thread, not the process (`proc/syscall.c:5730`).

### RC3. Signal handlers, `SIGCHLD` and `waitpid` are per thread, not per process (confidence: medium-high)

- Linux: threads created with `CLONE_SIGHAND` share one handler table
  (`kernel/fork.c:1690-1694`); process-directed signals go to any thread that
  does not block them (`kernel/signal.c:945-975`, `complete_signal`); any thread
  of the parent can `waitpid` for the process's children; thread exit sends no
  `SIGCHLD`.
- MaeroOS: `sys_clone` copies `sig_handlers` at creation time
  (`proc/syscall.c:5732-5735`), so a `sigaction` on one thread is invisible to
  the others. `proc_exit` sends `SIGCHLD` to `current_proc->parent`, which for
  a forked child is the *thread* that forked (`proc/scheduler.c:585-588`,
  `proc/syscall.c:633`), and it sends `SIGCHLD` for every *thread* exit too.
  `sys_waitpid` only matches children whose `parent` is the calling thread
  (`proc/syscall.c:942`); `sys_kill(pid)` delivers to the one `ptable` entry
  with that pid (`proc/syscall.c:1777-1810`).
- Firefox: content processes are forked on the "IPC Launch" thread
  (`GeckoChildProcessHost.cpp:178-181`, `:1882`; `process_util_linux.cc:269-276`).
  Their `SIGCHLD` therefore goes to the launcher thread, whose handler table was
  snapshotted before the IPC I/O thread installed libevent's `SIGCHLD` handler
  (`process_watcher_posix_sigchld.cc:247-253`), so the `ProcessWatcher` never
  runs and `waitpid` from the I/O thread gets `ECHILD`. Every thread exit also
  wakes the creating thread with a spurious `SIGCHLD` (`proc/signal.c:58-61`),
  which is one of the spurious futex returns in RC4.

### RC4. Kernel-injected spurious wake-ups plus wake-to-run latency widen glibc 2.36's condvar steal window (confidence: high that it widens the window, medium that it is the WaitForProcessHandle trigger)

- glibc 2.36 `__pthread_cond_wait_common`: after the futex returns, the waiter
  drops its group reference (`nptl/pthread_cond_wait.c:521`), reloads
  `__g_signals` (`:524`), and only then CASes a signal away (`:532-533`). If its
  group was closed and the slot reused between `:521` and `:532`, it steals a
  signal meant for a newer group and runs the "undo" code (`:540-583`). The
  undo is the code that BZ#25847 identifies as losing the wake-up
  (`glibc-2.41/NEWS:166-167`). glibc 2.41 removed stealing entirely (its wait
  loop passes the observed `signals` value to the futex and never CASes a
  foreign signal, `glibc-2.41/nptl/pthread_cond_wait.c:389-434`).
- The window is between a futex return and the next few instructions. On
  Linux that is nanoseconds and the futex only returns for a real wake, a
  timeout or a signal (`kernel/futex/waitwake.c:719-738`, spurious wake-ups
  retry inside the kernel). On MaeroOS the window is stretched by:
  1. the "gentle net" that marks every condvar waiter parked for 150 ms
     runnable whenever four or more exist (`proc/scheduler.c:142-180`), and the
     "aggressive net" that does the same to mutex waiters during the whole
     launch phase (`proc/scheduler.c:200-251`); `g_ipc_launch_started` is set by
     the first `socketpair()` and never cleared (`proc/syscall.c:5930-5936`,
     `:4513-4516`);
  2. `signal_send` waking any sleeper for any signal, including the per-thread
     `SIGCHLD` of RC3 and ignored signals (`proc/signal.c:54-61`);
  3. a stale `wake_tick`: `signal_send` does not clear it, `sleep_on` does not
     reset it (`proc/scheduler.c:292-301`), so a thread woken by a signal out of
     `poll()` and then blocking in an untimed `FUTEX_WAIT` is woken again when
     the old deadline passes (`proc/scheduler.c:116-121`);
  4. `sys_futex` returning 0 for a timeout (`proc/syscall.c:5327`) instead of
     `-ETIMEDOUT`, which makes glibc's timed waits take the "spurious" path once
     per timeout;
  5. wake-to-run latency: a woken thread waits for the next pass of an O(n)
     scheduler loop, with 20 ms quanta and no wake-up preemption outside the
     launch phase (`proc/scheduler.c:32`, `:413-424`), and an idle CPU sleeps in
     `hlt` until the next 10 ms tick (`proc/scheduler.c:88-105`).
- Answer to the brief's question: BZ#25847 is not reproducible with a correct
  glibc (2.41 has no steal path), and it is rare on Linux because the window is
  tiny. MaeroOS does not violate futex semantics here (spurious `FUTEX_WAIT`
  returns are permitted), but it manufactures the timing that the 2.36 bug
  needs, and the nets that were added to recover lost wake-ups are themselves
  the largest source of the spurious returns that feed the bug.

### RC5. Thread-exit and crash paths leave IPC endpoints and locks alive (confidence: medium)

A consequence of RC1 to RC3 rather than a separate mechanism, but it is the
shape the "intermittent stall" takes: the fd table is refcounted per thread
(`proc/syscall.c:586-595`), so a content process whose main thread died but
whose I/O thread is parked in `epoll_wait` (which ignores every signal except
`SIGKILL`, `proc/syscall.c:4409-4410`) keeps its socket open; the parent's
channel never reaches EOF (`usocket_release` only marks `writer_closed` on the
last reference, `proc/usocket.c:361-367`), so `Channel::ChannelImpl::ProcessIncomingMessages`
never sees `bytes_read == 0` (`ipc_channel_posix.cc:318-321`) and the launch
promise never resolves or rejects.

What is *not* a root cause, based on the code:

- Futex key/atomicity (see above), `FUTEX_WAIT_BITSET` absolute timeouts for
  both `CLOCK_REALTIME` and `CLOCK_MONOTONIC` (`proc/syscall.c:5244-5251`
  handles the `FUTEX_CLOCK_REALTIME` bit correctly; mozglue's
  `ConditionVariable` uses `CLOCK_MONOTONIC`, `mozglue/misc/ConditionVariable_posix.cpp:33,73`).
- Shared-memory sizing: Chromium sizes memfds with `posix_fallocate`
  (`ipc/chromium/src/base/shared_memory_posix.cc:367-380`), which MaeroOS
  implements (`proc/syscall.c:3778-3790`). `ftruncate64` is missing (section
  7) but is not on the launch path.
- The main-thread stack: only 64 pages are eagerly mapped
  (`include/kernel/config.h:17-18`), but the page-fault handler grows the stack
  on demand within 8 MiB (`arch/i686/mm/paging.c:501-523`), matching the
  advertised `RLIMIT_STACK` (`proc/syscall.c:4434`, `:4467`) that
  SpiderMonkey's quota is derived from (`js/xpconnect/src/XPCJSContext.cpp:1280-1286`).
- The ">512 MiB hangs at boot" claim: the higher-half direct map is now capped
  at 256 MiB and high frames are reached through temporary mappings
  (`arch/i686/mm/paging.c:172-208`); `run-firefox` already boots with `-m 2048M`
  (`Makefile:226-233`). No remaining `phys + KERNEL_VMA` assumption for
  arbitrary frames was found. Probe P18 confirms it.

---

## 2. The launch sequence Firefox needs

`ContentParent::LaunchSubprocessSync` (`dom/ipc/ContentParent.cpp:2799-2806`)
calls `GeckoChildProcessHost::AsyncLaunch` then `WaitForProcessHandle`, which
parks the main thread on `mMonitor` until `mProcessState >= PROCESS_CREATED`
(`GeckoChildProcessHost.cpp:871-879`). For that predicate to become true:

1. Main thread dispatches `BaseProcessLauncher::Launch` to the IPC I/O thread
   (`GeckoChildProcessHost.cpp:740-743`). Cross-thread dispatch to the I/O
   thread is a 1-byte `write()` to a pipe (`message_pump_libevent.cc:394-397`)
   that the I/O thread waits for in `epoll_wait` via libevent.
2. The I/O thread creates the channel: `socketpair(AF_UNIX, SOCK_STREAM)`,
   `O_NONBLOCK`, `FD_CLOEXEC` (`ipc_channel_posix.cc:1276-1298`),
   `getsockopt(SO_SNDBUF)` must return a positive value
   (`ipc_channel_posix.cc:165-181`), and it dispatches `PerformAsyncLaunch` to
   the "IPC Launch" `nsThread` (`GeckoChildProcessHost.cpp:178-181`, `:1882`).
   That dispatch is an `nsThread` event-queue `Notify`, i.e. a futex wake.
3. The launch thread runs `DoSetup` (environment, `fds_to_remap` for the channel,
   crash-reporter pipe and annotation pipe, `GeckoChildProcessHost.cpp:1231-1305`)
   and `DoLaunch` → `base::LaunchApp` (`process_util_linux.cc:231-345`): plain
   glibc `fork()` because the sandbox level is 0 with `MOZ_DISABLE_CONTENT_SANDBOX=1`
   (`security/sandbox/linux/launch/SandboxLaunch.cpp:285-286`, `userspace/ff/ff.c:64`).
   glibc `fork()` is `clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID | SIGCHLD)`
   (`sysdeps/unix/sysv/linux/arch-fork.h`). The child does `dup2` shuffles,
   `CloseSuperfluousFds` (reads `/proc/self/fd`, `process_util_posix.cc:117-160`),
   then `execve` of `firefox-bin -contentproc ...`.
4. The parent side resolves `FinishLaunch` on the launch thread and the
   `Then()` continuation on the I/O thread sets `PROCESS_CREATED` and
   `Notify()`s the monitor (`GeckoChildProcessHost.cpp:783-790`), a
   `pthread_cond_signal` → `FUTEX_WAKE` that must reach the main thread's
   `FUTEX_WAIT_BITSET`.

Nothing in step 3 depends on the child process doing anything; the child only
has to be forked. The predicate depends on two thread hand-offs (pipe write →
epoll; condvar → futex) and on the launcher thread staying alive. RC1 kills the
launcher or I/O thread silently; RC4 loses the hand-off in step 4; RC2 kills
the process during the glxtest spawn that happens on the main thread before
the first content launch.


---

## 3. Findings by area

Impact scale: **blocks paint** (can by itself prevent first paint or stall the
run), **degrades** (wrong behaviour Firefox tolerates or that costs time/memory),
**cosmetic**. Effort: S (< 1 day), M (1-3 days), L (a week). Probe numbers refer
to section 8.

### 3.1 futex (`sys_futex`, `proc/syscall.c:5197-5430`)

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| F1 | Compare-and-sleep is serialized by the BKL held from trap entry to the next iret (`isr.asm:169-176`, `bkl.c:18-40`); `sleep_on` sets `SLEEPING` before `cli`/`swtch` (`scheduler.c:292-301`). No lost wake vs. another CPU. | Same guarantee via hash-bucket lock (`waitwake.c:701-738`). | none | none | - | P4 |
| F2 | Timeout expiry returns 0 (`syscall.c:5327`), not `-ETIMEDOUT`; glibc retries once and gets `-ETIMEDOUT` on the second call because the deadline is then in the past (`:5256`). | `-ETIMEDOUT` (`waitwake.c:729-730`). | degrades (extra syscall per timed wait; feeds RC4 item 4) | Track the tick-wake reason: set a `timed_out` flag in `scheduler_tick` when it fires `wake_tick`; return `-ETIMEDOUT` when set and no wake happened. | S | P13 |
| F3 | Wake by `signal_send` returns 0 (`:5325-5327` only special-cases SIGKILL). | `-ERESTARTSYS` → `-EINTR` to user unless restarted (`waitwake.c:735-738`). | degrades (spurious 0 return; RC4 item 2) | Return `-EINTR` when `signal_interrupt_pending()`; do not wake for blocked/ignored signals (see S1). | S | P4 |
| F4 | Stale `wake_tick` is applied to a later untimed wait (`signal.c:58-61` leaves `wake_tick`; `scheduler.c:292-301` does not reset it; `syscall.c:5322` clears it only after the futex wait). | Timeout is per call. | degrades (RC4 item 3) | Clear `wake_tick` in `signal_send` and at the top of `sleep_on`. | S | P5 |
| F5 | Futex word read via `copy_from_user` which returns `-EFAULT` when the page is merely not present (`:5206`, `:5225`; `access_ok` walks PTEs, `syscall.c:118-143`). | `get_user` faults the page in; `-EFAULT` only for unmapped memory. | conditional (glibc calls `futex_fatal_error` on `EFAULT`, `nptl/futex-internal.c:118-125`); only reachable for a shared futex on an untouched memfd page | Let `copy_from_user` call `vma_handle_fault` on `-EFAULT`, or touch the page first. | S | P20 |
| F6 | Shared futex whose page is not present in the waker falls back to the private key (`:5332-5336`), so it cannot wake a cross-process waiter registered by physical page. | Key is always the (inode, page) pair. | conditional (only for `PTHREAD_PROCESS_SHARED` in shm; not seen on the launch path) | Resolve through the VMA/shmap registry, not the PTE. | S | P20 |
| F7 | `FUTEX_REQUEUE`/`CMP_REQUEUE` wake `val + val2` waiters instead of moving them (`:5394-5415`); `val3` mismatch does return `-EAGAIN` (`:5404`). | Requeue; return woken (+ requeued for CMP_REQUEUE). | cosmetic for glibc ≥ 2.25 (new condvar does not requeue); wrong for anything that does | Implement real requeue by rewriting `sleep_chan` of the selected waiters. | M | - |
| F8 | `FUTEX_WAKE_OP` wakes both addresses and never performs the atomic op on `uaddr2` (`:5416-5428`). | Performs op, wakes conditionally. | cosmetic today (glibc 2.36 uses it only in the old condvar) but incorrect | Implement the op encoding. | S | - |
| F9 | `val3` bitset ignored for WAIT_BITSET/WAKE_BITSET. | Mask match. | cosmetic (glibc passes MATCH_ANY) | Store/compare bitset. | S | - |
| F10 | PI ops (6, 7, 8, 11, 12) → `-ENOSYS` (`:5429`). | Supported. | cosmetic (Firefox does not use PI mutexes) | none now | - | - |
| F11 | The "safety nets" wake waiters without a `FUTEX_WAKE` (`scheduler.c:142-252`); the aggressive net rate-limits itself by rewriting `g_futex_progress_tick` (`:250`). | No such thing. | **blocks paint** indirectly (RC4) | Remove after S1-S4 land. | S | P4 |
| F12 | `FUTEX_WAIT` with `FUTEX_CLOCK_REALTIME` (op 0 + 256) is accepted as relative (`:5252-5255`). | `-ENOSYS`/`-EINVAL`. | cosmetic | Reject. | S | - |

### 3.2 sleep/wake/scheduler (`proc/scheduler.c`, `proc/process.c`, `proc/signal.c`)

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| S1 | `signal_send` wakes any sleeper for any signal, even blocked or ignored ones (`signal.c:54-61`); `sigsuspend` therefore also returns for blocked signals (`syscall.c:5652-5670`). | `signal_wake_up` only for deliverable signals (`complete_signal`, `kernel/signal.c:945-975`). | degrades (RC4 item 2) | Wake only if the signal is unblocked and not ignored; `SIGKILL`/`SIGSTOP` always. | S | P4 |
| S2 | Fatal `SIG_DFL` signal exits only the receiving thread (`signal.c:124-127`, `paging.c:641-666`); `exit_group` SIGKILLs siblings but does not wait (`syscall.c:3298-3310`); SIGKILL is delivered only at a syscall boundary (`scheduler.c:254-260` never delivers). | Whole group exits (`kernel/signal.c:985-993`, `:1328-1348`, `:3039`); pending SIGKILL is handled on return from interrupt. | **blocks paint** (RC1, RC5) | On a fatal signal: mark the group `exiting`, `signal_send(SIGKILL)` to every thread of the tgid, wake sleepers; make `scheduler_tick`/the IRQ return path deliver pending fatal signals to the interrupted user thread. | M | P1, P17 |
| S3 | Per-thread `sig_handlers`/`sig_flags` copied at clone (`syscall.c:5732-5735`); `CLONE_SIGHAND` ignored. | Shared `sighand_struct` (`kernel/fork.c:1690-1694`). | **blocks paint** indirectly (RC3) | Refcounted shared handler table like `fdtable`. | S | P3 |
| S4 | `SIGCHLD` sent to the creating *thread* on every child *or thread* exit (`scheduler.c:585-588`); `waitpid` matches `parent == current thread` (`syscall.c:942`); `kill(pid)` hits one `ptable` entry (`syscall.c:1777-1810`). | `SIGCHLD` to the parent process for process exit only; any thread may wait; process-directed signals pick a thread. | **blocks paint** indirectly (RC3) | Record the parent as a tgid; no `SIGCHLD` for `CLONE_THREAD` exits; `waitpid` matches by parent tgid; `kill(tgid)` picks a thread not blocking the signal. | M | P3 |
| S5 | `SA_RESTART` "restarts" by re-executing `int 0x80` with `eax == -EINTR` (`signal.c:181-189`), i.e. syscall number 0xFFFFFFFC → `-ENOSYS`. | Restarts with the original syscall number (`orig_ax`). | degrades (any restarted read/poll returns `ENOSYS`) | Save the syscall number in the trapframe (`orig_eax`) and restore it. | S | P9 |
| S6 | `sa_mask` ignored, signal not blocked during its own handler (`syscall.c:3477-3483`, `signal.c:82-290`); `uc_sigmask` zero; `siginfo.si_code = 0`, no `si_addr` (`signal.c:233-237`); `sigaltstack` is a stub (`syscall.c:4526-4529`). | Mask applied, `si_addr`/`si_code` filled, alt stack honoured. | degrades (re-entrant handlers; Breakpad/`WasmTrapHandler` read `si_addr`) | Implement mask save/restore in the frame, fill `si_addr` from CR2, honour `SA_ONSTACK`. | M | P19, P20 |
| S7 | Realtime signals 32-64 accepted but never deliverable (`syscall.c:3457-3464`; `NSIGS=32`, `signal.h:4`); `rt_sigprocmask` copies 4 of 8 bytes (`syscall.c:3499-3501`). | 64 signals. | degrades (glibc `SIGCANCEL`/`SIGSETXID` cannot work: `pthread_cancel`, `setuid` in threaded programs) | Widen masks to 64 bits. | M | - |
| S8 | Wake-to-run: woken thread waits for the next O(n) `ptable` scan; wake-up preemption only when `g_ipc_launch_started` (`scheduler.c:413-424`); idle BSP sleeps in `hlt` until the next 10 ms tick, idle AP spins 200000 iterations (`scheduler.c:88-105`); 20 ms quantum (`scheduler.c:32`). | Wake-up preemption for higher/equal-priority wakes; idle CPUs receive a reschedule IPI. | degrades (RC4 item 5; also makes `pthread_cond_signal`→wait round trips slow) | Always yield at return-to-user when a wake happened; send an IPI (or use `mwait`/`hlt` with a wake IPI) to idle CPUs; keep a runnable queue instead of scanning. | M | P4 |
| S9 | `scheduler_tick` runs the nets and the timed-wake scan on every CPU's tick, i.e. 200 Hz on `-smp 2`, and prints backtraces every 8 sweeps (`scheduler.c:202-225`). | n/a | degrades (BKL time, serial output) | Remove with F11. | S | - |
| S10 | `proc_exit` frees the leader's VMA list even when siblings live (`scheduler.c:552` → `vma_clear`), so a leader that exits first (or a stray `exit(1)` from the leader) makes every sibling fault on its next stack page. | `mm` lives until the last user. | degrades (only if the leader exits alone; Firefox does not) | Move VMAs to a refcounted `mm` object. | M | - |
| S11 | Threads' `parent` is the creating thread; when that thread exits its children (threads and processes) are reparented to init (`scheduler.c:570-576`) and can no longer be waited for. | Children belong to the process. | degrades (RC3) | Same as S4. | M | P3 |
| S12 | `vfork` parent loop `while (parent->vfork_waiting) sleep_on(...)` (`syscall.c:5805-5806`) is woken by every signal and re-sleeps; fine. `vfork_wake_parent` at exec (`syscall.c:1647`) and exit (`scheduler.c:476-481`). | `wait_for_vfork_done`. | none | none | - | P2 |

### 3.3 clone/fork/exec

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| C1 | `CLONE_VM` without `CLONE_THREAD` (glibc `posix_spawn`, Breakpad's `clone(CLONE_VM\|CLONE_FS\|CLONE_FILES)`) shares the parent's tgid (`syscall.c:5751-5757`); `exit_group` from the child kills the parent (`:3301-3307`); `getpid()` returns the parent's pid. | New thread group. | **blocks paint** (RC2) | `tgid = pid` unless `CLONE_THREAD`; keep the shared pgdir and `vfork_parent` routing in `mmap_owner` (`syscall.c:2421-2435`). | S | P2 |
| C2 | Fork-style `clone` (no `CLONE_VM`) ignores `CLONE_CHILD_SETTID`/`CLEARTID`/`PARENT_SETTID` (`syscall.c:5695-5702` → `do_fork`), so glibc's `THREAD_SELF->tid` in the child is the parent thread's tid (`arch-fork.h` passes `&THREAD_SELF->tid`). | Set in child, cleared and futex-woken at exit (`kernel/fork.c:2156-2160`, `:1490-1500`). | degrades (recursive-mutex owner checks and `pthread_kill(pthread_self())` in the child window before exec; Breakpad's forked dumper) | Honour the three tid flags in `do_fork`. | S | P3 |
| C3 | `exec` does not kill sibling threads; the caller becomes its own leader (`syscall.c:1646`) and the old pgdir is only released by refcount (`:1714-1715`). | `de_thread` kills the group (`fs/exec.c:922-934`). | degrades (only matters if a multi-threaded process execs; posix_spawn children are single-threaded) | Kill and reap siblings before switching pgdir. | M | - |
| C4 | argv + envp + auxv + AT_RANDOM + AT_EXECFN are packed into the top **4 KiB page** of the stack with no bound check on `str_off` (`syscall.c:1500-1622`, unsigned decrement); `EXEC_MAXARGS 64`, `EXEC_ARGBUF 8192`, extra args silently dropped (`:1259-1298`). | `MAX_ARG_STRLEN` 128 KiB per string, `ARG_MAX` ≥ 128 KiB total; `E2BIG` on overflow. | **blocks paint** if the content-process environment plus ~25 argv strings exceeds ~3.8 KiB (the launcher inherits the parent's environment and prepends `LD_LIBRARY_PATH`, `GeckoChildProcessHost.cpp:1185-1192`); otherwise degrades | Build the initial stack across as many pages as needed; return `-E2BIG`; raise the arg limits. | S | P14 |
| C5 | Exec closes `FD_CLOEXEC` descriptors (`:1626-1630`) but `SOCK_CLOEXEC`, `MFD_CLOEXEC`, `EPOLL_CLOEXEC` are only partly honoured: `socket()`/`socketpair()`/`accept4()` ignore the flag (`syscall.c:5892`, `:5952-5958`, `:6002-6008`), `memfd_create` ignores `MFD_CLOEXEC` (`:4884-4902`). | Honoured. | degrades (every socket and memfd leaks into every exec'd child unless `CloseSuperfluousFds` closes it; extra references defeat EOF detection, RC5) | Honour the flags. | S | P15, P16 |
| C6 | `dup2` clears cloexec (`:1919`), `dup3(O_CLOEXEC)` sets it (`:5608-5609`): correct. `F_DUPFD_CLOEXEC` correct (`:2134-2146`). | same | none | - | - | - |
| C7 | AT_HWCAP present, `AT_PLATFORM`, `AT_SYSINFO*`, `AT_MINSIGSTKSZ`, `AT_FLAGS` absent (`syscall.c:1560-1588`). glibc falls back to `int $0x80` without `AT_SYSINFO` (`sysdeps/unix/sysv/linux/i386/sysdep.h:114-122`). | Full aux vector, vDSO. | cosmetic (perf: no vDSO `clock_gettime`) | Provide a vDSO page later. | L | - |
| C8 | PIE at `0x10000000`, ld.so at `0x40000000`, mmap cursor above ld.so, monotonic (`syscall.c:1246-1247`, `:1426`, `:2931-2951`). Hints below the cursor are refused; hints above it are honoured if free. | Top-down mmap with ASLR; hints honoured if free. | degrades (see M3) | - | - | - |
| C9 | `clone3` maps `stack + stack_size`, `tls`, `child_tid`, `parent_tid` (`syscall.c:5832-5858`); `CLONE_SETTLS` reads `user_desc.base_addr` only (`:5763-5767`); single GDT TLS slot reloaded per switch (`gdt.c:73-75`, `scheduler.c:69`). glibc thread creation flags (`nptl/pthread_create.c:277-280`) are all handled or harmless. | same | none | - | - | - |
| C10 | `set_robust_list` stores the head (`syscall.c:6476-6478`); exit walks the list and marks `FUTEX_OWNER_DIED` scoped to the tgid (`scheduler.c:508-547`). | same | none | - | - | - |
| C11 | Every `fork`/`clone`/`exec`/`waitpid` prints to the serial console (`syscall.c:791-806`, `:963-964`, `:1717-1743`); `[argv]`, `[fdtab]`, `[fk]` dumps. | n/a | degrades (serial at 115200 baud stalls the BKL holder) | Remove (section 6). | S | - |

### 3.4 AF_UNIX (`proc/usocket.c`, `proc/syscall.c:5888-6284`)

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| U1 | All sockets are byte streams; `SOCK_SEQPACKET`/`SOCK_DGRAM` types are stored but never used (`usocket.c:94`, `:99-100`). | Record boundaries for SEQPACKET/DGRAM. | degrades (Firefox uses `SOCK_STREAM`, `ipc_channel_posix.cc:1276`; D-Bus/others would break) | Implement record queues. | M | - |
| U2 | `SCM_RIGHTS` batches are tagged at the *end* of the `sendmsg` data (`usocket.c:288-303`) and delivered when the reader reaches that position (`:308-321`); reads are capped at the boundary (`:208-213`). | fds are attached to the skb of the sent bytes and delivered with the first `recvmsg` that consumes any of them (`net/unix/af_unix.c:2643-2669`). | degrades (works with Chromium's parser because fds may arrive on any read of the message, `ipc_channel_posix.cc:370-403`, `:470-497`; breaks readers that expect fds with the first byte) | Tag at `total_in` *before* writing the data. | S | P10 |
| U3 | A `recvmsg` that finds fds ready but no data returns 0 with fds installed (`syscall.c:6071-6148`, `total == 0`, `eagain` not set) — Chromium treats 0 as EOF and closes the channel (`ipc_channel_posix.cc:318-321`). | Cannot happen. | degrades (corner case after a `read()`/`recv()` consumed the data) | Return `-EAGAIN`/block until data unless the caller passed no control buffer. | S | P10 |
| U4 | `MSG_PEEK`, `MSG_DONTWAIT`, `MSG_WAITALL`, `MSG_NOSIGNAL`, `MSG_TRUNC` ignored (`syscall.c:6027-6148`); `SIGPIPE` always raised on `EPIPE` (`usocket.c:235`, `:243`). `MSG_CMSG_CLOEXEC` honoured (`syscall.c:6117`); `MSG_CTRUNC` never set; excess fds are installed but unreported (`:6131`). | All honoured. | degrades (Chromium sets `O_NONBLOCK` so `MSG_DONTWAIT` is moot; NSPR ignores `SIGPIPE`, `nsprpub/pr/src/md/unix/unix.c:2820`) | Implement the flags; set `MSG_CTRUNC` and close undeliverable fds. | S | P10 |
| U5 | `poll` never reports `POLLHUP`/`POLLERR` for sockets (`syscall.c:4073-4076`), `epoll` never reports `EPOLLHUP`/`EPOLLERR`/`EPOLLRDHUP` (`:4294-4296`); hangup is visible only as readable-then-`read()==0` (`usocket.c:330-334`). | `POLLHUP`/`EPOLLHUP` on peer close. | degrades (libevent maps HUP to READ, so Chromium copes) | Report `POLLHUP` when `writer_closed`, `POLLERR` when `reader_closed`. | S | P10 |
| U6 | `shutdown` is a no-op (`syscall.c:6150`); `getsockname`/`getpeername` return 0 without writing (`:6012-6014`); `SO_PEERCRED`/`SO_ERROR` read as 4 zero bytes (`:6152-6173`); `setsockopt` no-op. | Real semantics. | degrades | Implement `shutdown(SHUT_WR)` as `writer_closed`; fill `sockaddr_un`/`ucred`. | S | - |
| U7 | 64 KiB per-direction ring (`usocket.c:10`); `SO_SNDBUF` reports 65536 (`syscall.c:6167`); writes above the ring block or return `-EAGAIN` (`usocket.c:232-241`). Chromium's `kMaximumMessageSize` is 256 MiB (`ipc_channel.h:85`) and it writes in `EAGAIN`-driven pieces (`ipc_channel_posix.cc:684-700`). | 208 KiB default. | none (correct), perf only | - | - | - |
| U8 | Wakes: readers/writers sleep on the ring (`usocket.c:204`, `:240`) and are woken by `wake_up(b)`; pollers are woken via the global `io_activity` channel (`:222`, `:256`, `:376`), so every socket write wakes every poller in the system. | Per-socket wait queues. | degrades (thundering herd across ~60 Firefox threads) | Per-object waitqueues for poll/epoll. | M | - |
| U9 | `bind` on a filesystem path creates no inode (`usocket.c:118-129`); abstract names accepted (`syscall.c:5987-5989`). | Socket inode created. | cosmetic (libxcb tries the abstract name first) | - | S | - |

### 3.5 epoll / eventfd / pipe / poll / select

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| E1 | `poll`/`epoll_wait` are interrupted only by `SIGKILL` (`syscall.c:4143-4146`, `:4409-4410`); a caught signal stays pending until the wait ends, and the handler runs then. | `-EINTR` immediately. | degrades (GLib's `SIGCHLD` self-pipe pattern: the handler that would make the poll return cannot run until the poll returns; timeouts save it) | Return `-EINTR` when `signal_interrupt_pending()`. | S | P9 |
| E2 | `select` ignores the timeout except for zero; it polls every 50 ms for up to 600 attempts (30 s) then returns 0 (`syscall.c:3852-3883`); `exceptfds` ignored; `pselect6` mask ignored (`:6404-6405`). | Exact timeout. | degrades | Compute the deadline in ticks like `poll`. | S | P8 |
| E3 | `epoll` is level-triggered only; `EPOLLET`, `EPOLLONESHOT` ignored; a closed fd stays in the set and a reused fd number is silently watched (`syscall.c:4225-4267`); `EPOLL_MAX_ITEMS 256` (`:4186`). | Per-file registration removed on last close. | degrades (Chromium/libevent `EPOLL_CTL_DEL` before close, so mostly safe) | Remove items in `fd_release`; implement ET/ONESHOT. | M | - |
| E4 | `epoll_wait`/`poll`/`select` sleep on `io_activity` with a 500 ms cap (`:3898-3901`, `:4133-4138`, `:4397-4402`), re-scanning all items on every wake. | Wait-queue callbacks. | degrades (CPU churn; latency up to a tick) | Per-object waitqueues. | M | - |
| E5 | The disabled epoll "self-heal" that reported empty pipes as readable (`syscall.c:4322-4396`, `FF_EPOLL_SELF_HEAL 0`) and the `g_ff_io_nudge` counters bumped on every Firefox pipe/socket write (`pipe.c:93-107`, `usocket.c:258-281`). | n/a | none while disabled; must not be re-enabled | Delete. | S | - |
| E6 | Pipe buffer 4 KiB (`pipe.h:4`); `PIPE_BUF` atomicity not implemented; `pipe_read`/`pipe_write` return `-EINTR` correctly (`pipe.c:37`, `:79`). | 64 KiB, `PIPE_BUF` 4096 atomic. | degrades (glxtest output and libevent wake bytes fit) | Raise to 64 KiB. | S | - |
| E7 | `eventfd` correct including `EFD_SEMAPHORE`, blocking, `-EINTR` (`syscall.c:461-536`). | same | none | - | - | - |
| E8 | `timerfd_*` (322, 325, 326, 410, 411), `signalfd`/`signalfd4` (321, 327), `inotify_*` → `-ENOSYS` (`syscall.c:6536`). GLib's `g_child_watch` uses `waitpid` polling or `pidfd`, its main loop uses `poll`; libevent uses `epoll` + a `socketpair` for signals; GLib's inotify backend falls back cleanly on `ENOSYS`. | Supported. | cosmetic for first paint | Implement `timerfd` (used by GLib ≥ 2.72 `g_source` monotonic timers only when built with it; verify with P12) | M | - |
| E9 | `poll` with `nfds > 1024` → `-EINVAL` (`syscall.c:4021-4027`); `MAX_FD 128` (`config.h:12`). | `RLIMIT_NOFILE` (1024 soft / 4096+ hard typical); Firefox opens hundreds of fds with several content processes. | **degrades now, blocks paint later** (`EMFILE` at 128 open files: 4 memfds + ~6 fds per child, omni.ja, fonts, sockets) | Raise `MAX_FD` to 1024 (the fd table is heap-allocated, `syscall.c:575-581`) and report it in `RLIMIT_NOFILE`. | S | - |


### 3.6 Memory (`sys_mmap2`, `sys_munmap`, `sys_mprotect`, `sys_madvise`, `mm/pmm.c`, `arch/i686/mm/paging.c`)

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| M1 | Anonymous mappings smaller than 4 MiB are populated eagerly and mapped `PRESENT\|WRITABLE\|USER` regardless of `prot` (`syscall.c:3013-3014`, `:3075-3088`, flags at `:3086`); only ≥ 4 MiB regions become demand VMAs that honour `PROT_NONE` (`:2692-2696`). `mprotect` can only toggle the write bit on present pages (`:3226-3229`); `PROT_NONE`/`PROT_READ` on a present page leaves it readable and executable. | `prot` honoured for every page; `PROT_NONE` faults. | degrades: guard pages of Firefox's 256 KiB-class thread stacks (`nsIThreadManager::DEFAULT_STACK_SIZE` = 256 KiB in optimized builds, `xpcom/threads/nsIThreadManager.idl:53`, used by `xpcom/threads/nsThreadPool.cpp:63`; glibc maps the whole stack `PROT_NONE` then `mprotect`s the usable part, `nptl/allocatestack.c:160-177`) are not guards; mozjemalloc's decommit (`mmap(PROT_NONE, MAP_FIXED)`, `memory/build/mozjemalloc.cpp:1650-1651`) keeps memory committed; JIT `DecommitPages` likewise (`js/src/jit/ProcessExecutableMemory.cpp:546`). W^X toggling (`mprotect` RW↔RX) does work because it only needs the W bit. | Honour `prot` on the eager path (`PROT_NONE` → no PTE + `prot 0` VMA; no `PROT_WRITE` → read-only PTE); make `mprotect(PROT_NONE)` unmap present pages into a `prot 0` VMA. | M | P6 |
| M2 | `mprotect` on unmapped ranges returns 0 (`:3221-3231`), `PROT_EXEC` cannot be enforced (no NX without PAE). | `-ENOMEM` for gaps. | cosmetic | - | - | - |
| M3 | Virtual addresses are never reused: `mmap_next` only grows (`syscall.c:2926-2951`); `munmap` does not lower it (`:3170-3201`); hints below the cursor are refused (`:2913-2929`). ~2 GiB of space between the interpreter and `USER_STACK_BASE` (`config.h:16-18`). | Free-space search, reuse. | degrades → eventual `-ENOMEM` (mozjemalloc `MOZ_CRASH`es on `ENOMEM` from `mmap`, `mozjemalloc.cpp:1652-1663`). Each glibc thread stack costs 8 MiB of address space plus guard; glibc caches only 40 MiB of stacks. | Keep a free list of unmapped ranges (or lower the cursor when the top range is unmapped); allow hints anywhere free. | M | P11 |
| M4 | `MAP_FIXED` over an existing eager mapping frees the old frame and allocates a new zeroed one (`:3076-3088`), after a `tlb_shootdown` batch (`:2853-2875`). `MAP_FIXED_NOREPLACE` (0x100000) treated as a hint. `MAP_SHARED\|MAP_ANONYMOUS` becomes private (`:2978`, requires an fd). | `MAP_FIXED` replaces; `NOREPLACE` → `-EEXIST`; shared anon shares across fork. | degrades (mozjemalloc `pages_commit`/`pages_decommit` rely on `MAP_FIXED` replace → correct) | Implement `NOREPLACE`; back shared-anon with the shmap registry. | S | - |
| M5 | `MAP_SHARED` of a file: frames come from a per-node registry (`:2741-2851`, `SHMAP_MAX 256`), eager, shared with `PAGE_SHARED`; a `MAP_PRIVATE` mapping of the same node copies from the registry (`:3113-3128`). `write(2)` to the file after mapping is not reflected in the frames (`tmpfs_write` writes the tmpfs buffer, `fs/tmpfs.c:77-103`). `memfd` is a named `/tmp/.memfd-N` tmpfs file (`:4884-4902`), never unlinked; `ftruncate`/`fallocate` allocate the full size in the **kernel heap** (`tmpfs.c:105-138`) in addition to the shared frames. | Page cache; one copy. | degrades → **can halt the machine**: the kernel heap is 256 MiB (`config.h:13-14`) and exhaustion is `for(;;) hlt` (`mm/heap.c:62-65`). A 20 MiB shared image surface costs 20 MiB of kernel heap plus 20 MiB of frames. | Back tmpfs files ≥ 1 page by the shmap frames only (no heap copy); return `-ENOMEM` instead of halting. | M | P16 |
| M6 | `madvise(MADV_DONTNEED)` zeroes present pages in place but skips read-only pages, i.e. every COW page after a fork (`syscall.c:4972-4983`, the `PAGE_WRITABLE` test at `:4979`); `MADV_FREE` no-op. | Zaps the range; next touch gives a zero page (`mm/madvise.c:860-871`). | degrades (Firefox 115's mozjemalloc does not use `madvise` for purging, `mozjemalloc.cpp:1628-1651`, `:2128-2131`; SpiderMonkey's `MarkPagesUnusedSoft` uses `MADV_DONTNEED` but treats the content as undefined afterwards, `js/src/gc/Memory.cpp:802-850`; glibc's `malloc` in `glxtest` does rely on zero after `shrink_heap`, `malloc/arena.c:631`) | Unmap the pages (decref) instead of zeroing; the demand pager gives zero pages back. | S | P6 |
| M7 | `mremap` → `-ENOMEM` (`syscall.c:6498`), `msync`, `mlock*`, `mincore` (`:4988-4991` → `-ENOMEM`) unimplemented. | Supported. | cosmetic for Firefox (mozjemalloc does not use `mremap`; glibc `realloc` falls back) | Implement `mremap` for the JS `ArrayBuffer` growth path later. | M | - |
| M8 | COW break sends a TLB shootdown IPI and spins for acknowledgement on every fault (`paging.c:471`, `smp.c:76-126`, up to 2M `pause` iterations). After `fork()` every writable page of a ~200 MiB parent is COW (`syscall.c:726-736`). | Batched TLB flush. | degrades (long BKL holds after each content-process fork; the launch thread and main thread contend) | Batch shootdowns per fault burst; skip the IPI when the pgdir is not shared or the other CPU runs a different pgdir. | M | - |
| M9 | `pgdir_shares` has 64 slots; a 65th multi-threaded process is not recorded and its pgdir is freed by the first thread exit (`paging.c:672-707`). | n/a | latent memory corruption once ≥ 64 threaded processes exist | Put the share count in a per-pgdir structure. | S | - |
| M10 | Physical frame reuse is quarantined (512 frames, `pmm.c:208-240`), per-frame refcounts are 16-bit. `>512 MiB`: direct map capped at 256 MiB, high frames via temp maps (`paging.c:172-208`). | n/a | none found | - | - | P18 |
| M11 | `/proc/self/maps` lists the image, heap, demand VMAs and a 256 KiB `[stack]` line in an 8 KiB static buffer (`fs/procfs.c:292-331`); file-backed VMAs show no path; `/proc/self/status` reports `VmSize: 4096 kB` (`procfs.c:145`). glibc `pthread_getattr_np` derives the main stack size from `RLIMIT_STACK` and the gap below `[stack]` (`nptl/pthread_getattr_np.c:80-138`), so 8 MiB is reported, matching the growth window. | Complete maps. | cosmetic (Breakpad's dumper and `nsMemoryReporterManager` read maps) | Emit all mappings with paths; dynamic buffer. | S | - |
| M12 | `sysinfo` hardcodes 128 MiB total / 64 MiB free (`syscall.c:5115-5126`); `/proc/meminfo` is real (`procfs.c:659-681`). glibc `sysconf(_SC_PHYS_PAGES)` uses `sysinfo`. | Real values. | degrades (Firefox sizes caches and `dom.ipc.processCount` heuristics from physical memory) | Fill from `pmm_total_frames`/`pmm_free_frames`. | S | - |
| M13 | `brk` maps eagerly and zeroes through the user address (`syscall.c:1192-1209`); no `RLIMIT_DATA`. | Lazy. | none | - | - | - |
| M14 | Stack growth window is `[USER_STACK_TOP - 8 MiB, USER_STACK_BASE)` (`paging.c:508-512`); a mapping placed there by a hint would be silently overlaid by stack pages. Hints are capped at `USER_STACK_BASE` (`syscall.c:2911-2914`) so only a cursor that has climbed to within 8 MiB of the stack collides. | `stack_guard_gap`. | latent | Exclude the window from the mmap cursor. | S | - |

### 3.7 Time

| # | MaeroOS behaviour | Linux behaviour | Impact | Fix sketch | Effort | Probe |
|---|---|---|---|---|---|---|
| T1 | All clocks derive from the 100 Hz PIT tick (`syscall.c:3340-3375`, `:2192-2203`); resolution 10 ms, `clock_getres` reports 10 ms (`:4824-4839`). | ns resolution (TSC/HPET). | degrades (mozilla `TimeStamp::Now` granularity 10 ms; every `poll`/futex timeout rounds up to a tick, `:4042`, `:5257`) | Use the TSC (calibrated against the PIT) or the HPET for `clock_gettime`; keep the tick for scheduling. | M | P12 |
| T2 | `CLOCK_MONOTONIC` (1) and `MONOTONIC_RAW` (4) are monotonic; every other id, including `CLOCK_PROCESS_CPUTIME_ID` (2), `CLOCK_THREAD_CPUTIME_ID` (3), `CLOCK_REALTIME_COARSE` (5), `CLOCK_MONOTONIC_COARSE` (6) and `CLOCK_BOOTTIME` (7) returns wall-clock time (`:3346-3352`, `:3368-3369`). | Distinct clocks; 6 and 7 are monotonic. | degrades: libevent selects `CLOCK_MONOTONIC_COARSE` unconditionally when `clock_gettime` on it succeeds (`ipc/chromium/src/third_party/libevent/evutil_time.c:326-331`), so every timer of Firefox's IPC I/O threads runs on what MaeroOS returns for id 6, i.e. the wall clock. Benign today only because that clock is also PIT-derived and never steps; any RTC resync or `settimeofday` would break libevent's timeouts. | Map 6 and 7 to monotonic, 2 and 3 to `utime_ticks`, reject unknown ids with `-EINVAL`. | S | P12 |
| T3 | `FUTEX_WAIT_BITSET` absolute deadlines are converted with the same tick clock for both `FUTEX_CLOCK_REALTIME` and monotonic (`:5244-5251`); already-expired deadlines return `-ETIMEDOUT` (`:5256`). | same | none | - | - | P13 |
| T4 | `clock_nanosleep` ignores `clockid` and `TIMER_ABSTIME` (`:4723-4728`): an absolute request sleeps for "now" seconds; `nanosleep` is not interruptible (returns 0 early on any wake, `rem` always zero, `:2276-2302`). | `TIMER_ABSTIME` honoured; `-EINTR` with remaining time. | degrades (`clock_nanosleep(TIMER_ABSTIME)` is used by C++ `std::this_thread::sleep_until` and Rust's `thread::sleep_until`; not seen on the launch path) | Implement both flags. | S | P12 |
| T5 | `gettimeofday`/`CLOCK_REALTIME` = RTC epoch at boot + ticks (`:2196-2198`); `times()` returns user ticks only; `getrusage` (77), `setitimer`/`getitimer` (104/105), `timer_*` (259-263) missing. | Supported. | cosmetic (Firefox's watchdogs use `TimeStamp`/`poll` timeouts: glxtest 4 s via `poll`, `widget/gtk/GfxInfo.cpp:41`, `:130-135`; `WaitUntilConnected` uses `pthread_cond_timedwait` with `TimeStamp` arithmetic, `GeckoChildProcessHost.cpp:835-869`) | Add `getrusage`, `setitimer` (GLib/NSPR profiling), `timer_create` (glibc `timer_*` via `SIGEV_THREAD` helper thread needs `rt_sigtimedwait`). | M | - |
| T6 | Timed waits cannot expire early relative to the same tick clock, and cannot fire *late* by more than one tick plus scheduling latency (RC4 item 5). The launcher's watchdog (`userspace/ff/ff.c:168`) polls with `usleep`; fine. | - | none | - | - | - |


---

## 4. Missing syscalls (Linux i386 table vs. `syscall_dispatch`)

Method: every `case N:` label in `syscall_dispatch` (`proc/syscall.c:6359-6553`) was extracted and compared with `arch/x86/entry/syscalls/syscall_32.tbl` (Linux master, 462 entries). Result: 180 numbers implemented (4 of them custom MaeroOS numbers 500/501/502/505), 286 table entries absent. "Used by" is from reading glibc 2.36 and the Firefox sources; the shipped binaries were not available for `objdump`.

Wiring mistakes found while diffing: 159 (`sched_get_priority_max`) is wired to `sys_sched_yield` (`proc/syscall.c:6426`); 253 (`lookup_dcookie`) is wired to `sys_clock_nanosleep` (`:6459`) while the real `clock_nanosleep` (267) is absent; 138 (`setfsuid`) is wired to `sys_seteuid` (`:6390`); 9 (`link`) returns `-EPERM` (`:6473`); 176 (`rt_sigpending`) returns 0 without writing the set (`:6434`).

| # | Name | Impact on first paint | Who calls it / notes |
|---|---|---|---|
| 194 | `ftruncate64` | **degrades** | glibc `ftruncate()` in a `_FILE_OFFSET_BITS=64` build is `ftruncate64` (`sysdeps/unix/sysv/linux/ftruncate64.c`); sqlite journal truncation, `nsLocalFile::SetFileSize`, Breakpad. Route to `sys_ftruncate` with the 64-bit length pair. |
| 40 | `rmdir` | degrades | glibc `rmdir` → 40; Firefox profile cleanup and GIO use it. Map to `unlinkat(AT_REMOVEDIR)` logic. |
| 77 | `getrusage` | degrades | glibc `getrusage` used by NSPR `PR_GetProcessCPU`, Firefox telemetry and `TimeStamp::ProcessCreation` fallbacks; return zeros/`utime_ticks`. |
| 117 | `ipc` | degrades | glibc 2.36 on i386 routes `shmget`/`shmat`/`semop` through `ipc(117)` unless built for ≥ 5.1 kernels (Debian: 3.2 baseline). libxcb's MIT-SHM (`XShm`) and GTK's `gdk_x11` shm images need it if maeroX advertises MIT-SHM; Firefox's `nsShmImage` checks `XShmQueryExtension` first. Return `-ENOSYS` deliberately (already the default) so the X client path stays on `PutImage`. |
| 160 | `sched_get_priority_min` | degrades | see 159. |
| 177 | `rt_sigtimedwait` | degrades | glibc `sigtimedwait`/`sigwait`; GLib's `g_unix_signal` does not need it; glibc's `timer_create(SIGEV_THREAD)` helper thread does. |
| 190 | `vfork` | degrades | glibc `vfork()` (NSPR does not use it; some GLib/GTK helpers do). Implement as `clone(CLONE_VM|CLONE_VFORK)` with its own tgid (C1). |
| 193 | `truncate64` | degrades | same as 194 for `truncate()`. |
| 205 | `getgroups32` | degrades | glibc `getgroups`; `nsLocalFile`/GIO permission checks call it. Return 0 groups. |
| 320 | `utimensat` | degrades | glibc `utimensat`/`futimens`/`utimes` → 412 then 320; sqlite and `nsLocalFile::SetLastModifiedTime` call it; return 0. |
| 412 | `utimensat_time64` | degrades | see 320. |
| 421 | `rt_sigtimedwait_time64` | degrades | see 177. |
| 0 | `restart_syscall` | cosmetic | only used by the kernel itself for restarted `nanosleep`/`poll`. |
| 8 | `creat` | cosmetic | glibc `creat` → `open(O_CREAT|O_WRONLY|O_TRUNC)`; 8 is legacy. |
| 13 | `time` | cosmetic | glibc `time()` uses `clock_gettime`. |
| 27 | `alarm` | cosmetic | glibc `alarm` → `setitimer`. |
| 29 | `pause` | cosmetic | glibc uses `ppoll`/`rt_sigsuspend`. |
| 36 | `sync` | cosmetic | return 0. |
| 74 | `sethostname` | cosmetic | - |
| 79 | `settimeofday` | cosmetic | - |
| 80 | `getgroups` | cosmetic | glibc uses 205. |
| 81 | `setgroups` | cosmetic | glibc uses 206. |
| 99 | `statfs` | cosmetic | glibc uses 268. |
| 100 | `fstatfs` | cosmetic | glibc uses 269. |
| 104 | `setitimer` | cosmetic | profiler only. |
| 105 | `getitimer` | cosmetic | profiler only. |
| 133 | `fchdir` | cosmetic | GLib `g_spawn` with a working directory uses it; return 0 after updating `cwd` from the fd path. |
| 144 | `msync` | cosmetic | `MS_SYNC` on memfd mappings; return 0. |
| 147 | `getsid` | cosmetic | return `sid`. |
| 150 | `mlock` | cosmetic | return 0. |
| 151 | `munlock` | cosmetic | return 0. |
| 152 | `mlockall` | cosmetic | return `-EPERM`. |
| 153 | `munlockall` | cosmetic | return 0. |
| 154 | `sched_setparam` | cosmetic | `pthread_setschedparam` fallback; return 0 for SCHED_OTHER. |
| 155 | `sched_getparam` | cosmetic | same. |
| 156 | `sched_setscheduler` | cosmetic | Firefox's `hal::SetProcessPriority`/thread priorities call `setpriority` (accepted) and `sched_setscheduler`; return 0 for SCHED_OTHER. |
| 157 | `sched_getscheduler` | cosmetic | same. |
| 164 | `setresuid` | cosmetic | glibc uses 208. |
| 165 | `getresuid` | cosmetic | glibc uses 209. |
| 170 | `setresgid` | cosmetic | glibc uses 210 (missing too); Firefox does not drop privileges. |
| 171 | `getresgid` | cosmetic | glibc uses 211. |
| 173 | `rt_sigreturn` | cosmetic | glibc's `__restore_rt` is never reached because the kernel installs its own trampoline (`proc/signal.c:268-280`) and ignores `sa_restorer`; keep it that way or implement 173 when honouring `sa_restorer`. |
| 187 | `sendfile` | cosmetic | glibc uses 239. |
| 198 | `lchown32` | cosmetic | glibc `lchown` uses 198, not 16 (`proc/syscall.c:6491`). |
| 203 | `setreuid32` | cosmetic | - |
| 204 | `setregid32` | cosmetic | - |
| 206 | `setgroups32` | cosmetic | - |
| 210 | `setresgid32` | cosmetic | - |
| 212 | `chown32` | cosmetic | glibc `chown` uses 212, not 182 (`proc/syscall.c:6493`). |
| 226 | `setxattr` | cosmetic | fontconfig/GIO probe `getxattr`; `ENOSYS` → treated as unsupported. |
| 229 | `getxattr` | cosmetic | see 226. |
| 230 | `lgetxattr` | cosmetic | see 226. |
| 231 | `fgetxattr` | cosmetic | see 226. |
| 232 | `listxattr` | cosmetic | see 226. |
| 238 | `tkill` | cosmetic | old glibc; alias to `tgkill`. |
| 239 | `sendfile64` | cosmetic | GIO `splice` fallback path. |
| 241 | `sched_setaffinity` | cosmetic | thread pools may pin; return 0. |
| 244 | `get_thread_area` | cosmetic | glibc does not need it. |
| 259 | `timer_create` | cosmetic | glibc `timer_create` with `SIGEV_THREAD` also needs 177. |
| 260 | `timer_settime` | cosmetic | see 259. |
| 261 | `timer_gettime` | cosmetic | see 259. |
| 262 | `timer_getoverrun` | cosmetic | see 259. |
| 263 | `timer_delete` | cosmetic | see 259. |
| 264 | `clock_settime` | cosmetic | - |
| 267 | `clock_nanosleep` | cosmetic | glibc uses 407 first and falls back to 267 only on `ENOSYS`; 407 exists. Note that 253 (`lookup_dcookie`) is wired to `sys_clock_nanosleep` by mistake (`proc/syscall.c:6459`). |
| 284 | `waitid` | cosmetic | glibc `waitid`; GLib's `g_child_watch` uses `waitpid`. |
| 294 | `migrate_pages` | cosmetic | - |
| 297 | `mknodat` | degrades | glibc 2.36 implements `mknod()` and `mkfifo()` as `mknodat(AT_FDCWD, ...)` (`io/mknod.c` → `sysdeps/unix/sysv/linux/mknodat.c:33`), so syscall 14 is never used by glibc and `mkfifo` fails with `ENOSYS`. Route 297 to `sys_mknod` with `dirfd` resolution. |
| 298 | `fchownat` | cosmetic | - |
| 303 | `linkat` | cosmetic | glibc 2.36 implements `link()` via `linkat` (`io/link.c`), so the `-EPERM` stub on 9 (`proc/syscall.c:6473`) is dead code and `link()` currently fails with `ENOSYS`; Firefox tolerates either. |
| 304 | `symlinkat` | cosmetic | glibc keeps 83 on i386. |
| 310 | `unshare` | cosmetic | sandbox probe child expects failure; `ENOSYS` is fine. |
| 313 | `splice` | cosmetic | GIO tries `splice` for pipe copies and falls back. |
| 318 | `getcpu` | cosmetic | glibc `sched_getcpu`; `-ENOSYS` → returns -1. |
| 321 | `signalfd` | cosmetic | not used by Firefox/GLib on this path. |
| 322 | `timerfd_create` | cosmetic | GLib ≥ 2.72 does not use it by default; Rust `tokio` would (not in Firefox 115 i686). |
| 325 | `timerfd_settime` | cosmetic | see 322. |
| 326 | `timerfd_gettime` | cosmetic | see 322. |
| 327 | `signalfd4` | cosmetic | see 321. |
| 333 | `preadv` | cosmetic | glibc `preadv` → 333/378; sqlite does not use it. |
| 334 | `pwritev` | cosmetic | see 333. |
| 353 | `renameat2` | cosmetic | glibc `rename` uses 38/302. |
| 354 | `seccomp` | cosmetic | `SandboxInfo` expects `EFAULT`/`EINVAL`/`ENOSYS` (`security/sandbox/linux/SandboxInfo.cpp:80-88`); `ENOSYS` is fine. Note `prctl(PR_SET_SECCOMP, ...)` returning 0 (`proc/syscall.c:4500-4523`) contradicts it; make `prctl` return `-EINVAL` for unknown options. |
| 358 | `execveat` | cosmetic | glibc `fexecve` falls back to `/proc/self/fd`. |
| 375 | `membarrier` | cosmetic | not used on x86 by SpiderMonkey. |
| 376 | `mlock2` | cosmetic | - |
| 377 | `copy_file_range` | cosmetic | GIO file copy falls back to read/write. |
| 384 | `arch_prctl` | cosmetic | x86-64 only in practice. |
| 395 | `shmget` | cosmetic | see 117. |
| 396 | `shmctl` | cosmetic | see 117. |
| 397 | `shmat` | cosmetic | see 117. |
| 398 | `shmdt` | cosmetic | see 117. |
| 404 | `clock_settime64` | cosmetic | - |
| 408 | `timer_gettime64` | cosmetic | see 259. |
| 409 | `timer_settime64` | cosmetic | see 259. |
| 410 | `timerfd_gettime64` | cosmetic | see 322. |
| 411 | `timerfd_settime64` | cosmetic | see 322. |
| 413 | `pselect6_time64` | cosmetic | glibc falls back to 308 on `ENOSYS`. |
| 424 | `pidfd_send_signal` | cosmetic | GLib ≥ 2.66 tries `pidfd_open` for child watches and falls back to `waitpid`. |
| 434 | `pidfd_open` | cosmetic | see 424. |
| 436 | `close_range` | cosmetic | glibc `closefrom` falls back to `/proc/self/fd`. |
| 437 | `openat2` | cosmetic | - |
| 439 | `faccessat2` | cosmetic | glibc falls back to 307 on `ENOSYS`. |
| 441 | `epoll_pwait2` | cosmetic | - |
| 449 | `futex_waitv` | cosmetic | - |
| 454 | `futex_wake` | cosmetic | see 455. |
| 455 | `futex_wait` | cosmetic | new futex2 family, unused by glibc 2.36/2.41. |
| 456 | `futex_requeue` | cosmetic | see 455. |

Remaining absent numbers, not used by the Firefox/GTK/glibc launch path as far as the sources show (kept for completeness): 17 break, 18 oldstat, 21 mount, 22 umount, 25 stime, 26 ptrace, 28 oldfstat, 30 utime, 31 stty, 32 gtty, 34 nice, 35 ftime, 44 prof, 51 acct, 52 umount2, 53 lock, 56 mpx, 58 ulimit, 59 oldolduname, 61 chroot, 62 ustat, 67 sigaction, 68 sgetmask, 69 ssetmask, 70 setreuid, 71 setregid, 72 sigsuspend, 73 sigpending, 84 oldlstat, 86 uselib, 87 swapon, 89 readdir, 95 fchown, 98 profil, 101 ioperm, 103 syslog, 109 olduname, 110 iopl, 111 vhangup, 112 idle, 113 vm86old, 115 swapoff, 121 setdomainname, 123 modify_ldt, 124 adjtimex, 126 sigprocmask, 127 create_module, 128 init_module, 129 delete_module, 130 get_kernel_syms, 131 quotactl, 134 bdflush, 135 sysfs, 136 personality, 137 afs_syscall, 139 setfsgid, 149 _sysctl, 161 sched_rr_get_interval, 166 vm86, 167 query_module, 169 nfsservctl, 178 rt_sigqueueinfo, 184 capget, 185 capset, 188 getpmsg, 189 putpmsg, 215 setfsuid32, 216 setfsgid32, 217 pivot_root, 227 lsetxattr, 228 fsetxattr, 233 llistxattr, 234 flistxattr, 235 removexattr, 236 lremovexattr, 237 fremovexattr, 245 io_setup, 246 io_destroy, 247 io_getevents, 248 io_submit, 249 io_cancel, 257 remap_file_pages, 271 utimes, 273 vserver, 274 mbind, 275 get_mempolicy, 276 set_mempolicy, 277 mq_open, 278 mq_unlink, 279 mq_timedsend, 280 mq_timedreceive, 281 mq_notify, 282 mq_getsetattr, 283 kexec_load, 286 add_key, 287 request_key, 288 keyctl, 289 ioprio_set, 290 ioprio_get, 299 futimesat, 314 sync_file_range, 315 tee, 316 vmsplice, 317 move_pages, 335 rt_tgsigqueueinfo, 336 perf_event_open, 337 recvmmsg, 338 fanotify_init, 339 fanotify_mark, 341 name_to_handle_at, 342 open_by_handle_at, 343 clock_adjtime, 344 syncfs, 345 sendmmsg, 346 setns, 347 process_vm_readv, 348 process_vm_writev, 349 kcmp, 350 finit_module, 357 bpf, 374 userfaultfd, 378 preadv2, 379 pwritev2, 380 pkey_mprotect, 381 pkey_alloc, 382 pkey_free, 385 io_pgetevents, 393 semget, 394 semctl, 399 msgget, 400 msgsnd, 401 msgrcv, 402 msgctl, 405 clock_adjtime64, 416 io_pgetevents_time64, 417 recvmmsg_time64, 418 mq_timedsend_time64, 419 mq_timedreceive_time64, 420 semtimedop_time64, 423 sched_rr_get_interval_time64, 425 io_uring_setup, 426 io_uring_enter, 427 io_uring_register, 428 open_tree, 429 move_mount, 430 fsopen, 431 fsconfig, 432 fsmount, 433 fspick, 438 pidfd_getfd, 440 process_madvise, 442 mount_setattr, 443 quotactl_fd, 444 landlock_create_ruleset, 445 landlock_add_rule, 446 landlock_restrict_self, 447 memfd_secret, 448 process_mrelease, 450 set_mempolicy_home_node, 451 cachestat, 452 fchmodat2, 453 map_shadow_stack, 457 statmount, 458 listmount, 459 lsm_get_self_attr, 460 lsm_set_self_attr, 461 lsm_list_modules, 462 mseal, 463 setxattrat, 464 getxattrat, 465 listxattrat, 466 removexattrat, 467 open_tree_attr, 468 file_getattr, 469 file_setattr, 470 listns, 471 rseq_slice_yield, 472 fchroot.

---

## 5. Recommended implementation order

Each item is sized for one worker task and names the probe that must pass
afterwards (section 8). The order front-loads the items that turn stalls into
crashes and crashes into visible exits, so that later debugging sees real
failures instead of parked threads.

1. **Group-wide fatal signals** (S2, RC1). On `SIG_DFL` fatal delivery, on
   `SIGKILL`, and in `sys_exit_group`: mark the tgid as exiting, `signal_send`
   `SIGKILL` to every thread of the group, wake the sleepers, and have the
   timer IRQ return path (`arch/i686/cpu/irq.c:46-52` → `scheduler_tick`) call
   `signal_deliver_pending` for an interrupted ring-3 thread so CPU-bound
   threads die too. Keep `proc_exit` per thread; make `sys_waitpid` report the
   leader only once every sibling is a zombie. Probes P1, P17.
2. **Own thread group for `CLONE_VM` without `CLONE_THREAD`** (C1, RC2):
   `tgid = pid`, `parent = calling thread's tgid leader`; keep `vfork_parent`
   routing in `mmap_owner`; `exit_group` in the child then only kills the child.
   Also honour `CLONE_CHILD_SETTID`/`CLEARTID`/`PARENT_SETTID` in `do_fork`
   (C2). Probes P2, P3.
3. **Shared signal-handler table and process-scoped signals** (S3, S4, S11,
   RC3): refcounted `sighand` shared under `CLONE_SIGHAND`; `parent` recorded
   as a tgid; `SIGCHLD` only for process exit and only to the parent process;
   `waitpid` by parent tgid; `kill(tgid)` picks a thread not blocking the
   signal. Probe P3.
4. **Stop injecting spurious wake-ups** (S1, F3, F4, F2): `signal_send` wakes
   only for deliverable signals; clear `wake_tick` in `signal_send` and at the
   top of `sleep_on`; `sys_futex` returns `-ETIMEDOUT` on tick expiry and
   `-EINTR` on signal wake. Probes P4, P5, P13.
5. **Remove the Firefox-specific nets and traces** (section 6) once 1-4 are
   in, and verify with P4 that no `FUTEX_WAIT` returns without a wake, a timeout
   or a signal.
6. **Wake-to-run latency** (S8): unconditional yield at return-to-user when a
   wake happened (drop the `g_ipc_launch_started` gate); a reschedule IPI to an
   idle CPU (`smp.c` already has the IPI plumbing); a runnable list instead of
   the `MAX_PROCS` scan. Probe P4 (latency histogram).
7. **`poll`/`epoll`/`select` signal and timeout semantics** (E1, E2, U5, E3):
   `-EINTR` for deliverable signals; real `select` timeout; `POLLHUP`/`EPOLLHUP`
   on peer close; remove epoll items on last close. Probes P8, P9, P10.
8. **Syscall additions, in this order**: `ftruncate64`/`truncate64` (194/193),
   `rmdir` (40), `mknodat` (297), `getgroups32` (205), `getrusage` (77),
   `utimensat` (320/412), `sched_get_priority_*` fix (159/160),
   `clock_nanosleep` (267) with `TIMER_ABSTIME` in 407 too, `vfork` (190) on top
   of item 2, `msync`/`mlock*` no-ops, `tkill`, `rt_sigtimedwait` (177/421),
   `setitimer`/`getitimer`. Raise `MAX_FD` to 1024 and report it (E9). Probe P7.
9. **mmap protection and address-space reuse** (M1, M3, M4, M14): honour
   `prot` on eager pages, `mprotect(PROT_NONE)` unmaps, free-list reuse of
   unmapped ranges, `MAP_FIXED_NOREPLACE`, exclude the stack growth window.
   Probes P6, P11.
10. **exec robustness** (C4, C5): multi-page initial stack, `-E2BIG`, honour
    `SOCK_CLOEXEC`/`MFD_CLOEXEC`/`EPOLL_CLOEXEC`. Probes P14, P15, P16.
11. **AF_UNIX record and ancillary semantics** (U2, U3, U4, U6, U1): fds tagged
    at the start of the message, `MSG_CTRUNC`, `MSG_PEEK`/`DONTWAIT`/`NOSIGNAL`,
    `shutdown`, `SO_PEERCRED`, then `SOCK_SEQPACKET`. Probe P10.
12. **Clocks** (T1, T2, T4): TSC-based `clock_gettime`, correct clock ids,
    `clock_nanosleep` flags. Probe P12.
13. **Memory robustness** (M5, M8, M9, M12): tmpfs/memfd backed by the shmap
    frames only, `-ENOMEM` instead of halting the kernel heap, batched COW
    shootdowns, per-pgdir share counts, real `sysinfo`. Probes P16, P18.
14. **Signal frame fidelity** (S5, S6, S7): `orig_eax` restart, `sa_mask`,
    `si_addr`/`si_code`, `sigaltstack`, 64-bit signal sets. Probes P9, P19,
    P20.

Not recommended: patching glibc or keeping the nets. With items 1-6 done, glibc
2.36's condvar is exposed to the same timing as on Linux; upgrading the disk
image to glibc 2.41 is still the right long-term move because it removes the
steal path entirely, but it is not a kernel task.

---

## 6. Firefox-specific hacks and tracing to remove

These distort scheduling (extra wake-ups, serial output under the BKL, extra
yields) or hard-code Firefox layout facts (`libxul` at `0x43300000`, thread
names, process names starting with `firef`). Remove after items 1-5 of
section 5.

| File:lines | What it is |
|---|---|
| `proc/scheduler.c:109-110`, `:124-252` | The BZ#25847 "gentle" condvar net, the "aggressive" mutex net, and the `[wbt]` backtrace dump inside `scheduler_tick`. |
| `proc/scheduler.c:322-330`, `:412-424` | `g_resched_pending` gated on `g_ipc_launch_started`; keep the yield, drop the gate (item 6). |
| `proc/scheduler.c:450-472` | `[ff-exit]` stack scan on every Firefox thread exit. |
| `proc/scheduler.c:50-58` | `[SMP]` first-dispatch print. |
| `proc/process.c:36-123` | `proc_debug_snapshot`/`user_backtrace` (called every 300 ticks from `syscall_dispatch`, `proc/syscall.c:6335-6339`). |
| `proc/process.h:150-175` | `futex_cond`, `futex_wait`, `wait_bt_n`, `wait_bt[28]` (112 bytes per process) once the nets are gone; keep `futex_shared`/`futex_phys`. |
| `proc/syscall.c:2442-2473` | `g_ipc_launch_started`, `g_futex_progress_tick`, `g_ff_io_nudge`, `g_ipc_launch_tid`, `g_ipclaunch_waitaddr`, `g_wfph_watch_addr/tgid`, `g_forker_tid`. |
| `proc/syscall.c:794-806` | `[fk]` forker tracking in `do_fork`; also the unconditional `printk` at `:791-793`. |
| `proc/syscall.c:963-964`, `:986-997` | `waitpid` prints. |
| `proc/syscall.c:1121-1128`, `:1145-1154`, `:4575-4584` | `dbg_str_has`, `[iconopen]` traces in `open`/`openat`. |
| `proc/syscall.c:1302`, `:1310-1312`, `:1717-1743` | `[execfail]`, exec print, `[argv]`/`[fdtab]` dumps. |
| `proc/syscall.c:1894-1908` | `[dup2fd]` trace. |
| `proc/syscall.c:2382-2391` | `[gdents]` icon-dir trace. |
| `proc/syscall.c:3058-3064`, `:3093-3110`, `:3133-3141` | `[gdbaid]`, `[shmmap]` traces in `mmap2`. |
| `proc/syscall.c:3440-3448` | `[rtsig]` trace. |
| `proc/syscall.c:3709-3714` | `[lockop]` symlink trace. |
| `proc/syscall.c:3903-4004` | `fx_capture_wait_bt` (`LIBXUL_TEXT_LO/HI` hard-coded), `[wburl]`, `fx_lockdbg` and all its call sites (`:1145`, `:2214`, `:3247`, `:3410`, `:3524`, `:3684`, `:3738`, `:4575`, `:4595-4601`, `:4659`, `:4677`). |
| `proc/syscall.c:4010-4018`, `:4022-4026`, `:4059-4070`, `:4092-4131` | `[pollarg]`, `[poll-einval]`, `[pollnval]`, `[pollfd]`, `[wbt]` in `sys_poll`. |
| `proc/syscall.c:4285-4286`, `:4306-4319`, `:4322-4396` | `g_ff_io_nudge` snapshot, `[epw]`, the disabled epoll self-heal (`FF_EPOLL_SELF_HEAL`). |
| `proc/syscall.c:4501-4521` | `[tname]` and the `IPC Launch` thread-name trigger in `prctl`. |
| `proc/syscall.c:5214-5222`, `:5261-5281`, `:5298-5318`, `:5337-5390`, `:5413` | All `[fk]`, `[fxw]`, `[ilfx]`, `[wfph]`, `[fxk]` futex traces and `g_futex_progress_tick` updates. |
| `proc/syscall.c:5800-5801`, `:5807-5808` | `[launch]` vfork prints. |
| `proc/syscall.c:5930-5936` | `[ipc]` socketpair print and the `g_ipc_launch_started = 1` trigger. |
| `proc/syscall.c:6123-6125` | `[scm]` print (keep as a rate-limited warning if desired). |
| `proc/syscall.c:6340-6357` | Bochs magic-breakpoint hunt for `gtkprobe`. |
| `proc/syscall.c:6554-6561` | Unimplemented-syscall print (keep, but rate-limit). |
| `proc/syscall.c:6566-6644` | `[ftrace]`, `[ipclaunch]`, `[postfork]`, `[einval]` per-syscall traces on Firefox threads. |
| `proc/usocket.c:189-203`, `:258-281`, `:342-357`, `:394-398` | `[uflow]` traces, `g_ff_io_nudge`, `usocket_dbg_ptrs`, `usocket_rx_state`, `usocket_rx_id/tx_id`. |
| `proc/pipe.c:53-66`, `:93-107` | `pipe_inject_byte` (self-heal helper) and the `g_ff_io_nudge` bump. |
| `arch/i686/mm/paging.c:377-426`, `:547-633` | Hardware watchpoint machinery and the page-fault forensic dumps (`[ALIAS]`, `mem@`, `[bt]`); keep a one-line fault report. |
| `arch/i686/cpu/smp.c:70-71`, `:124`, `proc/process.c:95-97` | `[tlb]` counters and print. |
| `mm/pmm.c:157-169` | `[pmm-UAF]` check (cheap; may stay as a debug assert). |
| `include/kernel/config.h:7-12` comments | Firefox sizing notes; the values themselves stay. |

Also remove the two comments that state incorrect facts, so future readers do
not re-derive them: `proc/syscall.c:4937-4945` (mozjemalloc does not use
`MADV_DONTNEED` for purging in Firefox 115, see M6) and
`proc/syscall.c:5193-5196` ("Only FUTEX_WAIT/WAKE are needed for musl").

---

## 7. What the `intermittent stall` most likely is, mechanically

Putting RC1-RC4 together, the observed run-to-run variance is explained by
which thread happens to crash or lose a wake-up first:

- If a worker thread of the parent dies (RC1) before the first content launch,
  the main thread parks in whatever monitor that worker was supposed to signal;
  the launcher sees "no paint in 40 s" and kills the tree.
- If the launch thread dies after `fork()` but before `FinishLaunch`, the
  promise never settles: exactly `WaitForProcessHandle` parked forever.
- If `glxtest`'s `posix_spawn` `execve` fails on that attempt (RC2), the whole
  parent is SIGKILLed: "exited before paint".
- If none of that happens, the run competes against the widened glibc 2.36
  steal window (RC4) on every condvar hand-off, including the `PROCESS_CREATED`
  notify; a lost notify is again a park in `WaitForProcessHandle`.
- The nets then wake all parked condvar waiters every 150 ms, which recovers
  some lost notifies (the predicate is set) and manufactures new steal
  opportunities for the others, which is consistent with "about 30-50 % of
  attempts reach a window" (`proc/syscall.c:4361-4364`).

---

## 8. Probe programs

Small C programs, statically linked against the glibc in `testfiles/lib`, run
from the shell in QEMU. Each prints `PASS`/`FAIL` and the observed value; the
expected Linux result is stated so the same binary can be checked on the host.

**P1 fatal-signal-scope (RC1, S2).** Thread B does `raise(SIGSEGV)` (or writes
to address 0) 100 ms after start; main thread sleeps 2 s and then prints
`FAIL still alive`. Linux: the process dies with SIGSEGV before printing.
Variant: B calls `abort()`.

**P2 spawn-exit-group (RC2, C1).** `posix_spawn(&pid, "/nonexistent", ...)`
from a process with two threads; expect `posix_spawn` to return `ENOENT` and
the process to print `PASS`. On MaeroOS the process is killed. Second case:
`posix_spawn` of `/bin/busybox true`, then `getpid()` inside a
`posix_spawn`-launched helper (`sh -c 'echo $$'`) must differ from the parent.

**P3 sigchld-thread (RC3, S3, S4, C2).** Thread T installs nothing; main
installs a `SIGCHLD` handler after creating T; T `fork()`s a child that exits
immediately; main `waitpid(child, 0)` and checks the handler ran. Linux: handler
runs in some thread, `waitpid` succeeds. Also check `gettid()` in the forked
child equals `getpid()` (C2) and that a `pthread_exit` in T sends no `SIGCHLD`.

**P4 spurious-futex (RC4, F1, F3, F11, S1).** Six threads park in
`pthread_cond_wait` on separate condvars with no signaller for 10 s; count
returns of the raw `futex(FUTEX_WAIT)` (use `syscall()` directly on a private
word, expected value 0). Linux: 0 returns. Also send `SIGUSR1` (`SIG_IGN`) and a
blocked `SIGUSR2` to a waiter: Linux still 0 returns. Measure wake-to-run
latency with `clock_gettime` around a signal/wait ping-pong (S8).

**P5 stale-wake-tick (F4).** Thread T: `poll(pipe, 5000 ms)`; main sends
`SIGUSR1` (handled) after 100 ms, T's `poll` returns `EINTR`; T then does an
untimed `FUTEX_WAIT` on a word nobody wakes for 10 s. Linux: never returns.
MaeroOS: returns at about 5 s.

**P6 mmap-prot and madvise (M1, M6).** `mmap(64 KiB, PROT_NONE)`, install a
`SIGSEGV` handler, read the page: Linux faults, MaeroOS reads 0. Then
`mmap(RW)`, fill with `0xe5`, `fork()`, in the parent
`madvise(MADV_DONTNEED)` and read: Linux 0, MaeroOS `0xe5`.

**P7 ftruncate64 (194).** `memfd_create` + `ftruncate(fd, 1 << 20)` +
`fstat`: Linux size 1 MiB; MaeroOS `ENOSYS`. Also `mkfifo("/tmp/f")` (297) and
`rmdir` (40).

**P8 select-timeout (E2).** `select` on an idle pipe with a 100 ms timeout;
measure elapsed. Linux ~100 ms; MaeroOS ~30 s.

**P9 poll-eintr and SA_RESTART (E1, S5).** `poll(idle fd, -1)` with a
`SIGALRM` handler after 200 ms: Linux returns `EINTR` at 200 ms. Then the same
with `SA_RESTART`: Linux restarts the poll (no return); MaeroOS returns
`ENOSYS`.

**P10 unix-socket semantics (U2-U5).** Over a `socketpair`: (a) `sendmsg` 8 KiB
with one fd; `recvmsg` in 4 KiB pieces; Linux delivers the fd with the first
piece, MaeroOS with the last. (b) `send` 1 byte + fd via `sendmsg`, `recv` the
byte with plain `recv`, then `recvmsg` with a control buffer: Linux blocks (fd
was consumed with the byte and closed), MaeroOS returns 0 with the fd. (c)
`MSG_PEEK` must not consume. (d) `poll` after the peer closes must report
`POLLHUP`.

**P11 address-space reuse (M3).** Loop `mmap(8 MiB) / munmap` 100 000 times:
Linux fine; MaeroOS `ENOMEM` after roughly 250 iterations.

**P12 clocks (T1, T2, T4).** Print `clock_getres` and consecutive
`clock_gettime` deltas for ids 0-7; `CLOCK_MONOTONIC_COARSE` must be close to
`CLOCK_MONOTONIC`, not to `CLOCK_REALTIME`. `clock_nanosleep(CLOCK_MONOTONIC,
TIMER_ABSTIME, now + 100 ms)` must return within 200 ms.

**P13 futex-timeout code (F2, T3).** `FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME`
with a deadline 300 ms ahead on a word nobody wakes: Linux `ETIMEDOUT` at
300 ms; MaeroOS returns 0 at 300 ms. Repeat with `CLOCK_MONOTONIC`.

**P14 exec-arg-size (C4).** `execve` a helper with 6 KiB of environment (100
variables) and 70 argv entries; the helper prints `argc` and `strlen` of the
biggest variable. Linux: all present; MaeroOS: argc 64 and probable crash.

**P15 socket cloexec (C5).** `socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` and
`socketpair` with `SOCK_CLOEXEC`: `fcntl(F_GETFD)` must contain `FD_CLOEXEC`.

**P16 memfd cloexec and size (C5, M5).** `memfd_create("x", MFD_CLOEXEC)`
→ `F_GETFD` has `FD_CLOEXEC`; `posix_fallocate(fd, 0, 64 MiB)` then map and
touch: watch `/proc/meminfo` and the kernel heap (a second 64 MiB memfd must not
halt the machine).

**P17 signal-to-busy-thread (S2).** Thread spins in user mode; main sends
`SIGTERM` with a handler that sets a flag; measure delivery latency. Linux
< 1 ms; MaeroOS only at the spinner's next syscall (never, if it makes none).

**P18 high memory (M10).** Boot with `-m 1024M` and `-m 2048M`; program
`calloc`s and touches 700 MiB in 4 KiB steps, verifying a pattern; also
`fork()` with 100 MiB dirty and write in both.

**P19 siginfo (S6).** `SIGSEGV` handler with `SA_SIGINFO` prints `si_addr` and
`si_code` for a write to `0x1234`: Linux `0x1234`/`SEGV_MAPERR`; MaeroOS 0/0.

**P20 shared futex (F5, F6).** Two processes map the same memfd; A
`FUTEX_WAIT` (shared) on word 0 of page 3 without ever touching page 3 before
the wait; B writes 1 and `FUTEX_WAKE`s: Linux wakes A; MaeroOS `EFAULT` in A or
a lost wake.

---

## 9. Unverified points and suggestions

- **Shipped binaries not inspected.** Whether any other library on the disk
  image (`libgio`, `libgtk`, NSPR) calls `posix_spawn`/`vfork` on the startup
  path was inferred from source (GLib `gspawn.c:2200`, Firefox `GfxInfo.cpp:602`).
  `objdump -T` on `testfiles/fflib/*.so` and `testfiles/firefox/*.so` in the
  main checkout would settle it in minutes and should be run before item 2 of
  section 5 is declared done.
- **D-Bus autolaunch.** GIO's session-bus autolaunch spawns `dbus-launch` via
  `g_spawn_sync`; if it takes GLib's `posix_spawn` path and the binary is
  absent, RC2 kills the parent. Confirm with `MOZ_LOG` or by adding
  `DBUS_SESSION_BUS_ADDRESS=disabled:` to the launcher environment as an
  experiment.
- **`SOCK_SEQPACKET` in Firefox 115.** Not used by `IPC::Channel`
  (`ipc_channel_posix.cc:1276`); not checked for the fork server or
  `CrashReporter` pipes.
- **`CrossProcessSemaphore`/pshared futexes.** Not traced to the first-paint
  path; F5/F6 are listed as conditional.
- Suggestion (out of scope for the kernel): once items 1-6 land, replace the
  glibc 2.36 on the disk image with 2.41 or newer; BZ#25847 is fixed there and
  the kernel no longer needs to compensate.
- Suggestion: add the P-series probes to `tools/smoke_*.py` as a
  `smoke-linux-abi` target so each fix lands with its regression test.
