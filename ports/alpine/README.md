# Alpine Linux x86 userland

MaeroOS runs unmodified Alpine Linux x86 (32-bit, musl) packages: bash, GNU
coreutils, python3, vim, less, nano, git, OpenSSH's client and apk itself, which
installs and removes packages in the guest.

    make smoke-alpine             # build disk-alpine.img if needed, boot, check
    python3 ports/alpine/prepare.py   # just build disk-alpine.img

To use it by hand, boot with the image as the disk and enter the root:

    qemu-system-i386 -kernel kernel.elf -initrd initrd.tar \
        -drive file=disk-alpine.img,format=raw,index=0,media=disk \
        -serial stdio -m 1024M
    # log in as root/root, then:
    toybox chroot /disk/alpine /bin/bash -l
    apk add tree && tree /etc/apk && apk del tree

## Layout decision: a chroot, not the root filesystem

The Alpine root is the directory `/alpine` on its own ext2 image, mounted
like any disk at `/disk`, and you enter it with `chroot /disk/alpine`.
MaeroOS keeps its initrd root, its `init`, `getty`, `login`, desktop and
`/etc`.

The other option was to put Alpine at the top of the disk. The disk is
already the *root overlay* (`fs/vfs.c`): a path outside `/dev`, `/proc`,
`/tmp` and `/disk` is looked up on the disk first and on the initrd
second. That would have put Alpine's `/etc/passwd`, `/etc/shadow`,
`/bin/sh` and `/sbin/init` (busybox) in front of MaeroOS's own, which
breaks login, init and every existing smoke suite. A chroot keeps the two
userlands apart. The only kernel work it needs is `chroot(2)`, which
apk-tools also calls itself (`chroot(".")` around package scripts with
`--root`).

How the chroot works (`proc/syscall.c` `sys_chroot`, `fs/vfs.c`
`vfs_lookup`):

