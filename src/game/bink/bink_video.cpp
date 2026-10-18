// Bink 1 video decoder (see bink_video.h).
//
// Byte-exactness notes. The format's arithmetic is reproduced as defined,
// including where it wraps rather than saturates: IDCT output and residue
// additions are taken modulo 256, and dequantisation multiplies modulo 2^32.
// Motion vectors are bounds-checked against the reference plane as a linear
// buffer, so a vector a few pixels past the right edge reads the start of
// the next row: the plane stride must be the same as the reference decoder's
// (the width rounded up to 16, which is what FFmpeg uses for the 640-pixel
// cinematics).
#include "game/bink/bink_video.h"

#include "game/bink/bink_tables.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bink {
namespace {

// Block types (distinct names: kRun and kPattern also name bundles).
enum BlockType { kSkipBlock, kScaledBlock, kMotionBlock, kRunBlock, kResidueBlock, kIntraBlock, kFillBlock, kInterBlock,
                 kPatternBlock, kRawBlock };

// Huffman lookup: indexed by the next 7 bits (LSB first); each entry is
// (code length << 4) | code index.
uint8_t g_lookup[16][128];
bool g_lookup_ready = false;

void build_lookup()
{
    for (int t = 0; t < 16; t++)
        for (int s = 0; s < 16; s++) {
            const int len = kTreeLengths[t][s];
            for (int fill = 0; fill < (1 << (7 - len)); fill++)
                g_lookup[t][kTreeCodes[t][s] | (fill << len)] = (uint8_t)(len << 4 | s);
        }
    g_lookup_ready = true;
}

int log2i(uint32_t v) { return 31 - __builtin_clz(v); }

// Run lengths of block-type codes 12..15: repeat the last type this often.
const uint8_t kTypeRuns[4] = {4, 8, 12, 32};

void merge(BitReader &br, uint8_t *dst, const uint8_t *src, int size)
{
    const uint8_t *a = src, *b = src + size;
    int na = size, nb = size;
    do {
        if (!br.bit()) {
            *dst++ = *a++;
            na--;
        } else {
            *dst++ = *b++;
            nb--;
        }
    } while (na && nb);
    while (na--)
        *dst++ = *a++;
    while (nb--)
        *dst++ = *b++;
}

inline void copy8(uint8_t *dst, const uint8_t *src, uint32_t stride)
{
    if (dst == src)
        return;
    for (int y = 0; y < 8; y++, dst += stride, src += stride)
        std::memcpy(dst, src, 8);
}

inline void fill(uint8_t *dst, uint8_t v, uint32_t stride, int n)
{
    for (int y = 0; y < n; y++, dst += stride)
        std::memset(dst, v, n);
}

// An 8x8 block drawn at twice its size.
inline void scale2x(const uint8_t *src, uint8_t *dst, uint32_t stride)
{
    for (int y = 0; y < 8; y++, src += 8, dst += 2 * stride) {
        uint8_t row[16];
        for (int x = 0; x < 8; x++)
            row[2 * x] = row[2 * x + 1] = src[x];
        std::memcpy(dst, row, 16);
        std::memcpy(dst + stride, row, 16);
    }
}

// ---- inverse DCT -------------------------------------------------------------------
//
// A scaled 8-point AAN transform in 32-bit integers, columns then rows. The
// constants are sqrt(2), 2cos(pi/8), sqrt(2)cos(3pi/8)*2 and its negative
// counterpart, in units of 1/2048, multiplied modulo 2^32.

inline int mul(int x, int k) { return (int)((uint32_t)x * (uint32_t)k) >> 11; }

struct Out8 { int v[8]; };

template <int step> inline Out8 idct8(const int32_t *s)
{
    const int a0 = s[0] + s[4 * step], a1 = s[0] - s[4 * step];
    const int a2 = s[2 * step] + s[6 * step], a3 = mul(2896, s[2 * step] - s[6 * step]);
    const int a4 = s[5 * step] + s[3 * step], a5 = s[5 * step] - s[3 * step];
    const int a6 = s[1 * step] + s[7 * step], a7 = s[1 * step] - s[7 * step];
    const int b0 = a4 + a6;
    const int b1 = mul(3784, a5 + a7);
    const int b2 = mul(-5352, a5) - b0 + b1;
    const int b3 = mul(2896, a6 - a4) - b2;
    const int b4 = mul(2217, a7) + b3 - b1;
    return {{a0 + a2 + b0, a1 + a3 - a2 + b2, a1 - a3 + a2 + b3, a0 - a2 - b4,
             a0 - a2 + b4, a1 - a3 + a2 - b3, a1 + a3 - a2 - b2, a0 + a2 - b0}};
}

// Columns of `in` into tmp (8 ints apart); a column with only a DC term stays flat.
inline void idct_columns(const int32_t *in, int32_t *tmp)
{
    for (int x = 0; x < 8; x++) {
        const int32_t *c = in + x;
        if (!(c[8] | c[16] | c[24] | c[32] | c[40] | c[48] | c[56])) {
            for (int y = 0; y < 8; y++)
                tmp[x + 8 * y] = c[0];
            continue;
        }
        const Out8 o = idct8<8>(c);
        for (int y = 0; y < 8; y++)
            tmp[x + 8 * y] = o.v[y];
    }
}

inline int row_out(int v) { return (v + 0x7F) >> 8; }

void idct_put(uint8_t *dst, uint32_t stride, const int32_t *coeffs)
{
    int32_t tmp[64];
    idct_columns(coeffs, tmp);
    for (int y = 0; y < 8; y++, dst += stride) {
        const Out8 o = idct8<1>(tmp + 8 * y);
        for (int x = 0; x < 8; x++)
            dst[x] = (uint8_t)row_out(o.v[x]);
    }
}

void idct_add(uint8_t *dst, uint32_t stride, const int32_t *coeffs)
{
    int32_t tmp[64];
    idct_columns(coeffs, tmp);
    for (int y = 0; y < 8; y++, dst += stride) {
        const Out8 o = idct8<1>(tmp + 8 * y);
        for (int x = 0; x < 8; x++)
            dst[x] = (uint8_t)(dst[x] + row_out(o.v[x]));
    }
}

void dequantize(int32_t *block, const uint32_t *q, const int *coef_pos, int count)
{
    block[0] = (int)((uint32_t)block[0] * q[0]) >> 11;
    for (int i = 0; i < count; i++) {
        const int p = coef_pos[i], at = kScan[p];
        block[at] = (int)((uint32_t)block[at] * q[p]) >> 11;
    }
}

// Coefficient value of `nbits` magnitude bits: the top bit is implied, and a
// sign bit follows (with no magnitude bits the value is +-1).
inline int coefficient(BitReader &br, int nbits)
{
    if (!nbits)
        return br.bit() ? -1 : 1;
    return br.signed_by_bit((int)br.bits((unsigned)nbits) | 1 << nbits);
}

}  // namespace

