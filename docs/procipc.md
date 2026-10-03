# /proc, System V IPC, utmp and inotify

    make smoke-procipc                               # in make check (inotify on
                                                     # tmpfs, ext2, vfat, exFAT, ext4)
    make smoke-procipc SMOKE_PROCIPC_ARGS=--alpine   # + Alpine procps-ng and htop
    make smoke-abi                                   # p45 (SysV), p46 (inotify), p47 (/proc)

## /proc

`fs/procfs.c`, with the per-process part in `fs/procpid.inc` and the
system-wide files in `fs/procsys.inc`.

**System files.** `/proc/stat` has a `cpu` line and one `cpuN` line per CPU,
`intr`, `ctxt`, `btime`, `processes`, `procs_running` and `procs_blocked`. The
CPU times come from the scheduler's dispatch timestamps: `cpus[].busy_ns` (a
thread was on the CPU) and `cpus[].idle_ns` (the idle wait), in USER_HZ (100)
ticks. Busy time is split into user and system in the proportion of the timer
ticks that found the CPU in ring 3 or in the kernel. There is no nice, iowait,
irq, softirq or steal time. `/proc/uptime`'s second field is idle time summed
over the CPUs, as on Linux. `/proc/loadavg` is a real load average: every 5 s
the BSP's tick counts the running and runnable threads and folds them in with
Linux's fixed-point exponential factors (`proc/scheduler.c`). There is no
uninterruptible sleep state, so nothing counts as blocked. `/proc/meminfo` has
the fields procps and htop read, most of them 0 (no page cache, no swap).
`/proc/vmstat` has the page counts and `ctxt`, with 0 for the paging
counters. `/proc/sys` has `vm/overcommit_memory`, `kernel/{shmmax, shmall,
shmmni, sem, msgmax, msgmnb, msgmni, pid_max, threads-max, ostype, osrelease,
hostname, ngroups_max}`, `fs/file-max` and `fs/inotify/max_*` (read-only).
`/proc/sysvipc/{shm,sem,msg}` are Linux's tables, which `ipcs` reads.

**Per process.** `/proc/self` is a symlink to `<tgid>` and `/proc/thread-self`
to `<tgid>/task/<tid>`. Before this change `/proc/self` was a directory of its
own. `/proc/<pid>/` and `/proc/<pid>/task/<tid>/` hold `stat` (all 52
fields), `statm`, `status` (`Uid`/`Gid` with real, effective, saved, `Threads`,
`VmRSS` and friends, signal masks, `Cpus_allowed`), `cmdline`, `comm`
(writable by the process itself, also through `prctl(PR_SET_NAME)`),
`environ`, `auxv`, `maps`, `limits`, `io` (counted at the read and write
syscalls), `mountinfo`, `mounts`, `cgroup`, `oom_score{,_adj}`, `wchan`,
`loginuid`, `sched`, the `exe`, `cwd` and `root` links, `fd/` and `fdinfo/`.
`task/` is only in the process view. A thread's tid also works as
`/proc/<tid>` but is not listed in `/proc`, as on Linux. RSS is counted from the
page tables (`pgdir_count_resident`).

**Who may read what.** Linux's `ptrace_may_access(PTRACE_MODE_READ_FSCREDS)`
is `procfs_may_read()`. The caller qualifies if it is in the same thread
group, is root, or its effective uid and gid equal all of the target's real,
effective and saved ids while the target is dumpable. `environ`, `auxv`,
`maps`, `io`, `fd/`, `fdinfo/` and the `exe`/`cwd`/`root` links refuse others
with `EACCES`. `stat`, `status`, `cmdline` and the rest stay readable.
Directories and files belong to the process's effective ids, or to root when
it is not dumpable. An exec that changes the effective ids (a set-uid image),
a `set*id` call that changes the effective uid or gid (Linux `commit_creds`),
and `prctl(PR_SET_DUMPABLE, 0)` make the whole process non-dumpable. The next
exec of an ordinary image, or `PR_SET_DUMPABLE` 1, undoes it. The
`/proc/<pid>/fd/N` readlink rule from the alpinenet review (`proc_fd_readlink`
in `proc/syscall.c`) uses the same check, now also for `task/<tid>/fd/N`.
`/proc/self/fd/N` of a regular file still hands back the file's real node, so
Firefox's reopen of a memfd shares its frames.

**Node lifetime.** Lookups do not close what they return, so procfs nodes
must outlive the walk that found them. Each (pid, file, fd, thread view)
gets one heap node, reused by later lookups and counted by open references.
Its contents are built at read time from whichever process holds that pid,
and pids are never reused. A node is freed once its process has been gone for
3 s and nothing holds it open.

## System V IPC

`proc/sysvipc.c` is reached through the `ipc(2)` multiplexer (117, which musl
uses) and the direct calls 393-402 and `semtimedop_time64` (420, which glibc
uses). The structures are the kernel's `IPC_64` layouts for i386, and a
command without `IPC_64` is treated the same way. Keys, `IPC_PRIVATE`,
`IPC_CREAT`/`IPC_EXCL`, Linux's `ipcperms()` (owner or creator, then either
group, then other, with root bypassing), `IPC_SET`/`IPC_RMID` limited to the
owner, creator or root, `IPC_STAT`, `IPC_INFO`, `*_INFO` and `*_STAT{,_ANY}`
are all as on Linux. Ids are `seq * slots + slot`.

