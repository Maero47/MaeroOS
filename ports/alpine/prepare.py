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
            "git", "openssh-client", "alsa-utils"]
# Fetched into the guest's offline repo /repo, not installed: the smoke test
# adds and deletes them.  tree depends on musl only; openssh-server needs
# openssh-keygen and the libraries openssh-client already installed.
REPO_PACKAGES = ["tree", "openssh-server", "openssh-server-common"]

# ALPINE_X=1: the X11 desktop-apps image (docs/alpinex.md).  The same base
# root, plus an offline repo holding X_PACKAGES and everything they depend on
# (`apk fetch -R`), split over /repo/main and /repo/community like the
# mirror.  Its pins live in their own lock (alpine-x.lock) and it is a
# separate image, so disk-alpine.img and smoke-alpine stay small.
X = bool(os.environ.get("ALPINE_X"))
X_PACKAGES = ["xterm", "xeyes", "xclock", "xev", "xdpyinfo", "xwininfo",
              "mousepad", "galculator", "feh", "ristretto", "mpv", "gimp",
              "font-dejavu", "adwaita-icon-theme", "hicolor-icon-theme"]
X_LOCK = os.path.join(HERE, "alpine-x.lock")
X_PREINSTALL = ["mousepad", "font-dejavu", "hicolor-icon-theme"]

IMG = os.environ.get("ALPINE_IMG", os.path.join(
    ROOT, "disk-alpinex.img" if X else "disk-alpine.img"))


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
    main_idx = find_index(REPO_PACKAGES, "main")
    shutil.copy(main_idx, os.path.join(repo, "APKINDEX.tar.gz"))
    repos = "/repo/main\n"
    if X:
        add_x_repo(apk_static, root, main_idx, relock)
        repos += "/repo/community\n"
        add_xapp(stage)
        # apk in the guest needs well over 15 minutes for a GTK 3 stack (the
        # ICU data alone takes minutes), so the GTK apps' runtime and Mousepad
        # are installed here; xterm, xeyes & co. are installed in the guest.
        log("apk add (preinstalled) " + " ".join(X_PREINSTALL))
        apk(apk_static, root, "--no-network",
            "--repository", os.path.join(root, "repo", "main"),
            "--repository", os.path.join(root, "repo", "community"),
            "add", *X_PREINSTALL)
        entry = os.path.join(stage, "apps", "mousepad")
        os.makedirs(entry, exist_ok=True)
        with open(os.path.join(entry, "manifest"), "w") as f:
            f.write("exec=/disk/xapp\nargs=mousepad\ntitle=Mousepad\nalpine=1\n")
    with open(os.path.join(root, "etc", "apk", "repositories"), "w") as f:
        f.write("# Offline repo on the MaeroOS disk (ports/alpine/prepare.py).\n"
                "# Online: " + f"{MIRROR}/{BRANCH}/main\n" + repos)
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
    if X:
        # Room to install the whole repo in the guest: packages unpack to
        # about three times their .apk size.  The image file is sparse.
        size_mb += 3 * kib // 1024 + 512
    log(f"mke2fs {os.path.basename(IMG)} ({size_mb} MiB, tree {kib // 1024} MiB)")
    if os.path.exists(IMG):
        os.unlink(IMG)
    subprocess.run(["unshare", "-r", find_tool("mke2fs"), "-q", "-t", "ext2",
                    "-b", "1024", "-d", stage, "-F", IMG, f"{size_mb}M"],
                   check=True)
    log("done: " + IMG)


def find_index(names, what):
    idx = [os.path.join(APK_CACHE, n) for n in os.listdir(APK_CACHE)
           if n.startswith("APKINDEX.") and n.endswith(".tar.gz")
           and index_has(os.path.join(APK_CACHE, n), names)]
    if len(idx) != 1:
        sys.exit(f"[alpine] cannot tell the {what} APKINDEX in {APK_CACHE}: {idx}")
    return idx[0]


