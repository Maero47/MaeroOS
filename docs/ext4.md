# mount(2), partitions and ext4

## Using it

```sh
cat /proc/partitions                   # hda, hdb, hdb1, hdb2, hdc5, ...
mkdir -p /mnt
mount -t ext4 /dev/hdb2 /mnt           # busybox: "is write-protected, mounting read-only"
mount -t ext2 /dev/sdb1 /data          # read-write (fs/ext2.c), see "Read-write" below
mount -o ro -t ext4 /dev/hdc5 /mnt2    # the same, without the retry
cat /proc/mounts
umount /mnt
mount -t tmpfs none /scratch           # also: proc, devtmpfs, --bind
mount -o remount,ro /scratch
```

Only root may mount or unmount (`EPERM` otherwise). In QEMU, extra disks go on
the other IDE positions: `-drive file=x.img,format=raw,index=1` is `hdb`,
`index=2` is `hdc`, `index=3` is `hdd`. AHCI disks are `sda`, `sdb`, ... (on
q35 `index=N` is AHCI port N), NVMe namespaces `nvme0n1`, ...; their
partitions are `sdb2`, `nvme0n1p5`.

## Pieces

| File | Role |
|---|---|
| `drivers/blkdev.c` | the table of every disk (IDE, then AHCI, then NVMe) with read/write, size and dev_t; which one is the boot disk |
| `drivers/ata.c` | probes the primary slave and the secondary channel besides the boot disk; `ata_dev_read/write(dev, ...)` with plain LBA28 PIO for them (ATAPI devices are skipped) |
| `drivers/blkpart.c` | one `blkpart_t` per disk and partition, a view over `blkdev.c`'s table: MBR primaries 1-4, logical partitions from 5 (walking the EBR chain), GPT entries (header and entry-array CRC32 checked, backup header as fallback); `/dev/<name>` nodes (raw access for root; writes to the disk mounted at `/disk` are `EBUSY`; `docs/install.md`) and `/proc/partitions` |
| `fs/vfs.c` | the mount table: `vfs_mount_add/find/remove`, crossing in the path walk, the mount each lookup ends on (for `MS_RDONLY`), `/proc/mounts` |
| `fs/mount.c` | `mount_do()`/`umount_do()`: types `ext4`/`ext3`/`ext2` (block device source), `tmpfs`, `proc`, `devtmpfs`; `MS_REMOUNT`, `MS_BIND`, propagation flags (accepted, no-op) |
| `fs/ext2.c` | the read-write ext2 driver: `/disk` and every read-write mount |
| `fs/ext4.c` | the read-only ext2/3/4 driver |
| `proc/syscall.c` | `mount` (21), `umount` (22), `umount2` (52); `EROFS` on read-only mounts for open-for-write, create, mkdir, mknod, unlink, rmdir, rename, symlink, link, truncate, chmod, chown, the utime family, and through descriptors for fchmod, fchown, ftruncate, fallocate, futimens |
| `userspace/mntprobe` | bind pinning, umount-by-name after slot reuse, `EROFS`/`EBUSY` through descriptors (run by `smoke-ext4`) |

A chrooted process (`chroot(2)`) resolves mount and umount targets, and bind
sources, from its own root like any other path, so it can only reach mounts
inside its jail, and `..` from a mount's root inside the jail goes back to the
directory it is mounted on. umount's "a cwd inside keeps it busy" check
resolves each process's cwd from that process's root (`smoke-alpine` tests
this inside the Alpine chroot).

### Mount table semantics

* Mountpoints are matched by node identity. Every filesystem here keeps one
  node per object for as long as it is mounted (initrd/devfs/procfs nodes are
  static, tmpfs nodes live with their link, ext2 and ext4 cache one node per
  inode), so this is stable.
* The walk keeps a stack of (parent, mount) pairs, so `..` from a mounted root
  goes back to the directory that holds the mountpoint.
* Mounts stack: mounting on a directory that is itself a mounted root covers
  it; `umount` of the lower one is `EBUSY` while the upper one exists.
* `umount` is `EBUSY` while a file of the filesystem is open (descriptor or
  mapping) or a process's cwd is inside it; `MNT_DETACH` removes the mount
  anyway and leaves a still-busy instance allocated.
* The boot mounts (`/disk`, `/tmp`, `/dev`, `/proc`, made by `vfs_mount()`)
  are listed in `/proc/mounts` but cannot be unmounted.
* `rmdir`/`unlink` of a mountpoint is `EBUSY`.
* A bind mount pins the mount its source directory lives on: that mount is
  `EBUSY` to unmount while the bind exists.
* `umount <path>` removes the mount the path walk crossed last to reach
  `<path>` (mounts are numbered in the order they are made), so a bind of a
  mounted root is told apart from the mount itself.
* Open files remember the mount they were opened on: `fchmod`, `fchown`,
  `ftruncate` and `fallocate` through a descriptor are `EROFS` on a
  read-only mount, and `remount,ro` is `EBUSY` while a file of the mount is
  open for writing (as on Linux), so no descriptor can write to it.

## The ext4 driver

Read support:

* superblock rev 0 and dynamic; block sizes 1 KiB to 64 KiB
* 32- and 64-byte group descriptors (`64bit`), `flex_bg`, `meta_bg`,
  `sparse_super`/`sparse_super2` (for locating `meta_bg` descriptors)