- **shm**: frames are allocated and zeroed at `shmget`. `shmat` maps them
  shared (`mm_shm_attach_at`, also at a fixed address and read-only), and the
  attachment is recorded per address space: fork copies it, while exec and
  exit drop it, and `shm_nattch` counts address spaces. `IPC_RMID` on an
  attached segment marks it `SHM_DEST` and unfindable by key, and the last
  detach frees it.
  Limits: 128 segments, 32 MiB each, 64 MiB in all.
- **sem**: `semop` applies all of its operations or none. A blocked caller
  sleeps until the set changes, a signal arrives (`EINTR`) or the timeout runs
  out (`EAGAIN`). `IPC_NOWAIT`, `GETNCNT`/`GETZCNT`, `SETVAL`/`SETALL` (which
  clear the `SEM_UNDO` adjustments of what they set) work as on Linux. Undo
  adjustments belong to the process and are applied, clamped to 0..32767, when
  its last thread exits. `IPC_RMID` wakes sleepers with `EIDRM`.
- **msg**: messages are kept in FIFO order per queue, and `msgrcv` selects by
  exact type, by the lowest type up to `-type`, or with `MSG_EXCEPT`. A short
  buffer gets `E2BIG` unless `MSG_NOERROR` truncates. A full queue blocks the
  sender. `MSG_COPY` returns `ENOSYS`.

**MIT-SHM.** maeroX is unchanged. It could now offer the MIT-SHM extension
(`shmget`/`shmat` by both the client and the server, with `IPC_RMID` after
attaching), because cross-process attach by id, the permission checks and
`IPC_RMID`-while-attached all work.

## utmp and wtmp

`userspace/libc/utmp.c` provides `getutxent`, `getutxid`, `getutxline`,
`pututxline`, `utmpxname`, `updwtmpx` and `logwtmp`, plus the `utmp` names. A
record is glibc's 384-byte i386 layout, written under `flock`. At boot `init`
empties `/var/run/utmp` and writes a `reboot` record to it and to
`/var/log/wtmp`. When it cannot write there (an initrd-only boot) it first
mounts a tmpfs on `/var/run` or `/var/log`. `login` writes a `USER_PROCESS`
record for the session, with its pid, the line from `ttyname(0)` (getty's
`TTY` for the console) and the user. When the session's process exits, `init`
turns that record into `DEAD_PROCESS` and appends the logout to wtmp. toybox
`who`, `w`, `last` and `uptime` read these files. busybox is linked against
musl, whose utmp functions are stubs, so its `who` prints nothing. The same is
true of Alpine's.

## inotify

`fs/inotify.c`. An instance is a synthetic node behind an ordinary
descriptor, so `read`, `poll`/`select`/`epoll` (`read_ready_fn` and
`io_wake`), `FIONREAD`, `close`, `dup` and fork need no new descriptor type.
`/proc/<pid>/fdinfo` shows the watches. A watch holds a reference on its
node. Events:

| From | Events |
|---|---|
| `open(O_CREAT)`, `mkdir`, `mknod`, `bind` of a socket, `symlink`, `link` (on the directory) | `IN_CREATE` (`IN_ISDIR` for a directory) |
| `unlink`, `rmdir` | `IN_DELETE` on the directory; `IN_DELETE_SELF` then `IN_IGNORED` on the object when its last link went |
| `rename` | `IN_MOVED_FROM`/`IN_MOVED_TO` with one cookie, `IN_MOVE_SELF` on the object |
| `write`, `writev`, `pwrite64`, `truncate`, `O_TRUNC` of an existing file | `IN_MODIFY` |
| `chmod`, `chown`, `utimes` family, `link` (on the target) | `IN_ATTRIB` |
| `close` of a descriptor (each copy, also at exit) | `IN_CLOSE_WRITE` or `IN_CLOSE_NOWRITE` |
| `inotify_rm_watch`, `IN_ONESHOT` | `IN_IGNORED` |

An event about a file goes to the file's own watches and, with its name, to
its directory's. The directory is the one the caller's last path lookup of
the file went through (`vfs_last_parent`); descriptor operations look the
descriptor's path up again first. An event identical to the last one queued
is merged. A user may hold 128 instances (root is not limited), and an
instance cannot watch another instance. Past 16384 queued events the queue ends in one `IN_Q_OVERFLOW`
(wd -1). `IN_MASK_ADD`, `IN_MASK_CREATE`, `IN_ONLYDIR`, `IN_DONT_FOLLOW` and
`IN_ONESHOT` are supported.

The hooks are in `fs/vfs.c` (`vfs_create`, `vfs_unlink`, `vfs_rename`,
`vfs_link`, `vfs_symlink`, `vfs_setattr`, `vfs_settimes`, `vfs_truncate`)
and in `proc/syscall.c`, where the syscalls go straight to a filesystem's
`create_fn` or `truncate_fn`, and at write and close. Every one of them
returns at once while no watch exists. These syscalls used to return `ENOSYS`
so that GLib would poll. GLib now uses its inotify backend.

Not generated: `IN_OPEN`, `IN_ACCESS`, `IN_UNMOUNT`. A file renamed away
from the path its descriptor was opened by reports `IN_MODIFY` and
`IN_CLOSE_WRITE` only to its own watches.
