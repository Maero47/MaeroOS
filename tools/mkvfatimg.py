#!/usr/bin/env python3
"""Test disks for smoke-vfat (build/vfattest/), made with mkfs.fat and mtools.

  fat32.img   MBR, partition 1 (type 0x0c) FAT32 with 512-byte clusters:
              long and Turkish names, nested directories, a 5 MiB file,
              8.3 names in lower case, a directory with 300 entries.
  usb.img     FAT32 on the whole device (no partition table), as many USB
              sticks come: the stick for the QEMU usb-storage device.
  fat16.img   MBR, partition 1 FAT16, partition 2 FAT12.
  crafted.img FAT16 on the whole device whose root directory has malformed
              long-name sequences written in by hand (ordinal 0 after a
              complete set, a lone 0x40, ordinals past 20, a bad checksum,
              an orphan set at the end): the files must still read, under
              their short names.
  fuzz-N.img  copies of the FAT16 volume with random bytes written over the
              FAT, the root directory and the first data clusters.

manifest.json records, per volume, the partition start (sectors), and the
md5 of every file by path ("/" separated, UTF-8).  Needs mkfs.fat, mcopy,
mmd and sfdisk on the host.
"""
import hashlib
import json
import os
import random
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "vfattest")
SRC = os.path.join(OUT, "src")

ENV = dict(os.environ, LC_ALL="C.UTF-8", MTOOLS_SKIP_CHECK="1")


def tool(name):
    for d in [""] + ["/usr/sbin/", "/sbin/", os.path.expanduser("~/opt/bin/")]:
        p = shutil.which(name) if not d else (d + name if os.access(d + name, os.X_OK) else None)
        if p:
            return p
    sys.exit(f"mkvfatimg: {name} not found")


def run(*args, **kw):
    subprocess.run(args, check=True, env=ENV, stdout=subprocess.DEVNULL, **kw)


def mbr(image, size_mib, parts):
    """`parts`: [(start_sector, sectors, type_hex)]."""
    with open(image, "wb") as f:
        f.truncate(size_mib << 20)
    script = "label: dos\n" + "".join(
        f"start={s}, size={n}, type={t}\n" for s, n, t in parts)
    subprocess.run([tool("sfdisk"), "--no-reread", "--no-tell-kernel", image],
                   input=script.encode(), check=True, stdout=subprocess.DEVNULL)


