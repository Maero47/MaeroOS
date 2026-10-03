# 64-bit file offsets

Files are no longer limited to 4 GiB − 1 bytes: sizes and offsets are 64-bit
from the system calls down to every filesystem driver.

## VFS

* `vfs_node_t.size` is `uint64_t`; `read_fn`/`write_fn` take a `uint64_t`
  offset (one call still moves at most 4 GiB − 1 bytes) and `truncate_fn` a
  `uint64_t` size (`fs/vfs.h`). `VFS_OFF_MAX` is 2^63 − 1 (a signed `loff_t`).
* A descriptor's position (`proc_file_t.offset`) is `uint64_t`.
* Drivers refuse a write at or past their largest file with `VFS_WRITE_EFBIG`
  and shorten a write that straddles it, and a truncate past it with `-EFBIG`
  (Linux `s_maxbytes`):

| Filesystem | Largest file |
|---|---|
| ext2/3/4, extents (`fs/ext2.c`) | 2^32 − 1 blocks, and 2^32 − 1 sectors of `i_blocks` (2 TiB with 4 KiB blocks) |
| ext2/3/4, block maps | 12 + p + p² + p³ blocks (p = block size / 4), within the same `i_blocks` bound; the triply indirect tree is allocated now |
| ext4 read-only driver (`fs/ext4.c`) | reads 64-bit sizes whole |
| exFAT | 2^32 − 1 clusters (ValidDataLength/DataLength were already 64-bit) |
| vfat | 4 GiB − 1 (`EFBIG` at 4 GiB − 1 and beyond) |
| tmpfs | 1 GiB (its page table is dense) |
| memfd | 4 GiB − 1 |
| block devices | the whole disk or partition (`/dev/sdX` reads and writes past 4 GiB) |

A file growing past 2 GiB − 1 on ext2/3/4 sets the `large_file` feature.
Growing a block-mapped ext2 file with `truncate` leaves a hole, as with extents.

## System calls (i386 ABI)

* `_llseek` (140) and `lseek` (19): the target is computed in 64 bits; past
  2^63 − 1 or below 0 is `EINVAL`. `lseek` returns `EOVERFLOW` when the result
  does not fit a 32-bit `off_t`, after moving the position (as Linux
  `ksys_lseek` does). `SEEK_DATA`/`SEEK_HOLE` treat the whole file as data.
  Pipes, sockets, epoll and eventfd descriptors are `ESPIPE`.
* `pread64`/`pwrite64` (180/181): the full 64-bit offset on every file
  (negative: `EINVAL`, pipe or socket: `ESPIPE`).
* `truncate64`/`ftruncate64` (193/194) and `fallocate` (324) take 64-bit
  lengths; `fallocate` mode 0 grows the file (zeroes, no reservation),
  `FALLOC_FL_KEEP_SIZE` does nothing, other modes are `EOPNOTSUPP`.
* `stat64`/`fstat64`/`lstat64`/`fstatat64`/`statx` report the exact size; the
  old `stat`/`fstat`/`lstat` (106/108/107) are `EOVERFLOW` past 2 GiB − 1.
* `O_LARGEFILE` (0100000): `open` of a regular file larger than 2 GiB − 1
  without it is `EOVERFLOW`, and `write` without it stops at 2 GiB − 1
  (`EFBIG`) — Linux `generic_file_open`/`generic_write_check_limits`. musl and
  LFS glibc builds always pass it; the in-tree libc (32-bit `off_t`) does not.
  `F_GETFL` reports it, `F_SETFL` keeps it.
* `sendfile` (187, 32-bit offset, stops at 2 GiB − 1 with `EOVERFLOW`),
  `sendfile64` (239) and `copy_file_range` (377): between regular files;
  other descriptors are `EINVAL`, on which busybox and coreutils fall back to
  read/write.
* `mmap2` page offsets reach 2^44 bytes; mappings keep a 64-bit file offset.
* Record locks (`F_SETLK64`/`F_GETLK64`/`F_OFD_*`) use 64-bit ranges; ranges
  past 2^63 − 1 are `EOVERFLOW`, and the 32-bit `F_GETLK` is `EOVERFLOW` when
  the conflicting lock does not fit.

## Test

`make smoke-largefile` (in `make check`, `tools/smoke_largefile.py`) with
`testfiles/lfprobe` (`ports/largefile/lfprobe.c`, rebuilt with
`ports/largefile/build.sh`; it passes on a Linux host too, which does not
report `EOVERFLOW` from compat `lseek`).
