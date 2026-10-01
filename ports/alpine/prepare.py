#!/usr/bin/env python3
"""Build an Alpine Linux x86 root and an ext2 disk image that carries it.

    python3 ports/alpine/prepare.py            # -> disk-alpine.img
    ALPINE_OFFLINE=1 python3 ports/alpine/prepare.py   # cache only, no network
    ALPINE_RELOCK=1  python3 ports/alpine/prepare.py   # accept new versions

The image holds the root at /alpine (the guest sees it as /disk/alpine and
enters it with `chroot /disk/alpine /bin/sh`; see ports/alpine/README.md) and
an offline package repository inside it at /repo, so `apk add`/`apk del` work
in the guest without a network.

Steps, all as the invoking user (`unshare -r` stands in for root):
  1. Fetch the Alpine minirootfs tarball and the apk-tools-static package;
     both must match the sha256 in alpine.lock.  The minirootfs carries the
     Alpine signing keys (/etc/apk/keys) everything after is checked against.
  2. Run apk.static on the host (an i386 static binary; x86-64 Linux runs it)
     with --root on the extracted minirootfs to add PACKAGES.  apk verifies the
     signed APKINDEX and every package's signature itself.
  3. Compare every installed package's name-version and every downloaded
     .apk's sha256 with alpine.lock: Alpine drops superseded builds from its
     stable mirrors, so a pin can become unfetchable.  Then the cached copy
     (ports/alpine/cache, gitignored) still works offline; ALPINE_RELOCK=1
     takes the mirror's current versions and rewrites the lock.
  4. `apk fetch` the REPO_PACKAGES (not installed) into /repo/main/x86 with
     the signed main APKINDEX.tar.gz, as the guest's offline repository.
  5. mke2fs -d the tree into disk-alpine.img (1 KiB blocks, like disk.img);
     inside `unshare -r` the files are owned by root in the image.
"""
import gzip
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CACHE = os.path.join(HERE, "cache")
BUILD = os.path.join(ROOT, "build", "alpine")

# ALPINE_BRANCH picks a release: its minirootfs, its apk.static and its lock
# file.  v3.22 (apk-tools 2) is what smoke-alpine is proven with; v3.24 has
# apk-tools 3 (see README.md).
RELEASES = {
    "v3.22": ("alpine-minirootfs-3.22.6-x86.tar.gz",
              "apk-tools-static-2.14.12-r0.apk", "alpine.lock"),
    "v3.24": ("alpine-minirootfs-3.24.2-x86.tar.gz",
              "apk-tools-static-3.0.8-r0.apk", "alpine-v3.24.lock"),
}
BRANCH = os.environ.get("ALPINE_BRANCH", "v3.22")
if BRANCH not in RELEASES:
    sys.exit(f"[alpine] ALPINE_BRANCH must be one of {sorted(RELEASES)}")
MINIROOTFS, APK_STATIC, _lock = RELEASES[BRANCH]
LOCK = os.path.join(HERE, _lock)
APK_CACHE = os.path.join(CACHE, "apk" if BRANCH == "v3.22" else "apk-" + BRANCH)
MIRROR = os.environ.get("ALPINE_MIRROR", "https://dl-cdn.alpinelinux.org/alpine")
ARCH = "x86"

# Installed on the host into the image.
PACKAGES = ["bash", "coreutils", "python3", "vim", "less", "nano",
            "git", "openssh-client"]
# Fetched into the guest's offline repo /repo, not installed: the smoke test
# adds and deletes them.  tree depends on musl only.
REPO_PACKAGES = ["tree"]

IMG = os.environ.get("ALPINE_IMG", os.path.join(ROOT, "disk-alpine.img"))


