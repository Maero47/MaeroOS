# MaeroOS Roadmap

The goal is not to clone Linux internals; it is to provide enough Linux/POSIX
behaviour that ordinary Unix software runs unmodified. What already works, and
the test that proves it, is in the status table of [README.md](README.md).

## Standing rules

- Keep `make`, `make initrd`, `make check` and every other `make smoke*` target
  passing.
- Add a regression check to the relevant `tools/smoke*.py` (or a `*probe`
  binary it runs) with every bug fix.
- Touch user memory only through `copy_from_user` / `copy_to_user`.
- Import third-party code only when it matches the kernel ABI and its license
  is recorded in the README's third-party table.

## Done since the previous roadmap

Each is covered by the test named, which README.md's status table describes.

- **ext4 read-write, finished for what `mkfs.ext4` makes.** htree lookups and new
  directories indexed past one block, the orphan file (and the old orphan list) for
  open-unlinked files and multi-commit deletes and truncates, an ext3/ext4 `/disk`
  with its journal, and an ext4 root from `maeros-install` (`make smoke-ext4rw`,
  `make smoke-install`, `docs/ext4.md`).
- **64-bit file offsets** from the system calls down to ext2/ext4, exFAT, vfat, tmpfs
  and block devices, with `EOVERFLOW`/`EFBIG` where Linux has them
  (`make smoke-largefile`, `docs/largefile.md`).
- **Network hardware.** virtio-net on a generic virtio PCI transport with MSI-X
  (`make smoke-net-virtio`, `docs/virtio.md`); a Realtek r8169 driver, tested on the
  host against a simulated chip only (`docs/r8169.md`).
- **Display modes.** Runtime mode setting on Bochs/QEMU std VGA, VirtualBox VGA and
  virtio-gpu (2D), a Settings page with keep-or-revert, and the desktop re-laid out on a
  change (`make smoke-gfxmode`, `docs/display.md`).
- **Firefox content sandbox on.** seccomp-bpf with `TSYNC`, `no_new_privs` and the
  file broker give level 4 with no syscall refused (`make smoke-firefox-web`,
  `docs/sandbox.md`).
- **Missing interfaces filled.** `/proc/stat`, `loadavg`, `vmstat` and the full
  `/proc/<pid>/`, System V shm/sem/msg, inotify and utmp/wtmp; toybox now builds
  `killall`, `vmstat`, `who`, `w`, `last`, `ipcs`, `ipcrm` and `inotifyd`
  (`make smoke-procipc`, `docs/procipc.md`).
- **SMP stage 1.** Idle CPUs halt off the Big Kernel Lock, per-object poll wakes,
  interrupt-driven console output and targeted TLB shootdowns: single-process kernel
  loads on `-smp 4` run at the `-smp 1` rate and fork+exec is 17× faster; lock
  primitives with a lock-order checker are in place for the next stages
  (`make smoke-klock`, `make stress-smp`, `docs/smp-plan.md`).

## Open work

What still stands between MaeroOS and daily use on real hardware:

- **SMP scaling.** Stages 2 and later of `docs/smp-plan.md`: wait queues with object
  locks and atomic refcounts, then a per-thread BKL dropped across context switches,
  a scheduler lock with per-CPU run queues, and locks for memory, timers, fd tables,
  the VFS, block devices, filesystems, the network and drivers until only a residual
  lock is left. Several processes in the kernel at once still serialise on the BKL.
- **ext4 writes, the rest.** `meta_bg`, `inline_data`, `bigalloc`, quotas,
  filesystems past 2^32 blocks, an htree deeper than one interior level for inserts
  (`largedir`), and per-file `fsync` (`docs/ext4.md`).
- **Networking hardware.** Wi-Fi (an 802.11 stack and a driver), and the r8169 on
  real hardware.
- **Networking stack.** DHCPv6 and static IPv6 addresses, IPv6 nameservers
  in the native resolver, and a blocking `SO_LINGER` timeout (`docs/net.md`).
- **Graphics.** No GPU acceleration: the desktop composites in software, and cards
  other than Bochs/VirtualBox VGA and virtio-gpu keep the boot loader's mode.
- **Power.** ACPI sleep states (suspend to RAM); today only S5 power-off,
  reboot and the power button work.
- **USB.** USB 3 hubs on real hardware (the code is in, but QEMU has no
  SuperSpeed hub to test it on), isochronous transfers (webcams, USB audio),
  UAS, a HID keyboard driven in report protocol (today boot protocol, plus a
  separate media-key interface), the volume keys wired to an ALSA mixer.
- **Firefox.** Startup is about 1.7 s from launch to first paint
  (`docs/perf/firefox-startup.md`); what is left there is Firefox's own code
  and a disk read path that polls with interrupts off. Scripts beyond CJK,
  Indic (Devanagari, Bengali, Tamil), Arabic and Hebrew have no font. Without
  user namespaces the level-4 content sandbox has no chroot or network/PID
  namespaces (`docs/sandbox.md`). Audio is only checked by an opt-in run.
- **Credentials.** A separate fsuid; search permission on the directories a
  path lookup walks through.
- **Packages.** Signing-key rotation and revocation; today the key is per
  build host.
- **Distribution.** A deterministic release artifact: kernel ELF, initrd,
  disk image and a documented QEMU command.