def index_versions(path):
    """{name-version} of every package an APKINDEX lists."""
    with tarfile.open(path, "r:gz") as t:
        text = t.extractfile("APKINDEX").read().decode()
    out, name = set(), None
    for line in text.split("\n"):
        if line.startswith("P:"):
            name = line[2:]
        elif line.startswith("V:") and name:
            out.add(name + "-" + line[2:])
    return out


def add_x_repo(apk_static, root, main_idx, relock):
    """X_PACKAGES and their whole dependency closure into /repo/main and
    /repo/community (each .apk next to the index that lists it), pinned in
    alpine-x.lock the way alpine.lock pins the base."""
    files, pkgs = {}, set()
    if os.path.exists(X_LOCK):
        for line in open(X_LOCK):
            f = line.split()
            if f and f[0] == "file":
                files[f[1]] = f[2]
            elif f and f[0] == "pkg":
                pkgs.add(f[1])
    fetched = os.path.join(CACHE, "fetch-x-" + BRANCH)
    os.makedirs(fetched, exist_ok=True)
    if not os.environ.get("ALPINE_OFFLINE"):
        log("apk fetch -R " + " ".join(X_PACKAGES))
        apk(apk_static, root, "fetch", "-R", "-o", fetched, *X_PACKAGES)
    comm_idx = find_index(["xterm"], "community")
    lists = {"main": index_versions(main_idx), "community": index_versions(comm_idx)}
    dirs = {r: os.path.join(root, "repo", r, ARCH) for r in lists}
    os.makedirs(dirs["community"], exist_ok=True)
    shutil.copy(comm_idx, os.path.join(dirs["community"], "APKINDEX.tar.gz"))
    have = set()
    for name in sorted(os.listdir(fetched)):
        if not name.endswith(".apk"):
            continue
        nv = name[:-4]
        where = [r for r in lists if nv in lists[r]]
        if not where:
            sys.exit(f"[alpine] {name} is in no cached APKINDEX (stale fetch cache?)")
        src = os.path.join(fetched, name)
        check_file(src, files, relock)
        have.add(nv)
        shutil.copy(src, dirs[where[0]])
    if relock or not pkgs:
        pkgs = have
    elif have != pkgs:
        sys.exit("[alpine] X repo differs from alpine-x.lock\n"
                 f"  not in the lock: {sorted(have - pkgs)}\n"
                 f"  locked, missing: {sorted(pkgs - have)}")
    with open(X_LOCK, "w") as out:
        out.write("# Pins for the X11 apps repo of ports/alpine/prepare.py (ALPINE_X=1).\n")
        for name in sorted(files):
            out.write(f"file {name} {files[name]}\n")
        for p in sorted(pkgs):
            out.write(f"pkg {p}\n")
    log(f"X repo: {len(have)} packages")


def add_xapp(stage):
    """The desktop side of the X apps image: the set-uid helper /disk/xapp
    (userspace/xapp, installs/removes/runs the curated apps in the chroot)
    and the launcher-entry directory /disk/apps.  mke2fs -d keeps the modes;
    under unshare -r every file is root's."""
    helper = os.path.join(ROOT, "testfiles", "xapp")
    if not os.path.exists(helper):
        sys.exit("[alpine] testfiles/xapp is missing: run make userspace first")
    dst = os.path.join(stage, "xapp")
    shutil.copy(helper, dst)
    os.chmod(dst, 0o4755)
    os.makedirs(os.path.join(stage, "apps"), exist_ok=True)
    os.chmod(os.path.join(stage, "apps"), 0o755)


def find_tool(name):
    for d in os.environ.get("PATH", "").split(":") + ["/sbin", "/usr/sbin"]:
        p = os.path.join(d, name)
        if os.access(p, os.X_OK):
            return p
    sys.exit(f"[alpine] {name} not found (install e2fsprogs)")


if __name__ == "__main__":
    main()
