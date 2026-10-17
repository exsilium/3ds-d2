#!/usr/bin/env python3
"""Writes an empty but valid MPQ archive (format version 0): a 32-byte header,
an encrypted hash table of 16 free entries and an empty block table.

Diablo II only checks that its cinematic and sound archives exist and open.
The 3DS build plays neither (no Bink; audio comes later), so the CIA can carry
these stubs instead of ~1.2 GB of video and sound data.

    python3 -I make_stub_mpq.py <out.mpq> [more.mpq ...]

Format and hashing per the public MPQ documentation; own implementation.
"""
import struct
import sys


def crypt_table():
    table = [0] * 0x500
    seed = 0x00100001
    for i in range(0x100):
        idx = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            hi = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[idx] = hi | (seed & 0xFFFF)
            idx += 0x100
    return table


TABLE = crypt_table()


def hash_string(s, kind):
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().encode("ascii"):
        seed1 = (TABLE[kind * 0x100 + ch] ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed2 = (ch + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def encrypt(words, key):
    seed = 0xEEEEEEEE
    out = []
    for w in words:
        seed = (seed + TABLE[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        out.append((w ^ (key + seed)) & 0xFFFFFFFF)
        key = (((~key << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (w + seed + (seed << 5) + 3) & 0xFFFFFFFF
    return out


def stub(path):
    entries = 16
    # A free hash entry: name hashes, locale/platform and block index all 0xFF.
    hash_table = [0xFFFFFFFF] * (4 * entries)
    enc = encrypt(hash_table, hash_string("(hash table)", 3))
    header_size, hash_off = 32, 32
    block_off = hash_off + 16 * entries
    archive_size = block_off   # empty block table
    with open(path, "wb") as f:
        f.write(b"MPQ\x1a")
        f.write(struct.pack("<IIHHIIII", header_size, archive_size, 0, 3, hash_off, block_off, entries, 0))
        f.write(struct.pack("<%dI" % len(enc), *enc))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        stub(p)
