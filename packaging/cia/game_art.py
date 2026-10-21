#!/usr/bin/env python3
"""The CIA's HOME-menu art taken from the user's own 1.14d files (nothing of
the game's is in this repository): icon.png is Game.exe's Lord of
Destruction icon (32x32, scaled to the HOME menu's 48x48), banner.cgfx the
title screen's burning logo (data\\global\\ui\\FrontEnd\\D2logo*.DC6 in
d2data.mpq), animated, and banner.wav the opening of the title music
(data\\global\\music\\introedit.wav in d2xmusic.mpq), as long as the HOME
menu plays. make_art.py's drawn art stays the fallback.

    python3 -I game_art.py <game-dir> <out-dir> [<pycgfx-dir>]

<game-dir> holds Game.exe and d2data.mpq, d2exp.mpq for the "Lord of
Destruction" line under the logo and d2xmusic.mpq for the tune (without
them the banner is the logo alone and banner.wav is left as it is). Writes <out-dir>/icon.png,
banner.cgfx, banner.wav and banner-preview.png (frame 0 at 256x128). The
banner model is pycgfx's (banner_cgfx.py, which needs gltflib and Pillow).
"""
import os
import struct
import sys

sys.dont_write_bytecode = True   # no __pycache__ in the source tree
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from make_art import png        # noqa: E402
from mpq_extract import AudioTables, Mpq     # noqa: E402

LOD_ICON_GROUP = 103   # 101 is empty, 102 the classic red Diablo, 103 Baal's LoD icon
LOGO = "data\\global\\ui\\FrontEnd\\D2logo%s.DC6"
PALETTE = "data\\global\\palette\\ACT1\\pal.dat"
# The LoD main menu's background (d2exp.mpq, 800x600 in 256x256 tiles), with
# "EXPANSION SET / Lord of Destruction" painted under where the logo goes.
LOD_MENU = "data\\global\\ui\\FrontEnd\\gameselectscreenEXP.dc6"
LOD_TEXT_ROWS = (182, 230)    # the two lines; the scene there is darker
LOD_KEY = (110, 200)          # brightness: transparent below, opaque above
LOD_WARM = (20, 70)           # or warmth (red minus blue): the gold text against the grey scene
LOD_COLUMNS = (240, 556)      # the lines (241-554); the scene has bright bits beside them
LOD_HALO = (1, 170)           # a dark halo of this radius and alpha behind the letters
MENU_FONT = "data\\local\\font\\latin\\font24.DC6"   # the main menu's gold face
VERSION_TEXT = "v1.14d"   # a line under the subtitle, as far below it as the lines are apart
VERSION_LIGHTEN = 0.15  # the version's colour: the lines' gold, this much toward white
VERSION_HALO = (1, 110)   # a thinner, lighter halo than the lines'

TITLE_MUSIC = "data\\global\\music\\introedit.wav"
# Storm's codec tables in Game.exe 1.14d (as src/game/d2_native.cpp uses them)
HUFF_WEIGHTS, ADPCM_STEPS, ADPCM_ADJ = 0x006CF3A0, 0x006CFD38, 0x006CFCB8
TUNE_SECONDS, FADE_SECONDS = 2.95, 0.5   # the HOME menu stops a banner's tune at 3 s


# --- Game.exe's icon ----------------------------------------------------------

