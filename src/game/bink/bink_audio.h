// Bink audio decoder, RDFT variant (the one Diablo II's cinematics use; the
// DCT variant is refused). Platform-neutral, in float; tests/bink checks it
// against FFmpeg within a tolerance (both are float, with different FFTs).
//
// A packet is a 32-bit decoded-size field and then blocks, each 32-bit
// aligned. A block is one frame of frame_len samples (stereo is coded as one
// interleaved signal, so frame_len counts both channels): two raw spectral
// values, a quantiser per critical band, run-length-grouped coefficients,
// an inverse real FFT, and a linear crossfade over the first 1/16 with the
// previous block's tail. Each block yields frame_len * 15/16 samples.
#ifndef D2_GAME_BINK_BINK_AUDIO_H
#define D2_GAME_BINK_BINK_AUDIO_H

#include "game/bink/bink_bits.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bink {

class AudioDecoder {
public:
    // False for what this decoder does not handle (DCT, > 2 channels).
    bool init(uint32_t rate, uint32_t channels, bool dct);
    void release();
    // Decodes one packet (as stored: with its size prefix). Appends interleaved samples in
    // [-1, 1] to `out`. False if the packet is damaged.
    bool decode(const uint8_t *data, size_t size, std::vector<float> &out);
    uint32_t channels() const { return channels_; }
    uint32_t rate() const { return rate_; }

private:
    bool decode_block(BitReader &br, std::vector<float> &out);
    void inverse_rdft(float *x);

    uint32_t rate_ = 0, channels_ = 0;
    int frame_len_ = 0, overlap_ = 0, bands_count_ = 0;
    int bands_[26] = {};
    float quant_table_[96] = {};
    bool first_ = true;
    std::vector<float> coeffs_, previous_;
    // FFT of frame_len / 2 points: twiddles e^(2 pi i k / m) and bit reversal.
    std::vector<float> cos_, sin_, post_cos_, post_sin_;
    std::vector<uint16_t> bitrev_;
    std::vector<float> re_, im_;
};

}  // namespace bink

#endif
