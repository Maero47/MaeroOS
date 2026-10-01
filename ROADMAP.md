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

- **Execute protection.** PAE page tables with NX and W^X for user and kernel
  pages (`make smoke` runs `wxprobe`); user pages in RAM above 4 GiB (checked
  by hand with `-m 6G`, not by a suite).
- **sshd.** `openssh-server` from Alpine runs in the chroot and the host logs
  in through it (`make smoke-alpine-net`), on top of rtnetlink, `AF_PACKET`,
  raw sockets and real `flock`/`fcntl` locks.
- **Filesystems from USB sticks.** USB disks join the block-device table as
  `sdX`, and FAT12/16/32 mounts read-write (`make smoke-vfat`).
- **Installing to a disk.** `maeros-install` writes a GPT disk that boots
  under BIOS and UEFI (`make smoke-install`).
- **Sound for Linux programs.** The ALSA PCM/control ABI on HDA and AC'97
  (`make smoke-audio`).

## Open work

What still stands between MaeroOS and daily use on real hardware:

- **ext4 writes, the rest.** A default `mkfs.ext4` filesystem mounts read-write
  with jbd2 journaling (`make smoke-ext4rw`, `docs/ext4.md`); still missing are
  indexing new directories (they stay linear), htree lookups, the orphan file, `meta_bg`,
  `inline_data`, `bigalloc`, quotas and filesystems past 2^32 blocks. The
  installed root is ext2.
- **SMP scaling.** Replace the single Big Kernel Lock with finer locking.
- **Networking hardware.** Wi-Fi (an 802.11 stack and a driver), Realtek
  r8169 and virtio-net; today only RTL8139 and e1000 are supported.
- **Networking stack.** A loopback interface for AF_INET, IPv6, `SO_LINGER`.
- **Graphics.** No GPU acceleration: the desktop draws into the boot
  framebuffer, and the resolution is the one the boot loader set.
- **Power.** ACPI sleep states (suspend to RAM); today only S5 power-off,
  reboot and the power button work.
- **USB.** USB 3 hubs, xHCI interrupts (MSI or INTx) instead of polling, more
  than one mass-storage device at a time, more than one HID interface per
  device (keyboards' media keys), keyboard LEDs.
- **Firefox.** Turn the content sandbox back on, cover CJK and Indic text
  with fonts, and shorten the roughly 5 s startup
  (`docs/perf/firefox-startup.md`); audio is only checked by an opt-in run.
- **Credentials.** A separate fsuid; search permission on the directories a
  path lookup walks through.
- **Missing interfaces.** SysV IPC, utmp, `/proc/stat` and
  the full per-pid `/proc/<pid>/` set, which would let toybox build `killall`,
  `vmstat`, `who`, `netcat` and `wget`; inotify (deliberately `ENOSYS` today).
- **Filesystems.** exFAT (recognised and refused today).
- **Packages.** Signing-key rotation and revocation; today the key is per
  build host.
- **Distribution.** A deterministic release artifact: kernel ELF, initrd,
  disk image and a documented QEMU command.