def pe_resources(exe):
    """{type: {id: data}} for the PE's resource tree (first language of each)."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
    secs = [struct.unpack_from("<8sIIII", exe, pe + 24 + opt + 40 * i)[1:] for i in range(nsec)]
    rsrc = struct.unpack_from("<I", exe, pe + 24 + 96 + 8 * 2)[0]

    def off(rva):
        for vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError("rva %#x outside the sections" % rva)

    base = off(rsrc)

    def entries(o):
        named, ids = struct.unpack_from("<HH", exe, o + 12)
        return [struct.unpack_from("<II", exe, o + 16 + 8 * i) for i in range(named + ids)]

    def leaf(t):
        while t & 0x80000000:
            t = entries(base + (t & 0x7FFFFFFF))[0][1]
        rva, size = struct.unpack_from("<II", exe, base + t)
        return exe[off(rva):off(rva) + size]

    return {typ: {rid: leaf(t2) for rid, t2 in entries(base + (t & 0x7FFFFFFF))}
            for typ, t in entries(base) if t & 0x80000000}


def pe_read(exe, va, size):
    """size bytes at a virtual address of the PE image."""
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec, opt = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
    rva = va - struct.unpack_from("<I", exe, pe + 24 + 28)[0]
    for i in range(nsec):
        vsize, sva, rsize, raw = struct.unpack_from("<IIII", exe, pe + 24 + opt + 40 * i + 8)
        if sva <= rva and rva + size <= sva + rsize:
            return exe[raw + rva - sva:raw + rva - sva + size]
    raise ValueError("va %#x not in the file's sections" % va)


def icon_rgba(exe, group):
    """The largest image of an icon group as (size, rows of RGBA)."""
    res = pe_resources(exe)
    g = res[14][group]
    count = struct.unpack_from("<H", g, 4)[0]
    ents = [struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i) for i in range(count)]
    w, _, _, _, _, bpp, _, rid = max(ents, key=lambda e: ((e[0] or 256), e[5]))
    dib = res[3][rid]
    hdr, dw, dh2, _, bits = struct.unpack_from("<IiiHH", dib, 0)
    assert bits in (4, 8) and dw == (w or 256), (bits, dw)
    ncol = struct.unpack_from("<I", dib, 32)[0] or (1 << bits)
    pal = [dib[hdr + 4 * i:hdr + 4 * i + 3][::-1] for i in range(ncol)]   # BGRX -> RGB
    xor = hdr + 4 * ncol
    stride = (dw * bits + 31) // 32 * 4
    mask = xor + stride * dw
    mstride = (dw + 31) // 32 * 4
    rows = []
    for y in range(dw - 1, -1, -1):   # bottom-up
        row = []
        for x in range(dw):
            if bits == 8:
                c = dib[xor + y * stride + x]
            else:
                c = dib[xor + y * stride + x // 2] >> (0 if x & 1 else 4) & 15
            clear = dib[mask + y * mstride + x // 8] >> (7 - x % 8) & 1
            row.append(tuple(pal[c]) + (0 if clear else 255,))
        rows.append(row)
    return dw, rows


# --- images -----------------------------------------------------------------

def resize(rows, nw, nh):
    """Bilinear, on premultiplied alpha. rows: lists of RGBA tuples."""
    h, w = len(rows), len(rows[0])
    pre = [[(r * a, g * a, b * a, a) for r, g, b, a in row] for row in rows]
    out = []
    for y in range(nh):
        fy = min(max((y + 0.5) * h / nh - 0.5, 0), h - 1)
        y0 = int(fy)
        y1, ty = min(y0 + 1, h - 1), fy - y0
        row = []
        for x in range(nw):
            fx = min(max((x + 0.5) * w / nw - 0.5, 0), w - 1)
            x0 = int(fx)
            x1, tx = min(x0 + 1, w - 1), fx - x0
            p = [pre[y0][x0][c] * (1 - tx) * (1 - ty) + pre[y0][x1][c] * tx * (1 - ty) +
                 pre[y1][x0][c] * (1 - tx) * ty + pre[y1][x1][c] * tx * ty for c in range(4)]
            a = p[3]
            row.append(tuple(round(v / a) if a else 0 for v in p[:3]) + (round(a),))
        out.append(row)
    return out


def area_resize(rows, nw, nh):
    """Box-filter downscale (each output pixel averages the source area it
    covers) of RGB rows, or of RGBA rows on premultiplied alpha."""
    if len(rows[0][0]) == 4:
        pre = [[(r * a // 255, g * a // 255, b * a // 255) for r, g, b, a in row] for row in rows]
        alpha = area_resize([[(a, a, a) for _, _, _, a in row] for row in rows], nw, nh)
        colour = area_resize(pre, nw, nh)
        return [[(min(255, c[0] * 255 // a[0]), min(255, c[1] * 255 // a[0]), min(255, c[2] * 255 // a[0]), a[0])
                 if a[0] else (0, 0, 0, 0) for c, a in zip(crow, arow)] for crow, arow in zip(colour, alpha)]
    h, w = len(rows), len(rows[0])
    sx, sy = w / nw, h / nh

    def spans(n, s, limit):
        res = []
        for i in range(n):
            a, b = i * s, (i + 1) * s
            parts = []
            j = int(a)
            while j < b and j < limit:
                parts.append((j, min(b, j + 1) - max(a, j)))
                j += 1
            res.append(parts)
        return res

    xs, ys = spans(nw, sx, w), spans(nh, sy, h)
    out = []
    for yparts in ys:
        row = []
        for xparts in xs:
            acc = [0.0, 0.0, 0.0]
            for j, wy in yparts:
                src = rows[j]
                for i, wx in xparts:
                    p, k = src[i], wx * wy
                    acc[0] += p[0] * k
                    acc[1] += p[1] * k
                    acc[2] += p[2] * k
            n = sx * sy
            row.append(tuple(min(255, round(v / n)) for v in acc))
        out.append(row)
    return out


# --- the burning logo ---------------------------------------------------------

def dc6_frames(data):
    """[(width, height, x offset, y offset, {(x, y): palette index})]."""
    _, _, _, _, dirs, n = struct.unpack_from("<6I", data, 0)
    frames = []
    for ptr in struct.unpack_from("<%dI" % (dirs * n), data, 24):
        flip, w, h, ox, oy, _, _, length = struct.unpack_from("<8i", data, ptr)
        px = {}
        x, y = 0, 0 if flip else h - 1
        i, end = ptr + 32, ptr + 32 + length
        while i < end and 0 <= y < h:
            b = data[i]
            i += 1
            if b == 0x80:
                x, y = 0, y + (1 if flip else -1)
            elif b & 0x80:
                x += b & 0x7F
            else:
                for c in data[i:i + b]:
                    px[(x, y)] = c
                    x += 1
                i += b
        frames.append((w, h, ox, oy, px))
    return frames


def logo_frames(mpq):
    """The title screen's logo, as the game draws it (each half's black
    lettering, then its flames added on top), made transparent where the
    game shows its black background: 348x160 RGBA frames. The lettering is
    opaque; the flames, which the game adds onto black, get an alpha of
    their brightest channel and the colour that gives the same result over
    black."""
    pal = mpq.read(PALETTE)
    rgb = [(pal[3 * i + 2], pal[3 * i + 1], pal[3 * i]) for i in range(256)]
    parts = {k: dc6_frames(mpq.read(LOGO % k))
             for k in ("BlackLeft", "BlackRight", "FireLeft", "FireRight")}
    count = len(parts["FireLeft"])
    W, H, X0, BASE = 348, 160, 174, 120   # the halves sit either side of x=0
    out = []
    for f in range(count):
        canvas = [[(0, 0, 0)] * W for _ in range(H)]
        solid = [[False] * W for _ in range(H)]
        for k, add in (("BlackLeft", False), ("BlackRight", False), ("FireLeft", True), ("FireRight", True)):
            frames = parts[k]
            w, h, ox, oy, px = frames[f % len(frames)]
            left, top = ox + X0, oy - h + BASE
            for (x, y), c in px.items():
                cx, cy = left + x, top + y
                if 0 <= cx < W and 0 <= cy < H:
                    p = rgb[c]
                    if add:
                        q = canvas[cy][cx]
                        p = (min(255, p[0] + q[0]), min(255, p[1] + q[1]), min(255, p[2] + q[2]))
                    else:
                        solid[cy][cx] = True
                    canvas[cy][cx] = p
        rgba = []
        for row, srow in zip(canvas, solid):
            line = []
            for (r, g, b), opaque in zip(row, srow):
                a = 255 if opaque else max(r, g, b)
                line.append((r * 255 // a, g * 255 // a, b * 255 // a, a) if a else (0, 0, 0, 0))
            rgba.append(line)
        out.append(rgba)
    return out


def banner_frames(mpq, step):
    """Every step-th logo frame on the banner's 256x128 texture (scaled to
    the full width, centred vertically, transparent around): RGBA rows."""
    frames = logo_frames(mpq)[::step]
    lh = round(160 * 256 / 348)
    top = (128 - lh) // 2
    clear = (0, 0, 0, 0)
    out = []
    for f in frames:
        small = area_resize(f, 256, lh)
        out.append([[clear] * 256 for _ in range(top)] + small +
                   [[clear] * 256 for _ in range(128 - lh - top)])
    return out


def menu_text(data_mpq, text, colour):
    """text in the main menu's font (font24) in one colour, the glyphs'
    shading kept as alpha: RGBA rows. Each glyph is trimmed to its pixels
    and set 2 pixels apart (the font's width table is not needed for a few
    characters)."""
    pal = data_mpq.read(PALETTE)
    shade = [max(pal[3 * i:3 * i + 3]) for i in range(256)]
    glyphs = dc6_frames(data_mpq.read(MENU_FONT))
    cells = [glyphs[ord(ch)][4] for ch in text]
    top = min(y for px in cells for _, y in px)
    bottom = max(y for px in cells for _, y in px) + 1
    peak = max(shade[c] for px in cells for c in px.values())
    rows = [[] for _ in range(bottom - top)]
    for px in cells:
        x0, x1 = min(x for x, _ in px), max(x for x, _ in px) + 1
        for r, row in enumerate(rows):
            row += [(0, 0, 0, 0)] * (x1 - x0 + 2)
        base = len(rows[0]) - (x1 - x0 + 2)
        for (x, y), c in px.items():
            rows[y - top][base + x - x0] = colour + (min(255, shade[c] * 2 * 255 // peak),)
    return [row[:-2] for row in rows]


def lod_subtitle(data_mpq, exp_mpq):
    """The "EXPANSION SET / Lord of Destruction" lines cut out of the LoD
    menu background: RGBA rows at the logo's scale, 256 wide, transparent
    where the scene was. The alpha ramps over LOD_KEY of the brightest
    channel or LOD_WARM of red minus blue, whichever is higher: the gold
    lettering keeps its darker strokes, the grey snow, stone and sky go
    (above the lines the scene is never warmer than 20). The game
    shows them on a dark scene; on the HOME menu's light background they get
    a dark halo instead (LOD_HALO), as the logo has its black lettering.
    Under them, VERSION_TEXT in the main menu's font, in the lines' gold made
    a little lighter and with a lighter halo, so it reads as a caption."""
    pal = data_mpq.read(PALETTE)
    rgb = [(pal[3 * i + 2], pal[3 * i + 1], pal[3 * i]) for i in range(256)]
    y0, y1 = LOD_TEXT_ROWS
    lo, hi = LOD_KEY
    wlo, whi = LOD_WARM
    tiles = dc6_frames(exp_mpq.read(LOD_MENU))
    band = [[(0, 0, 0, 0)] * 800 for _ in range(y1 - y0)]
    x = y = 0
    for w, h, _, _, px in tiles:   # left to right, then down
        if x >= 800:
            x, y = 0, y + 256
        for (a, b), c in px.items():
            if y0 <= y + b < y1:
                r, g, bl = rgb[c]
                alpha = max(0, min(255, max((max(r, g, bl) - lo) * 255 // (hi - lo),
                                            (r - bl - wlo) * 255 // (whi - wlo))))
                band[y + b - y0][x + a] = (r, g, bl, alpha)
        x += w
    c0, c1 = LOD_COLUMNS
    for row in band:   # nothing outside the lines' columns
        row[:c0] = [(0, 0, 0, 0)] * c0
        row[c1:] = [(0, 0, 0, 0)] * (800 - c1)
    used = [i for i in range(c0, c1) if any(row[i][3] == 255 for row in band)]
    solid = [p for row in band for p in row if p[3] == 255]
    gold = tuple(sum(p[c] for p in solid) // len(solid) for c in range(3))
    light = tuple(round(c + (255 - c) * VERSION_LIGHTEN) for c in gold)
    lines = len(band)   # the rows that get the halo
    version = menu_text(data_mpq, VERSION_TEXT, light)
    vx = (used[0] + used[-1] + 1 - len(version[0])) // 2
    # the gap between the two lines' ink, again under the second (a row
    # less: scaled to the banner, the two gaps then measure the same)
    ink = [y for y, row in enumerate(band) if any(p[3] >= 128 for p in row)]
    breaks = [(a, b) for a, b in zip(ink, ink[1:]) if b - a > 1]
    (line1_end, line2_top), line2_end = breaks[0], ink[-1]
    band = band[:line2_end + 1] + [[(0, 0, 0, 0)] * 800 for _ in range(line2_top - line1_end - 2)]
    for row in version:
        band.append([(0, 0, 0, 0)] * vx + row + [(0, 0, 0, 0)] * (800 - vx - len(row)))
    radius, shade = LOD_HALO
    left, right = used[0] - radius - 2, used[-1] + radius + 3
    band = [row[left:right] for row in band]
    halo = [[0] * len(band[0]) for _ in band]
    for yy, row in enumerate(band):
        radius, shade = LOD_HALO if yy < lines else VERSION_HALO
        for xx, p in enumerate(row):
            if p[3] > 128:
                for dy in range(-radius, radius + 1):
                    for dx in range(-radius, radius + 1):
                        if 0 <= yy + dy < len(band) and 0 <= xx + dx < len(row) and dx * dx + dy * dy <= radius * radius:
                            halo[yy + dy][xx + dx] = max(halo[yy + dy][xx + dx], shade)
    # the letters over the halo (both premultiplied, "over")
    band = [[(r, g, b, a) if a == 255 else
             (lambda out: (r * a // out, g * a // out, b * a // out, out) if out else (0, 0, 0, 0))(
                 a + h * (255 - a) // 255)
             for (r, g, b, a), h in zip(row, hrow)] for row, hrow in zip(band, halo)]
    left, right = 0, len(band[0])
    # the logo is 348 game pixels drawn 256 wide: the same scale here, or
    # smaller if the lines would be wider than the texture
    scale = min(256 / 348, 256 / (right - left))
    width = round((right - left) * scale)
    height = round(len(band) * scale)
    text = area_resize(band, width, height)
    pad = (256 - width) // 2
    clear = (0, 0, 0, 0)
    return [[clear] * pad + row + [clear] * (256 - width - pad) for row in text]


def over(rows, bg):
    """RGBA rows composited over an opaque colour: RGB rows."""
    return [[tuple((c * a + k * (255 - a)) // 255 for c, k in zip(p[:3], bg)) for p in row
             for a in (p[3],)] for row in rows]


# --- the title music -----------------------------------------------------------

def wav_chunks(data):
    """{chunk id: bytes} of a RIFF WAVE (data may be cut short)."""
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    chunks, o = {}, 12
    while o + 8 <= len(data):
        cid, size = struct.unpack_from("<4sI", data, o)
        chunks[cid] = data[o + 8:o + 8 + size]
        o += 8 + size + (size & 1)
    return chunks


def banner_tune(exe, mpq_path):
    """The title music's first TUNE_SECONDS as 16-bit stereo, peak-normalised
    and faded out: (rate, frames as bytes)."""
    tables = AudioTables(pe_read(exe, HUFF_WEIGHTS, 0x102 * 9),
                         struct.unpack("<89I", pe_read(exe, ADPCM_STEPS, 89 * 4)),
                         struct.unpack("<32i", pe_read(exe, ADPCM_ADJ, 32 * 4)))
    mpq = Mpq(mpq_path, tables)
    head = wav_chunks(mpq.read(TITLE_MUSIC, 4096))
    _, channels, rate, _, _, bits = struct.unpack_from("<HHIIHH", head[b"fmt "])
    assert bits == 16 and channels in (1, 2), (bits, channels)
    frames = int(rate * TUNE_SECONDS)
    pcm = wav_chunks(mpq.read(TITLE_MUSIC, 4096 + frames * 2 * channels))[b"data"]
    s = struct.unpack("<%dh" % (frames * channels), pcm[:frames * 2 * channels])
    if channels == 1:
        s = [v for v in s for _ in (0, 1)]
    if rate == 22050:   # to 44.1 kHz, the tested banners' rate: each frame, then the midpoint
        up = []
        for i in range(frames):
            j = min(i + 1, frames - 1)
            up += [s[2 * i], s[2 * i + 1], (s[2 * i] + s[2 * j]) // 2, (s[2 * i + 1] + s[2 * j + 1]) // 2]
        s, rate, frames = up, 44100, frames * 2
    peak = max(1, max(abs(v) for v in s))
    gain = 0.89 * 32767 / peak
    fade = int(rate * FADE_SECONDS)
    out = []
    for i in range(frames):
        g = gain * min(1.0, (frames - i) / fade)
        out += [round(s[2 * i] * g), round(s[2 * i + 1] * g)]
    return rate, struct.pack("<%dh" % len(out), *out)


def write_wav(path, rate, pcm):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


def pick(directory, name):
    """The file of that name in any case, or None."""
    for f in os.listdir(directory):
        if f.lower() == name:
            return os.path.join(directory, f)
    return None


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__.strip())
        return 2
    game, out = argv[1:3]
    pycgfx_dir = argv[3] if len(argv) == 4 else "/opt/pycgfx"
    from banner_cgfx import build
    exe_path, mpq_path, exp_path, music = (pick(game, n) for n in ("game.exe", "d2data.mpq", "d2exp.mpq", "d2xmusic.mpq"))
    if not exe_path or not mpq_path:
        print("game_art.py: %s needs Game.exe and d2data.mpq" % game, file=sys.stderr)
        return 1
    with open(exe_path, "rb") as f:
        exe = f.read()
    size, icon = icon_rgba(exe, LOD_ICON_GROUP)
    icon = resize(icon, 48, 48)
    png(os.path.join(out, "icon.png"), 48, 48, [p for row in icon for p in row])

    # D2_BANNER: animated (default) 3 of the 30 frames at 256x128 (RGBA4,
    # as pycgfx writes, two to a 256x256 atlas); stack: the same model
    # without its animation (frame 0 in place); still: the first frame, one
    # plane; static: no model of ours, the first frame on black as
    # bannertool's still banner.
    # D2_BANNER_FRAMES: how many of the 30 frames (evenly spaced) the
    # animated and stack banners use, 3 by default (the most the HOME menu
    # took animated).
    mode = os.environ.get("D2_BANNER", "animated")
    count = int(os.environ.get("D2_BANNER_FRAMES", "3"))
    frames = banner_frames(Mpq(mpq_path), 1)
    # the still banner bannertool makes (D2_BANNER=static) is opaque: on black
    png(os.path.join(out, "banner-preview.png"), 256, 128,
        [p + (255,) for row in over(frames[0], (0, 0, 0)) for p in row])
    if mode in ("animated", "stack"):
        frames = [frames[k * len(frames) // count] for k in range(count)]
    else:
        frames = frames[:1]
    subtitle = lod_subtitle(Mpq(mpq_path), Mpq(exp_path)) if exp_path else None
    # D2_BANNER_TURN=1: billboard only the planes' parent, the layout first
    # tested on hardware; the HOME menu turns the banner. Default: every
    # plane faces the viewer.
    facing = os.environ.get("D2_BANNER_TURN", "0") != "1"
    data = build(frames, pycgfx_dir, animate=mode != "stack", subtitle=subtitle, facing=facing)
    with open(os.path.join(out, "banner.cgfx"), "wb") as f:
        f.write(data)
    tune = "none (no d2xmusic.mpq)"
    if music:
        rate, pcm = banner_tune(exe, music)
        write_wav(os.path.join(out, "banner.wav"), rate, pcm)
        tune = "%.2f s at %d Hz" % (len(pcm) / 4 / rate, rate)
    print("game art: icon %dx%d -> 48x48, banner %d frame(s) of %dx%d, CGFX %d bytes, tune %s"
          % (size, size, len(frames), len(frames[0][0]), len(frames[0]), len(data), tune))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
