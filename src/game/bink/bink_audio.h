// Bink audio decoder, RDFT variant (the one Diablo II's cinematics use; the
// DCT variant is refused). Platform-neutral, in fixed point: no floating
// point at all, setup included (issue #24: on the 3DS, decoding a soundtrack
// in float on the game's thread, as it travelled to Act II, brought the
// console down). tests/bink checks its 16-bit output against the float
// decoder it replaced, which matched FFmpeg.
//
// A packet is a 32-bit decoded-size field and then blocks, each 32-bit
// aligned. A block is one frame of frame_len samples (stereo is coded as one
// interleaved signal, so frame_len counts both channels): two raw spectral
// values, a quantiser per critical band, run-length-grouped coefficients,
// an inverse real FFT, and a linear crossfade over the first 1/16 with the
// previous block's tail. Each block yields frame_len * 15/16 samples.
//
// The signal is kept in 16-bit sample units with kFrac fractional bits in
// 32-bit integers (a full-scale sample is 2^25: room for 64 times that),
// the FFT's twiddles in Q30, products in 64 bits.
#ifndef D2_GAME_BINK_BINK_AUDIO_H
#define D2_GAME_BINK_BINK_AUDIO_H

#include "game/bink/bink_bits.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bink {

class AudioDecoder {
public:
    // False for what this decoder does not handle (DCT, > 2 channels, frames over 4096).
    bool init(uint32_t rate, uint32_t channels, bool dct);
    void release();
    // Decodes one packet (as stored: with its size prefix). Appends interleaved 16-bit
    // samples to `out`. False if the packet is damaged.
    bool decode(const uint8_t *data, size_t size, std::vector<int16_t> &out);
    uint32_t channels() const { return channels_; }
    uint32_t rate() const { return rate_; }

    static constexpr int kFrac = 10;

private:
    bool decode_block(BitReader &br, std::vector<int16_t> &out);
    int32_t read_raw(BitReader &br) const;
    void inverse_rdft(int32_t *x);

    uint32_t rate_ = 0, channels_ = 0;
    int frame_len_ = 0, overlap_ = 0, overlap_shift_ = 0, bands_count_ = 0;
    int raw_shift_ = 0;   // a raw value's scale: 2^(kFrac + 1) / sqrt(frame_len), as a shift
    bool raw_odd_ = false;   // ... and 1/sqrt(2) when frame_len is an odd power of two
    int bands_[26] = {};
    int64_t quant_table_[96] = {};   // one coefficient unit at each step, Q16 over the signal's units
    bool first_ = true;
    std::vector<int32_t> coeffs_, previous_;
    // FFT of frame_len / 2 points: twiddles e^(2 pi i k / m), Q30, and bit reversal.
    std::vector<int32_t> cos_, sin_, post_cos_, post_sin_;
    std::vector<uint16_t> bitrev_;
    std::vector<int32_t> re_, im_;
};

}  // namespace bink

#endif
