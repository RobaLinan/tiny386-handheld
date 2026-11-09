#!/usr/bin/env python3
"""Convert an image into an 800x480 RGB565 background for the boot menu.

The panel is scanned portrait and the VGA framebuffer stores logical
(x, y) at x * 480 + y, so the output is transposed to that layout and can
be read straight into the DPI frame buffer.

Usage: menu_bg.py <input image> <output.raw> [dim]
"""

import sys

from PIL import Image

W, H = 800, 480


def cover(im):
    im = im.convert("RGB")
    sw, sh = im.size
    scale = max(W / sw, H / sh)
    nw, nh = max(W, int(sw * scale + 0.5)), max(H, int(sh * scale + 0.5))
    im = im.resize((nw, nh), Image.LANCZOS)
    left = (nw - W) // 2
    top = (nh - H) // 2
    return im.crop((left, top, left + W, top + H))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dim = float(sys.argv[3]) if len(sys.argv) > 3 else 0.62
    im = cover(Image.open(sys.argv[1]))
    px = im.load()
    out = bytearray(W * H * 2)
    for y in range(H):
        for x in range(W):
            r, g, b = px[x, y]
            r, g, b = int(r * dim), int(g * dim), int(b * dim)
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            o = (x * H + y) * 2
            out[o] = v & 0xff
            out[o + 1] = v >> 8
    with open(sys.argv[2], "wb") as f:
        f.write(out)
    print(f"{sys.argv[2]}: {W}x{H}, dim {dim}")


if __name__ == "__main__":
    main()
