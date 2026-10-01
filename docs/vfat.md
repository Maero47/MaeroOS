# FAT (vfat) and USB sticks

## Using it

```sh
cat /proc/partitions                      # a USB stick is the next sdX: sdb, sdb1, ...
mkdir -p /mnt/usb
mount -t vfat /dev/sdb1 /mnt/usb          # read-write
mount /dev/sdb1 /mnt/usb                  # the same: busybox finds vfat via /proc/filesystems
mount -t vfat -o ro /dev/sda1 /boot/efi   # an EFI system partition, read-only
mount -t vfat -o uid=1000,gid=1000,umask=077 /dev/sdb1 /mnt/usb
df /mnt/usb                               # real cluster counts
mount -o remount,ro /mnt/usb              # flushed and marked clean; remount,rw undoes it
umount /mnt/usb
```

`msdos` and `fat` are accepted as type names too (same driver, long names
included). A stick formatted without a partition table (FAT starting at
sector 0, common on USB sticks) is mounted through the whole-disk device
(`/dev/sdb`); `drivers/blkpart.c` recognises the FAT boot sector and does not
read its boot code as partition entries.

Mount options: `uid=`, `gid=` (owner of every file, default root), `umask=`,
`dmask=`, `fmask=` (octal, default 022). Files and directories get
`0777 & ~mask`; a file with the read-only attribute loses its write bits, and
`chmod` sets or clears that attribute (the other mode bits and `chown` are
accepted and have no effect, as with Linux's `quiet` option). Unknown options
are ignored.

## USB mass storage in the disk table

`drivers/usb/usb_msc.c` still exposes the raw `/dev/usbdisk0`, and now also
puts the attached stick into `drivers/blkdev.c`'s disk table as the next SCSI
disk name after the AHCI disks (`sdb` next to one AHCI disk; Linux numbers SATA
and USB disks in one `sd` sequence too), with `dev_t` 8:16·n. Its partitions
are scanned like any other disk's and appear in `/dev` and `/proc/partitions`.

Hot-plug: the scan needs disk I/O, which needs the USB lock that `kusbd` holds
while it attaches a device, so attach only flags the disk and `kusbd` registers
it (`usb_msc_service()`) right after it drops the lock. Unplugging marks the
disk slot and every partition of it gone at once: they leave `/dev` and
`/proc/partitions`, and every read or write through them fails, so a filesystem
still mounted from the stick gets I/O errors (never another disk's data) and
can be unmounted. The partition entries are not freed (a mount or an open
`/dev` node may still point at them). Plugging the stick back in reuses the
slot and the name. One stick at a time (the MSC driver's limit).

## Pieces

| File | Role |
|---|---|
| `fs/vfat.c` | the driver |
| `fs/mount.c` | `-t vfat`/`msdos`/`fat`, mount options, `remount` calls the filesystem's `set_ro` hook |
| `fs/vfs.c` | `vfs_mounts_shutdown()`: `poweroff`/`reboot`/`halt` make every writable mount read-only first (flushed, volume marked clean) |
| `fs/procfs.c` | `/proc/filesystems` (vfat first, so busybox `mount` without `-t` probes it before the ext types) |
| `proc/syscall.c` | `statfs`/`fstatfs` ask the mount's `statfs` hook (type `msdos`, cluster size, total and free clusters) |
| `drivers/blkdev.c`, `drivers/blkpart.c`, `drivers/usb/usb_msc.c`, `drivers/usb/xhci.c` | USB disks in the disk table, hot-plug, superfloppy detection |

## The driver

* FAT12, FAT16 and FAT32, the type decided as Linux does: no 16-bit FAT size
  in the BPB means FAT32, otherwise the cluster count (fewer than 4085: FAT12).
  Sector sizes 512 to 4096, clusters up to 64 KiB, FAT32 with mirrored FATs or
  one active FAT (`BPB_ExtFlags`). exFAT is detected and refused with a message.
* Long names (VFAT): UTF-16 in the entries, UTF-8 in the VFS, surrogate pairs
  for characters outside the BMP; a long name whose checksum does not match its
  short entry is ignored (as Windows does). Names are case-insensitive and
  lookups match the long or the 8.3 name. Case folding covers ASCII, Latin-1,
  Latin Extended-A (Turkish ç ğ ö ş ü), Greek and Cyrillic; the Turkish dotted
  and dotless i (İ U+0130, ı U+0131) fold to themselves, so `ı` and `I` (and `i`
  and `İ`) are different names. Short names in OEM bytes are shown as code page
  437.
* New names: a valid 8.3 name in one case per part is stored as a short entry
  only, with the Windows NT lower-case flags (`readme.md`, `README.md`);
  anything else gets long-name entries and a short alias built as fatgen103
  describes (upper case, invalid characters to `_`, spaces and inner dots
  dropped, `~N` tail, the lowest free N). Characters `"*/:<>?\|` and control
  characters are refused (`EINVAL`); trailing dots are dropped.
* Directories grow by a cluster when they have no run of free entries long
  enough (a FAT12/16 root directory cannot: `ENOSPC`), up to 65536 entries.
* Files: clusters allocated from the FSInfo hint onwards; data moves straight
  between the caller's buffer and the device in runs of contiguous clusters;
  writing past the end zero-fills the gap; truncate frees or zero-extends.
  Sizes up to 4 GiB − 1 (`EFBIG` beyond). A file unlinked while open keeps its
  clusters until its last descriptor closes.
* rename: within and across directories, directories with their contents
  (".." follows), case-only changes, and over an existing entry: the existing
  short entry is pointed at the moved object in one sector write, so the
  target name never disappears, then the old entries are deleted. A directory
  cannot move below itself (`EINVAL`).
* Timestamps: creation, modification and access date; `utimensat` sets them.
  FAT stores local time without a zone; it is read and written as UTC here.
* Metadata (FAT, directories, boot sector, FSInfo) goes through a 256 KiB
  write-back sector cache that is flushed when each operation ends, with every
  FAT copy written for a mirrored FAT. Between system calls the volume is
  consistent; there is nothing for `sync` to do.
* Dirty flag: mounting read-write sets bit 0 of `BS_Reserved1` in the boot
  sector (the flag Linux and dosfstools use) and clears the clean-shutdown bit
  in FAT[1] (FAT16/32, the one Windows uses); `umount`, `remount,ro` and
  power-off clear both again. A volume that was already dirty when mounted is
  reported ("run fsck.vfat") and left dirty.
* FSInfo free count and next-free hint are kept up to date; when FSInfo has no
  valid count (and on FAT12/16), the first `statfs` counts free clusters.
* One node per directory entry, keyed by the entry's position on the volume
  and kept until umount (the mount table matches mountpoints by node
  identity); a rename re-keys it. One sleeping lock per mounted volume.
* Read errors end a read early (the bytes before the error are returned, as
  `fs/ext4.c` does), because some kernel callers of `vfs_read` take the result
  as a length; write errors return `EIO`.

Not supported: exFAT, symlinks and hard links (`EPERM`), device nodes, FIFOs
and sockets on FAT, `chown`, time zones, updating the FAT32 backup boot sector.

## Sources

Written from Microsoft's "FAT: General Overview of On-Disk Format" (fatgen103,
v1.03, December 2000): BPB and FSInfo layout, the FAT type by cluster count,
FAT12 entry packing, directory entries, long-name entries and their checksum,
and the basis-name and numeric-tail rules for short names. The boot sector's
dirty bit and the NT lower-case flags are documented by the Linux kernel's
`Documentation/filesystems/vfat.rst` and dosfstools' manual pages. The OSDev
wiki's FAT page was read for orientation. No code was copied from FatFs,
Linux's `fs/fat` (GPL), dosfstools or mtools; they were used as test tools
only (`mkfs.fat`, `fsck.fat`, `mcopy`, `mdir`, `mtype`).

## Tests

`make smoke-vfat` (`tools/smoke_vfat.py`, images from `tools/mkvfatimg.py` into
`build/vfattest/`; needs `mkfs.fat`, `fsck.fat`, `sfdisk` and mtools on the
host). See the README's status table for what it checks.
