#!/usr/bin/env python3
"""Generate a Windows-7-style aurora wallpaper (1024x768 P6 PPM)."""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "testfiles", "wallpaper.ppm")

W, H = 1920, 1080
img = Image.new("RGB", (W, H))
px = img.load()

# Deep blue vertical gradient base (Win7 default vibe)
for y in range(H):
    t = y / H
    r = int(8 + 30 * t)
    g = int(40 + 80 * t)
    b = int(90 + 110 * t)
    for x in range(W):
        px[x, y] = (r, g, b)

# Aurora light streaks (translucent curved bands)
overlay = Image.new("RGB", (W, H), (0, 0, 0))
od = ImageDraw.Draw(overlay, "RGBA")
for i, (cx, amp, hue) in enumerate([
        (300, 180, (60, 180, 255)), (620, 240, (120, 220, 255)),
        (860, 160, (40, 140, 230)), (480, 300, (180, 240, 255))]):
    pts = []
    for y in range(-50, H + 50, 8):
        x = cx + amp * math.sin(y / 170.0 + i * 1.7) * (0.4 + 0.6 * y / H)
        pts.append((x, y))
    od.line(pts, fill=hue + (160,), width=110 - i * 14)
overlay = overlay.filter(ImageFilter.GaussianBlur(40))
bright = overlay.convert("L").point(lambda v: min(255, int(v * 2.5)))
img = Image.composite(Image.blend(img, overlay, 0.65), img, bright)

# Soft glow bottom-left like the Win7 beta fish spot
glow = Image.new("L", (W, H), 0)
gd = ImageDraw.Draw(glow)
gd.ellipse((80, H - 380, 560, H + 80), fill=90)
glow = glow.filter(ImageFilter.GaussianBlur(80))
white = Image.new("RGB", (W, H), (120, 200, 255))
img = Image.composite(white, img, glow.point(lambda v: v // 2))
img = img.convert("RGB")

with open(OUT, "wb") as f:
    f.write(b"P6\n%d %d\n255\n" % (W, H))
    f.write(img.tobytes())
print(OUT)
