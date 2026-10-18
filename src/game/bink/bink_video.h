// Bink 1 video decoder (revisions 'd' to 'i': every Diablo II cinematic is
// 'i'). Platform-neutral; its output is byte-identical to FFmpeg's decode of
// the same file (tests/bink).
//
// A frame is three planes, Y at full size and U, V at half size (4:2:0,
// BT.601 limited range), coded as 8x8 blocks of ten kinds: copies from the
// previous frame (motion), DCT, run-length, pattern, fill and raw blocks.
// Their parameters come from nine "bundles" per plane, each coded with one
// of 16 fixed Huffman code sets and refilled one block row at a time.
//
// Memory: two frames (current and reference) plus the bundles, about 1.1 MB
// for 640x292 and 1.7 MB for 640x480, held between init() and release().
#ifndef D2_GAME_BINK_BINK_VIDEO_H
#define D2_GAME_BINK_BINK_VIDEO_H

#include "game/bink/bink_bits.h"

#include <cstddef>
#include <cstdint>

namespace bink {

class VideoDecoder {
public:
    VideoDecoder() = default;
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;
    ~VideoDecoder() { release(); }

    // revision: the fourth byte of the file's signature; flags: the header's
    // video flags. False for anything this decoder does not handle (or no
    // memory).
    bool init(uint32_t width, uint32_t height, char revision, uint32_t flags);
    void release();

    // Decodes one frame's video data into the next picture. False when the
    // data is damaged; the picture is then incomplete.
    bool decode(const uint8_t *data, size_t size);

    // The last decoded picture: plane 0 = Y, 1 = U (Cb), 2 = V (Cr). Planes
    // extend beyond width x height to whole blocks.
    const uint8_t *plane(int i) const { return frame_[shown_][i]; }
    uint32_t stride(int i) const { return i ? stride_ / 2 : stride_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    enum Src { kBlockTypes, kSubBlockTypes, kColors, kPattern, kXOff, kYOff, kIntraDc, kInterDc, kRun, kSrcCount };

    struct Tree {
        uint8_t codes;      // which of the 16 code sets
        uint8_t syms[16];   // code index -> value
    };
    struct Bundle {
        uint8_t *data, *end;   // storage
        uint8_t *filled;       // end of the values decoded so far
        uint8_t *read;         // next value to use
        bool done;             // a zero count ended this bundle for the plane
        unsigned count_bits;   // bits of each refill's value count
        Tree tree;
    };

    bool decode_plane(BitReader &br, int p, bool chroma);
    void read_tree(BitReader &br, Tree &t);
    int huff(BitReader &br, const Tree &t);
    bool refill_count(BitReader &br, Bundle &b, uint32_t &n);
    bool read_block_types(BitReader &br, Bundle &b);
    bool read_colors(BitReader &br, Bundle &b);
    bool read_patterns(BitReader &br, Bundle &b);
    bool read_motion(BitReader &br, Bundle &b);
    bool read_dcs(BitReader &br, Bundle &b, bool has_sign);
    bool read_runs(BitReader &br, Bundle &b);
    int read_dct(BitReader &br, int32_t block[64], int *coef_pos, int &coef_count);
    void read_residue(BitReader &br, int16_t block[64], int budget);

    uint8_t u8(Src s) { return *bundle_[s].read++; }
    int s8(Src s) { return (int8_t)*bundle_[s].read++; }
    int s16(Src s)
    {
        int16_t v;
        std::memcpy(&v, bundle_[s].read, 2);
        bundle_[s].read += 2;
        return v;
    }

    uint32_t width_ = 0, height_ = 0, stride_ = 0;
    uint32_t luma_rows_ = 0, chroma_rows_ = 0;
    char revision_ = 0;
    bool has_alpha_ = false, swap_planes_ = false;
    bool first_ = true;
    int shown_ = 0;                     // frame_[shown_] holds the last picture
    uint8_t *frame_[2][4] = {};         // two pictures: Y, U, V, alpha
    uint8_t *memory_ = nullptr;
    Bundle bundle_[kSrcCount] = {};
    Tree col_high_[16] = {};
    int col_last_ = 0;
};

}  // namespace bink

#endif
