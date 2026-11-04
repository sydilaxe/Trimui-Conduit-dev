#!/usr/bin/env python3
"""Draw Apps/Conduit/icon.png, the 128x128 TrimUI OS app icon: a waveform in a loop.

usage: mkicon.py OUT.png
"""
import sys
from PIL import Image, ImageDraw

S = 4  # drawn 4x and scaled down, for smooth edges
img = Image.new("RGBA", (128 * S, 128 * S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((4 * S, 4 * S, 124 * S, 124 * S), radius=24 * S, fill=(24, 28, 38, 255))
d.ellipse((18 * S, 18 * S, 110 * S, 110 * S), outline=(255, 170, 40, 255), width=7 * S)
pts = [(26, 64), (40, 64), (47, 44), (55, 88), (64, 34), (73, 92), (80, 50), (86, 64), (102, 64)]
d.line([(x * S, y * S) for x, y in pts], fill=(120, 220, 255, 255), width=7 * S, joint="curve")
img.resize((128, 128), Image.LANCZOS).save(sys.argv[1])
