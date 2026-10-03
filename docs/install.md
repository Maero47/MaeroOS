# Installing MaeroOS to a disk

## Using it

Boot `maeros-limine.iso` (`make limine-iso`) with the disk you want to install
to attached, log in as root and run:

```sh
maeros-install            # lists the disks, asks which one, asks you to type "yes"
maeros-install -l         # just list the disks
maeros-install -n /dev/sdb    # dry run: the layout, nothing written
maeros-install -y /dev/sdb    # no questions (scripts, tools/smoke_install.py)
maeros-install --ext2 /dev/sdb   # an ext2 root instead of ext4
```

On the desktop, the launcher's **Install MaeroOS** entry and the **Install**
desktop icon open a Terminal running `sudo maeros-install`, which asks for the
session user's password and then the same two questions. Both appear only when
the system carries `/boot/kernel.elf`, meaning it was booted from the Limine
image or installed from it.

Everything on the target disk is erased. A disk that holds a mounted filesystem,
such as the one the live system runs from, is refused. Remove the live medium
afterwards and boot from the disk. It boots under legacy BIOS (Limine's BIOS
stages) and UEFI (x86_64 `BOOTX64.EFI` and IA32 `BOOTIA32.EFI`). Secure Boot must
be off, because Limine is not signed.

Options: `-n` (dry run), `--ext2` (an ext2 root; ext4 is the default), `--source DIR`
(default `/disk`), `--boot-dir DIR` (default `/boot`), `--initrd FILE` (default
`/dev/initrd`), `--esp-mib N`.

## What it writes

| Partition | Size | Contents |
|---|---|---|
| 1 BIOS boot (`21686148-...`) | 1 MiB at 1 MiB | Limine BIOS stage 2 |
| 2 EFI System (`C12A7328-...`) | 2 x (kernel + initrd + Limine) + 64 MiB, at least 128 MiB | FAT32: `/EFI/BOOT/BOOTX64.EFI`, `/EFI/BOOT/BOOTIA32.EFI`, `/boot/limine/{limine.conf,limine-bios.sys,LICENSE}`, `/boot/kernel.elf`, `/boot/initrd.tar` |
| 3 MaeroOS root (Linux filesystem `0FC63DAF-...`) | the rest | ext4 (or ext2 with `--ext2`): a copy of `/disk` |

The protective MBR carries Limine's BIOS stage 1. The installer writes
`limine.conf` with `cmdline: root=PARTUUID=<partition 3's unique GUID>`.

Where the files come from:

* the kernel and Limine's files: `/boot` in the live system. The `limine-iso`
  target appends `boot/kernel.elf` and `boot/limine/{limine-bios.sys,
  limine-bios-hdd.bin, BOOTX64.EFI, BOOTIA32.EFI, LICENSE}` to the ISO's copy
  of `initrd.tar`. `tools/fetch-limine.sh` makes `limine-bios-hdd.bin` from
  the `limine-bios-hdd.h` array in the Limine release.
* the initrd: `/dev/initrd`, which is the boot module as it was loaded. Since
  that is the ISO's initrd, it carries `/boot` as well, so an installed system
  can install itself again.
* the root: `/disk`, including an Alpine root if one is there. Regular files,
  directories, symlinks (fast and slow), hard links, device nodes, fifos and
  sockets are copied with their modes, owners and timestamps. Filesystems
  mounted below `/disk` (`/proc` in a chroot, say) are copied as empty
  directories.

## Pieces

