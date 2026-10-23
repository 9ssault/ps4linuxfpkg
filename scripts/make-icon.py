#!/usr/bin/env python3
"""Generate sce_sys/icon0.png (512x512) with no external dependencies.

Draws a simple penguin on a dark gradient so the package has its own icon
instead of borrowing artwork from another project.
"""
import math
import os
import struct
import zlib

W = H = 512


def lerp(a, b, t):
    return int(a + (b - a) * t)


def in_ellipse(x, y, cx, cy, rx, ry):
    dx = (x - cx) / rx
    dy = (y - cy) / ry
    return dx * dx + dy * dy <= 1.0


def triangle(x, y, ax, ay, bx, by, cx, cy):
    d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
    d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
    d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
    neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (neg and pos)


def pixel(x, y):
    # PlayStation blue gradient background.
    t = y / (H - 1)
    r = lerp(0, 0, t)
    g = lerp(26, 65, t)
    b = lerp(58, 143, t)

    # Faint horizontal sheen.
    if y % 6 == 0:
        r, g, b = r + 3, g + 6, b + 10

    cx, cy = W / 2, H / 2 - 6

    # Drawn back-to-front: the first matching test wins in this chain, so the
    # details have to be tested before the body silhouette they sit on.
    # Feet
    if in_ellipse(x, y, cx - 52, cy + 178, 46, 20) or in_ellipse(x, y, cx + 52, cy + 178, 46, 20):
        r, g, b = 255, 159, 28
    # Wings
    elif in_ellipse(x, y, cx - 145, cy + 30, 24, 108) or in_ellipse(x, y, cx + 145, cy + 30, 24, 108):
        r, g, b = 214, 232, 255
    # Beak
    elif triangle(x, y, cx - 28, cy - 26, cx + 28, cy - 26, cx, cy + 10):
        r, g, b = 255, 159, 28
    # Pupils, then eyes behind them
    elif in_ellipse(x, y, cx - 40, cy - 78, 11, 13) or in_ellipse(x, y, cx + 40, cy - 78, 11, 13):
        r, g, b = 8, 26, 48
    elif in_ellipse(x, y, cx - 44, cy - 82, 29, 31) or in_ellipse(x, y, cx + 44, cy - 82, 29, 31):
        r, g, b = 242, 247, 255
    # Belly
    elif in_ellipse(x, y, cx, cy + 74, 92, 122):
        r, g, b = 0, 112, 209
    # Body silhouette
    elif in_ellipse(x, y, cx, cy + 10, 146, 186):
        r, g, b = 242, 247, 255

    # Rounded frame corner vignette so it reads as an app icon.
    dx = abs(x - W / 2) / (W / 2)
    dy = abs(y - H / 2) / (H / 2)
    if dx ** 6 + dy ** 6 > 1.0:
        return (0, 0, 0)
    return (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)))


def main():
    raw = bytearray()
    for y in range(H):
        raw.append(0)  # filter type 0
        for x in range(W):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")

    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "sce_sys", "icon0.png")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(png)
    print("wrote %s (%d bytes)" % (out, len(png)))


if __name__ == "__main__":
    main()
