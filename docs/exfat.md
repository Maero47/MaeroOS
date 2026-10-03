# exFAT (big USB sticks and SD cards)

Most USB sticks over 32 GiB and SDXC cards come formatted as exFAT. The kernel
reads and writes it through `fs/exfat.c`, mounted with `mount(2)` like vfat:

```sh
mount -t exfat /dev/sdb1 /mnt/usb          # read-write
mount /dev/sdb1 /mnt/usb                   # the same: busybox finds exfat via /proc/filesystems
mount -t exfat -o ro /dev/sdc /mnt/card    # read-only; a stick without a partition table is /dev/sdX
mount -t exfat -o uid=1000,gid=1000,umask=077 /dev/sdb1 /mnt/usb
```

The options are vfat's (`uid=`, `gid=`, `umask=`, `dmask=`, `fmask=`): exFAT
has no owners or Unix modes, so every file shows the mount's owner and the
masked mode, minus the write bits when the read-only attribute is set (`chmod
a-w` sets it, `chmod u+w` clears it). Any disk the block layer knows works:
IDE, AHCI, NVMe, USB mass storage (hot-plug as described in `docs/vfat.md`), a
partition or the whole device. The same rules as vfat apply: one mount per
device (`EBUSY`), raw writes to a disk with a mounted filesystem are `EBUSY`,
`umount` is `EBUSY` while a file is open or a working directory is inside,
`remount,ro`/`remount,rw` flush and flip the dirty flag, and `poweroff` /
`reboot` flush every writable mount and mark it clean.

## The driver

* **Boot region**: the main boot sector's signature, `MustBeZero` field,
  revision 1.x, sector size 512 to 4096 bytes, clusters up to 32 MiB, and the
  boot checksum sector (the 32-bit checksum over sectors 0-10, VolumeFlags and
  PercentInUse excluded). If the main region fails and the backup region
  (sectors 12-23) is good, the volume is mounted read-only from the backup
  (`EROFS` for read-write; busybox `mount` then retries read-only itself), and
  `remount,rw` is refused (`EROFS`) as well: VolumeDirty would go into the
  broken main boot sector. The
  FAT, the cluster heap and the cluster count must fit inside the volume, and
  the volume inside its partition.
* **Root directory**: a FAT chain; a chain that loops or exceeds 256 MiB is
  refused at mount. Its allocation bitmap entry (contiguous, as every
  formatter writes it) and up-case table entry are read from it. An up-case
  table that does not match its checksum, or a missing one, is replaced by a
  built-in mapping (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic) and the
  volume is read-only, so no name is written with a hash other systems would
  not compute; `remount,rw` is refused too. Two FATs (TexFAT) are read-only
  in the same way.
