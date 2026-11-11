// See dcc_decode.h. The comments name the game's code each part mirrors.
#include "game/dcc_decode.h"

#include <climits>
#include <cstring>

namespace game {

namespace {

// The game's own buffers' sizes: a direction past them would overrun the
// game's globals, so it is left to the game's code.
constexpr size_t kImageMax = 0x90d8a8 - 0x8f03e8;   // the direction image (0x8f03e8)
constexpr size_t kEntryMax = 0x40000 / 4;           // the pixel entries (0x90d8a8), 4 bytes each
constexpr size_t kCellMax = 0x1b774 / 20;           // the cells (0x94d8a8), 20 bytes each
constexpr uint32_t kFrameMax = 256;                 // frames a direction (0x969020's records)

// 0x6e5f70: a field's width in bits, by its 4-bit code.
constexpr uint8_t kWidths[16] = {0, 1, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 26, 28, 30, 32};

inline uint32_t low_mask(unsigned n) { return n >= 32 ? 0xffffffffu : (1u << n) - 1; }

// The game's bit reader (0x60b200 and its in-line copies): least significant
// bit first, from `base`. Reading past the end marks it bad.
struct Bits {
    const uint8_t *base = nullptr;
    uint64_t len = 0;   // in bits
    uint64_t pos = 0;
    bool bad = false;

    uint32_t get(unsigned n)
    {
        if (!n)
            return 0;
        if (pos + n > len) {
            bad = true;
            pos += n;
            return 0;
        }
        const size_t byte = (size_t)(pos >> 3);
        const unsigned sh = (unsigned)(pos & 7);
        const size_t have = (size_t)((len + 7) >> 3) - byte;
        uint64_t w = 0;
        for (size_t i = 0; i < 5 && i < have; i++)
            w |= (uint64_t)base[byte + i] << (8 * i);
        pos += n;
        return (uint32_t)(w >> sh) & low_mask(n);
    }
    // A signed field (0x6e5fac's sign bit, 0x6e60b8's extension).
    int32_t get_signed(unsigned n)
    {
        uint32_t v = get(n);
        if (n && n < 32 && (v & (1u << (n - 1))))
            v |= ~low_mask(n);
        return (int32_t)v;
    }
    void align_byte() { pos = (pos + 7) & ~(uint64_t)7; }
};

// A frame's record (0x969020, 0x2c bytes each).
struct Rec {
    uint32_t var0, w, h;
    int32_t x, y;
    uint32_t opt, coded;
    uint32_t bottom_up;
    size_t img;   // +0x20: its top-left in the direction image (an offset here)
};

struct Box {
    int32_t xmin, ymin, w, h;
    int32_t cells_x;
};

// The cell walk (0x60b2b0, its state 0x3c bytes): every frame's cells, a
// frame's rows of cells top to bottom, each row left to right. A frame's
// first column and row are cut at the direction's 4-pixel grid, and a cell
// that would leave one pixel takes it (5 wide or high).
struct Walk {
    int32_t frame = -1, fw = 0, fh = 0, fx = 0, fy = 0;
    size_t ptr = 0;
    int32_t stride = 0;
    int32_t row_cell = 0, cell = 0, cx = 0, cy = 0, row_off = 0, pix_off = 0, cw = 0, ch = 0;

