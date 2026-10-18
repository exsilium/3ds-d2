#!/usr/bin/env python3
"""Lists and extracts files from a Diablo II MPQ (format version 0).

    tools/mpq_extract.py <archive.mpq>                     list (names from (listfile))
    tools/mpq_extract.py <archive.mpq> <name> <out-file>   extract one file

Handles what the cinematics archives use: encrypted hash/block tables,
stored, zlib-compressed and (single-sector) encrypted files. PKWARE implode
and the audio codecs are not supported; such files are reported, not decoded.
"""
import struct
import sys
import zlib

_CRYPT = []


def _crypt_table():
    seed = 0x00100001
    table = [0] * 0x500
    for i in range(0x100):
        j = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            hi = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[j] = hi | (seed & 0xFFFF)
            j += 0x100
    return table


def hash_string(s, kind):
    if not _CRYPT:
        _CRYPT.extend(_crypt_table())
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().replace('/', '\\').encode('latin-1'):
        seed1 = (_CRYPT[(kind << 8) + ch] ^ (seed1 + seed2)) & 0xFFFFFFFF
        seed2 = (ch + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def decrypt(data, key):
    if not _CRYPT:
        _CRYPT.extend(_crypt_table())
    n = len(data) // 4
    words = list(struct.unpack('<%dI' % n, data[:n * 4]))
    seed = 0xEEEEEEEE
    for i in range(n):
        seed = (seed + _CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        ch = words[i] ^ ((key + seed) & 0xFFFFFFFF)
        key = ((((~key) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (ch + seed + (seed << 5) + 3) & 0xFFFFFFFF
        words[i] = ch
    return struct.pack('<%dI' % n, *words) + data[n * 4:]


# PKWARE Data Compression Library "implode": a header (literal coding, log2 of
# the dictionary size), then bit-packed tokens read LSB first. Each token is a
# 0 bit + literal, or a 1 bit + length + distance; length 519 ends the stream.
# The length, distance and coded-literal tables are fixed Huffman codes,
# given here as (code length) lists in canonical order of the format.
_LEN_BASE = [3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264]
_LEN_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]


def _expand(compact):
    """Code lengths from the DCL's compact form: each byte is
    (count - 1) << 4 | length, for consecutive symbols."""
    out = []
    for b in compact:
        out += [b & 15] * ((b >> 4) + 1)
    return out


_LEN_BITS = _expand([2, 35, 36, 53, 38, 23])
_DIST_BITS = _expand([2, 20, 53, 230, 247, 151, 248])


def _codes(lengths):
    """Canonical codes as the DCL assigns them (longest codes first, counting
    down), returned as {(length, bit-reversed code): symbol} for LSB reading."""
    table = {}
    code = 0
    order = sorted(range(len(lengths)), key=lambda s: (lengths[s], s))
    # DCL codes: the shortest gets all-zero bits; assignment counts upward in
    # canonical (length, symbol) order, then each code is read MSB first.
    prev = 0
    for s in order:
        code <<= lengths[s] - prev
        prev = lengths[s]
        table[(lengths[s], code)] = s
        code += 1
    return table


def explode(data, want):
    if len(data) < 2:
        raise ValueError('implode: short')
    lit_coded, dict_bits = data[0], data[1]
    if lit_coded:
        raise NotImplementedError('implode: coded literals')
    pos, bitbuf, bitcnt = 2, 0, 0

    def bits(n):
        nonlocal pos, bitbuf, bitcnt
        while bitcnt < n:
            bitbuf |= data[pos] << bitcnt
            pos += 1
            bitcnt += 8
        v = bitbuf & ((1 << n) - 1)
        bitbuf >>= n
        bitcnt -= n
        return v

    def decode(table, maxlen):
        code = 0
        for n in range(1, maxlen + 1):
            code = (code << 1) | (bits(1) ^ 1)   # DCL stores codes inverted
            if (n, code) in table:
                return table[(n, code)]
        raise ValueError('implode: bad code')

    lens, dists = _codes(_LEN_BITS), _codes(_DIST_BITS)
    out = bytearray()
    while len(out) < want:
        if bits(1) == 0:
            out.append(bits(8))
            continue
        sym = decode(lens, 7)
        length = _LEN_BASE[sym] + bits(_LEN_EXTRA[sym])
        if length == 519:
            break
        extra = 2 if length == 2 else dict_bits
        dist = (decode(dists, 8) << extra) + bits(extra) + 1
        for _ in range(length):
            out.append(out[-dist])
    return bytes(out)


class Mpq:
    def __init__(self, path):
        self.f = open(path, 'rb')
        hdr = self.f.read(32)
        magic, hsize, _, ver, shift, hpos, bpos, hcount, bcount = struct.unpack('<4sIIHHIIII', hdr)
        if magic != b'MPQ\x1a':
            raise ValueError('not an MPQ: %s' % path)
        self.sector = 512 << shift
        self.f.seek(hpos)
        h = decrypt(self.f.read(hcount * 16), hash_string('(hash table)', 3))
        self.hashes = [struct.unpack_from('<IIHHI', h, i * 16) for i in range(hcount)]
        self.f.seek(bpos)
        b = decrypt(self.f.read(bcount * 16), hash_string('(block table)', 3))
        self.blocks = [struct.unpack_from('<IIII', b, i * 16) for i in range(bcount)]

    def find(self, name):
        n = len(self.hashes)
        i = hash_string(name, 0) % n
        a, b = hash_string(name, 1), hash_string(name, 2)
        for _ in range(n):
            ha, hb, _, _, blk = self.hashes[i]
            if blk == 0xFFFFFFFF:
                return None
            if ha == a and hb == b and blk != 0xFFFFFFFE:
                return self.blocks[blk]
            i = (i + 1) % n
        return None

    def read(self, name):
        blk = self.find(name)
        if blk is None:
            raise KeyError(name)
        pos, csize, size, flags = blk
        if flags & 0x100:
            raise NotImplementedError('%s: PKWARE implode' % name)
        self.f.seek(pos)
        raw = self.f.read(csize)
        key = None
        if flags & 0x10000:
            base = name.replace('/', '\\').split('\\')[-1]
            key = hash_string(base, 3)
            if flags & 0x20000:
                key = ((key + pos) ^ size) & 0xFFFFFFFF
        if not (flags & 0x200) and not (flags & 0x01000000):
            if key is None:
                return raw[:size]
            out = bytearray()
            for s in range(0, size, self.sector):
                out += decrypt(raw[s:s + self.sector], (key + s // self.sector) & 0xFFFFFFFF)
            return bytes(out[:size])
        if flags & 0x01000000:
            data = decrypt(raw, key) if key is not None else raw
            return self._unpack(data, size) if csize < size else data[:size]
        nsec = (size + self.sector - 1) // self.sector
        table = raw[:(nsec + 1) * 4]
        if key is not None:
            table = decrypt(table, (key - 1) & 0xFFFFFFFF)
        offs = struct.unpack('<%dI' % (nsec + 1), table)
        out = bytearray()
        for s in range(nsec):
            chunk = raw[offs[s]:offs[s + 1]]
            if key is not None:
                chunk = decrypt(chunk, (key + s) & 0xFFFFFFFF)
            want = min(self.sector, size - s * self.sector)
            out += self._unpack(chunk, want) if len(chunk) < want else chunk
        return bytes(out[:size])

    @staticmethod
    def _unpack(chunk, want):
        method = chunk[0]
        if method == 0x02:
            return zlib.decompress(chunk[1:])
        if method == 0x08:
            return explode(chunk[1:], want)
        raise NotImplementedError('compression 0x%02x' % method)

    def listfile(self):
        try:
            text = self.read('(listfile)')
        except (KeyError, NotImplementedError):
            return []
        return [l for l in text.decode('latin-1').replace('\r', '\n').replace(';', '\n').split('\n') if l]


def main(argv):
    if len(argv) == 2:
        m = Mpq(argv[1])
        for name in m.listfile():
            blk = m.find(name)
            if blk:
                print('%10d %10d %08x  %s' % (blk[2], blk[1], blk[3], name))
        return 0
    if len(argv) == 4:
        data = Mpq(argv[1]).read(argv[2])
        with open(argv[3], 'wb') as f:
            f.write(data)
        print('%s: %d bytes' % (argv[3], len(data)))
        return 0
    print(__doc__.strip())
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