// ---- setup -----------------------------------------------------------------------------

bool VideoDecoder::init(uint32_t width, uint32_t height, char revision, uint32_t flags)
{
    release();
    if (!width || !height || width > 1920 || height > 1200 || revision < 'd' || revision > 'i')
        return false;
    if (!g_lookup_ready)
        build_lookup();
    width_ = width;
    height_ = height;
    revision_ = revision;
    has_alpha_ = (flags & 0x00100000u) != 0;
    swap_planes_ = revision >= 'h';
    stride_ = (width + 15) & ~15u;
    const uint32_t bw = (width + 7) >> 3, bh = (height + 7) >> 3;
    luma_rows_ = bh * 8;
    chroma_rows_ = ((height + 15) >> 4) * 8;

    const size_t luma = (size_t)stride_ * luma_rows_, chroma = (size_t)(stride_ / 2) * chroma_rows_;
    const size_t picture = luma * (has_alpha_ ? 2 : 1) + 2 * chroma;
    // Bundle capacity per plane: the most values a plane of whole blocks can
    // use, plus one block row of slack so a damaged row stays in bounds until
    // the check at its end.
    const size_t blocks = (size_t)bw * bh, slack = (size_t)bw * 64 + 64;
    const size_t per_block[kSrcCount] = {1, 1, 64, 8, 1, 1, 2, 2, 64};
    size_t bundles = 0;
    for (size_t n : per_block)
        bundles += blocks * n + slack;
    memory_ = (uint8_t *)std::malloc(2 * picture + bundles);
    if (!memory_)
        return false;
    std::memset(memory_, 0, 2 * picture);
    uint8_t *p = memory_;
    for (auto &f : frame_) {
        f[0] = p, p += luma;
        f[1] = p, p += chroma;
        f[2] = p, p += chroma;
        f[3] = has_alpha_ ? p : nullptr;
        p += has_alpha_ ? luma : 0;
    }
    for (int s = 0; s < kSrcCount; s++) {
        bundle_[s].data = p;
        p += blocks * per_block[s] + slack;
        bundle_[s].end = p - slack;
    }
    first_ = true;
    shown_ = 0;
    return true;
}