def log(msg):
    print("[alpine] " + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_lock():
    """alpine.lock: `file <name> <sha256>` and `pkg <name-version>` lines."""
    files, pkgs = {}, set()
    if os.path.exists(LOCK):
        for line in open(LOCK):
            f = line.split()
            if not f or f[0].startswith("#"):
                continue
            if f[0] == "file":
                files[f[1]] = f[2]
            elif f[0] == "pkg":
                pkgs.add(f[1])
    return files, pkgs


def write_lock(files, pkgs):
    with open(LOCK, "w") as out:
        out.write("# Pins for ports/alpine/prepare.py (ALPINE_RELOCK=1 rewrites it).\n")
        out.write("# file: sha256 of a downloaded file; pkg: an installed name-version.\n")
        for name in sorted(files):
            out.write(f"file {name} {files[name]}\n")
        for p in sorted(pkgs):
            out.write(f"pkg {p}\n")


def fetch(url, dest):
    if os.path.exists(dest):
        return
    if os.environ.get("ALPINE_OFFLINE"):
        sys.exit(f"[alpine] {os.path.basename(dest)} is not cached and ALPINE_OFFLINE is set")
    log("fetch " + url)
    tmp = dest + ".part"
    with urllib.request.urlopen(url, timeout=120) as r, open(tmp, "wb") as out:
        shutil.copyfileobj(r, out)
    os.rename(tmp, dest)


def check_file(path, lock_files, relock):
    name = os.path.basename(path)
    got = sha256(path)
    want = lock_files.get(name)
    if want is None or relock:
        lock_files[name] = got
    elif want != got:
        sys.exit(f"[alpine] {name}: sha256 {got} does not match the lock {want}")


def extract_apk_static(apk, dest):
    """An .apk is concatenated gzip streams of tar segments (signature,
    control, data); gzip reads across the streams and ignore_zeros across
    the segments' end-of-archive blocks."""
    with gzip.open(apk, "rb") as f, \
            tarfile.open(fileobj=f, mode="r|", ignore_zeros=True) as t:
        for m in t:
            if m.name == "sbin/apk.static":
                with open(dest, "wb") as out:
                    out.write(t.extractfile(m).read())
                os.chmod(dest, 0o755)
                return
    sys.exit("[alpine] no sbin/apk.static in " + apk)


def index_has(path, names):
    with tarfile.open(path, "r:gz") as t:
        text = t.extractfile("APKINDEX").read().decode()
    return all(f"\nP:{n}\n" in "\n" + text for n in names)


def apk(apk_static, root, *args):
    repos = []
    for r in ("main", "community"):
        repos += ["-X", f"{MIRROR}/{BRANCH}/{r}"]
    cmd = ["unshare", "-r", apk_static, "--root", root, "--arch", ARCH,
           "--cache-dir", APK_CACHE, "--repositories-file", "/dev/null",
           "--no-progress"] + repos
    if os.environ.get("ALPINE_OFFLINE"):
        cmd.append("--no-network")
    cmd += list(args)
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE,
                          text=True).stdout