* extent trees of any depth (index and leaf nodes, holes, unwritten extents
  read as zeros) and classic direct/indirect block maps
* file sizes from `i_size_high` (regular files, and directories with
  `largedir`); offsets in the VFS are 32-bit, so a file past 4 GiB shows its
  first 4 GiB
* directories: linear scan for `readdir` (with a cursor so listing a large
  directory is linear, not quadratic), htree (`dir_index`) for lookups:
  legacy, half-MD4 and TEA hashes, signed and unsigned variants, the hash seed,
  up to three index levels (`largedir`), names whose hash continues into the
  next leaf; a malformed index falls back to a linear scan and logs it (so do,
  silently, the rare cases the index walk does not follow: a collision run
  that crosses an index node boundary)
* fast and slow symlinks
* `metadata_csum`: the superblock checksum is verified at mount (mismatch:
  not mounted), every inode's checksum on read and every extent block's
  (mismatch: the read fails with an error and is logged); the checksum seed
  comes from the UUID or `s_checksum_seed` (`metadata_csum_seed`)
* device numbers of character/block special files (`st_rdev`)

Refused at mount: `needs_recovery` (the journal must be replayed first,
`EUCLEAN`), and the incompatible features `compression`, `journal_dev`,
`dirdata`, `inline_data`, `encrypt`, `casefold` and any unknown bit
(`EOPNOTSUPP`). A filesystem that was not cleanly unmounted, or that records
errors, mounts with a warning. Unknown read-only-compatible features do not
matter to a reader.

### Read-write: the ext2 driver

A read-write mount request of type `ext2`, `ext3` or `ext4` goes to
`fs/ext2.c`, the driver behind `/disk`, which keeps one instance per mounted
filesystem (geometry, block cache, node cache, open-inode table). It takes the
filesystem when:

* the only incompatible feature is `filetype` (no `extent`, `64bit`,
  `flex_bg`, `meta_bg`, `inline_data`, ...), and `needs_recovery` is not set;
* every read-only-compatible feature is one it keeps intact: `sparse_super`,
  `large_file`, `btree_dir`, `dir_nlink`. Others (`metadata_csum`,
  `gdt_csum`/`uninit_bg`, `huge_file`, `extra_isize`, ...) would be left
  inconsistent by its writes.

So plain ext2 and ext3 (an empty journal is written around, as Linux's ext2
driver does) mount read-write; a modern ext4 gets `EROFS` and busybox retries
read-only, which is this driver's. `mount -t ext2 -o ro` stays with the ext2
driver when it can read the filesystem, so `remount,rw` works there.

While mounted read-write the superblock is marked not clean (mount count and
time updated) and marked clean again at `umount` or `remount,ro`; writes are
synchronous (the disk drivers flush every write), so that is all `umount` has
to sync. A directory with an htree index loses its index flag when the driver
changes it (it stays a valid linear directory; Linux's ext2 does the same),
deleting an inode releases its extended-attribute block, and `mkdir`/`rmdir`
keep the group's directory count. `umount` is `EBUSY` while a file or
directory of the instance is open. Raw writes through `/dev/<name>` to a
mounted device, or to a disk or partition overlapping one, are `EBUSY`, as
they are for `/disk`'s disk.

`make smoke-ext2rw` (`tools/smoke_ext2rw.py`, images in `build/ext2rw/`)
mounts an ext2 on AHCI and an ext3 on NVMe read-write together and checks the
result on the host with `e2fsck -fn` and `debugfs`.

### Why the ext4 driver is read-only

Every Linux ext4 filesystem made by a distribution has a journal
(`has_journal`). Writing to it correctly needs either jbd2 transactions or
the guarantee that the journal is empty and stays consistent with metadata
written around it (which is what the ext2 driver relies on for ext3, above). Writing directly and clearing `has_journal` would change
the filesystem behind the owner's back; writing without updating the
metadata checksums (`metadata_csum` is the default) would corrupt it. Neither
is acceptable, so the driver never issues a write: a read-write mount request
gets `EROFS` (busybox `mount` then retries read-only), `remount,rw` gets
`EROFS`, and every mutating operation on its nodes returns `EROFS` as well.
`smoke-ext4` checks afterwards that both test filesystems are byte-identical
to the images they were copied from.

What write support would need, in order: block and inode bitmap allocation
with group-descriptor and bitmap checksums (crc32c, already here), extent
insertion and splitting with extent-block checksums, directory entry insertion
in linear and htree directories (with leaf splitting and the dx tail
checksum), orphan handling, and jbd2 transactions (descriptor, data and commit
blocks, with the v3 checksums) so that a crash leaves a replayable journal.

### Sources

Written from the on-disk format as documented by the Linux kernel's
`Documentation/filesystems/ext4/` (kernel.org, "ext4 Data Structures and
Algorithms"; documentation, not code), the UEFI specification (GPT) and RFC
1320 (MD4, which the half-MD4 directory hash shortens to three rounds over
eight words). FreeBSD `sys/fs/ext2fs` (BSD-2-Clause) and HelenOS
`uspace/lib/ext4` (BSD-3-Clause) were read for layout details only. No code
was copied from any source; in particular nothing from Linux or lwext4 (GPL).

## Tests

`make smoke-ext4` (`tools/smoke_ext4.py`, images from `tools/mkext4img.py`
into `build/ext4test/`; needs `mke2fs`, `e2fsck`, `debugfs` and `sfdisk` on
the host). See the README's status table for what it checks.
