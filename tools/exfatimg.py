#!/usr/bin/env python3
"""A small exFAT image writer and checker for smoke-exfat (host side).

exfatprogs makes and checks volumes (mkfs.exfat, fsck.exfat) but cannot copy
files in without mounting, so this module adds files and directories to a
freshly made volume, and reads a volume back independently of the kernel
driver: every file's contents, and a consistency check of its own (entry-set
checksums, name hashes, cluster ownership against the allocation bitmap,
lengths, the VolumeDirty flag).  Written from Microsoft's exFAT
specification (2019).

  Volume(path, offset).add_dir("a/b")
  Volume(path, offset).add_file("a/b/c.txt", data, fragment=False, size=None)
  Volume(path, offset).tree()   -> ({path: md5}, [dirs], {path: size})
  Volume(path, offset).check()  -> [problems]

`size` larger than the data makes a sparse file of that DataLength whose
ValidDataLength is the data's length.  `fragment` stores the file as a FAT
chain of every other cluster instead of a contiguous (NoFatChain) run.
"""
import hashlib
import struct

FILE, STREAM, NAME, BITMAP, UPCASE, LABEL = 0x85, 0xC0, 0xC1, 0x81, 0x82, 0x83


def boot_sum(data, first):
    s = 0
    for i, b in enumerate(data):
        if first and i in (106, 107, 112):
            continue
        s = (((s & 1) << 31) | (s >> 1)) + b & 0xFFFFFFFF
    return s


def set_sum(ents):
    s = 0
    raw = b"".join(ents)
    for i, b in enumerate(raw):
        if i in (2, 3):
            continue
        s = ((((s & 1) << 15) | (s >> 1)) + b) & 0xFFFF
    return s


