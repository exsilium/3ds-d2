#!/usr/bin/env python3
"""Generates the CIA's HOME-menu art: icon.png (48x48), banner.png (256x128)
and banner.wav (one second of silence). Drawn from scratch (a gradient and a
pixel font), so no game artwork is used or needed.

    python3 -I make_art.py <out-dir>
"""
import struct
import sys
import zlib

FONT = {  # 5x7 pixel glyphs
    '3': ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    'D': ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    'S': ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    '2': ["01110", "10001", "00001", "00110", "01000", "10000", "11111"],
    '-': ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    'I': ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    ' ': ["00000"] * 7,
}


def png(path, w, h, pixels):
    raw = b"".join(b"\x00" + bytes(c for px in pixels[y * w:(y + 1) * w] for c in px) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)))   # RGBA
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def canvas(w, h):
    # Dark red at the top fading to black, like the game's own palette.
    return [(int(110 * (1 - y / h)) + 20, 8, 6, 255) for y in range(h) for _ in range(w)]


def text(px, w, s, x0, y0, scale, color):
    x = x0
    for ch in s:
        glyph = FONT[ch]
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit == "1":
                    for dy in range(scale):
                        for dx in range(scale):
                            px[(y0 + gy * scale + dy) * w + x + gx * scale + dx] = color
        x += 6 * scale


def main(out):
    gold = (230, 180, 70, 255)
    icon = canvas(48, 48)
    text(icon, 48, "D2", 6, 10, 3, gold)
    png(f"{out}/icon.png", 48, 48, icon)

    banner = canvas(256, 128)
    text(banner, 256, "3DS-D2", 22, 30, 6, gold)
    text(banner, 256, "D2", 104, 86, 4, (200, 200, 200, 255))
    png(f"{out}/banner.png", 256, 128, banner)

    rate, samples = 22050, 22050
    data = b"\x00\x00" * samples
    with open(f"{out}/banner.wav", "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


if __name__ == "__main__":
    main(sys.argv[1])