    bool next(const Box &b, const Rec *recs, uint32_t nframes)
    {
        cx += cw;
        if (cx < fw) {
            pix_off += cw;
            cell++;
            cw = cx + 5 < fw ? 4 : fw - cx;
            return true;
        }
        cy += ch;
        if (cy < fh) {
            cx = 0;
            const int32_t first = -(fx % 4);
            cw = first + 5 < fw ? first + 4 : fw;
            row_off += stride * ch;
            pix_off = row_off;
            row_cell += b.cells_x;
            cell = row_cell;
            ch = cy + 5 < fh ? 4 : fh - cy;
            return true;
        }
        for (frame++; frame < (int32_t)nframes; frame++) {
            const Rec &r = recs[frame];
            if (r.w == 0 || r.h == 0)
                continue;
            fw = (int32_t)r.w;
            fh = (int32_t)r.h;
            fx = r.x - b.xmin;
            fy = r.y - b.ymin - (r.bottom_up ? 0 : (int32_t)r.h);
            ptr = r.img;
            stride = b.w;
            row_cell = (fy / 4) * b.cells_x + fx / 4;
            cell = row_cell;
            cx = cy = row_off = pix_off = 0;
            const int32_t fxm = -(fx % 4), fym = -(fy % 4);
            cw = fxm + 5 < fw ? fxm + 4 : fw;
            ch = fym + 5 < fh ? fym + 4 : fh;
            return true;
        }
        return false;
    }
};

// Stage 2's cell (0x94d8a8 again, cleared): the last place it was drawn.
struct Cell2 {
    bool drawn;
    size_t dst;
    int32_t stride, w, h;
};

// 0x60bdb0: a frame's rows as runs. 0x80 ends a row, 0x80|n skips n
// transparent pixels, n is followed by n opaque ones (n <= 127). Bottom row
// first unless the frame is bottom-up. Its runs must fill its coded bytes.
bool write_runs(const uint8_t *img, const Rec &r, int32_t stride, uint8_t *out, size_t room)
{
    const uint8_t *row = img + r.img;
    ptrdiff_t step = stride;
    if (!r.bottom_up) {
        row += (ptrdiff_t)(r.h - 1) * stride;
        step = -step;
    }
    size_t o = 0;
    const int32_t w = (int32_t)r.w;
    for (uint32_t y = 0; y < r.h; y++, row += step) {
        int32_t x = 0, end = 0;
        while (x < w) {
            while (x < w && row[x] == 0)
                x++;
            if (x >= w)
                break;
            while (end < x) {
                const int32_t n = x - end > 127 ? 127 : x - end;
                if (o >= room)
                    return false;
                out[o++] = (uint8_t)(0x80 | n);
                end += n;
            }
            int32_t s = x;
            while (x < w && row[x] != 0)
                x++;
            while (s < x) {
                const int32_t n = x - s > 127 ? 127 : x - s;
                if (o + 1 + (size_t)n > room)
                    return false;
                out[o++] = (uint8_t)n;
                std::memcpy(out + o, row + s, (size_t)n);
                o += (size_t)n;
                s += n;
            }
            end = x;
        }
        if (o >= room)
            return false;
        out[o++] = 0x80;
    }
    return o == r.coded;
}

inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

}  // namespace

struct DccScratch {
    std::vector<Rec> recs;
    std::vector<int32_t> cell1;   // stage 1: each cell's last pixel entry (-1: none)
    std::vector<Cell2> cell2;
    std::vector<uint8_t> entries; // stage 1's pixel entries, 4 bytes each
    std::vector<uint8_t> img;     // the direction image
};

DccScratch *dcc_scratch_new()
{
    DccScratch *s = new DccScratch;
    s->recs.reserve(kFrameMax);
    s->cell1.reserve(kCellMax);
    s->cell2.reserve(kCellMax);
    s->entries.reserve(kEntryMax * 4);
    s->img.reserve(kImageMax);
    return s;
}

void dcc_scratch_free(DccScratch *s) { delete s; }

bool dcc_decode_group(const DccGroup &g, DccScratch &s)
{
    if (!g.src || !g.out || g.frames == 0 || g.frames > kFrameMax)
        return false;
    Bits r;
    r.base = g.src;
    r.len = (uint64_t)g.src_len * 8;
    size_t out_pos = 0;
    for (uint32_t d = 0; d < g.dirs; d++) {
        // The direction's header (0x60c130): its OutSizeCoded (not needed
        // here), the stream flags and the frame fields' widths.
        r.pos += 32;
        const uint32_t flags = r.get(2);
        const uint32_t codes = r.get(28);
        unsigned wd[7];
        for (int k = 0; k < 7; k++)
            wd[k] = kWidths[(codes >> (4 * k)) & 0xf];
        s.recs.resize(g.frames);
        uint32_t opt_sum = 0;
        for (uint32_t f = 0; f < g.frames; f++) {
            Rec &c = s.recs[f];
            c.var0 = r.get(wd[0]);
            c.w = r.get(wd[1]);
            c.h = r.get(wd[2]);
            c.x = r.get_signed(wd[3]);
            c.y = r.get_signed(wd[4]);
            c.opt = r.get(wd[5]);
            c.coded = r.get(wd[6]);
            c.bottom_up = r.get(1);
            opt_sum += c.opt;
        }
        if (opt_sum) {   // the frames' optional bytes, from the next byte on
            r.align_byte();
            r.pos += (uint64_t)opt_sum * 8;
        }
        // The direction's box (0x60b530), over every frame.
        int64_t xmin = INT32_MAX, ymin = INT32_MAX, xmax = INT32_MIN, ymax = INT32_MIN;
        for (const Rec &c : s.recs) {
            const int32_t top = c.y - (c.bottom_up ? 0 : (int32_t)c.h);
            if (c.x < xmin)
                xmin = c.x;
            if ((int64_t)c.x + c.w > xmax)
                xmax = (int64_t)c.x + c.w;
            if (top < ymin)
                ymin = top;
            if ((int64_t)top + c.h > ymax)
                ymax = (int64_t)top + c.h;
        }
        const int64_t bw = xmax - xmin, bh = ymax - ymin;
        if (bw <= 0 || bh <= 0 || bw * bh > (int64_t)kImageMax)
            return false;
        Box box{(int32_t)xmin, (int32_t)ymin, (int32_t)bw, (int32_t)bh, (int32_t)((bw + 3) / 4)};
        const size_t ncells = (size_t)box.cells_x * (size_t)((bh + 3) / 4);
        if (ncells > kCellMax)
            return false;
        // The streams' sizes, then the colours the direction uses (0x60b640).
        const uint32_t eq_size = (flags & 2) ? r.get(20) : 0;
        const uint32_t pm_size = r.get(20);
        uint32_t et_size = 0, rp_size = 0;
        if (flags & 1) {
            et_size = r.get(20);
            rp_size = r.get(20);
        }
        uint8_t colours[256];
        unsigned ncolours = 0;
        for (int k = 0; k < 8; k++) {
            const uint32_t bits = r.get(32);
            for (int b = 0; b < 32; b++)
                if (bits & (1u << b))
                    colours[ncolours++] = (uint8_t)(k * 32 + b);
        }
        if (r.bad)
            return false;
        Bits eq = r, pm = r, et = r, rp = r, disp = r;
        pm.pos = eq.pos + eq_size;
        et.pos = pm.pos + pm_size;
        rp.pos = et.pos + et_size;
        disp.pos = rp.pos + rp_size;
        const uint64_t eq_start = eq.pos, disp_start = disp.pos;

        // Stage 1 (0x60b9a0): each frame cell's four colours, as entries.
        s.cell1.assign(ncells, -1);
        s.entries.clear();
        Walk w;
        while (w.next(box, s.recs.data(), g.frames)) {
            if (w.cell < 0 || (size_t)w.cell >= ncells)
                return false;
            const int32_t prev = s.cell1[(size_t)w.cell];
            uint32_t mask = 0xf;
            if (prev >= 0) {
                if (eq_size && eq.get(1))
                    continue;   // the same as the cell's last
                mask = pm.get(4);
            }
            uint32_t raw = 0;
            if (mask && et_size)
                raw = et.get(1);
            if (raw && !rp_size)
                return false;   // the game's code would read through a null stream
            const uint32_t m = mask | 0xfffffff0u;
            const size_t at = s.entries.size();
            if (at / 4 >= kEntryMax)
                return false;
            s.entries.resize(at + 4);
            uint8_t *e = &s.entries[at];
            const uint8_t *p = prev >= 0 ? &s.entries[(size_t)prev * 4] : nullptr;
            uint8_t last = 0;
            for (int i = 3; i >= 0; i--) {
                if (!(m & (1u << i))) {
                    e[i] = p ? p[i] : 0;
                    continue;
                }
                uint8_t v;
                if (raw) {
                    v = (uint8_t)rp.get(8);
                } else {
                    v = last;
                    uint32_t step;
                    do {
                        step = disp.get(4);
                        v = (uint8_t)(v + step);
                    } while (step == 15 && !disp.bad);
                }
                e[i] = v;
                if (v != last) {
                    last = v;
                    continue;
                }
                // A colour the same as the last ends the cell's reads: the
                // ones read move down to the lowest masked places, the
                // masked ones left are 0, and the unmasked below take the
                // cell's last entry (0x60bc2e).
                int src = i, dst = -1;
                for (;;) {
                    for (src++; !(m & (1u << src)); src++) {
                    }
                    if (src >= 4)
                        break;
                    for (dst++; !(m & (1u << dst)); dst++) {
                    }
                    e[dst] = e[src];
                }
                for (;;) {
                    for (dst++; !(m & (1u << dst)); dst++) {
                    }
                    if (dst >= 4)
                        break;
                    e[dst] = 0;
                }
                for (int j = i - 1; j >= 0; j--)
                    if (!(m & (1u << j)))
                        e[j] = p ? p[j] : 0;
                break;
            }
            s.cell1[(size_t)w.cell] = (int32_t)(at / 4);
        }
        if (eq.bad || pm.bad || et.bad || rp.bad || disp.bad)
            return false;
        for (uint8_t &b : s.entries) {
            if (b >= ncolours)
                return false;
            b = colours[b];
        }
        // Each frame's place in the direction image (0x60ca80).
        for (Rec &c : s.recs)
            c.img = (size_t)((int64_t)(c.y - (c.bottom_up ? 0 : (int32_t)c.h) - box.ymin) * box.w +
                             (c.x - box.xmin));
        r.pos = disp.pos;   // the pixel codes follow the displacements

        // Stage 2 (0x60b7b0): the frames' cells drawn into the direction
        // image, each frame written out once its last cell is drawn.
        s.img.resize((size_t)box.w * (size_t)box.h);
        s.cell2.assign(ncells, Cell2{false, 0, 0, 0, 0});
        Bits eq2 = r;
        eq2.pos = eq_start;
        (void)disp_start;
        size_t cursor = 0;
        Walk w2;
        bool more = w2.next(box, s.recs.data(), g.frames);
        int32_t out_frame = 0;
        uint8_t *img = s.img.data();
        while (more) {
            if (w2.cell < 0 || (size_t)w2.cell >= ncells)
                return false;
            Cell2 &cell = s.cell2[(size_t)w2.cell];
            const size_t dst = w2.ptr + (size_t)w2.pix_off;
            const int32_t cw = w2.cw, ch = w2.ch, stride = w2.stride;
            if ((size_t)(ch - 1) * (size_t)stride + dst + (size_t)cw > s.img.size())
                return false;
            if (eq_size && cell.drawn && eq2.get(1)) {
                if (cw == cell.w && ch == cell.h) {
                    if (dst != cell.dst) {
                        uint8_t tmp[5 * 5];
                        for (int32_t y = 0; y < ch; y++)
                            std::memcpy(tmp + 5 * y, img + cell.dst + (size_t)y * cell.stride, (size_t)cw);
                        for (int32_t y = 0; y < ch; y++)
                            std::memcpy(img + dst + (size_t)y * stride, tmp + 5 * y, (size_t)cw);
                    }
                } else {
                    for (int32_t y = 0; y < ch; y++)
                        std::memset(img + dst + (size_t)y * stride, 0, (size_t)cw);
                }
            } else {
                if (cursor * 4 + 4 > s.entries.size())
                    return false;
                const uint8_t *e = &s.entries[cursor * 4];
                unsigned bits = 0;
                for (unsigned i = 1;;) {
                    if (e[i] == e[i - 1])
                        break;
                    bits++;
                    i = 1u << bits;
                    if (i + 1 > 4)
                        break;
                }
                if (bits == 0) {
                    for (int32_t y = 0; y < ch; y++)
                        std::memset(img + dst + (size_t)y * stride, e[0], (size_t)cw);
                } else {
                    for (int32_t y = 0; y < ch; y++)
                        for (int32_t x = 0; x < cw; x++)
                            img[dst + (size_t)y * stride + (size_t)x] = e[r.get(bits)];
                }
                cursor++;
            }
            cell = Cell2{true, dst, stride, cw, ch};
            more = w2.next(box, s.recs.data(), g.frames);
            if (w2.frame != out_frame) {
                // The frame finished: its header and runs (0x60cc10).
                const Rec &c = s.recs[(size_t)out_frame];
                if (out_pos + 0x20 + (size_t)c.coded > g.out_len)
                    return false;
                uint8_t *h = g.out + out_pos;
                put32(h + 0x00, c.var0);
                put32(h + 0x04, c.w);
                put32(h + 0x08, c.h);
                put32(h + 0x0c, (uint32_t)c.x);
                put32(h + 0x10, (uint32_t)c.y);
                put32(h + 0x14, 0);
                put32(h + 0x18, 0);
                put32(h + 0x1c, c.coded);
                if (!write_runs(img, c, box.w, h + 0x20, c.coded))
                    return false;
                out_pos += (size_t)c.coded + 0x23;
                out_frame = w2.frame;
            }
        }
        if (r.bad || eq2.bad)
            return false;
        r.align_byte();
    }
    return true;
}

}  // namespace game