class Volume:
    def __init__(self, path, offset=0):
        self.path, self.base = path, offset
        self.f = open(path, "r+b")
        bs = self.rd(0, 512)
        assert bs[3:11] == b"EXFAT   ", "not exFAT"
        self.bs = bs
        self.bps = 1 << bs[108]
        self.cb = self.bps << bs[109]
        self.fat_off, self.fat_len, self.heap, self.ncl, self.root = struct.unpack_from("<IIIII", bs, 80)
        self.flags = struct.unpack_from("<H", bs, 106)[0]
        self.upcase = list(range(65536))
        self.bitmap_clus = None
        root_chain = self.chain(self.root)
        self.root_chain = root_chain
        for e in self.dir_entries(root_chain, len(root_chain) * self.cb):
            if e[0] == BITMAP:
                self.bitmap_clus = struct.unpack_from("<I", e, 20)[0]
                self.bitmap_len = struct.unpack_from("<Q", e, 24)[0]
            elif e[0] == UPCASE:
                c, n = struct.unpack_from("<I", e, 20)[0], struct.unpack_from("<Q", e, 24)[0]
                self.upcase_sum = struct.unpack_from("<I", e, 4)[0]
                raw = self.read_clusters(self.chain(c), n)
                self.upcase_raw = raw
                idx, i = 0, 0
                while i + 1 < len(raw):
                    v = struct.unpack_from("<H", raw, i)[0]
                    i += 2
                    if v == 0xFFFF and i + 1 < len(raw):
                        idx += struct.unpack_from("<H", raw, i)[0]
                        i += 2
                    else:
                        if idx < 65536:
                            self.upcase[idx] = v
                        idx += 1

    # ── raw access ───────────────────────────────────────────────────
    def rd(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def wr(self, off, data):
        self.f.seek(self.base + off)
        self.f.write(data)

    def coff(self, c):
        return self.heap * self.bps + (c - 2) * self.cb

    def fat(self, c):
        return struct.unpack("<I", self.rd(self.fat_off * self.bps + 4 * c, 4))[0]

    def set_fat(self, c, v):
        self.wr(self.fat_off * self.bps + 4 * c, struct.pack("<I", v))

    def chain(self, c, limit=None):
        out = []
        while 2 <= c < self.ncl + 2 and len(out) <= self.ncl:
            out.append(c)
            if limit is not None and len(out) >= limit:
                break
            c = self.fat(c)
        return out

    def read_clusters(self, clus, n):
        out = bytearray()
        for c in clus:
            if len(out) >= n:
                break
            out += self.rd(self.coff(c), min(self.cb, n - len(out)))
        return bytes(out)

    def bit(self, c):
        b = self.rd(self.coff(self.bitmap_clus) + (c - 2) // 8, 1)[0]
        return (b >> ((c - 2) & 7)) & 1

    def set_bit(self, c, on):
        off = self.coff(self.bitmap_clus) + (c - 2) // 8
        b = self.rd(off, 1)[0]
        b = b | (1 << ((c - 2) & 7)) if on else b & ~(1 << ((c - 2) & 7))
        self.wr(off, bytes([b]))

    def dir_entries(self, clus, length):
        raw = self.read_clusters(clus, length)
        for i in range(0, len(raw) - 31, 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                return
            yield e

    # ── names ────────────────────────────────────────────────────────
    def up(self, units):
        return [self.upcase[u] for u in units]

    def name_hash(self, units):
        h = 0
        for u in self.up(units):
            for b in (u & 0xFF, u >> 8):
                h = ((((h & 1) << 15) | (h >> 1)) + b) & 0xFFFF
        return h

    # ── reading ──────────────────────────────────────────────────────
    def sets(self, clus, length):
        """[(index, [entries], name, stream fields)] of a directory."""
        raw = self.read_clusters(clus, length)
        ents = [raw[i:i + 32] for i in range(0, len(raw) - 31, 32)]
        out, i = [], 0
        while i < len(ents):
            e = ents[i]
            if e[0] == 0:
                break
            if e[0] != FILE:
                i += 1
                continue
            sc = e[1]
            group = ents[i:i + 1 + sc]
            st = group[1]
            nl = st[3]
            units = []
            for ne in group[2:]:
                units += list(struct.unpack_from("<15H", ne, 2))
            units = units[:nl]
            name = struct.pack(f"<{len(units)}H", *units).decode("utf-16-le", "replace")
            out.append((i, group, name, units))
            i += 1 + sc
        return out

    def node(self, group):
        f, s = group[0], group[1]
        attr = struct.unpack_from("<H", f, 4)[0]
        flags = s[1]
        valid = struct.unpack_from("<Q", s, 8)[0]
        first = struct.unpack_from("<I", s, 20)[0]
        size = struct.unpack_from("<Q", s, 24)[0]
        return attr, flags, valid, first, size

    def clusters_of(self, flags, first, size):
        n = -(-size // self.cb)
        if n == 0:
            return []
        if flags & 2:
            return list(range(first, first + n))
        return self.chain(first, n)

    def walk(self, clus, length, prefix, files, dirs, sizes, owners, problems):
        for idx, group, name, units in self.sets(clus, length):
            path = prefix + name
            if set_sum(group) != struct.unpack_from("<H", group[0], 2)[0]:
                problems.append(f"{path}: SetChecksum mismatch")
            if self.name_hash(units) != struct.unpack_from("<H", group[1], 4)[0]:
                problems.append(f"{path}: NameHash mismatch")
            if group[1][0] != STREAM or any(g[0] != NAME for g in group[2:]):
                problems.append(f"{path}: malformed entry set")
            attr, flags, valid, first, size = self.node(group)
            cl = self.clusters_of(flags, first, size)
            if len(cl) != -(-size // self.cb):
                problems.append(f"{path}: chain has {len(cl)} clusters for {size} bytes")
            if valid > size:
                problems.append(f"{path}: ValidDataLength > DataLength")
            for c in cl:
                if c in owners:
                    problems.append(f"{path}: cluster {c} also used by {owners[c]}")
                owners[c] = path
            if attr & 0x10:
                dirs.append(path)
                if size % self.cb:
                    problems.append(f"{path}: directory length not a cluster multiple")
                self.walk(cl, size, path + "/", files, dirs, sizes, owners, problems)
            else:
                h = hashlib.md5()
                left, pos = valid, 0
                for c in cl:
                    if left <= 0:
                        break
                    chunk = self.rd(self.coff(c), min(self.cb, left))
                    h.update(chunk)
                    left -= len(chunk)
                pad = size - valid
                while pad > 0:
                    k = min(pad, 1 << 20)
                    h.update(bytes(k))
                    pad -= k
                files[path] = h.hexdigest()
                sizes[path] = size

    def tree(self, problems=None):
        files, dirs, sizes, owners = {}, [], {}, {}
        self.walk(self.root_chain, len(self.root_chain) * self.cb, "", files, dirs, sizes, owners,
                  problems if problems is not None else [])
        self._owners = owners
        return files, dirs, sizes

    def check(self):
        """Problems this checker finds (independently of fsck.exfat)."""
        problems = []
        self.tree(problems)
        owners = dict(self._owners)
        sysc = list(self.root_chain) + self.chain(self.bitmap_clus, -(-self.bitmap_len // self.cb))
        for e in self.dir_entries(self.root_chain, len(self.root_chain) * self.cb):
            if e[0] == UPCASE:
                c, n = struct.unpack_from("<I", e, 20)[0], struct.unpack_from("<Q", e, 24)[0]
                sysc += self.chain(c, -(-n // self.cb))
        for c in sysc:
            owners[c] = "(system)"
        bm = self.read_clusters(self.chain(self.bitmap_clus), self.bitmap_len)
        marked = {c for c in range(2, self.ncl + 2) if (bm[(c - 2) // 8] >> ((c - 2) & 7)) & 1}
        used = set(owners)
        if marked - used:
            problems.append(f"{len(marked - used)} clusters marked in the bitmap but unused "
                            f"(e.g. {sorted(marked - used)[:5]})")
        if used - marked:
            problems.append(f"{len(used - marked)} clusters in use but free in the bitmap "
                            f"(e.g. {sorted(used - marked)[:5]})")
        flags = struct.unpack_from("<H", self.rd(0, 512), 106)[0]
        if flags & 2:
            problems.append("VolumeDirty is set")
        return problems

    # ── writing ──────────────────────────────────────────────────────
    def free_run(self, n, start=2):
        run, first = 0, None
        for c in range(start, self.ncl + 2):
            if self.bit(c):
                run = 0
                continue
            if run == 0:
                first = c
            run += 1
            if run == n:
                return first
        raise RuntimeError("no space")

    def alloc(self, n, fragment=False):
        if n == 0:
            return [], True
        if not fragment:
            first = self.free_run(n)
            cl = list(range(first, first + n))
            for c in cl:
                self.set_bit(c, 1)
            return cl, True
        cl, c = [], 2
        while len(cl) < n:
            if not self.bit(c) and (not cl or c > cl[-1] + 1):
                cl.append(c)
            c += 1
        for c in cl:
            self.set_bit(c, 1)
        for a, b in zip(cl, cl[1:] + [0xFFFFFFFF]):
            self.set_fat(a, b)
        return cl, False

    def lookup_dir(self, path):
        """(clusters, length, entry location or None) of directory `path`."""
        clus, length, loc = self.root_chain, len(self.root_chain) * self.cb, None
        if not path:
            return clus, length, loc
        for part in path.split("/"):
            for idx, group, name, units in self.sets(clus, length):
                if name == part:
                    attr, flags, valid, first, size = self.node(group)
                    loc = (clus, idx, len(group))
                    clus, length = self.clusters_of(flags, first, size), size
                    break
            else:
                raise KeyError(path)
        return clus, length, loc

    def ent_off(self, clus, idx):
        return self.coff(clus[idx * 32 // self.cb]) + (idx * 32) % self.cb

    def add_set(self, parent, name, attr, flags, first, size, valid):
        clus, length, _ = self.lookup_dir(parent)
        units = list(struct.unpack(f"<{len(name.encode('utf-16-le')) // 2}H", name.encode("utf-16-le")))
        nn = -(-len(units) // 15)
        ts = (45 << 25) | (6 << 21) | (15 << 16) | (12 << 11) | (30 << 5) | 5   # 2025-06-15 12:30:10
        f = bytearray(32)
        f[0], f[1] = FILE, 1 + nn
        struct.pack_into("<HHIII", f, 4, attr, 0, ts, ts, ts)
        f[20], f[21], f[22], f[23], f[24] = 0, 0, 0x80 | 12, 0x80 | 12, 0x80 | 12   # UTC+3
        s = bytearray(32)
        s[0], s[1], s[3] = STREAM, flags, len(units)
        struct.pack_into("<H", s, 4, self.name_hash(units))
        struct.pack_into("<Q", s, 8, valid)
        struct.pack_into("<IQ", s, 20, first, size)
        ents = [f, s]
        for i in range(nn):
            e = bytearray(32)
            e[0] = NAME
            part = units[i * 15:(i + 1) * 15]
            struct.pack_into(f"<{len(part)}H", e, 2, *part)
            ents.append(e)
        struct.pack_into("<H", f, 2, set_sum([bytes(x) for x in ents]))
        # free slots
        raw = self.read_clusters(clus, length)
        need, run, at = len(ents), 0, None
        for i in range(0, len(raw) // 32):
            if raw[i * 32] & 0x80:
                run = 0
                continue
            run += 1
            if run == need:
                at = i - need + 1
                break
        if at is None:
            raise RuntimeError(f"directory {parent!r} full (the builder does not grow directories)")
        for k, e in enumerate(ents):
            self.wr(self.ent_off(clus, at + k), bytes(e))

    def add_dir(self, path, clusters=1):
        parent, _, name = path.rpartition("/")
        cl, _ = self.alloc(clusters)
        for c in cl:
            self.wr(self.coff(c), bytes(self.cb))
        self.add_set(parent, name, 0x10, 0x03, cl[0], clusters * self.cb, clusters * self.cb)

    def add_file(self, path, data, fragment=False, size=None):
        parent, _, name = path.rpartition("/")
        size = len(data) if size is None else size
        n = -(-size // self.cb)
        cl, contiguous = self.alloc(n, fragment)
        for i, c in enumerate(cl):
            chunk = data[i * self.cb:(i + 1) * self.cb]
            if chunk:
                self.wr(self.coff(c), chunk)
        flags = 0x01 | (0x02 if contiguous and n else 0)
        self.add_set(parent, name, 0x20, flags, cl[0] if cl else 0, size, len(data))

    def write_at(self, path, off, data):
        """Overwrite bytes of an existing file (within its ValidDataLength)."""
        parent, _, name = path.rpartition("/")
        clus, length, _ = self.lookup_dir(parent)
        for idx, group, nm, units in self.sets(clus, length):
            if nm == name:
                attr, flags, valid, first, size = self.node(group)
                cl = self.clusters_of(flags, first, size)
                while data:
                    c = cl[off // self.cb]
                    k = min(len(data), self.cb - off % self.cb)
                    self.wr(self.coff(c) + off % self.cb, data[:k])
                    data, off = data[k:], off + k
                return
        raise KeyError(path)

    def read_at(self, path, off, n):
        parent, _, name = path.rpartition("/")
        clus, length, _ = self.lookup_dir(parent)
        for idx, group, nm, units in self.sets(clus, length):
            if nm == name:
                attr, flags, valid, first, size = self.node(group)
                cl = self.clusters_of(flags, first, size)
                out = b""
                while n > 0:
                    c = cl[off // self.cb]
                    k = min(n, self.cb - off % self.cb)
                    out += self.rd(self.coff(c) + off % self.cb, k)
                    n, off = n - k, off + k
                return out
        raise KeyError(path)

    def close(self):
        self.f.close()
