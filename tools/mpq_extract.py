#!/usr/bin/env python3
"""Lists and extracts files from a Diablo II MPQ (format version 0).

    tools/mpq_extract.py <archive.mpq>                     list (names from (listfile))
    tools/mpq_extract.py <archive.mpq> <name> <out-file>   extract one file

Handles encrypted hash/block tables, stored, zlib-compressed, PKWARE-imploded
and encrypted files. The compressed WAVs' codecs (Storm's Huffman and ADPCM)
need tables only Game.exe holds: Mpq(path, AudioTables(...)) decodes them,
without it they are reported, not decoded.
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


# ---- the compressed WAVs: Storm's adaptive Huffman, then its ADPCM ----------
# Ports of src/game/d2_native.cpp's HuffTree and adpcm_decode (the tree logic
# follows StormLib's huff.cpp, Ladislav Zezula, MIT), without the quick-link
# lookups, which never change what is decoded.

class AudioTables:
    """Storm's codec tables, as Game.exe holds them: the Huffman weights (one
    0x102-byte table per type), ADPCM's 89 step sizes and 32 index changes."""

    def __init__(self, huff_weights, adpcm_steps, adpcm_adj):
        self.huff_weights = huff_weights
        self.adpcm_steps = adpcm_steps
        self.adpcm_adj = adpcm_adj


class _Item:
    __slots__ = ('next', 'prev', 'value', 'weight', 'parent', 'child_lo')

    def __init__(self):
        self.next = self.prev = self.parent = self.child_lo = None
        self.value = self.weight = 0


class _Huffman:
    ITEMS = 515

    def __init__(self, weights):
        self.weights = weights

    # the weight-sorted list: next = lower weight; head.next is the root
    @staticmethod
    def _unlink(i):
        if i.next:
            i.prev.next = i.next
            i.next.prev = i.prev
            i.next = i.prev = None

    @staticmethod
    def _link_after(at, i):
        i.next = at.next
        i.prev = at.next.prev
        at.next.prev = i
        at.next = i

    def _higher_or_equal(self, i, weight):
        head = self.head
        if i:
            while i is not head:
                if i.weight >= weight:
                    return i
                i = i.prev
        return head

    def _create(self, value, weight, before):
        if self.used >= self.ITEMS:
            return None
        self.used += 1
        i = _Item()
        self._link_after(self.head.prev if before else self.head, i)
        i.value, i.weight = value, weight
        return i

    def _fixup(self, i, max_weight):
        if i.weight < max_weight:
            h = self._higher_or_equal(self.head.prev, i.weight)
            self._unlink(i)
            self._link_after(h, i)
            return max_weight
        return i.weight

    def _build(self, kind):
        self.head = head = _Item()
        head.next = head.prev = head
        self.used = 0
        self.by_byte = [None] * 0x102
        w = self.weights[0x102 * kind:0x102 * kind + 0x100]
        max_weight = 0
        for v in range(0x100):
            if w[v]:
                self.by_byte[v] = self._create(v, w[v], False)
                max_weight = self._fixup(self.by_byte[v], max_weight)
        self.by_byte[0x100] = self._create(0x100, 1, True)
        self.by_byte[0x101] = self._create(0x101, 1, True)
        lo = head.prev
        while lo is not head:
            hi = lo.prev
            if hi is head:
                break
            p = self._create(0, hi.weight + lo.weight, False)
            if not p:
                break
            lo.parent = hi.parent = p
            p.child_lo = lo
            max_weight = self._fixup(p, max_weight)
            lo = hi.prev

    def _inc_weights(self, i):
        while i:
            i.weight += 1
            higher = self._higher_or_equal(i.prev, i.weight)
            hi = higher.next
            if hi is not i:
                self._unlink(hi)
                self._link_after(i, hi)
                self._unlink(i)
                self._link_after(higher, i)
                lo = hi.parent.child_lo
                if i.parent.child_lo is i:
                    i.parent.child_lo = hi
                if lo is hi:
                    hi.parent.child_lo = i
                i.parent, hi.parent = hi.parent, i.parent
            i = i.parent

    def _insert_new(self, v1, v2):
        last = self.head.prev
        hi = self._create(v1, last.weight, True)
        if not hi:
            return
        hi.parent = last
        self.by_byte[v1] = hi
        lo = self._create(v2, 0, True)
        if not lo:
            return
        lo.parent = last
        last.child_lo = lo
        self.by_byte[v2] = lo
        self._inc_weights(lo)

    def decompress(self, data, want):
        """The stream is LSB-first bits (a first dword, then 16-bit words,
        which is the same thing); it ends at code 0x100 or a full buffer."""
        data = data + bytes(8)
        pos = 0

        def get8():
            nonlocal pos
            o, b = pos >> 3, pos & 7
            v = ((data[o] | data[o + 1] << 8) >> b) & 0xFF
            pos += 8
            return v

        kind = get8()
        self._build(kind)
        sparse = kind == 0
        out = bytearray()
        head = self.head
        while len(out) < want:
            item = head.next
            while item.child_lo:
                bit = data[pos >> 3] >> (pos & 7) & 1
                pos += 1
                item = item.child_lo.prev if bit else item.child_lo
            v = item.value
            if v == 0x101:
                v = get8()
                self._insert_new(head.prev.value, v)
                if not sparse:
                    self._inc_weights(self.by_byte[v])
            if v == 0x100:
                break
            out.append(v)
            if sparse:
                self._inc_weights(self.by_byte[v])
        return bytes(out)


