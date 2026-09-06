#!/usr/bin/env python3
"""Generate a GTK 3.24 icon-theme.cache (big-endian, format version 1.0).

GTK loads symbolic/window-control icons by looking up a name (e.g.
"window-minimize-symbolic") in the icon theme.  Without a cache, GTK 3 *should*
scan the theme dirs, but in our minimal setup that resolution fails and GTK
falls back to its compiled-in gresource icon (which can't be decoded) → NULL
pixbuf → crash.  A valid icon-theme.cache makes GTK resolve the icon from our
on-disk hicolor theme instead.

Format (all multi-byte integers BIG-ENDIAN), from gtk/gtkiconcache.c +
gtk/updateiconcache.c:

  header:  u16 major=1, u16 minor=0, u32 hash_off, u32 dirlist_off
  dirlist: u32 n_dirs, u32 dir_str_off[n_dirs]
  hash:    u32 n_buckets, u32 bucket_off[n_buckets]   (0xffffffff = empty)
  icon:    u32 chain_off, u32 name_str_off, u32 image_list_off
  imglist: u32 n_images, then per image: u16 dir_index, u16 flags, u32 data_off
  strings: NUL-terminated

The icon NAME stored is the filename with its LAST extension stripped
(updateiconcache.c: strrchr('.')), so "window-minimize-symbolic.png" -> key
"window-minimize-symbolic" (matches GTK's lookup).  flags: PNG=4, SVG=2, XPM=1.
"""
import os, sys, struct

HAS_SUFFIX_XPM, HAS_SUFFIX_SVG, HAS_SUFFIX_PNG, HAS_ICON_FILE = 1, 2, 4, 8

def icon_name_hash(s):
    b = s.encode('utf-8', 'surrogateescape')
    def sb(x):           # signed char
        return x - 256 if x >= 128 else x
    h = sb(b[0]) & 0xffffffff
    for c in b[1:]:
        h = ((h << 5) - h + sb(c)) & 0xffffffff
    return h

def spaced_prime(n):
    # any prime >= n works; small table is enough for our icon counts
    for p in (5,7,11,13,17,29,37,53,67,79,97,131,163,197,239,293,353,431,521,
              631,761,919,1103,1327,1597,1931,2333,2801,3371,4049,4861,5839,
              7013,8419,10103,12143,14591,17519,21023,25229,30293,36353):
        if p >= n:
            return p
    return 43669

def main():
    theme = sys.argv[1]                      # .../hicolor
    # 1. collect directories (relative) that contain images, and icons per dir
    dirs = []                                # list of relative dir paths
    icons = {}                               # name -> list[(dir_index, flags)]
    for root, _, files in os.walk(theme):
        rel = os.path.relpath(root, theme)
        if rel == '.':
            continue
        di = None
        for fn in files:
            if fn.endswith('.png'):   fl = HAS_SUFFIX_PNG
            elif fn.endswith('.svg'): fl = HAS_SUFFIX_SVG
            elif fn.endswith('.xpm'): fl = HAS_SUFFIX_XPM
            elif fn.endswith('.icon'):fl = HAS_ICON_FILE
            else: continue
            name = fn.rsplit('.', 1)[0]      # strip last extension
            if di is None:
                di = len(dirs); dirs.append(rel)
            lst = icons.setdefault(name, {})
            lst[di] = lst.get(di, 0) | fl
    if not icons:
        print("no icons found", file=sys.stderr); sys.exit(1)

    # 2. build string pool (deferred offsets) — collect all strings first
    strings = {}
    def add_str(s):
        strings.setdefault(s, None)
    for d in dirs: add_str(d)
    for name in icons: add_str(name)

    n_buckets = spaced_prime(len(icons) // 3 + 1)

    # 3. lay out the file.  Order: header, hash(buckets), icon entries,
    #    image lists, dir list, strings.  Two passes: assign offsets, then emit.
    HEADER = 12
    hash_off = HEADER
    hash_size = 4 + 4 * n_buckets
    # icon entries: 12 bytes each
    names = list(icons.keys())
    icon_entry_off = {}
    off = hash_off + hash_size
    for name in names:
        icon_entry_off[name] = off
        off += 12
    # image lists
    imglist_off = {}
    for name in names:
        imglist_off[name] = off
        off += 4 + 8 * len(icons[name])
    # directory list
    dirlist_off = off
    off += 4 + 4 * len(dirs)
    # strings
    for s in strings:
        strings[s] = off
        off += len(s.encode('utf-8', 'surrogateescape')) + 1

    # 4. bucket chains
    buckets = [0xffffffff] * n_buckets
    # group icons by bucket, chain in order
    for name in names:
        b = icon_name_hash(name) % n_buckets
        # prepend
        chain = buckets[b]
        # we store chain as "current head"; set this icon's chain to old head
        icons[name]['_chain'] = chain
        buckets[b] = icon_entry_off[name]

    # 5. emit
    buf = bytearray(off)
    def w16(o, v): struct.pack_into('>H', buf, o, v & 0xffff)
    def w32(o, v): struct.pack_into('>I', buf, o, v & 0xffffffff)
    # header
    w16(0, 1); w16(2, 0); w32(4, hash_off); w32(8, dirlist_off)
    # hash
    w32(hash_off, n_buckets)
    for i, bo in enumerate(buckets):
        w32(hash_off + 4 + 4*i, bo)
    # icon entries + image lists
    for name in names:
        eo = icon_entry_off[name]
        w32(eo, icons[name]['_chain'])
        w32(eo + 4, strings[name])
        w32(eo + 8, imglist_off[name])
        imgs = [(di, fl) for di, fl in icons[name].items() if di != '_chain']
        ilo = imglist_off[name]
        w32(ilo, len(imgs))
        for j, (di, fl) in enumerate(imgs):
            w16(ilo + 4 + 8*j, di)
            w16(ilo + 4 + 8*j + 2, fl)
            w32(ilo + 4 + 8*j + 4, 0)        # no embedded image data
    # dir list
    w32(dirlist_off, len(dirs))
    for i, d in enumerate(dirs):
        w32(dirlist_off + 4 + 4*i, strings[d])
    # strings
    for s, so in strings.items():
        sb = s.encode('utf-8', 'surrogateescape') + b'\x00'
        buf[so:so+len(sb)] = sb

    outp = os.path.join(theme, 'icon-theme.cache')
    with open(outp, 'wb') as f:
        f.write(buf)
    print(f"wrote {outp}: {len(buf)} bytes, {len(icons)} icons, "
          f"{len(dirs)} dirs, {n_buckets} buckets")

if __name__ == '__main__':
    main()