| File | Role |
|---|---|
| `userspace/install/install.c` | disk list (`/proc/partitions`, `/proc/mounts`), layout, GPT (both headers, CRC32), `limine.conf`, the Limine BIOS stage install, the interactive front end |
| `userspace/install/fat.c` | FAT32 formatter: BPB, FSInfo, backup boot sector, two FATs, one contiguous cluster run per file, VFAT long names |
| `userspace/install/ext2.c` | a populated filesystem in one pass, as `mke2fs -d` builds one: revision 1, 1 KiB blocks, `sparse_super`, `filetype`, `large_file`, lost+found with 12 blocks. ext4 (default): 256-byte inodes (`extra_isize`, creation times), extent trees of depth 0 to 2, `flex_bg` (16 groups' bitmaps and inode tables in the first), `metadata_csum` (superblock, descriptors, bitmaps, inodes, extent blocks, directory leaves and htree blocks), an empty internal jbd2 journal (inode 8, 1024 to 16384 blocks by size, backed up in `s_jnl_blocks`), every directory of more than one block written as an htree (half-MD4 with a random seed, names sorted by hash into leaves, one or two index levels), the orphan file (inode 12, 16 blocks), `huge_file`, `dir_nlink`. ext2 (`--ext2`): 128-byte inodes, direct/indirect block maps up to triple |
| `drivers/blkpart.c` | `/dev/<disk>` nodes are writable (root only, mode 0660), with partial sectors read-modify-written; writes to the disk mounted at `/disk` are `EBUSY`; GPT unique GUIDs are kept for `root=PARTUUID=` |
| `drivers/ata.c`, `drivers/blkdev.c` | the drive's real size (LBA48 IDENTIFY words) next to the LBA28 part used for I/O; `BLKGETSIZE`/`BLKGETSIZE64` on `/dev` nodes report the exact size |
| `proc/syscall.c` | `pread64`/`pwrite64` (and `read`/`write`/`_llseek`, all 64-bit now) on a disk node take the full offset, so the backup GPT at the end of a disk larger than 4 GiB can be reached |
| `fs/devfs.c`, `fs/initrd.c` | `/dev/initrd` (read-only, root only; Linux's block device 1,250) |
| `userspace/desktop/desktop.c`, `userspace/term/term.c` | the Install launcher entry and desktop icon; `term <slot> <command>` runs `shell -c <command>` instead of an interactive shell |
| `fs/ext2.c`, `drivers/ata.c` | `ext2_mount()` refuses a superblock with more blocks than its device or partition has; the IDE master's `ata_read`/`ata_write` reject sectors at or past the drive's end or 2^28 (as the other IDE positions already did), so nothing wraps onto LBA 0 |
| `kernel/main.c` | `root=/dev/<name>` or `root=PARTUUID=<guid>` picks the partition `fs/ext2.c` mounts at `/disk`. Without `root=`, the boot disk's whole device is used, as before. If `root=` names no device or the device holds no ext2/ext4, `/disk` stays unmounted rather than falling back to some other disk |
| `fs/ext2.c` | an ext4 `/disk` is mounted read-write with its journal (replayed if needed), `needs_recovery` while up, orphans finished at mount, `kjournald` started once processes exist (`ext2_start_flusher`); `reboot(2)` commits it and marks it clean (`ext2_shutdown`, from `proc/syscall.c`) |

The installer is plain POSIX C. Built on a Linux host (`cc -O2
userspace/install/[a-z]*.c`), it writes an image file. This is how the formats
were checked against `e2fsck -fn`, `fsck.fat -n` and `sgdisk -v`.

### Why the filesystem is written in userland

Writing the filesystem image directly needs no mounted target, and it does not
depend on an Alpine root being present (e2fsprogs' `mke2fs -d` in the chroot
would). The ext4 it writes is what `mke2fs -t ext4 -b 1024` makes (without
`64bit`, `resize_inode` and `ext_attr`), so `fs/ext2.c` mounts it read-write
with its journal at boot, and e2fsprogs accept it (`e2fsck -fn` clean, checked
by `smoke-install` before and after the installed system has run on it).

### Limits

* The disk must be at least about 128 MiB + 1 MiB + twice the size of `/disk`.
  The installer sizes the disk with `BLKGETSIZE64` (exact, odd sectors
  included; `/proc/partitions` counts 1 KiB blocks). It refuses a disk whose
  real size exceeds what the kernel can address: IDE I/O here is LBA28, so on
  an IDE disk past 128 GiB the backup GPT could not go at the real end. The
  kernel reads the LBA48 size from IDENTIFY words 100-103 and logs
  `[ATA]  hdX: N MiB, but LBA28 reaches only the first M MiB`.
  Disks of 2 TiB or more are refused. The block layer counts sectors in 32
  bits and saturates, so the real last sector, where the backup GPT goes, is
  unknown.
* The root filesystem is at most 1 TiB (2^30 blocks of 1 KiB). At that size the
  group descriptor table takes 4096 of group 0's 8192 blocks; much past 1.9 TiB
  it would no longer fit, and data and group 1's backup superblock would land
  on group 0's metadata. The rest of a bigger disk is left unpartitioned (the
  installer says how much). With ext4, group 0 also holds the first flex
  group's bitmaps and inode tables (2080 blocks at one inode per 16 KiB). `maeros-install -n` prints the
  layout and the filesystem geometry and writes nothing.
* ext4 blocks are 1 KiB, as in the ext2 layout (an extent covers at most 32 MiB).
  A file of more than 28224 extents (`4 * 84 * 84`, two tree levels) is refused;
  sequential allocation makes one extent per run between group metadata, so
  this is far beyond any real file. A directory of more than 123 * 126 leaves
  (some 400000 names) is written linearly instead of as an htree.
* The copy keeps no block or inode bitmap in memory: allocation is
  sequential, so a block is in use when it is metadata or lies below the
  cursor. Its memory use does not grow with the disk size. There is one
  inode per 16 KiB, and all inode tables are written (about 1% of the
  partition).
* One root partition, no swap and no separate `/home`. Hidden files in the
  live session's `/disk` (shell history, settings) are copied too.
* The desktop front end is the text installer in a Terminal, not a separate
  GUI app.

## Test: `make smoke-install` (part of `make check`)

See the docstring of `tools/smoke_install.py` and the README status table.
Artifacts go to `build/smoke-install/` (`target.img`, serial logs, desktop
screenshots).

## References

* UEFI Specification 2.10, section 5 (GPT: header, entry array, CRC32,
  protective MBR) and 13.3 (EFI System Partition, `\EFI\BOOT\BOOT{X64,IA32}.EFI`)
* Microsoft, "FAT: General Overview of On-Disk Format" (fatgen103): the FAT32
  BPB, FSInfo and long-name entries with their checksum
* Linux kernel `Documentation/filesystems/ext4/` and Dave Poirier, "The Second
  Extended File System" (https://www.nongnu.org/ext2-doc/): the on-disk layout.
  These are documentation; no code was taken from them.
* Limine 11.4.1 (BSD-2-Clause), `limine.c` `bios_install()`: stage 1 over the
  MBR keeping bytes 218-223 and 440-509, stage 2 in the BIOS boot partition, its
  byte offset stored at 0x1A4. `CONFIG.md` (`cmdline`, `module_path`,
  `boot():`) and `USAGE.md` (where Limine looks for `limine.conf` and
  `limine-bios.sys`)
* GPT partition type GUIDs: BIOS boot `21686148-6449-6E6F-744E-656564454649`,
  EFI System `C12A7328-F81F-11D2-BA4B-00A0C93EC93B`, Linux filesystem
  `0FC63DAF-8483-4772-8E79-3D69D8477DE4`
