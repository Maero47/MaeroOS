#!/usr/bin/env python3
"""
mkicons.py — rasterize a curated set of the user's Reversal-blue SVG icon
theme into MaeroOS .mic raster icons (full-color RGBA, anti-aliased).

MaeroOS can't render SVG live, so we pre-render only the icons the desktop
UI actually uses, at the sizes it draws.  Output goes to testfiles/icons/.

.mic format (little-endian):
    u32 magic   = 'MIC1' (0x3143494D)
    u16 width
    u16 height
    width*height*4 bytes RGBA (straight alpha, top-to-bottom)

Requires an SVG rasterizer: rsvg-convert (brew install librsvg) preferred,
else cairosvg.  Falls back gracefully (skips) so the build still works with
the legacy hex-art icons if no rasterizer is present.
"""
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THEME = os.path.expanduser("~/Downloads/Reversal-blue/Reversal-blue")
OUT = os.path.join(ROOT, "testfiles", "icons")

# MaeroOS icon name -> theme-relative SVG (without .svg).  Tried in order.
ICONS = {
    "terminal":  ["apps/scalable/utilities-x-terminal", "apps/scalable/terminal-1"],
    "browser":   ["apps/scalable/web-browser"],
    "firefox":   ["apps/scalable/firefox"],
    "files":     ["apps/scalable/system-file-manager"],
    "editor":    ["apps/scalable/accessories-text-editor"],
    "calc":      ["apps/scalable/accessories-calculator"],
    "sysmon":    ["apps/scalable/utilities-system-monitor"],
    "settings":  ["apps/scalable/preferences-system"],
    "store":     ["apps/scalable/com.gnome.Software", "apps/scalable/system-software-install"],
    "folder":    ["places/scalable/folder"],
    "home":      ["places/scalable/user-home"],
    "text":      ["mimes/scalable/text-x-generic"],
    "image":     ["mimes/scalable/image-x-generic"],
    "exec":      ["mimes/scalable/application-x-executable"],
    "pdf":       ["mimes/scalable/application-pdf"],
    "file":      ["mimes/scalable/unknown"],
    "disk":      ["devices/scalable/drive-harddisk"],
    "doom":      ["apps/scalable/doom", "mimes/scalable/application-x-executable"],
    "links":     ["apps/scalable/web-browser"],
    "busybox":   ["apps/scalable/utilities-x-terminal", "apps/scalable/terminal-1"],
    "start":     ["apps/scalable/applications-system", "places/24/start-here"],
}

# Sizes rendered per icon: name suffix -> pixels.  Desktop uses 48; taskbar 24.
SIZES = {"": 48, "_24": 24}


def find_svg(candidates):
    for c in candidates:
        p = os.path.join(THEME, c + ".svg")
        if os.path.exists(p):
            return p
    return None


def have_rsvg():
    try:
        subprocess.run(["rsvg-convert", "--version"], capture_output=True,
                       check=True)
        return True
    except Exception:
        return False


def rasterize(svg_path, px):
    """SVG -> list of RGBA bytes at px*px, via rsvg-convert then PIL."""
    from PIL import Image
    import io
    png = subprocess.run(
        ["rsvg-convert", "-w", str(px), "-h", str(px), "-a", svg_path],
        capture_output=True, check=True).stdout
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    if img.size != (px, px):
        canvas = Image.new("RGBA", (px, px), (0, 0, 0, 0))
        ox = (px - img.width) // 2
        oy = (px - img.height) // 2
        canvas.alpha_composite(img, (max(0, ox), max(0, oy)))
        img = canvas
    return img.tobytes()


def write_mic(path, px, rgba):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHH", 0x3143494D, px, px))
        f.write(rgba)


def main():
    if not os.path.isdir(THEME):
        print(f"mkicons: theme not found at {THEME}; keeping hex-art icons.")
        return 0
    if not have_rsvg():
        print("mkicons: rsvg-convert not found (brew install librsvg); "
              "keeping hex-art icons.")
        return 0
    os.makedirs(OUT, exist_ok=True)
    made = 0
    for name, cands in ICONS.items():
        svg = find_svg(cands)
        if not svg:
            print(f"mkicons: no SVG for {name} ({cands[0]}); skipped")
            continue
        for suffix, px in SIZES.items():
            try:
                rgba = rasterize(svg, px)
            except Exception as e:
                print(f"mkicons: {name}{suffix} failed: {e}")
                continue
            write_mic(os.path.join(OUT, f"{name}{suffix}.mic"), px, rgba)
            made += 1
    print(f"mkicons: wrote {made} .mic icons to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