- `chroot()` resolves the directory once and pins its node (`proc->root_node`,
  held with `vfs_retain` like an open file's node). Renaming the
  directory afterwards, or putting a symlink to `/` where it was, does
  not move a running jail. fork and clone take their own reference, and
  exit releases it.
- Every path the process passes in is relative to that node, and so are
  its cwd and fd paths. `vfs_lookup()` starts the walk there. `..` at the
  top stays there, and absolute symlink targets go back through the same
  lookup, so neither one leaves the root.
- `/dev` and `/proc` inside a chroot are the global ones. MaeroOS has no
  mount namespaces or bind mounts to put them there, and apk, python and
  ssh need `/dev/urandom`, `/dev/null` and `/proc/self`. A `..` that
  climbs out of them comes back to the chroot's `/`, not the global one,
  so a symlink to `/proc/..` cannot lead out. The Alpine root's own `/tmp`
  is a directory on the disk.
- `chroot()` resets the cwd to `/`. Linux leaves the cwd where it was, but
  here the cwd is a path string, and the old string would name a
  different directory inside the new root. A nested `chroot()` resolves
  its argument inside the current root. Only euid 0 may chroot (EPERM
  otherwise).
- Unlike Linux, a descriptor opened *before* `chroot()` does not reach
  the old tree: `fchdir`/`*at` calls resolve it by its path string, inside
  the new root. This is stricter than Linux, where such a descriptor is
  the classic chroot escape.

Probe `p31_chroot` covers all of this, including a rename and a symlink to
`/` under a running jail and symlinks to `/proc/..` and `/dev/../etc`
inside one.

## How the image is built (`prepare.py`)

1. Download the Alpine 3.22.6 x86 minirootfs (a permanent release file)
   and `apk-tools-static-2.14.12-r0.apk`. Both must match the sha256 in
   `alpine.lock`. The minirootfs carries the Alpine signing keys
   (`/etc/apk/keys`).
2. Run `apk.static --root <tree> add bash coreutils python3 vim less nano
   git openssh-client` on the host, inside `unshare -r` (an i386 static
   binary that x86-64 Linux runs). apk itself verifies the signed APKINDEX
   and every package signature against those keys.
3. Compare the installed `name-version` set and every downloaded `.apk`
   sha256 with `alpine.lock`. Downloads are cached in `ports/alpine/cache`
   (gitignored), so later runs work with `ALPINE_OFFLINE=1`.
4. `apk fetch tree` into `/repo/main/x86` next to the signed main
   APKINDEX. `/etc/apk/repositories` points at `/repo/main`. This is the
   guest's offline repository, and the smoke test installs and removes
   `tree` from it. apk verifies index and package signatures inside the
   guest too.
5. Build the image with `mke2fs -d <tree> disk-alpine.img` (1 KiB blocks,
   like `disk.img`) under `unshare -r`, so every file is owned by root.
   The image is about 256 MiB, with a 102 MiB tree. It takes about 2 s
   with a warm cache.

Alpine's stable mirrors delete superseded builds (`-rN` bumps), so a pin
can become unfetchable. The cache keeps working. On a fresh clone after
such a bump, `ALPINE_RELOCK=1` takes the mirror's current versions and
rewrites `alpine.lock`, and the diff of that file shows what changed.

`ALPINE_BRANCH` picks the release. The default is `v3.22` (apk-tools
2.14), with lock file `alpine.lock`. `v3.24` (apk-tools 3.0.8, the current
release) uses `alpine-v3.24.lock`. Both pass `make smoke-alpine`:

    ALPINE_BRANCH=v3.24 ALPINE_IMG=build/alpine324.img python3 ports/alpine/prepare.py
    ALPINE_IMG=build/alpine324.img python3 tools/smoke_alpine.py --net

`--net` (opt-in) adds an RTL8139 and serves the staged repo over HTTP from
the host. The guest also runs `apk add -X http://10.0.2.2:<port>/main tree`,
which exercises apk's own HTTP fetch over the kernel's TCP/IP. That covers
in-guest network apk without DNS; a mirror *name* still needs the
resolver.

## Kernel and ABI fixes this needed

Each fix has a probe in `ports/abiprobes` (run by `make smoke-abi`) that
passes on Linux and on MaeroOS:

| Gap | Symptom in Alpine | Probe |
|---|---|---|
| `chroot` (61) missing, and the in-tree libc's `chroot()` was a stub | no way in | p31 |
| `utimensat` (320/412), `futimesat` (299), `utimes` (271) and `utime` (30) missing; ext2 and tmpfs stamped files with seconds since boot (ext2) or 0 (tmpfs) | apk: "Failed to preserve modification time ... Function not implemented" for every file | p32 |
| `TCGETS`/`TCSETS`/`TIOCGWINSZ` succeeded on any fd | `isatty(pipe)` was true: `less` refused piped input, and git started a pager into a pipe | p33 |
| `statfs` returned ext2 for every path, existing or not | apk did not see procfs at `<root>/proc` and called `mount` (21) | p34 |
| xattr calls (226-237) missing | GNU `ls -l` logged `unimplemented 230` for every file | p35 |
| `statx` (musl's `stat`/`lstat`/`fstatat` on i386) ignored `dirfd` and `AT_SYMLINK_NOFOLLOW` | `lstat` never saw a symlink: `ls -l` showed busybox applet links as copies, and `test -L` was false | p36 |
| `link` returned EPERM, `linkat` (303) was missing, and `st_nlink` was always 1 | packages that ship hard links could not be installed in the guest (apk-tools 3 links files) | p37 |
| `sync` (36) and `syncfs` (344) missing | apk-tools 3 syncs after a commit | p38 |
| `splice` (313) missing | coreutils 9.11 `cat` (Alpine 3.24) splices on pipes and logged ENOSYS. It now gets EINVAL, as for an unspliceable pair on Linux, and falls back to read/write | p39 |

## Next steps

- **flock is still advisory-only.** `flock()` succeeds without excluding
  anyone, so two apk instances running at once are not kept apart. Real
  semantics need a lock table keyed by inode, with owners released on
  close and exit.
- **Hard links on tmpfs.** `link` works on ext2 only; tmpfs answers EPERM
  because a tmpfs node lives in exactly one directory list.
- **xattrs** are `EOPNOTSUPP` everywhere. apk 2 and 3 tolerate that for
  the packages used here. Packages that carry file capabilities
  (`security.capability`) would need ext2 EA blocks.
- **splice/tee/vmsplice** answer EINVAL instead of moving data.
- **Network apk by name.** Point `/etc/apk/repositories` at
  `https://dl-cdn.alpinelinux.org/...` once DNS lands. HTTP by IP is
  already proven (`--net`). HTTPS needs the CA bundle, which
  `ca-certificates-bundle` in the minirootfs already provides.
- **Alpine as `/`.** Booting with Alpine as the root filesystem needs
  OpenRC or a busybox init that works on MaeroOS, and `/dev`/`/proc`
  mounted, not passed through.
