# MaeroOS Linux-Like Roadmap

This kernel already boots on i686, starts `/init`, launches a shell, and runs
small statically linked user programs from the initrd. The next goal is not to
clone Linux internals; it is to provide enough Linux/POSIX behavior that normal
Unix-style software can be ported or reused.

## Phase 1: Stable Baseline

- Keep `make`, `make initrd`, and `make smoke` passing.
- Keep `/init -> /shell -> command -> waitpid` reliable.
- Move large syscall scratch buffers to heap-backed storage.
- Preserve guard pages around user stacks and avoid growing kernel stacks as a
  substitute for correct allocation.
- Add regression commands to `tools/smoke.py` as bugs are fixed.

## Phase 2: Linux ABI Surface

- Audit syscall numbers against Linux i386 for the userland already present.
- Harden `copy_from_user` / `copy_to_user` style helpers and use them
  consistently instead of direct user pointer dereferences.
- Improve `fork`, `execve`, `waitpid`, `exit`, `pipe`, `dup`, `fcntl`,
  `ioctl`, `stat`, `getdents`, `brk`, and signal behavior.
- Add `errno`-accurate failures for common POSIX edge cases.
- Make process IDs, sessions, process groups, and terminal foreground control
  predictable enough for a real shell.

## Phase 3: Filesystems And Devices

- Keep initrd as the early boot filesystem.
- Make tmpfs reliable for writable runtime files.
- Improve ext2 mount/read/write enough for a persistent root disk image.
- Expand devfs with `/dev/null`, `/dev/zero`, `/dev/tty`, `/dev/random`,
  `/dev/urandom`, and standard fd aliases.
- Grow procfs with `/proc/self`, `/proc/self/fd`, `/proc/meminfo`,
  `/proc/version`, and process status files.

## Phase 4: Imported Open-Source Userland

- Start with small portable tools that tolerate a minimal POSIX libc.
- Target a static BusyBox-style userland once syscall coverage is adequate.
- Consider importing or adapting code from xv6, ToaruOS, or musl only when it
  cleanly matches the kernel ABI and license obligations are documented.
- Avoid importing large projects before the syscall smoke suite can isolate
  whether failures are kernel bugs, libc bugs, or application assumptions.

## Phase 5: Boot And Distribution

- Keep QEMU `-kernel` boot for fast development.
- Keep ISO generation for GRUB boot testing.
- Add an ext2 root disk workflow once ATA/ext2 writes are stable.
- Add a deterministic release artifact: kernel ELF, initrd, optional disk image,
  and a documented QEMU command.
