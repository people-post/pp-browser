#!/usr/bin/env python3
"""Writes the two chat pictures the UI demo app seeds (assets/demo/, see DEV/PLAN-ui-demo-data.md).

Pure standard library (zlib + struct, no PIL): a 640x480 gradient landscape and a 480x640 portrait made of colour
blocks. They look clearly different, so a screen check can tell the two apart. Output is deterministic.

    python3 scripts/ui/gen_demo_images.py
"""
import math
import os
import struct
import zlib

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "assets", "demo")


def write_png(path, width, height, pixel):
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter: none
        for x in range(width):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))  # 8-bit RGB
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def landscape(x, y):
    # Sky fading from deep blue to warm orange, a dark hill line, a sun disc.
    w, h = 640, 480
    t = y / (h - 1)
    r = int(30 + 225 * t)
    g = int(60 + 120 * t)
    b = int(150 - 70 * t)
    hill = 340 + int(40 * math.sin(x / 70.0))
    if y > hill:
        shade = (y - hill) / (h - hill)
        return (int(20 + 25 * shade), int(70 - 30 * shade), int(45 - 15 * shade))
    if (x - 470) ** 2 + (y - 190) ** 2 < 45 ** 2:
        return (255, 236, 160)
    return (r, g, b)


def portrait(x, y):
    # Four horizontal bands of colour blocks with a lighter frame.
    w, h = 480, 640
    if x < 12 or y < 12 or x >= w - 12 or y >= h - 12:
        return (245, 245, 245)
    band = y * 4 // h
    col = x * 3 // w
    palette = [
        [(224, 80, 80), (240, 170, 60), (250, 220, 90)],
        [(70, 170, 110), (60, 150, 200), (90, 90, 190)],
        [(160, 80, 190), (230, 110, 160), (120, 120, 120)],
        [(40, 40, 50), (80, 80, 100), (200, 200, 210)],
    ]
    return palette[band][col]


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    write_png(os.path.join(OUT_DIR, "demo_landscape.png"), 640, 480, landscape)
    write_png(os.path.join(OUT_DIR, "demo_portrait.png"), 480, 640, portrait)