void VideoDecoder::release()
{
    std::free(memory_);
    memory_ = nullptr;
    std::memset(frame_, 0, sizeof frame_);
}

// ---- Huffman trees ---------------------------------------------------------------------

int VideoDecoder::huff(BitReader &br, const Tree &t)
{
    const uint8_t e = g_lookup[t.codes][br.peek(7)];
    br.skip(e >> 4);
    return t.syms[e & 15];
}

// A tree is one of the 16 code sets plus the order of the 16 values along
// it: either the first few values listed and the rest ascending, or 0..15
// shuffled by up to four merge passes that the bitstream steers.
void VideoDecoder::read_tree(BitReader &br, Tree &t)
{
    t.codes = (uint8_t)br.bits(4);
    if (!t.codes) {
        for (int i = 0; i < 16; i++)
            t.syms[i] = (uint8_t)i;
        return;
    }
    if (br.bit()) {
        uint8_t listed[16] = {};
        int last = (int)br.bits(3);
        for (int i = 0; i <= last; i++) {
            t.syms[i] = (uint8_t)br.bits(4);
            listed[t.syms[i]] = 1;
        }
        for (int v = 0; v < 16 && last < 15; v++)
            if (!listed[v])
                t.syms[++last] = (uint8_t)v;
        return;
    }
    uint8_t a[16], b[16], *in = a, *out = b;
    for (int i = 0; i < 16; i++)
        a[i] = (uint8_t)i;
    const int passes = (int)br.bits(2);
    for (int pass = 0; pass <= passes; pass++) {
        const int size = 1 << pass;
        for (int start = 0; start < 16; start += 2 * size)
            merge(br, out + start, in + start, size);
        std::swap(in, out);
    }
    std::memcpy(t.syms, in, 16);
}

// ---- bundles -----------------------------------------------------------------------------
//
// At the start of each block row a bundle is refilled only once everything
// decoded into it so far has been used. A refill starts with a value count;
// a count of zero ends the bundle for the rest of the plane.

bool VideoDecoder::refill_count(BitReader &br, Bundle &b, uint32_t &n)
{
    if (b.done || b.filled > b.read)
        return false;
    n = br.bits(b.count_bits);
    if (!n) {
        b.done = true;
        return false;
    }
    return true;
}

bool VideoDecoder::read_block_types(BitReader &br, Bundle &b)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if (n > (size_t)(b.end - b.filled))
        return false;
    uint8_t *const stop = b.filled + n;
    if (br.bit()) {
        std::memset(b.filled, (int)br.bits(4), n);
        b.filled = stop;
        return true;
    }
    int last = 0;
    while (b.filled < stop) {
        const int v = huff(br, b.tree);
        if (v < 12) {
            last = v;
            *b.filled++ = (uint8_t)v;
        } else {
            const int run = kTypeRuns[v - 12];
            if (stop - b.filled < run)
                return false;
            std::memset(b.filled, last, run);
            b.filled += run;
        }
    }
    return true;
}

// Colours: a high nibble coded with one of 16 trees chosen by the previous
// high nibble, then a low nibble. Before revision 'i' the byte is stored in
// sign-magnitude around 128.
bool VideoDecoder::read_colors(BitReader &br, Bundle &b)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if (n > (size_t)(b.end - b.filled))
        return false;
    auto one = [&]() {
        col_last_ = huff(br, col_high_[col_last_]);
        int v = col_last_ << 4 | huff(br, b.tree);
        if (revision_ < 'i') {
            const int sign = (int8_t)v >> 7;
            v = (((v & 0x7F) ^ sign) - sign) + 0x80;
        }
        return (uint8_t)v;
    };
    uint8_t *const stop = b.filled + n;
    if (br.bit()) {
        std::memset(b.filled, one(), n);
        b.filled = stop;
    } else {
        while (b.filled < stop)
            *b.filled++ = one();
    }
    return true;
}