* **Names**: UTF-16 in the file-name entries (up to 255 units, 15 per entry),
  UTF-8 in the VFS, surrogate pairs for characters outside the BMP. Lookups
  compare names through the volume's up-case table, so they are
  case-insensitive the way Windows sees them: Turkish `ç ğ ö ş ü` fold to
  `Ç Ğ Ö Ş Ü`, and dotted `İ` and dotless `ı` fold to themselves (the table
  mkfs.exfat and Windows write), so `ı` and `I` are different names, `i` and
  `I` the same. New names get their NameHash from the same table. Characters
  `"*/:<>?\|` and control characters are refused (`EINVAL`); trailing dots are
  dropped. A name whose UTF-8 form is longer than 255 bytes (the VFS's limit)
  is not shown.
* **Entry sets**: a file entry is used only when its SecondaryCount is 2 to
  18, the stream extension follows, the name entries cover NameLength, every
  secondary is in use, and the SetChecksum matches; anything else is skipped
  (and logged once). Every write recomputes the SetChecksum. New sets are
  written secondaries first, the file entry last.
* **Allocation**: the bitmap decides what is free (through the metadata
  cache). Files and directories start as NoFatChain (contiguous) runs and stay
  so while the next cluster is free; when it is not, the existing run is
  written into the FAT and the file continues as a FAT chain. Freeing clears
  bitmap bits only (the FAT entries of free clusters are undefined).
  `statfs` counts free clusters once from the bitmap, then keeps the count;
  `umount` stores PercentInUse.
* **Lengths**: DataLength and ValidDataLength are 64-bit. Reads past
  ValidDataLength return zeros; a write past it zero-fills the gap first;
  `truncate` up allocates clusters and leaves ValidDataLength where it was (the
  new part reads as zeros without being written). VFS offsets are 64-bit, so
  files past 4 GiB are read, written, created and truncated whole (`make
  smoke-largefile`); a file stops at 2^32 − 1 clusters (`EFBIG`).
* **Directories**: no `.` and `..` on disk (they are made up for `readdir`);
  a directory grows by a zeroed cluster when no run of free entries is long
  enough, up to 256 MiB.
* **rename**: within and across directories, directories with contents, a
  case-only change, and over an existing entry (whose clusters are freed, or
  freed at its last close if open). The new set is written before the old and
  the replaced ones are deleted, all in one locked operation flushed at its
  end. A directory cannot move below itself (`EINVAL`). Between two mounted
  volumes it is `EXDEV` (checked in `rename(2)` against the mount instances,
  since every exFAT volume shares the driver's rename hook, and again in the
  driver); `mv` then copies.
* **Timestamps**: written in UTC with UtcOffset `0x80` (valid, +0) and the
  10 ms field; read with the stored offset applied (a set without a valid
  offset is taken as UTC). Create, modify and access times; `utimensat` sets
  modify and access.
* **VolumeDirty**: set in the main boot sector while mounted read-write,
  cleared by `umount`, `remount,ro` and power-off (VolumeFlags is outside the
  boot checksum, so nothing else changes). A volume that was already dirty is
  reported ("run fsck.exfat") and left dirty.
* **Crafted media**: every on-disk value is checked before use. Cluster
  numbers must lie in the heap; a stream whose first cluster, length or
  NoFatChain run does not fit the heap, or overlaps the bitmap, up-case table
  or root cluster, marks the file "impossible" (reads give nothing, writes
  `EIO`, unlink removes the entries without freeing clusters). Chain walks are
  bounded by the file's length, and a chain shorter than its length is `EIO`.
  Copies into fixed buffers (entry sets, names) are bounded by the buffer and
  the entry count, never by an on-disk length alone; entry sets live in heap
  buffers, not on the kernel stack. The bitmap, up-case and root clusters are
  never freed through a file that claims them.
* Metadata goes through a 256 KiB write-back sector cache flushed when each
  operation ends; data moves straight between the caller's buffer and the
  device in runs of contiguous clusters. One node per entry set, keyed by the
  file entry's position (re-keyed by rename); one sleeping lock per volume.

Not supported: TexFAT writes (two FATs: read-only), a fragmented allocation
bitmap, symlinks and hard links (`EPERM`), device nodes, FIFOs and sockets,
`chown`, vendor extension entries (kept on reads, dropped by a rename), offsets
past 4 GiB through the VFS, updating the backup boot region.

## Tests

`make smoke-exfat` (part of `make check`, `tools/smoke_exfat.py`):

* `tools/mkexfatimg.py` makes the volumes with `mkfs.exfat` and fills them
  with `tools/exfatimg.py`, a small independent exFAT writer and checker
  (exfatprogs cannot copy files in without mounting): an MBR partition with
  4 KiB clusters (Turkish, non-BMP and long names, a 300-entry directory, a
  FAT-chain file, a ValidDataLength < DataLength file), a superfloppy USB
  stick with 32 KiB clusters, a 64 GiB sparse disk with 4096-byte sectors and
  a 5 GiB file, and crafted volumes.
* The guest mounts the partition on AHCI, the big disk on AHCI and the stick
  on qemu-xhci, compares every md5, runs the write workload (create, append,
  contiguous-to-chain conversion, write past the end and past
  ValidDataLength, truncate both ways, every rename case, unlink, `rm -r`,
  rmdir, utimes, statfs, unlink-while-open, umount/mount, ro/remount,rw,
  EBUSY), writes a marker at 4 GiB − 8 KiB of the 5 GiB file, and powers off
  with the stick and the big disk still mounted.
* On the host, `fsck.exfat -n` must be clean for every volume, and
  `tools/exfatimg.py` must find exactly the expected files and contents,
  matching SetChecksums and NameHashes, a bitmap that marks exactly the
  clusters in use, and VolumeDirty clear; the 5 GiB file keeps its length and
  the marker lands at the right byte.
* A second boot mounts crafted volumes: malformed entry sets (bad checksum, too
  few secondaries, a NameLength past its entries, clusters outside the heap,
  a 2^62-byte length, short and looping chains, a directory on the root's
  cluster, `/` and NUL in a name, stray secondaries), boot regions with a
  broken main checksum (read-only from the backup), a ClusterCount past the
  volume, root cluster 1 and both checksums broken (refused), an up-case table
  that fails its checksum (read-only), and two fuzzed volumes, read and
  written without a crash.

## Sources

Written from Microsoft's "exFAT file system specification" (published 2019,
learn.microsoft.com/windows/win32/fileio/exfat-specification): the boot
sector and boot checksum, FAT entries, the allocation bitmap, the up-case
table and its compression and checksum, the directory entry types, the
GeneralSecondaryFlags (AllocationPossible, NoFatChain), the SetChecksum and
NameHash algorithms, the timestamp, 10 ms and UtcOffset fields, and
VolumeFlags. The OSDev wiki's exFAT page was read for orientation. The Linux
kernel's `fs/exfat` (GPL) was not used as a source; no code was copied from it,
from exfatprogs or from any other implementation. exfatprogs (`mkfs.exfat`,
`fsck.exfat`, `dump.exfat`) is used as a host test tool only.
