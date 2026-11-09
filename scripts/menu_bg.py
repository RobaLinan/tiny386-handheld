#!/usr/bin/env python3
"""Convert an image into boot-menu backgrounds for the 800x480 panel.

Writes two files:
  <out>            800x480 full-screen background
  <out base>_thumb.raw   380x214 thumbnail for the game card

The panel is scanned portrait and the menu stores logical (x, y) at
x * 480 + y, so both outputs are transposed to that layout.

Usage: menu_bg.py <input image> <output.raw> [dim]
"""

import os
import sys

from PIL import Image

W, H = 800, 480
TW, TH = 380, 214


def cover(im, w, h):
    im = im.convert("RGB")
    sw, sh = im.size
    scale = max(w / sw, h / sh)
    nw, nh = max(w, int(sw * scale + 0.5)), max(h, int(sh * scale + 0.5))
    im = im.resize((nw, nh), Image.LANCZOS)
    left = (nw - w) // 2
    top = (nh - h) // 2
    return im.crop((left, top, left + w, top + h))


def write_raw(path, im, w, h, dim):
    px = im.load()
    out = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            r, g, b = int(r * dim), int(g * dim), int(b * dim)
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            o = (x * h + y) * 2
            out[o] = v & 0xff
            out[o + 1] = v >> 8
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: {w}x{h}")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dim = float(sys.argv[3]) if len(sys.argv) > 3 else 0.62
    src = Image.open(sys.argv[1])
    write_raw(sys.argv[2], cover(src, W, H), W, H, dim)
    base, _ = os.path.splitext(sys.argv[2])
    write_raw(base + "_thumb.raw", cover(src, TW, TH), TW, TH, dim)


if __name__ == "__main__":
    main()