bool VideoDecoder::read_patterns(BitReader &br, Bundle &b)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if (n > (size_t)(b.end - b.filled))
        return false;
    for (uint8_t *const stop = b.filled + n; b.filled < stop;) {
        const int lo = huff(br, b.tree);
        *b.filled++ = (uint8_t)(lo | huff(br, b.tree) << 4);
    }
    return true;
}

bool VideoDecoder::read_motion(BitReader &br, Bundle &b)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if (n > (size_t)(b.end - b.filled))
        return false;
    uint8_t *const stop = b.filled + n;
    if (br.bit()) {
        int v = (int)br.bits(4);
        if (v)
            v = br.signed_by_bit(v);
        std::memset(b.filled, (uint8_t)v, n);
        b.filled = stop;
        return true;
    }
    while (b.filled < stop) {
        int v = huff(br, b.tree);
        if (v)
            v = br.signed_by_bit(v);
        *b.filled++ = (uint8_t)v;
    }
    return true;
}

// DC values: the first in 11 bits (10 + sign for inter blocks), the rest as
// differences in groups of 8 sharing one bit width.
bool VideoDecoder::read_dcs(BitReader &br, Bundle &b, bool has_sign)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if ((size_t)n * 2 > (size_t)(b.end - b.filled))
        return false;
    auto put = [&](int v) {
        const int16_t s = (int16_t)v;
        std::memcpy(b.filled, &s, 2);
        b.filled += 2;
    };
    int v = (int)br.bits(has_sign ? 10 : 11);
    if (v && has_sign)
        v = br.signed_by_bit(v);
    put(v);
    for (uint32_t i = 1; i < n; i += 8) {
        const uint32_t group = std::min<uint32_t>(n - i, 8);
        const unsigned width = br.bits(4);
        for (uint32_t j = 0; j < group; j++) {
            if (width) {
                int d = (int)br.bits(width);
                if (d)
                    d = br.signed_by_bit(d);
                v += d;
                if (v < -32768 || v > 32767)
                    return false;
            }
            put(v);
        }
    }
    return true;
}

bool VideoDecoder::read_runs(BitReader &br, Bundle &b)
{
    uint32_t n;
    if (!refill_count(br, b, n))
        return true;
    if (n > (size_t)(b.end - b.filled))
        return false;
    uint8_t *const stop = b.filled + n;
    if (br.bit()) {
        std::memset(b.filled, (int)br.bits(4), n);
        b.filled = stop;
    } else {
        while (b.filled < stop)
            *b.filled++ = (uint8_t)huff(br, b.tree);
    }
    return true;
}

// ---- coefficients ------------------------------------------------------------------------
//
// DCT and residue coefficients are sent as bit planes, from the top magnitude
// bit down. A work list of pending regions starts with three groups of 16
// scan positions (4, 24, 44, each split later into four groups of 4) and the
// first few positions. Per plane, each region in the list is visited in
// order; a set bit "opens" it. An opened group of 4 resolves each position:
// either it becomes significant now, or it is put at the front of the list
// as a single position whose significance arrives in a later plane.

namespace {
enum Pending { kNone = 0, kGroup16 = 0, kSplit16 = 1, kGroup4 = 2, kSingle = 3 };
struct WorkList {
    int pos[128], mode[128];
    int head = 64, tail = 64;
    void push_back(int p, int m) { pos[tail] = p, mode[tail++] = m; }
    void push_front(int p, int m) { pos[--head] = p, mode[head] = m; }
};
}  // namespace