def adpcm_decompress(data, want, channels, steps, adj):
    """Storm's ADPCM: a byte, the shift, a 16-bit first sample per channel,
    then a byte per sample (channels alternating), see adpcm_decode()."""
    out = bytearray()
    pred, index = [0, 0], [0x2C, 0x2C]
    shift = data[1] & 31
    p = 2
    for i in range(channels):
        s = struct.unpack_from('<h', data, p)[0]
        p += 2
        pred[i & 1] = s
        if len(out) + 2 > want:
            return bytes(out)
        out += struct.pack('<h', s)
    ch = channels - 1
    end = len(data)
    while p < end:
        if channels == 2:
            ch ^= 1
        b = data[p]
        p += 1
        if b & 0x80:
            code = b & 0x7F
            if code == 0:
                if index[ch]:
                    index[ch] -= 1
                if len(out) + 2 > want:
                    break
                out += struct.pack('<h', pred[ch])
            elif code in (1, 2):
                index[ch] = min(index[ch] + 8, 0x58) if code == 1 else max(index[ch] - 8, 0)
                if channels == 2:
                    ch ^= 1   # no sample: the same channel decodes next
            continue
        step = steps[index[ch]]
        diff = step >> shift
        for bit in range(6):
            if b & (1 << bit):
                diff += step >> bit
        v = pred[ch]
        if b & 0x40:
            v = max(v - diff, -32768)
        else:
            v = min(v + diff, 32767)
        pred[ch] = v
        if len(out) + 2 > want:
            break
        out += struct.pack('<h', v)
        index[ch] = min(max(index[ch] + adj[b & 0x1F], 0), 0x58)
    return bytes(out)


class Mpq:
    def __init__(self, path, audio=None):
        self.audio = audio   # AudioTables, for the compressed WAVs
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

    def read(self, name, limit=None):
        """The file, or (limit) only the sectors that hold its first bytes."""
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
        if limit is not None:
            nsec = min(nsec, (limit + self.sector - 1) // self.sector)
        for s in range(nsec):
            chunk = raw[offs[s]:offs[s + 1]]
            if key is not None:
                chunk = decrypt(chunk, (key + s) & 0xFFFFFFFF)
            want = min(self.sector, size - s * self.sector)
            out += self._unpack(chunk, want) if len(chunk) < want else chunk
        return bytes(out[:size])

    def _unpack(self, chunk, want):
        """A sector compressed with one method or a chain of them (the mask
        in its first byte), undone in Storm's order."""
        mask, data = chunk[0], chunk[1:]
        if mask & ~0xCB:
            raise NotImplementedError('compression 0x%02x' % mask)
        if mask & 0xC1 and self.audio is None:
            raise NotImplementedError('compression 0x%02x (the WAV codecs need AudioTables)' % mask)
        if mask & 0x08:
            data = explode(data, want)
        if mask & 0x02:
            data = zlib.decompress(data)
        if mask & 0x01:
            data = _Huffman(self.audio.huff_weights).decompress(data, want)
        if mask & 0x80:
            data = adpcm_decompress(data, want, 2, self.audio.adpcm_steps, self.audio.adpcm_adj)
        elif mask & 0x40:
            data = adpcm_decompress(data, want, 1, self.audio.adpcm_steps, self.audio.adpcm_adj)
        return data

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
