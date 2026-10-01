#!/usr/bin/env python3
"""Build the ext4 test disks for tools/smoke_ext4.py.

Everything lands in build/ext4test/ (large, sparse files: never /tmp):

  disk-gpt.img  GPT, hdb when attached at IDE index 1
                  p1  8 MiB ext3 (journal, no extents: block-mapped files
                      with indirect and double-indirect blocks), 1 KiB
                      blocks, htree with the TEA hash
                  p2  256 MiB ext4, 4 KiB blocks, 64bit + metadata_csum +
                      huge_file + dir_index + flex_bg + extents; holds
                      big/ (5000 files, htree), a 300 MiB sparse file,
                      fast/slow/relative/absolute/dangling symlinks,
                      an unwritten (fallocated) extent, nested dirs
  disk-mbr.img  MBR, hdc when attached at IDE index 2
                  p1  4 MiB primary, no filesystem
                  p2  extended, holding logical p5:
                  p5  ext4, 1 KiB blocks, no 64bit / metadata_csum, with
                      huge/ (20000 files: a two-level htree)
  manifest.json what the guest must see: md5 of every regular file, the
                sorted listing of each big directory, symlink targets, the
                sector offsets of both filesystems.

Usage: tools/mkext4img.py [outdir]
"""
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                      os.path.join(ROOT, "build", "ext4test"))
MiB = 1024 * 1024


def tool(name):
    for d in ("", "/usr/sbin/", "/sbin/"):
        p = shutil.which(d + name) if not d else (d + name if os.path.exists(d + name) else None)
        if p:
            return p
    raise SystemExit(f"mkext4img: {name} not found (install e2fsprogs/util-linux)")


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def build_tree4k(src, rng):
    write(f"{src}/hello.txt", b"hello from ext4\n")
    write(f"{src}/rand-1k.bin", rng.randbytes(1000))
    write(f"{src}/rand-100k.bin", rng.randbytes(100 * 1024 + 7))
    write(f"{src}/rand-5m.bin", rng.randbytes(5 * MiB + 4093))
    write(f"{src}/deep/a/b/c/d/leaf.txt", b"deep leaf\n")
    os.makedirs(f"{src}/emptydir", exist_ok=True)
    # 5000 files in one directory: mke2fs builds it as an htree.
    for i in range(5000):
        write(f"{src}/big/f{i:05d}", f"file {i}\n".encode())
    # Long names hash over several 32-byte rounds.
    for i in range(20):
        name = ("long-name-%02d-" % i) + "x" * (40 + 9 * i)
        write(f"{src}/big/{name}", f"long {i}\n".encode())
    # 300 MiB sparse file: data at the start, unaligned in the middle, a
    # large run, and right up to the end.
    sp = f"{src}/sparse.bin"
    with open(sp, "wb") as f:
        f.truncate(300 * MiB)
        for off, n in ((0, 64 * 1024), (100 * MiB + 123, 10000),
                       (200 * MiB, MiB), (300 * MiB - 5000, 5000)):
            f.seek(off)
            f.write(rng.randbytes(n))
    # Symlinks: fast (< 60 bytes, in the inode), slow (a block), relative,
    # absolute, to a directory, dangling.
    os.symlink("hello.txt", f"{src}/link-rel")
    os.symlink("/mnt/deep/a/b/c/d/leaf.txt", f"{src}/link-abs")
    os.symlink("deep/a/b", f"{src}/link-dir")
    slow = "deep/" + "./" * 60 + "a/b/c/d/leaf.txt"
    os.symlink(slow, f"{src}/link-slow")
    os.symlink("does-not-exist", f"{src}/link-dangling")
    return {"link-rel": "hello.txt", "link-abs": "/mnt/deep/a/b/c/d/leaf.txt",
            "link-dir": "deep/a/b", "link-slow": slow,
            "link-dangling": "does-not-exist"}


def build_tree1k(src, rng):
    write(f"{src}/readme.txt", b"ext4 with 1 KiB blocks on a logical partition\n")
    write(f"{src}/rand-300k.bin", rng.randbytes(300 * 1024 + 11))
    for i in range(20000):
        write(f"{src}/huge/n{i:05d}", f"{i}\n".encode())
    os.symlink("readme.txt", f"{src}/readme-link")
    return {"readme-link": "readme.txt"}