int VideoDecoder::read_dct(BitReader &br, int32_t block[64], int *coef_pos, int &coef_count)
{
    WorkList w;
    w.push_back(4, kGroup16);
    w.push_back(24, kGroup16);
    w.push_back(44, kGroup16);
    w.push_back(1, kSingle);
    w.push_back(2, kSingle);
    w.push_back(3, kSingle);
    coef_count = 0;
    for (int nbits = (int)br.bits(4) - 1; nbits >= 0; nbits--) {
        for (int i = w.head; i < w.tail;) {
            if (!(w.mode[i] | w.pos[i]) || !br.bit()) {
                i++;
                continue;
            }
            int c = w.pos[i];
            switch (w.mode[i]) {
            case kGroup16:
                // Its first 4 positions now; the other 12 become kSplit16.
                w.pos[i] = c + 4;
                w.mode[i] = kSplit16;
                goto group4;
            case kGroup4:
                w.pos[i] = 0;
                w.mode[i++] = kNone;
            group4:
                for (int k = 0; k < 4; k++, c++) {
                    if (br.bit()) {
                        w.push_front(c, kSingle);
                    } else {
                        block[kScan[c]] = coefficient(br, nbits);
                        coef_pos[coef_count++] = c;
                    }
                }
                break;
            case kSplit16:
                w.mode[i] = kGroup4;
                for (int k = 0; k < 3; k++) {
                    c += 4;
                    w.push_back(c, kGroup4);
                }
                break;
            case kSingle:
                block[kScan[c]] = coefficient(br, nbits);
                coef_pos[coef_count++] = c;
                w.pos[i] = 0;
                w.mode[i++] = kNone;
                break;
            }
        }
    }
    return (int)br.bits(4);   // quantiser level
}

// Residue: the same significance scheme, one bit plane per mask from 2^n
// down to 1; positions already significant get a refinement bit per plane.
// `budget` caps how many coefficient updates the block carries.
void VideoDecoder::read_residue(BitReader &br, int16_t block[64], int budget)
{
    WorkList w;
    w.push_back(4, kGroup16);
    w.push_back(24, kGroup16);
    w.push_back(44, kGroup16);
    w.push_back(0, kGroup4);
    int nz[64], nz_count = 0;
    auto significant = [&](int c, int mask) {
        const int at = kScan[c];
        nz[nz_count++] = at;
        block[at] = (int16_t)(br.bit() ? -mask : mask);
        return --budget >= 0;
    };
    for (int mask = 1 << br.bits(3); mask; mask >>= 1) {
        for (int i = 0; i < nz_count; i++) {
            if (!br.bit())
                continue;
            block[nz[i]] = (int16_t)(block[nz[i]] < 0 ? block[nz[i]] - mask : block[nz[i]] + mask);
            if (--budget < 0)
                return;
        }
        for (int i = w.head; i < w.tail;) {
            if (!(w.mode[i] | w.pos[i]) || !br.bit()) {
                i++;
                continue;
            }
            int c = w.pos[i];
            switch (w.mode[i]) {
            case kGroup16:
                w.pos[i] = c + 4;
                w.mode[i] = kSplit16;
                goto group4;
            case kGroup4:
                w.pos[i] = 0;
                w.mode[i++] = kNone;
            group4:
                for (int k = 0; k < 4; k++, c++) {
                    if (br.bit())
                        w.push_front(c, kSingle);
                    else if (!significant(c, mask))
                        return;
                }
                break;
            case kSplit16:
                w.mode[i] = kGroup4;
                for (int k = 0; k < 3; k++) {
                    c += 4;
                    w.push_back(c, kGroup4);
                }
                break;
            case kSingle:
                w.pos[i] = 0;
                w.mode[i++] = kNone;
                if (!significant(c, mask))
                    return;
                break;
            }
        }
    }
}

// ---- planes ------------------------------------------------------------------------------

