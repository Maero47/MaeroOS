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

## Open work

- **Firefox.** Turn the content sandbox back on, cover CJK and Indic text
  with fonts, and shorten the roughly 5 s startup
  (`docs/perf/firefox-startup.md`).
- **Credentials.** A separate fsuid; search permission on the directories a
  path lookup walks through.
- **Sockets.** A loopback interface for AF_INET; `SO_LINGER`; sshd on top of
  the server sockets once an Alpine root is available.
- **ext4 writes.** Journaled writes (extent allocation, bitmaps and group
  descriptors with checksums, jbd2 transactions) so `mount -t ext4` can be
  read-write; today it is read-only (`docs/ext4.md`).
- **Missing interfaces.** SysV IPC, utmp, `/proc/stat` and
  the full per-pid `/proc/<pid>/` set, which would let toybox build `killall`,
  `vmstat`, `who`, `netcat` and `wget`.
- **Packages.** Signing-key rotation and revocation; today the key is per
  build host.
- **Memory protection.** Execute protection needs PAE page tables (the NX bit).
- **SMP scaling.** Replace the single Big Kernel Lock with finer locking.
- **USB.** Mount filesystems from `/dev/usbdisk0` (needs a block-device
  layer under ext2 instead of its direct `ata_read` calls), USB 3 hubs,
  xHCI interrupts (MSI or INTx) instead of polling, more than one HID
  interface per device (keyboards' media keys), keyboard LEDs.
- **Distribution.** A deterministic release artifact: kernel ELF, initrd,
  disk image and a documented QEMU command.