def main():
    relock = bool(os.environ.get("ALPINE_RELOCK"))
    lock_files, lock_pkgs = read_lock()
    os.makedirs(APK_CACHE, exist_ok=True)

    # 1. Base tarball and apk.static.
    rootfs_tgz = os.path.join(CACHE, MINIROOTFS)
    apk_pkg = os.path.join(CACHE, APK_STATIC)
    fetch(f"{MIRROR}/{BRANCH}/releases/{ARCH}/{MINIROOTFS}", rootfs_tgz)
    fetch(f"{MIRROR}/{BRANCH}/main/{ARCH}/{APK_STATIC}", apk_pkg)
    check_file(rootfs_tgz, lock_files, relock)
    check_file(apk_pkg, lock_files, relock)

    shutil.rmtree(BUILD, ignore_errors=True)
    stage = os.path.join(BUILD, "stage")
    root = os.path.join(stage, "alpine")
    os.makedirs(root)
    apk_static = os.path.join(BUILD, "apk.static")
    extract_apk_static(apk_pkg, apk_static)
    log("unpack " + MINIROOTFS)
    subprocess.run(["tar", "xzf", rootfs_tgz, "--no-same-owner", "-C", root],
                   check=True)

    # 2. Packages, verified by apk against the minirootfs keys.
    log("apk add " + " ".join(PACKAGES))
    apk(apk_static, root, "--update-cache", "add", *PACKAGES)

    # 3. Pins.
    installed = set(apk(apk_static, root, "--no-network", "info", "-v").split())
    locked = {p for p in lock_pkgs if not p.startswith("repo:")}
    if relock or not locked:
        lock_pkgs = set(installed)
    elif installed != locked:
        extra = sorted(installed - locked)
        missing = sorted(locked - installed)
        sys.exit("[alpine] installed packages differ from alpine.lock\n"
                 f"  not in the lock: {extra}\n  locked, not installed: {missing}\n"
                 "  (ALPINE_RELOCK=1 accepts the mirror's current versions)")

    # 4. The guest's offline repo: the signed main index and REPO_PACKAGES.
    repo = os.path.join(root, "repo", "main", ARCH)
    os.makedirs(repo)
    # apk fetch bypasses apk's package cache, so keep its downloads too.
    fetched = os.path.join(CACHE, "fetch-" + BRANCH)
    os.makedirs(fetched, exist_ok=True)
    if not os.environ.get("ALPINE_OFFLINE"):
        log("apk fetch " + " ".join(REPO_PACKAGES))
        apk(apk_static, root, "fetch", "-o", fetched, *REPO_PACKAGES)
    for name in os.listdir(fetched):
        if name.endswith(".apk"):
            shutil.copy(os.path.join(fetched, name), repo)
    for name in sorted(os.listdir(repo)):
        if name.endswith(".apk"):
            check_file(os.path.join(repo, name), lock_files, relock)
            if relock or not locked:
                lock_pkgs.add("repo:" + name[:-4])
            elif "repo:" + name[:-4] not in lock_pkgs:
                sys.exit(f"[alpine] repo package {name} is not in alpine.lock")
    # apk names a cached index after a hash of its URL; pick the one that
    # lists the repo packages (they all come from main).
    idx = [n for n in os.listdir(APK_CACHE)
           if n.startswith("APKINDEX.") and n.endswith(".tar.gz")
           and index_has(os.path.join(APK_CACHE, n), REPO_PACKAGES)]
    if len(idx) != 1:
        sys.exit(f"[alpine] cannot tell the main APKINDEX in {APK_CACHE}: {idx}")
    shutil.copy(os.path.join(APK_CACHE, idx[0]),
                os.path.join(repo, "APKINDEX.tar.gz"))
    with open(os.path.join(root, "etc", "apk", "repositories"), "w") as f:
        f.write("# Offline repo on the MaeroOS disk (ports/alpine/prepare.py).\n"
                "# Online: " + f"{MIRROR}/{BRANCH}/main\n/repo/main\n")
    # Linux mounts these; MaeroOS passes /dev and /proc through the chroot.
    os.chmod(os.path.join(root, "tmp"), 0o1777)

    for name in sorted(os.listdir(APK_CACHE)):
        if name.endswith(".apk"):
            check_file(os.path.join(APK_CACHE, name), lock_files, relock)
    write_lock(lock_files, lock_pkgs)

    # 5. The disk image.
    out = subprocess.run(["du", "-sk", stage], check=True, stdout=subprocess.PIPE,
                         text=True).stdout
    kib = int(out.split()[0])
    size_mb = max(256, (kib * 3 // 2) // 1024 + 64)
    log(f"mke2fs {os.path.basename(IMG)} ({size_mb} MiB, tree {kib // 1024} MiB)")
    if os.path.exists(IMG):
        os.unlink(IMG)
    subprocess.run(["unshare", "-r", find_tool("mke2fs"), "-q", "-t", "ext2",
                    "-b", "1024", "-d", stage, "-F", IMG, f"{size_mb}M"],
                   check=True)
    log("done: " + IMG)


def find_tool(name):
    for d in os.environ.get("PATH", "").split(":") + ["/sbin", "/usr/sbin"]:
        p = os.path.join(d, name)
        if os.access(p, os.X_OK):
            return p
    sys.exit(f"[alpine] {name} not found (install e2fsprogs)")


if __name__ == "__main__":
    main()
