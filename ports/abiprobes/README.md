# Linux-ABI probes

Fifty-nine small C programs.  P1-P20 are the probes of
`docs/audit/firefox-first-paint.md` section 8; P21-P59 were added with later
kernel fixes.  Each proves or disproves one kernel-semantics gap and prints
exactly one final line:

    PASS <name>
    FAIL <name>: <what was observed>
    SKIP <name>: <why it cannot run here>

The binaries are static musl i386 executables.  Run on an x86_64 Linux host
that executes i386 ELF files they print `PASS`: that is the reference
behaviour.  Run on MaeroOS under QEMU (`make smoke-abi`) they are the
regression tests for the fixes in audit section 5.

## Probe to file mapping

| Probe | File | Audit findings | Section 5 item |
|---|---|---|---|
| P1  | `p01_fatal_signal_scope.c`  | RC1, S2 | 1 |
| P2  | `p02_spawn_exit_group.c`    | RC2, C1, S12 | 2 |
| P3  | `p03_sigchld_thread.c`      | RC3, S3, S4, S11, C2 | 2, 3 |
| P4  | `p04_spurious_futex.c`      | RC4, F1, F3, F11, S1, S8 | 4, 5, 6 |
| P5  | `p05_stale_wake_tick.c`     | F4 (T4/E1 as triggers) | 4 |
| P6  | `p06_mmap_prot_madvise.c`   | M1, M6, M4 | 9 |
| P7  | `p07_ftruncate64.c`         | syscalls 194, 193, 297, 40 | 8 |
| P8  | `p08_select_timeout.c`      | E2 | 7 |
| P9  | `p09_poll_eintr_restart.c`  | E1, S5 | 7, 14 |
| P10 | `p10_unix_socket.c`         | U2, U3, U4, U5 | 7, 11 |
| P11 | `p11_addr_space_reuse.c`    | M3 | 9 |
| P12 | `p12_clocks.c`              | T1, T2, T4 | 12 |
| P13 | `p13_futex_timeout.c`       | F2, T3 | 4 |
| P14 | `p14_exec_arg_size.c`       | C4 | 10 |
| P15 | `p15_socket_cloexec.c`      | C5 | 10 |
| P16 | `p16_memfd_cloexec_size.c`  | C5, M5 | 10, 13 |
| P17 | `p17_signal_busy_thread.c`  | S2, S4 | 1 |
| P18 | `p18_high_memory.c`         | M10 (M8 timing printed) | 13 |
| P19 | `p19_siginfo.c`             | S6 | 14 |
| P20 | `p20_shared_futex.c`        | F5, F6 | 14 |
| P21 | `p21_sigsuspend.c`          | review round 1, finding 1 | - |
| P22 | `p22_mprotect_cow.c`        | review round 1, finding 3 (M1) | 9 |
| P23 | `p23_exec_dethread.c`       | C3 | 10 |
| P24 | `p24_unlink_open.c`         | tmpfs node lifetime (unlink while open) | — |
| P25 | `p25_ptmx_lookup.c`         | fix round 1: PTY leaked by a lookup | — |
| P26 | `p26_unlink_frees_space.c`  | perf round 2: unlink and truncate leaked blocks | — |
| P27 | `p27_indirect_blocks.c`     | perf round 3: the rewritten ext2 block map | — |
| P28 | `p28_alloc_failure.c`       | kernel allocators must refuse (ENOMEM/EMFILE), not halt; at fault-time OOM (every page touched) the touch is a SIGSEGV and a `read()` into an untouched buffer `EFAULT`, not a hang | — |
| P29 | `p29_tmpfs_big_file.c`      | a 288 MiB tmpfs file, bigger than the kernel heap window | — |
| P30 | `p30_waitid.c`              | `waitid` with `WNOWAIT`/`WNOHANG` (Firefox's process watcher) | — |
| P31 | `p31_chroot.c`              | `chroot` (absolute symlinks, `..`, fork, nesting; root only) for Alpine (`ports/alpine/`) | — |
| P32 | `p32_utimensat.c`           | `utimensat`/`futimens`/`utimes`/`utime`, wall-clock mtime of new files (apk preserves mtimes) | — |
| P33 | `p33_isatty.c`              | `TIOCGWINSZ`/`TCGETS`/`TCSETS` fail with `ENOTTY` on pipes and files (`isatty`) | — |
| P34 | `p34_statfs.c`              | `statfs("/proc")` is `PROC_SUPER_MAGIC`; `ENOENT`/`EBADF` (apk's procfs check) | — |
| P35 | `p35_xattr.c`               | `*getxattr` give `ENODATA`/`EOPNOTSUPP`, not `ENOSYS` (GNU `ls -l`) | — |
| P36 | `p36_lstat_statx.c`         | `lstat`/`fstatat(dirfd, NOFOLLOW)` through musl's `statx` (dirfd and the link itself) | — |
| P37 | `p37_link.c`                | `link`/`linkat` on ext2, `st_nlink` (apk-tools 3, packages with hard links) | — |
| P38 | `p38_sync.c`                | `sync`/`syncfs` (apk-tools 3) | — |
| P39 | `p39_splice.c`              | `splice` moves bytes or answers `EINVAL`, never `ENOSYS` (coreutils 9.8+ `cat`) | — |
| P40 | `p40_flock.c`               | `flock` and `fcntl` record locks exclude, wait and go away on close (apk's database lock) | — |
| P41 | `p41_renameat2.c`           | `renameat2` with flags 0 and `RENAME_NOREPLACE` (busybox 1.37 in Alpine 3.24) | — |
| P42 | `p42_netlink.c`             | rtnetlink `RTM_GETLINK` dump and error ack, `SIOCGIFINDEX`/`SIOCGIFNAME`/`SIOCGIFCONF` agree (busybox `ip`, `ifconfig`, udhcpc) | — |
| P43 | `p43_lock_close_race.c`     | a descriptor closed while `F_SETLKW`/`flock` waits leaves no lock behind (`F_SETLKW`: `EBADF`) | — |
| P44 | `p44_proc_fd_link.c`        | `/proc/<pid>/fd/N` links name the open file (`ttyname`); another user's are `EACCES` | — |
| P45 | `p45_seccomp.c`             | seccomp filters: validation, ALLOW/ERRNO/TRAP (SIGSYS siginfo + ucontext)/KILL_THREAD/KILL_PROCESS/LOG, STRICT, TSYNC, fork/exec inheritance; `no_new_privs` ignores set-uid on exec | — |
| P46 | `p46_sysv_ipc.c`            | System V shm/sem/msg: sharing across fork, IPC_RMID, SEM_UNDO, blocking, types, keys, permissions | — |
| P47 | `p47_inotify.c`             | inotify events of each directory operation, poll/epoll/FIONREAD, blocking read, IN_Q_OVERFLOW | — |
| P48 | `p48_proc_pid.c`            | `/proc/self` link, `/proc/<pid>` files, `/proc/stat`/`loadavg`; another user's `environ`/`fd`/links are `EACCES` | — |
| P49 | `p49_ipc_limits.c`          | an unprivileged user's inotify queues end in `IN_Q_OVERFLOW`, a full message queue is `EAGAIN`, shm stops at `ENOSPC`; fork and allocation still work | — |
| P50 | `p50_accounting.c`          | `/proc/self/fd`/`fdinfo` nodes are dropped once the fds close (`procfs_nodes`); inotify's spill bucket returns to 0 after a spilled user gets a slot | — |
| P51 | `p51_procfs_fair.c`         | an unprivileged user looking up ~6000 distinct `/proc/<pid>/fd`/`fdinfo` nodes in tight loops cannot make root's or another user's `/proc` lookups (or `ps`) fail; `procfs_nodes` stays within `procfs_nodes_max` and falls back once it stops; a user cannot hold (open/inotify-watch) more than its share of nodes; per-pid `/proc` dirs are refused as bind source/target | — |
| P52 | `p52_kill_zombie_child.c`   | `kill(pid, SIGKILL)` and `kill(-pgrp, SIGKILL)` of a process with an unreaped zombie child return and kill it (the SIGKILL subtree walk used to spin forever under the BKL) | — |
| P53 | `p53_ext2_open_many.c`      | with 160 other files open (each unlinked), a written, open, unlinked ext2 file keeps its data while other files are written (the 128-slot open-inode table let the 129th go untracked and freed) | — |
| P54 | `p54_dev_perms.c`           | as uid 65534: `/dev/null`, `zero`, `urandom`, `ptmx` open; `/dev/input/event*`, `fb0`, `dsp`, `snd/*` and disks are EACCES; root's `/dev/pts/N` is 0620 root-owned and EACCES, its own pty slave is its own (mode-0 device nodes used to be world read/write) | — |
| P55 | `p55_path_search.c`         | as uid 65534, in /tmp and /disk: open/stat/readlink/create/chdir under a 0700 directory are EACCES (also two levels down and through a symlink); a 0711 directory is walked through and chdir'd into but not listed (the walk checked no search permission) | — |
| P56 | `p56_mount_refs.c`          | a tmpfs bind mount whose source directory is removed still stats as an empty directory while 300 new directories are made (the table kept raw pointers, so the mount showed freed memory); rmdir/rename of a mountpoint and rename over one are EBUSY; umount works after | — |
| P57 | `p57_mode_zero.c`           | as uid 65534, in /tmp and /disk: a chmod-000 file, a file created with mode 0 and a file in a chmod-000 directory are EACCES; /etc/shadow does not open (also run by smoke-cmds on the diskless initrd, whose files were all 0755) | — |
| P58 | `p58_pty_reuse.c`           | a thread asleep in read() on a pty slave whose descriptors another thread closes does not return the input written to the next pty opened (the pair was freed and reused under the sleeper) | — |
| P53 | `p53_sigpage_interp.c`      | an ELF interpreter segment at the sigreturn page's address must not overwrite the shared trampoline (signal handlers still return afterwards) | — |
| P54 | `p54_setid_auxv.c`          | set-uid-root exec from uid 65534: `AT_SECURE=1`, `AT_EUID=0`; a plain exec `AT_SECURE=0` (root only) | — |
| P55 | `p55_fstat_low_map.c`       | old `fstat` (108) on eventfd/epoll/socket/pipe; unprivileged `MAP_FIXED` below 64 KiB refused | — |
| P56 | `p56_memfd_anon.c`          | a memfd has no name in `/tmp` (`/memfd:<name> (deleted)`), and a reopen through `/proc/self/fd` shares its pages | — |
| P57 | `p57_fb0_identity.c`        | a regular file named `fb0` maps its own bytes; read-only `/dev/fb0` gives no writable view (MaeroOS) | — |
| P58 | `p58_kill_target.c`         | MaeroOS syscall 505: no init, no process the caller could not signal (EPERM); own child allowed | — |
| P59 | `p59_lock_limits.c`         | record locks bounded per uid (ENOLCK past 4096 here); a second user can still lock; close releases all | — |

Every source starts with a comment that names the findings, states the Linux
behaviour it asserts with a kernel/libc source reference, and quotes the
MaeroOS behaviour the audit describes.

P21 and P22 do not come from the audit: they were added with the fixes for
the review of the merged kernel, so that the sigsuspend
livelock and the `mprotect(PROT_READ)` hole cannot come back unnoticed.  P12
gained the `SA_RESTART` sleep cases for the same reason (round 2).

Two deviations from the audit text, both explained in the sources:

- P9: Linux never restarts `poll()` after a handled signal, even with
  `SA_RESTART` (man 7 signal).  The probe asserts `EINTR` for `poll` in both
  cases and uses `read()` on a pipe for the `SA_RESTART` restart check (S5).
- P17 additionally checks that `kill(getpid(), sig)` reaches a thread that
  does not block the signal (S4), since it already has a spinning thread.

## Building

Toolchain: musl.cc `i686-linux-musl-cross` (sha512 in
<https://musl.cc/SHA512SUMS>), either on `PATH`, under `/opt` (the ports
Docker image) or under `~/opt`.  Override with `MUSL_PREFIX=...`.

    make abiprobes                 # from the repo root, or
    make -C ports/abiprobes        # same thing

Outputs go to `testfiles/abiprobes/` (gitignored), which the initrd picks up
as `/abiprobes/`.  The build flags are `-O2 -Wall -Wextra -static -no-pie`
and the sources compile warning free.

musl differences worth knowing when comparing with a glibc build:

- `posix_spawn` uses `clone(CLONE_VM|CLONE_VFORK|SIGCHLD)`, glibc 2.36 uses
  `clone3` with the same flags.  Both exit the child with `exit_group`, so P2
  exercises the same kernel path either way (C1).
- musl 1.2 has a 64-bit `time_t`; raw `SYS_futex` calls therefore build the
  kernel's old 32-bit `timespec` by hand (`struct kernel_old_timespec` in
  `probe.h`).
- glibc's condvar internals (BZ#25847) are not modelled; P4 exercises the raw
  futex behaviour underneath them.

## Running

On the host (Linux reference; every line must be PASS):

    make -C ports/abiprobes host-check

On MaeroOS under QEMU:

    make smoke-abi                          # boots with -m 1024M; needs disk.img (P26)
    python3 tools/smoke_abi.py --mem 2048M  # the audit's second P18 case
    python3 tools/smoke_abi.py --only p05,p13

P18 sizes its allocation from the QEMU memory (`mem - 324 MiB`, clamped to
64..1400), so `-m 1024M` runs the audit's 700 MiB case and `-m 2048M` a
genuinely larger 1400 MiB one.

P24 is not from the audit: it was added after an intermittent kernel panic in
`fdtable_put` was traced to `tmpfs_unlink()` freeing a node that a descriptor
still referenced.  It asserts the ordinary Unix contract that unlinking a file
removes only its name.  Note that it checks *semantics*, not the crash: under
the first-fit heap the kernel had then, a freed node kept its contents until something
happens to reuse that exact block, which a single process cannot force
reliably, so the probe passed even before the fix.  The empirical evidence for
the fix is the `make smoke-firefox` pass rate.

P25 is not from the audit either: a lookup of `/dev/ptmx` used to allocate the
master/slave pair, so `stat()`, `access()` and `execve()` each consumed one of
the eight and nothing ever gave it back.  It asserts that a lookup is free —
`stat`/`lstat`/`access` in bulk, then an open that must still succeed — and
that a `stat()` of an unopened slave does not change what the master's
`poll`/`read` report.  It compares the master before and after rather than
asserting an absolute hangup behaviour, because Linux (blocks until the slave
has been opened once) and MaeroOS (reports EOF) legitimately differ there.

P27 is a guard rather than a demonstration: nothing was broken when it was
written.  It exists because the ext2 indirect walk was rewritten for speed --
it used to copy each whole indirect block out of the block cache into a
kmalloc'd buffer and index the copy, and now reads the single 32-bit pointer
it wants straight out of the cache slot.  That is the one piece of the driver
whose failure mode is silently returning the wrong bytes rather than an error,
so it is worth asserting.  The probe writes 3 MiB (past the singly-indirect
range of a 1 KiB-block ext2, ~2.7 MiB into the doubly-indirect one), stamps
every 4-byte word with its own file offset, and reads it back forwards through
`read`, backwards through `pread` and again through a private `mmap` -- so a
page resolved to the wrong disk block reports the offset it actually came
from.  The triply-indirect range would need a file past ~64 MiB, too slow to
write over PIO here; Firefox's own 175 MiB `libxul.so` exercises it on every
`make smoke-firefox`.

`tools/smoke_abi.py` runs each probe from the shell over the serial console,
reads the verdict line and prints a summary `N pass, M xfail, K unexpected
fail, X xpass, S skip, H hang, R not run, V no verdict`.  Probes that the
audit expects to fail on the current kernel are listed in `XFAIL` inside the
script, so the target is green today; when a kernel fix flips one to PASS it
is reported as XPASS and its entry must be removed to make it required
(`--strict` turns XPASS into a failure).  `XFAIL` is empty today: every
probe is required to pass.

An `XFAIL` entry is satisfied only by a probe that ran and printed
`FAIL <name>: ...`.  A probe that printed no verdict line, that wedged the
guest shell, or that never ran is a hard failure whether or not it is listed:
those mean the harness itself is broken (binaries missing from the initrd,
exec failure, a crash before any output, a kernel wedge) and would otherwise
be indistinguishable from the expected ABI failures.  After a wedge the driver
kills QEMU, reboots and continues with the remaining probes.

Each probe arms an internal watchdog thread that prints a `FAIL` line and
exits, so a hung syscall still returns the shell prompt instead of wedging
the guest:

| Probes | Watchdog | Driver timeout |
|---|---|---|
| P1-P10, P12-P15, P17, P19-P22, P24, P25, P30-P39 | 60 s | 90 s |
| P23 | 90 s | 120 s |
| P11, P16 | 120 s | 150 s |
| P26, P27, P29 | 240 s | 270 s |
| P28 | 300 s | 330 s |
| P18 | `120 + MiB/2` s (470 s at 700 MiB) | watchdog + 30 s |

The driver derives its per-probe timeout as watchdog + 30 s, so the watchdog
always fires first, and it verifies the watchdog values in this table against
the `probe_watchdog()` calls in the sources before every run (a mismatch
aborts the run). P16 and P18 accept a size in MiB as `argv[1]`.