bool VideoDecoder::decode_plane(BitReader &br, int p, bool chroma)
{
    const uint32_t stride = chroma ? stride_ / 2 : stride_;
    const int bw = chroma ? (width_ + 15) >> 4 : (width_ + 7) >> 3;
    const int bh = chroma ? (height_ + 15) >> 4 : (height_ + 7) >> 3;
    const uint32_t w8 = (std::max<uint32_t>(width_ >> (chroma ? 1 : 0), 8) + 7) & ~7u;

    // Bits of each bundle's refill count: enough for a row's worth.
    const unsigned per_8px = (unsigned)log2i((w8 >> 3) + 511) + 1;
    bundle_[kBlockTypes].count_bits = per_8px;
    bundle_[kSubBlockTypes].count_bits = (unsigned)log2i((w8 >> 4) + 511) + 1;
    bundle_[kColors].count_bits = (unsigned)log2i((uint32_t)bw * 64 + 511) + 1;
    bundle_[kIntraDc].count_bits = bundle_[kInterDc].count_bits = per_8px;
    bundle_[kXOff].count_bits = bundle_[kYOff].count_bits = per_8px;
    bundle_[kPattern].count_bits = (unsigned)log2i((uint32_t)bw * 8 + 511) + 1;
    bundle_[kRun].count_bits = (unsigned)log2i((uint32_t)bw * 48 + 511) + 1;

    for (int s = 0; s < kSrcCount; s++) {
        Bundle &b = bundle_[s];
        if (s == kColors) {
            for (Tree &t : col_high_)
                read_tree(br, t);
            col_last_ = 0;
        }
        if (s != kIntraDc && s != kInterDc)
            read_tree(br, b.tree);
        b.filled = b.read = b.data;
        b.done = false;
    }
    if (br.overrun())
        return false;

    uint8_t *const cur = frame_[shown_ ^ 1][p];
    const uint8_t *const ref = first_ ? cur : frame_[shown_][p];
    const uint8_t *const ref_last = ref + ((size_t)(bh - 1) * stride + (size_t)(bw - 1)) * 8;
    auto motion_source = [&](const uint8_t *prev) -> const uint8_t * {
        const int dx = s8(kXOff), dy = s8(kYOff);
        const uint8_t *src = prev + dx + (ptrdiff_t)dy * (ptrdiff_t)stride;
        return src < ref || src > ref_last ? nullptr : src;
    };

    int32_t coeffs[64];
    int16_t residue[64];
    int coef_pos[64], coef_count;
    uint8_t small[64];   // an 8x8 block before scaling to 16x16

    for (int by = 0; by < bh; by++) {
        if (!read_block_types(br, bundle_[kBlockTypes]) || !read_block_types(br, bundle_[kSubBlockTypes]) ||
            !read_colors(br, bundle_[kColors]) || !read_patterns(br, bundle_[kPattern]) ||
            !read_motion(br, bundle_[kXOff]) || !read_motion(br, bundle_[kYOff]) ||
            !read_dcs(br, bundle_[kIntraDc], false) || !read_dcs(br, bundle_[kInterDc], true) ||
            !read_runs(br, bundle_[kRun]))
            return false;

        uint8_t *dst = cur + (size_t)8 * by * stride;
        const uint8_t *prev = ref + (size_t)8 * by * stride;
        for (int bx = 0; bx < bw; bx++, dst += 8, prev += 8) {
            const int type = u8(kBlockTypes);
            // The other three 8x8 positions of a 16x16 block.
            if (type == kScaledBlock && ((by & 1) || (bx & 1))) {
                bx++, dst += 8, prev += 8;
                continue;
            }
            switch (type) {
            case kSkipBlock:
                copy8(dst, prev, stride);
                break;
            case kScaledBlock: {
                const int sub = u8(kSubBlockTypes);
                switch (sub) {
                case kRunBlock: {
                    const uint8_t *order = kRunPatterns[br.bits(4)];
                    int done = 0;
                    do {
                        const int run = u8(kRun) + 1;
                        if ((done += run) > 64)
                            return false;
                        if (br.bit()) {
                            const uint8_t v = u8(kColors);
                            for (int k = 0; k < run; k++)
                                small[*order++] = v;
                        } else {
                            for (int k = 0; k < run; k++)
                                small[*order++] = u8(kColors);
                        }
                    } while (done < 63);
                    if (done == 63)
                        small[*order] = u8(kColors);
                    break;
                }
                case kIntraBlock:
                    std::memset(coeffs, 0, sizeof coeffs);
                    coeffs[0] = s16(kIntraDc);
                    dequantize(coeffs, kIntraQuant[read_dct(br, coeffs, coef_pos, coef_count)], coef_pos, coef_count);
                    idct_put(small, 8, coeffs);
                    break;
                case kFillBlock:
                    fill(dst, u8(kColors), stride, 16);
                    break;
                case kPatternBlock: {
                    const uint8_t c0 = u8(kColors), c1 = u8(kColors);
                    for (int y = 0; y < 8; y++) {
                        unsigned bits = u8(kPattern);
                        for (int x = 0; x < 8; x++, bits >>= 1)
                            small[8 * y + x] = bits & 1 ? c1 : c0;
                    }
                    break;
                }
                case kRawBlock:
                    for (int i = 0; i < 64; i++)
                        small[i] = u8(kColors);
                    break;
                default:
                    return false;
                }
                if (sub != kFillBlock)
                    scale2x(small, dst, stride);
                bx++, dst += 8, prev += 8;
                break;
            }
            case kMotionBlock: {
                const uint8_t *src = motion_source(prev);
                if (!src)
                    return false;
                copy8(dst, src, stride);
                break;
            }
            case kRunBlock: {
                const uint8_t *order = kRunPatterns[br.bits(4)];
                auto at = [&](int i) -> uint8_t & { return dst[(i >> 3) * stride + (i & 7)]; };
                int done = 0;
                do {
                    const int run = u8(kRun) + 1;
                    if ((done += run) > 64)
                        return false;
                    if (br.bit()) {
                        const uint8_t v = u8(kColors);
                        for (int k = 0; k < run; k++)
                            at(*order++) = v;
                    } else {
                        for (int k = 0; k < run; k++)
                            at(*order++) = u8(kColors);
                    }
                } while (done < 63);
                if (done == 63)
                    at(*order) = u8(kColors);
                break;
            }
            case kResidueBlock: {
                const uint8_t *src = motion_source(prev);
                if (!src)
                    return false;
                copy8(dst, src, stride);
                std::memset(residue, 0, sizeof residue);
                read_residue(br, residue, (int)br.bits(7));
                uint8_t *row = dst;
                for (int y = 0; y < 8; y++, row += stride)
                    for (int x = 0; x < 8; x++)
                        row[x] = (uint8_t)(row[x] + residue[8 * y + x]);
                break;
            }
            case kIntraBlock:
                std::memset(coeffs, 0, sizeof coeffs);
                coeffs[0] = s16(kIntraDc);
                dequantize(coeffs, kIntraQuant[read_dct(br, coeffs, coef_pos, coef_count)], coef_pos, coef_count);
                idct_put(dst, stride, coeffs);
                break;
            case kFillBlock:
                fill(dst, u8(kColors), stride, 8);
                break;
            case kInterBlock: {
                const uint8_t *src = motion_source(prev);
                if (!src)
                    return false;
                copy8(dst, src, stride);
                std::memset(coeffs, 0, sizeof coeffs);
                coeffs[0] = s16(kInterDc);
                dequantize(coeffs, kInterQuant[read_dct(br, coeffs, coef_pos, coef_count)], coef_pos, coef_count);
                idct_add(dst, stride, coeffs);
                break;
            }
            case kPatternBlock: {
                const uint8_t c0 = u8(kColors), c1 = u8(kColors);
                uint8_t *row = dst;
                for (int y = 0; y < 8; y++, row += stride) {
                    unsigned bits = u8(kPattern);
                    for (int x = 0; x < 8; x++, bits >>= 1)
                        row[x] = bits & 1 ? c1 : c0;
                }
                break;
            }
            case kRawBlock: {
                Bundle &b = bundle_[kColors];
                uint8_t *row = dst;
                for (int y = 0; y < 8; y++, row += stride, b.read += 8)
                    std::memcpy(row, b.read, 8);
                break;
            }
            default:
                return false;
            }
        }
        // A consistent stream never uses values it has not sent.
        for (const Bundle &b : bundle_)
            if (b.read > b.filled)
                return false;
        if (br.overrun())
            return false;
    }
    br.align32();   // the next plane starts on a 32-bit boundary
    return true;
}

bool VideoDecoder::decode(const uint8_t *data, size_t size)
{
    if (!memory_)
        return false;
    BitReader br;
    br.init(data, size);
    bool ok = true;
    if (has_alpha_) {
        if (revision_ >= 'i')
            br.skip(32);
        ok = decode_plane(br, 3, false);
    }
    if (revision_ >= 'i')
        br.skip(32);
    // From revision 'h' the chroma planes come V first.
    for (int plane = 0; ok && plane < 3; plane++) {
        const int p = plane && swap_planes_ ? plane ^ 3 : plane;
        ok = decode_plane(br, p, plane != 0);
        if (br.position() >= br.size())
            break;
    }
    shown_ ^= 1;
    first_ = false;
    return ok;
}

}  // namespace bink
