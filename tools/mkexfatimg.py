#!/usr/bin/env python3
"""Test disks for smoke-exfat (build/exfattest/), made with mkfs.exfat and
filled with tools/exfatimg.py.

  ahci.img      MBR, partition 1 (type 0x07) exFAT, 4 KiB clusters: long,
                Turkish and non-BMP names, nested directories, a 5 MiB file,
                a fragmented (FAT chain) file, a file whose ValidDataLength is
                less than its length, a 300-entry directory, an empty file,
                and a contiguous file with its next cluster taken.
  usb.img       exFAT on the whole device (no partition table), 32 KiB
                clusters: the stick for the QEMU usb-storage device.
  big.img       a 64 GiB sparse disk, exFAT with 4096-byte sectors and 128 KiB
                clusters, holding a 5 GiB file with markers at known offsets
                (around 4 GiB and past it).  Built in place, never copied.
  crafted.img   exFAT whose root holds malformed entry sets (bad SetChecksum,
                too few secondaries, a name longer than its entries, clusters
                outside the heap, a 2^62-byte length, a looping FAT chain, a
                directory that is its own parent's root, names with '/' and
                NUL units) next to good files.
  badboot.img   MBR with four exFAT volumes whose boot regions were edited:
                main region checksum broken (backup good: read-only mount),
                ClusterCount past the volume, root cluster 1, both regions
                broken.
  upcase.img    exFAT whose up-case table does not match its checksum: only a
                read-only mount.
  fuzz-N.img    copies of a small volume with random bytes over the FAT, the
                bitmap, the root directory and the first data clusters.

manifest.json records, per volume, the partition start and the md5 of every
file by path.
"""
import hashlib
import json
import os
import random
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from exfatimg import Volume, boot_sum, set_sum  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "exfattest")
GiB = 1 << 30


def tool(name):
    for d in ["/usr/sbin/", "/sbin/", os.path.expanduser("~/opt/bin/")]:
        if os.access(d + name, os.X_OK):
            return d + name
    p = shutil.which(name)
    if p:
        return p
    sys.exit(f"mkexfatimg: {name} not found")


def mkfs(path, size, *extra):
    with open(path, "wb") as f:
        f.truncate(size)
    subprocess.run([tool("mkfs.exfat"), "-q", *extra, path], check=True,
                   stdout=subprocess.DEVNULL)


def mbr(image, size, parts):
    """`parts`: [(start_sector, sectors, type_hex)]."""
    with open(image, "wb") as f:
        f.truncate(size)
    script = "label: dos\n" + "".join(f"start={s}, size={n}, type={t}\n" for s, n, t in parts)
    subprocess.run([tool("sfdisk"), "--no-reread", "--no-tell-kernel", image],
                   input=script.encode(), check=True, stdout=subprocess.DEVNULL)


def splice(image, start, part):
    with open(part, "rb") as a, open(image, "r+b") as b:
        b.seek(start * 512)
        while True:
            chunk = a.read(1 << 20)
            if not chunk:
                break
            b.write(chunk)
    os.remove(part)


def md5s(files):
    return {p: hashlib.md5(d).hexdigest() for p, d in files.items()}


def populate(path, files, dirs, offset=0, frag=(), dir_clusters=None):
    v = Volume(path, offset)
    for d in dirs:
        v.add_dir(d, (dir_clusters or {}).get(d, 1))
    for p, data in files.items():
        v.add_file(p, data, fragment=p in frag)
    v.close()