def mkfs(image, fat, offset, sectors, extra=()):
    run(tool("mkfs.fat"), "-F", str(fat), *extra, "-i", "1234ABCD",
        f"--offset={offset}", image, str(sectors // 2))


def populate(image, offset, files, dirs):
    """Copy `files` ({path: bytes}) into the volume at byte offset."""
    spec = f"{image}@@{offset * 512}"
    for d in dirs:
        run(tool("mmd"), "-i", spec, "::/" + d)
    shutil.rmtree(SRC, ignore_errors=True)
    for path, data in files.items():
        local = os.path.join(SRC, *path.split("/"))
        os.makedirs(os.path.dirname(local), exist_ok=True)
        with open(local, "wb") as f:
            f.write(data)
        run(tool("mcopy"), "-i", spec, "-m", local, "::/" + path)


def lfn_sum(n11):
    s = 0
    for b in n11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def bpb(img, base=0):
    with open(img, "rb") as f:
        f.seek(base)
        b = f.read(512)
    bps = int.from_bytes(b[11:13], "little")
    res = int.from_bytes(b[14:16], "little")
    nf = b[16]
    rents = int.from_bytes(b[17:19], "little")
    fsz = int.from_bytes(b[22:24], "little")
    root = base + (res + nf * fsz) * bps
    return bps, res, nf, fsz, root, rents


def craft(img):
    """Rewrite the root directory with malformed long-name entries mixed in.
    Each file keeps its short entry, so it stays readable by that name."""
    bps, res, nf, fsz, root, rents = bpb(img)
    with open(img, "r+b") as f:
        f.seek(root)
        raw = f.read(rents * 32)
        ents = [raw[i:i + 32] for i in range(0, len(raw), 32)]
        live = [e for e in ents if e[0] not in (0, 0xE5)]
        out, shorts = [], []
        for e in live:
            if e[11] & 0x3F == 0x0F:
                out.append(e)
                continue
            if e[11] & 0x08:
                out.append(e)
                continue
            sm = lfn_sum(e[:11])
            n = len(shorts)
            shorts.append(e)
            def lfn(first):
                x = bytearray(32)
                x[0] = first
                x[11] = 0x0F
                x[13] = sm
                for k in (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30):
                    x[k:k + 2] = b"A\0"
                return bytes(x)
            if n == 0:
                # The reported case: a complete set, then ordinal 0 (no 0x40).
                out += [lfn(0x20), lfn(0x80), lfn(0xA0)]
            elif n == 1:
                out += [lfn(0x40)]                      # ordinal 0 starting a set
            elif n == 2:
                out += [lfn(0x55), lfn(0x14)]           # ordinal 21, then 20
            elif n == 3:
                bad = bytearray(lfn(0x41)); bad[13] ^= 0xFF
                out += [bytes(bad)]                     # checksum mismatch
            elif n == 4:
                out += [lfn(0x42), lfn(0x02), lfn(0x01), lfn(0x03)]   # an extra part
            out.append(e)
        # An orphan set at the end of the directory.
        tail = bytearray(32); tail[0] = 0x41; tail[11] = 0x0F; tail[13] = 0x77
        out.append(bytes(tail))
        out += [bytes(32)] * (rents - len(out))
        assert len(out) == rents
        f.seek(root)
        f.write(b"".join(out))


def fuzz(src, dst, seed):
    shutil.copyfile(src, dst)
    bps, res, nf, fsz, root, rents = bpb(dst)
    rnd = random.Random(seed)
    data = root + rents * 32
    with open(dst, "r+b") as f:
        for _ in range(3000):
            region = rnd.random()
            if region < 0.3:
                off = rnd.randrange(res * bps, root)          # the FATs
            elif region < 0.7:
                off = rnd.randrange(root, data)               # root directory
            else:
                off = rnd.randrange(data, data + 400 * 2048)  # first clusters
            f.seek(off)
            f.write(bytes([rnd.randrange(256)]))


def md5s(files):
    return {p: hashlib.md5(d).hexdigest() for p, d in files.items()}


def main():
    os.makedirs(OUT, exist_ok=True)
    rnd = random.Random(1923)
    man = {}

    # ── fat32.img ──────────────────────────────────────────────────────
    img = os.path.join(OUT, "fat32.img")
    start, nsec = 2048, (96 << 11) - 2048
    mbr(img, 96, [(start, nsec, "c")])
    mkfs(img, 32, start, nsec, ["-s", "1", "-n", "MAEROFAT32"])
    files = {
        "hello.txt": b"hello from FAT32\n",
        "readme.md": b"lower-case 8.3 name\n",
        "MixedCase.Txt": b"mixed case needs a long name\n",
        "This is a long file name.text": b"long name\n" * 100,
        "Türkçe ğüşıöç İI.txt": "Merhaba dünya: çğıöşü ÇĞİÖŞÜ\n".encode(),
        "Klasör/alt dizin/derin dosya.txt": b"deep\n",
        "Klasör/şarkı sözleri.mp3": bytes(rnd.getrandbits(8) for _ in range(70000)),
        "big.bin": rnd.randbytes(5 << 20),
        "many/x": b"",
    }
    del files["many/x"]
    for i in range(300):
        files[f"many/entry number {i:03d}.dat"] = f"{i}\n".encode()
    populate(img, start, files, ["Klasör", "Klasör/alt dizin", "many"])
    man["fat32"] = {"image": img, "start": start, "sectors": nsec, "files": md5s(files),
                    "dirs": ["Klasör", "Klasör/alt dizin", "many"]}

    # ── usb.img (superfloppy) ──────────────────────────────────────────
    img = os.path.join(OUT, "usb.img")
    with open(img, "wb") as f:
        f.truncate(72 << 20)
    run(tool("mkfs.fat"), "-F", "32", "-s", "1", "-n", "MAEROUSB", "-i", "5B5B5B5B",
        "--mbr=n", img)
    files = {
        "usb-hello.txt": b"hello from the USB stick\n",
        "Belgeler/Ödev ılık şekerli çay.txt": "Ödev\n".encode() * 50,
        "Belgeler/photo.jpg": rnd.randbytes(1 << 20),
        "DOS.TXT": b"plain 8.3\n",
    }
    populate(img, 0, files, ["Belgeler"])
    man["usb"] = {"image": img, "start": 0, "sectors": 72 << 11, "files": md5s(files),
                  "dirs": ["Belgeler"]}

    # ── fat16.img: FAT16 + FAT12 ───────────────────────────────────────
    img = os.path.join(OUT, "fat16.img")
    s1, n1 = 2048, 32 << 11
    s2, n2 = s1 + n1, 4 << 11
    mbr(img, 40, [(s1, n1, "6"), (s2, n2, "1")])
    mkfs(img, 16, s1, n1, ["-n", "MAEROFAT16"])
    mkfs(img, 12, s2, n2, ["-n", "MAEROFAT12"])
    f16 = {
        "fat16.txt": b"hello from FAT16\n",
        "Uzun İsimli Dosya.txt": "FAT16 üzerinde uzun isim\n".encode(),
        "sub/data.bin": rnd.randbytes(300000),
    }
    populate(img, s1, f16, ["sub"])
    f12 = {
        "fat12.txt": b"hello from FAT12\n",
        "odd cluster chain.bin": rnd.randbytes(123457),
    }
    populate(img, s2, f12, [])
    man["fat16"] = {"image": img, "start": s1, "sectors": n1, "files": md5s(f16), "dirs": ["sub"]}
    man["fat12"] = {"image": img, "start": s2, "sectors": n2, "files": md5s(f12), "dirs": []}

    # ── crafted.img and fuzz-N.img ─────────────────────────────────────
    img = os.path.join(OUT, "crafted.img")
    with open(img, "wb") as f:
        f.truncate(16 << 20)
    run(tool("mkfs.fat"), "-F", "16", "-s", "4", "-i", "C0FFEE00", "--mbr=n", img)
    cf = {f"crafted file number {i}.txt": f"crafted {i}\n".encode() * (i + 1) for i in range(6)}
    cf["sub dir/inner file.txt"] = b"inner\n"
    populate(img, 0, cf, ["sub dir"])
    fz = os.path.join(OUT, "fuzz-base.img")
    shutil.copyfile(img, fz)
    craft(img)
    man["crafted"] = {"image": img, "md5s": sorted(md5s(cf).values()),
                      "top": len([p for p in cf if "/" not in p])}
    man["fuzz"] = []
    for n in range(3):
        dst = os.path.join(OUT, f"fuzz-{n}.img")
        fuzz(fz, dst, 4000 + n)
        man["fuzz"].append(dst)
    os.remove(fz)

    shutil.rmtree(SRC, ignore_errors=True)
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(man, f, indent=1, ensure_ascii=False)
    print(f"[MKVFATIMG] images in {OUT}")


if __name__ == "__main__":
    main()
