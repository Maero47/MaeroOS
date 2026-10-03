# mount(2), partitions and ext4

## Using it

```sh
cat /proc/partitions                   # hda, hdb, hdb1, hdb2, hdc5, ...
mkdir -p /mnt
mount -t ext4 /dev/hdb2 /mnt           # read-write (fs/ext2.c), see "Read-write" below;
                                       # a feature it cannot write: busybox retries read-only
mount -t ext2 /dev/sdb1 /data          # read-write ext2
mount -o ro -t ext4 /dev/hdc5 /mnt2    # read-only (fs/ext4.c)
sync                                   # commits ext3/ext4 journals now (else within 1 s)
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
  anyway; a still-busy instance stays allocated and keeps its device taken (a
  second mount of it, and raw writes through `/dev`, are `EBUSY`) until it is
  idle, when the next such check releases it.
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
  `largedir`); VFS offsets are 64-bit, so a file past 4 GiB reads whole
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
filesystem (geometry, block cache, node cache, open-inode table). It writes
ext2, ext3 and a typical ext4 as `mke2fs -t ext4` makes it today:

| feature | what the driver does |
|---|---|
| `extent` | files and directories it creates get extent trees; lookups walk the tree; an append contiguous on the disk lengthens the last extent in place; anything else (a hole filled, an unwritten extent written, a truncate) rebuilds the tree from its extent list at the smallest depth that holds it, reusing the old tree blocks |
| `metadata_csum`, `metadata_csum_seed` | crc32c of the superblock, group descriptors, block and inode bitmaps (in the descriptors), inodes, extent blocks and directory leaf blocks (the 12-byte tail), recomputed on every write |
| `64bit` | 64-byte group descriptors; filesystems of 2^32 blocks or more are refused (i686: block numbers are 32-bit here) |
| `flex_bg`, `uninit_bg` semantics | allocation follows the descriptors' bitmap and table locations; `BLOCK_UNINIT` bitmaps are built from the layout (and checked against the free count) on first use, `INODE_UNINIT` ones start empty, `bg_itable_unused` moves past each inode handed out |
| `dir_index` | lookups go through the index: the name's hash (legacy, half-MD4 or TEA, signed or unsigned, with the superblock's seed) is binary-searched in the root and up to two interior levels (`largedir`) down to a leaf, and on into the following leaves while the next index entry continues the same hash (a collision run); `.` and `..` come from the root block. An index that does not hold together (root info, counts or limits, a block past the directory, an interior block that is not one) is not trusted: the directory is scanned linearly, the error logged, and the superblock marked as having errors so the next `e2fsck` checks it, as Linux does. Inserts: a new name goes into the leaf its hash selects; a full leaf splits at the median hash into a new block and the index gains an entry; a full root moves its entries into a new interior block (one more level), a full interior block splits; root and interior blocks get their dx tail checksum. Deeper than one interior level, the index is dropped instead (turning the index blocks into plain checksummed leaves). Unlink and rename find the entry's leaf through the index too. A linear directory whose one block is full becomes indexed when the next name arrives (Linux's `make_indexed_dir`): its entries move into a new block 1, block 0 becomes the root with the default hash, all in one transaction |
| `orphan_file` | see "Orphans" below |
| `huge_file`, `extra_isize`, `dir_nlink`, `large_file`, `sparse_super`, `resize_inode` | kept intact; new inodes get `i_extra_isize` and a creation time |
| `has_journal` | jbd2, below |

Refused for writing (`EROFS`, busybox retries read-only and `fs/ext4.c`
serves it): `meta_bg`, `inline_data`, `encrypt`, `casefold`, `bigalloc`,
`quota`, `project`, `sparse_super2`, `gdt_csum` without `metadata_csum`, an
external journal, a journal with v1 checksums. `mount -t ext2 -o ro` stays with the ext2 driver when it
can read the filesystem, so `remount,rw` works there.

**The journal.** Every metadata block an operation changes goes into the
running transaction (and the block cache), not to its place; file data goes
to its place at once (ordered mode). A transaction commits once a second
(at the end of an operation, or from the `kjournald` thread when nothing else
happens), when it is as large as the journal allows (half the log, at most
1024 blocks), at `sync`/`fsync`/`syncfs`, at `umount`/`remount,ro`/power-off,
and before the allocator reuses blocks it freed: the journal superblock is
pointed at the log, then descriptor blocks with the tags and the block copies
(escaped when they start with the jbd2 magic), then the commit block; only
then are the blocks written in place and the journal marked empty again. The
format follows the journal's own features: checksum v3 (what Linux turns on
for a `metadata_csum` filesystem) or v2, `64bit` tags, or none. While
mounted read-write the superblock carries `needs_recovery`, as Linux has it,
so a log left behind by a crash is replayed rather than discarded.

**Replay.** A read-write mount whose journal is not empty (or that says
`needs_recovery`) replays it first: the log is scanned from `s_start` for
transactions closed by a commit block (whose checksum must match), revoke
records are collected, and every logged block that is not revoked by its own
or a later transaction and whose tag checksum matches is written to its
place. A read-only mount of such a filesystem is refused (`EUCLEAN`), so it
never shows a state the journal would change.

While mounted read-write the superblock is marked not clean and marked clean
again at `umount`, `remount,ro` or power-off. Deleting an inode releases its
extended-attribute block, `mkdir`/`rmdir` keep the group's directory count.
`umount` is `EBUSY` while a file or directory of the instance is open. Raw
writes through `/dev/<name>` to a mounted device, or to a disk or partition
overlapping one, are `EBUSY`, as they are for `/disk`'s disk.

**Orphans.** An inode whose last name goes while a descriptor or mapping
still holds it, an inode being deleted with a journal (its blocks may be
freed over several transactions), and a regular file being cut short over
several transactions are recorded on disk until that is finished: in the
orphan file (`orphan_file`: per block an array of inode numbers, then the
magic `0x0B10CA04` and a crc32c of the block number and contents seeded like
the orphan file's inode; adding the first entry sets `orphan_present`, taking
the last one out clears it), or, without one or when it is full, in the old
list that starts at `s_last_orphan` and continues through each inode's
`i_dtime`. Each change goes into the transaction of the operation that makes
it. While a truncate frees blocks in steps, the inode each step commits
already carries the new size. A read-write mount (and `remount,rw`) finishes
what it finds after the journal replay: an inode without links is deleted
(its entry cleared in the same transaction as the inode is freed), one with
links is cut to its `i_size` (the tail of the last block zeroed); it logs
`orphans: N deleted, M truncated`. Plain ext2 keeps no orphan record (as
Linux's ext2 driver), so there a file unlinked while open and lost to a crash
stays allocated until `e2fsck`.

**`/disk`.** An ext3/ext4 filesystem at `/disk` (the root `maeros-install`
writes, or `root=` naming one) is mounted like a read-write `mount(2)`: the
same feature checks (a feature that cannot be written leaves `/disk`
read-only), the journal replayed if needed, `needs_recovery` while mounted,
the orphans finished, the flusher started once processes exist; `reboot(2)`
(power-off, restart, halt) commits it and marks it clean
(`ext2_shutdown()`), and `/proc/mounts` lists it as `ext4` (`ext3`). A plain
ext2 `/disk` is handled as before.

`mount -o x4crash` is a test hook: the commit that ends the next `write(2)`
stops before writing anything in place and the instance then refuses all
writes, as if the power had gone; `smoke-ext4rw` has `e2fsck` replay the log.
`x4crashunlink` and `x4crashtrunc` do the same at the first commit inside the
next `unlink(2)` or truncate.

`make smoke-ext2rw` (`tools/smoke_ext2rw.py`, images in `build/ext2rw/`)
mounts an ext2 on AHCI and an ext3 on NVMe read-write together;
`make smoke-ext4rw` (`tools/smoke_ext4rw.py`, `build/ext4rw/`) does the same
for `mkfs.ext4` filesystems with and without a journal, a dirty journal and
the simulated power loss. Both check the result on the host with
`e2fsck -fn` and `debugfs`.

`make smoke-ext4rw` also grows a new directory to 5000 names (it becomes
indexed; `debugfs htree`, `e2fsck -fn`), times 10000 creates and 20000
lookups (half of them misses) with `fsprobe mkfiles`/`lookups` in an indexed
and a `^dir_index` directory, and loses power with a file open and
unlinked, in the middle of freeing a file (old orphan list) and in the middle
of a truncate; a second boot finishes all three.

Lookups in a 10000-name directory (4 KiB blocks, KVM, `fsprobe lookups`:
10000 hits and 10000 misses):

| kernel | create 10000 | 20000 lookups | per lookup |
|---|---|---|---|
| before (linear lookups, main `a593519`) | 13.8 s | 4.33 s | 216 us |
| after, indexed directory | 3.9-5.1 s | 0.50-0.62 s | 24-30 us |
| after, `^dir_index` filesystem (linear) | 10.2 s | 3.34 s | 167 us |

Not done: inserting into an htree deeper than one interior level (the index
is dropped instead), fast commits, and per-file `fsync` (it commits
everything).

### The read-only driver

`fs/ext4.c` never issues a write: it serves `mount -o ro` of ext2/3/4 and
the read-only fallback for what the ext2 driver will not write; a read-write
mount request it gets is `EROFS`, as are `remount,rw` and every mutating
operation on its nodes. `smoke-ext4` checks afterwards that both of its test
filesystems are byte-identical to the images they were copied from.

### Sources

Written from the on-disk format as documented by the Linux kernel's
`Documentation/filesystems/ext4/` (kernel.org, "ext4 Data Structures and
Algorithms"; documentation, not code), the UEFI specification (GPT) and RFC
1320 (MD4, which the half-MD4 directory hash shortens to three rounds over
eight words), and for the journal `Documentation/filesystems/ext4/journal.rst`
("Journal (jbd2)"), and for the orphan file and orphan list
`Documentation/filesystems/ext4/` ("Orphan File", "Super Block": `s_last_orphan`,
`s_orphan_file_inum`); the orphan-file block checksum was checked against what
mke2fs 1.47 writes. FreeBSD `sys/fs/ext2fs` (BSD-2-Clause) and HelenOS
`uspace/lib/ext4` (BSD-3-Clause) were read for layout details only. No code
was copied from any source; in particular nothing from Linux or lwext4 (GPL).
The crc32c table is generated at run time from the Castagnoli polynomial.

## Tests

`make smoke-ext4` (`tools/smoke_ext4.py`, images from `tools/mkext4img.py`
into `build/ext4test/`; needs `mke2fs`, `e2fsck`, `debugfs` and `sfdisk` on
the host). See the README's status table for what it checks.