def fix_boot(f, base, region, edit):
    """Apply `edit(bytearray)` to the boot sector of `region` (0 main, 1
    backup) and recompute that region's checksum."""
    f.seek(base)
    bps = 1 << f.read(512)[108]
    at = base + region * 12 * bps
    f.seek(at)
    reg = bytearray(f.read(12 * bps))
    edit(reg)
    # One checksum over sectors 0..10 (VolumeFlags and PercentInUse skipped).
    s = boot_sum(reg[:11 * bps], True)
    reg[11 * bps:12 * bps] = struct.pack("<I", s) * (bps // 4)
    f.seek(at)
    f.write(reg)


def find_set(v, name):
    clus, length = v.root_chain, len(v.root_chain) * v.cb
    for idx, group, nm, units in v.sets(clus, length):
        if nm == name:
            return clus, idx, [bytearray(g) for g in group]
    raise KeyError(name)


def put_set(v, clus, idx, ents, fix_sum=True):
    if fix_sum:
        struct.pack_into("<H", ents[0], 2, set_sum([bytes(e) for e in ents]))
    for k, e in enumerate(ents):
        v.wr(v.ent_off(clus, idx + k), bytes(e))


def craft(path):
    """Malformed entry sets, written over good ones made by the builder."""
    v = Volume(path)
    # Bad SetChecksum: the set is ignored.
    clus, idx, e = find_set(v, "badsum.txt")
    e[0][2] ^= 0x55
    put_set(v, clus, idx, e, fix_sum=False)
    # SecondaryCount 1 (no name entries): ignored.
    clus, idx, e = find_set(v, "fewsec.txt")
    e[0][1] = 1
    put_set(v, clus, idx, e)
    # NameLength 200 with one name entry: ignored (no copy past the entries).
    clus, idx, e = find_set(v, "longname.txt")
    e[1][3] = 200
    put_set(v, clus, idx, e)
    # First cluster outside the heap, 1 MiB long.
    clus, idx, e = find_set(v, "badclus.bin")
    struct.pack_into("<IQ", e[1], 20, 0xFFFFFF00, 1 << 20)
    struct.pack_into("<Q", e[1], 8, 1 << 20)
    put_set(v, clus, idx, e)
    # A length of 2^62 bytes.
    clus, idx, e = find_set(v, "hugelen.bin")
    struct.pack_into("<Q", e[1], 24, 1 << 62)
    struct.pack_into("<Q", e[1], 8, 1 << 62)
    put_set(v, clus, idx, e)
    # A FAT chain that loops: 3 clusters long by its length, a 2-cycle on disk.
    clus, idx, e = find_set(v, "loop.bin")
    first = struct.unpack_from("<I", e[1], 20)[0]
    second = v.fat(first)
    v.set_fat(second, first)
    # A FAT chain that ends early (3 clusters by its length).
    clus, idx, e = find_set(v, "short.bin")
    first = struct.unpack_from("<I", e[1], 20)[0]
    v.set_fat(first, 0xFFFFFFFF)
    # A directory whose storage is the root directory's cluster.
    clus, idx, e = find_set(v, "loopdir")
    struct.pack_into("<I", e[1], 20, v.root)
    e[1][1] = 0x03
    put_set(v, clus, idx, e)
    # Name units '/' and NUL: shown as '_'.
    clus, idx, e = find_set(v, "slashname")
    struct.pack_into("<H", e[2], 2 + 2 * 1, ord("/"))
    struct.pack_into("<H", e[2], 2 + 2 * 3, 0)
    put_set(v, clus, idx, e)
    # A stray name entry and a stream entry with no file entry before them.
    raw_off = None
    rl = v.read_clusters(v.root_chain, len(v.root_chain) * v.cb)
    for i in range(0, len(rl), 32):
        if rl[i] == 0:
            raw_off = i
            break
    stray = bytearray(32)
    stray[0] = 0xC1
    stray2 = bytearray(32)
    stray2[0] = 0xC0
    v.wr(v.ent_off(v.root_chain, raw_off // 32), bytes(stray))
    v.wr(v.ent_off(v.root_chain, raw_off // 32 + 1), bytes(stray2))
    v.close()


def fuzz(src, dst, seed, writes):
    """Random bytes over the FAT (past the root directory's entry), the
    bitmap, the root directory (past its bitmap and up-case entries, so the
    volume still mounts and the damage reaches the file code) and the first
    data clusters."""
    shutil.copyfile(src, dst)
    v = Volume(dst)
    fat0 = v.fat_off * v.bps + 4 * (v.root + 1)
    bm = v.coff(v.bitmap_clus)
    root = v.coff(v.root)
    data = v.coff(v.root + 1)
    v.close()
    rnd = random.Random(seed)
    with open(dst, "r+b") as f:
        for _ in range(writes):
            r = rnd.random()
            if r < 0.2:
                off = rnd.randrange(fat0, fat0 + 512)
            elif r < 0.3:
                off = rnd.randrange(bm, bm + 64)
            elif r < 0.7:
                off = rnd.randrange(root + 96, root + 4096)
            else:
                off = rnd.randrange(data, data + 200 * 4096)
            f.seek(off)
            f.write(bytes([rnd.randrange(256)]))


def main():
    os.makedirs(OUT, exist_ok=True)
    rnd = random.Random(2019)
    man = {}

    # ── ahci.img ───────────────────────────────────────────────────────
    img = os.path.join(OUT, "ahci.img")
    start, nsec = 2048, (160 << 11) - 2048
    mbr(img, 160 << 20, [(start, nsec, "7")])
    part = os.path.join(OUT, "part.tmp")
    mkfs(part, nsec * 512, "-c", "4K", "-L", "MAEROEXFAT")
    files = {
        "hello.txt": b"hello from exFAT\n",
        "readme.md": b"short name, lower case\n",
        "This is a long file name with many words in it.text": b"long name\n" * 100,
        "Türkçe ğüşıöç İI.txt": "Merhaba dünya: çğıöşü ÇĞİÖŞÜ\n".encode(),
        "Gülümseme 😀 dosyası.txt": "non-BMP: 😀\n".encode(),
        "Klasör/alt dizin/derin dosya.txt": b"deep\n",
        "Klasör/şarkı sözleri.mp3": rnd.randbytes(70000),
        "big.bin": rnd.randbytes(5 << 20),
        "frag.bin": rnd.randbytes(300000),
        "empty.txt": b"",
        "blocked.bin": rnd.randbytes(8192),
        "blocker.bin": rnd.randbytes(4096),
    }
    for i in range(300):
        files[f"many/entry number {i:03d}.dat"] = f"{i}\n".encode()
    dirs = ["Klasör", "Klasör/alt dizin", "many"]
    populate(part, files, dirs, frag={"frag.bin"}, dir_clusters={"many": 12})
    v = Volume(part)
    v.add_file("sparse.bin", b"valid head\n" * 100, size=200000)
    v.close()
    files["sparse.bin"] = b"valid head\n" * 100 + bytes(200000 - 1100)
    splice(img, start, part)
    man["ahci"] = {"image": img, "start": start, "sectors": nsec, "files": md5s(files), "dirs": dirs}

    # ── usb.img (superfloppy) ──────────────────────────────────────────
    img = os.path.join(OUT, "usb.img")
    mkfs(img, 96 << 20, "-c", "32K", "-L", "MAEROUSB")
    files = {
        "usb-hello.txt": b"hello from the USB stick\n",
        "Belgeler/Ödev ılık şekerli çay.txt": "Ödev\n".encode() * 50,
        "Belgeler/photo.jpg": rnd.randbytes(1 << 20),
        "DOS.TXT": b"plain 8.3\n",
    }
    populate(img, files, ["Belgeler"])
    man["usb"] = {"image": img, "start": 0, "sectors": 96 << 11, "files": md5s(files),
                  "dirs": ["Belgeler"]}

    # ── big.img: 64 GiB sparse, 4096-byte sectors, a 5 GiB file ─────────
    img = os.path.join(OUT, "big.img")
    if os.path.exists(img):
        os.remove(img)
    mkfs(img, 64 * GiB, "-s", "4096", "-c", "128K", "-L", "BUYUK")
    huge = 5 * GiB + 123
    marks = {0: rnd.randbytes(4096), 4 * GiB - 8192: rnd.randbytes(4096),
             4 * GiB - 4096: rnd.randbytes(4096), 4 * GiB + GiB // 2: rnd.randbytes(4096),
             huge - 123: rnd.randbytes(123)}
    v = Volume(img)
    v.add_dir("Arşiv")
    v.add_file("Arşiv/küçük.txt", b"small file on a big volume\n")
    v.add_file("huge.bin", b"", size=huge)
    # Make all of it valid (the data in between reads as the sparse zeros).
    clus, idx, e = find_set(v, "huge.bin")
    struct.pack_into("<Q", e[1], 8, huge)
    put_set(v, clus, idx, e)
    for off, data in marks.items():
        v.write_at("huge.bin", off, data)
    v.close()
    man["big"] = {"image": img, "huge": huge,
                  "marks": {str(k): hashlib.md5(d).hexdigest() for k, d in marks.items()}}

    # ── crafted.img ────────────────────────────────────────────────────
    img = os.path.join(OUT, "crafted.img")
    mkfs(img, 16 << 20, "-c", "4K")
    good = {f"good file {i}.txt": f"good {i}\n".encode() * (i + 1) for i in range(4)}
    bad = {"badsum.txt": b"x\n", "fewsec.txt": b"y\n", "longname.txt": b"z\n",
           "badclus.bin": b"c" * 5000, "hugelen.bin": b"h" * 5000,
           "slashname": b"s\n"}
    v = Volume(img)
    v.add_dir("sub dir")
    v.add_dir("loopdir")
    v.close()
    good["sub dir/inner file.txt"] = b"inner\n"
    populate(img, dict(good, **bad), [])
    v = Volume(img)
    v.add_file("loop.bin", rnd.randbytes(3 * 4096), fragment=True)
    v.add_file("short.bin", rnd.randbytes(3 * 4096), fragment=True)
    v.close()
    craft(img)
    man["crafted"] = {"image": img, "good": md5s(good)}

    # Fuzz sources: a populated small volume.
    fz = os.path.join(OUT, "fuzz-base.img")
    mkfs(fz, 16 << 20, "-c", "4K")
    populate(fz, {f"fuzz file {i}.bin": rnd.randbytes(1000 * (i + 1)) for i in range(20)},
             ["d1", "d2"], frag={"fuzz file 3.bin", "fuzz file 7.bin"})
    man["fuzz"] = []
    for n, writes in enumerate((60, 400)):
        dst = os.path.join(OUT, f"fuzz-{n}.img")
        fuzz(fz, dst, 5000 + n, writes)
        man["fuzz"].append(dst)
    os.remove(fz)

    # ── badboot.img ────────────────────────────────────────────────────
    img = os.path.join(OUT, "badboot.img")
    n = 8 << 11
    starts = [2048 + i * n for i in range(4)]
    mbr(img, 2048 * 512 + 4 * n * 512 + (1 << 20), [(s, n, "7") for s in starts])
    for s in starts:
        part = os.path.join(OUT, "part.tmp")
        mkfs(part, n * 512, "-c", "4K")
        populate(part, {"boot.txt": b"boot test\n"}, [])
        splice(img, s, part)
    with open(img, "r+b") as f:
        # 1: main region checksum broken, backup good.
        f.seek(starts[0] * 512 + 11 * 512)
        f.write(b"\xEE" * 4)
        # 2: ClusterCount past the volume, in both regions (checksums fixed).
        for reg in (0, 1):
            fix_boot(f, starts[1] * 512, reg, lambda r: struct.pack_into("<I", r, 92, 0x00FFFFFF))
        # 3: root directory cluster 1.
        for reg in (0, 1):
            fix_boot(f, starts[2] * 512, reg, lambda r: struct.pack_into("<I", r, 96, 1))
        # 4: both regions' checksums broken.
        for reg in (0, 1):
            f.seek(starts[3] * 512 + (reg * 12 + 11) * 512)
            f.write(b"\xEE" * 4)
    man["badboot"] = {"image": img}

    # ── upcase.img ─────────────────────────────────────────────────────
    img = os.path.join(OUT, "upcase.img")
    mkfs(img, 16 << 20, "-c", "4K")
    populate(img, {"Upcase Test.txt": b"upcase\n"}, [])
    v = Volume(img)
    for e in v.dir_entries(v.root_chain, v.cb):
        if e[0] == 0x82:
            c = struct.unpack_from("<I", e, 20)[0]
            v.wr(v.coff(c) + 200, b"\x12\x34")
    v.close()
    man["upcase"] = {"image": img}

    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(man, f, indent=1, ensure_ascii=False)
    print(f"[MKEXFATIMG] images in {OUT}")


if __name__ == "__main__":
    main()