def regular_files(src):
    out = {}
    for dp, dns, fns in os.walk(src):
        for fn in fns:
            p = os.path.join(dp, fn)
            if os.path.islink(p):
                continue
            out[os.path.relpath(p, src)] = md5(p)
    return out


def listing(dirpath):
    names = sorted(os.listdir(dirpath))
    blob = "".join(n + "\n" for n in names).encode()
    return {"count": len(names), "md5": hashlib.md5(blob).hexdigest()}


def concat_md5(dirpath):
    """md5 of every file of the directory, concatenated in sorted order."""
    h = hashlib.md5()
    for n in sorted(os.listdir(dirpath)):
        with open(os.path.join(dirpath, n), "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def index_dirs(e2fsck, img):
    """mke2fs -d writes every directory linear; e2fsck -D rebuilds the large
    ones as hash-indexed (htree) directories.  Exit 1 = "fixed", expected."""
    r = subprocess.run([e2fsck, "-fyD", img], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    if r.returncode not in (0, 1):
        raise SystemExit(f"e2fsck -fyD {img} failed ({r.returncode})")


def sfdisk_parts(img):
    out = subprocess.run([tool("sfdisk"), "-J", img], check=True,
                         capture_output=True, text=True).stdout
    return json.loads(out)["partitiontable"]["partitions"]


def main():
    mke2fs, debugfs, e2fsck = tool("mke2fs"), tool("debugfs"), tool("e2fsck")
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    rng = random.Random(4242)
    env = dict(os.environ, E2FSPROGS_FAKE_TIME="1700000000")

    # ── 4 KiB-block ext4 on GPT ─────────────────────────────────────────
    src4 = f"{OUT}/tree4k"
    links4 = build_tree4k(src4, rng)
    fs4 = f"{OUT}/fs4k.img"
    run(mke2fs, "-q", "-F", "-t", "ext4", "-b", "4096", "-L", "maero-ext4",
        "-O", "64bit,metadata_csum,huge_file,dir_index,flex_bg,extent",
        "-E", "hash_seed=6f2a1b3c-7d4e-4f50-8a91-b2c3d4e5f607",
        "-U", "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d",
        "-d", src4, fs4, "256M", env=env)
    # An unwritten (preallocated) extent past the data: reads as zeros.
    with open(f"{OUT}/prealloc.src", "wb") as f:
        f.write(b"P" * 8192)
    run(debugfs, "-w", "-R", f"write {OUT}/prealloc.src prealloc.bin", fs4,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run(debugfs, "-w", "-R", "fallocate /prealloc.bin 2 9", fs4,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run(debugfs, "-w", "-R", "sif /prealloc.bin size 40960", fs4,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    index_dirs(e2fsck, fs4)
    stat = subprocess.run([debugfs, "-R", "stat /prealloc.bin", fs4],
                          capture_output=True, text=True).stdout
    prealloc_md5 = hashlib.md5(b"P" * 8192 + b"\0" * (40960 - 8192)).hexdigest()
    has_unwritten = "[u]" in stat or "Uninit" in stat
    run(e2fsck, "-fn", fs4, stdout=subprocess.DEVNULL)

    gpt = f"{OUT}/disk-gpt.img"
    with open(gpt, "wb") as f:
        f.truncate(2 * MiB + 8 * MiB + 256 * MiB)
    layout = ("label: gpt\n"
              "start=2048, size=16384, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4\n"
              "start=18432, size=524288, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, "
              "name=maero-ext4\n")
    run(tool("sfdisk"), "-q", gpt, input=layout.encode(), stdout=subprocess.DEVNULL)
    p = sfdisk_parts(gpt)
    gpt_start = p[1]["start"]

    # ── ext3, block-mapped, TEA-hashed htree, on GPT p1 ─────────────────
    src3 = f"{OUT}/tree3"
    write(f"{src3}/blockmapped.bin", rng.randbytes(3 * MiB + 333))
    for i in range(800):
        write(f"{src3}/dir/e{i:04d}", f"{i}\n".encode())
    fs3 = f"{OUT}/fs3.img"
    run(mke2fs, "-q", "-F", "-t", "ext3", "-b", "1024", "-O", "dir_index",
        "-d", src3, fs3, "8M", env=env)
    run(tool("tune2fs"), "-E", "hash_alg=tea", fs3, stdout=subprocess.DEVNULL)
    index_dirs(e2fsck, fs3)
    run(e2fsck, "-fn", fs3, stdout=subprocess.DEVNULL)
    tea = "Hash Version: 2" in subprocess.run(
        [debugfs, "-R", "htree_dump /dir", fs3], capture_output=True, text=True).stdout
    ext3_start = p[0]["start"]
    run("dd", f"if={fs3}", f"of={gpt}", "bs=512", f"seek={ext3_start}",
        "conv=notrunc,sparse", "status=none")
    run("dd", f"if={fs4}", f"of={gpt}", "bs=512", f"seek={gpt_start}",
        "conv=notrunc,sparse", "status=none")

    # ── 1 KiB-block ext4 (no 64bit, no csum) on an MBR logical partition ─
    src1 = f"{OUT}/tree1k"
    links1 = build_tree1k(src1, rng)
    fs1 = f"{OUT}/fs1k.img"
    run(mke2fs, "-q", "-F", "-t", "ext4", "-b", "1024", "-N", "24000",
        "-O", "^64bit,^metadata_csum,dir_index", "-L", "maero-1k",
        "-d", src1, fs1, "96M", env=env)
    index_dirs(e2fsck, fs1)
    run(e2fsck, "-fn", fs1, stdout=subprocess.DEVNULL)
    levels = subprocess.run([debugfs, "-R", "htree_dump /huge", fs1],
                            capture_output=True, text=True).stdout
    mbr = f"{OUT}/disk-mbr.img"
    fs1_sectors = os.path.getsize(fs1) // 512
    with open(mbr, "wb") as f:
        f.truncate((12288 + fs1_sectors + 2048) * 512)
    layout = ("label: dos\n"
              "start=2048, size=8192, type=83\n"
              f"start=10240, size={2048 + fs1_sectors}, type=5\n"
              f"start=12288, size={fs1_sectors}, type=83\n")
    run(tool("sfdisk"), "-q", mbr, input=layout.encode(), stdout=subprocess.DEVNULL)
    mbr_start = [q for q in sfdisk_parts(mbr) if q["node"].endswith("5")][0]["start"]
    run("dd", f"if={fs1}", f"of={mbr}", "bs=512", f"seek={mbr_start}",
        "conv=notrunc,sparse", "status=none")

    manifest = {
        "ext3": {"image": gpt, "fs": fs3, "start": ext3_start, "dev": "hdb1",
                 "files": regular_files(src3), "dir": listing(f"{src3}/dir"),
                 "tea_htree": tea},
        "gpt": {"image": gpt, "fs": fs4, "start": gpt_start, "dev": "hdb2",
                "files": regular_files(src4), "links": links4,
                "big": listing(f"{src4}/big"),
                "big_htree": "Root node dump" in subprocess.run(
                    [debugfs, "-R", "htree_dump /big", fs4],
                    capture_output=True, text=True).stdout, "big_concat": concat_md5(f"{src4}/big"),
                "sparse_size": 300 * MiB,
                "prealloc_md5": prealloc_md5, "has_unwritten": has_unwritten},
        "mbr": {"image": mbr, "fs": fs1, "start": mbr_start, "dev": "hdc5",
                "files": regular_files(src1), "links": links1,
                "huge": listing(f"{src1}/huge"),
                "htree_levels": ("Indirect levels: 1" in levels or
                                 "Number of indirect levels: 1" in levels)},
    }
    with open(f"{OUT}/manifest.json", "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"mkext4img: {gpt} (ext4 at sector {gpt_start}), "
          f"{mbr} (ext4 at sector {mbr_start}); unwritten extent: {has_unwritten}; "
          f"two-level htree: {manifest['mbr']['htree_levels']}")


if __name__ == "__main__":
    main()
