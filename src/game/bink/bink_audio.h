// Bink audio decoder for both of the format's variants: RDFT (the one
// Diablo II's original cinematics use) and DCT (found in re-encoded
// cinematics, for example those of localised releases). Platform-neutral,
// in fixed point: no floating point at all, setup included (issue #24: on the 3DS, decoding a soundtrack
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
// The DCT variant differs from the RDFT one in four ways:
// - Each channel is coded on its own (planar) instead of as one interleaved
//   signal, so frame_len depends on the sample rate alone (2048 at 44.1 kHz,
//   mono or stereo), and so do the critical bands.
// - A block starts with 2 unused bits, then holds one spectrum per channel,
//   each coded exactly like an RDFT block's (raw values, quantisers,
//   run-length-grouped coefficients).
// - Each spectrum is turned into samples by an inverse discrete cosine
//   transform (DCT-III) of frame_len points:
//       sample[n] = sum over k of coefficient[k] * cos(pi * k * (2n + 1) / (2 * frame_len))
//   computed with the same FFT as the RDFT variant after a pre-rotation of
//   the coefficients (Makhoul's method; see inverse_dct in the .cpp).
// - The crossfade with the previous block weights each channel's sample by
//   its position in the interleaved output, as the format's reference
//   decoder does, so the two channels fade at slightly different points.
// The scale of a coefficient works out the same as in the RDFT variant
// (2^(kFrac + 1) / sqrt(frame_len) per unit), so the quantiser table and
// the raw-value reader are shared.
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
    // False for what this decoder does not handle (> 2 channels, frames over 4096).
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
    bool decode_block_dct(BitReader &reader, std::vector<int16_t> &out);
    // One channel's spectrum, as both variants code it, into frame_len coefficients.
    void read_channel_spectrum(BitReader &reader, int32_t *coefficients);
    // In place: frame_len DCT coefficients become frame_len samples.
    void inverse_dct(int32_t *coefficients);
    int32_t read_raw(BitReader &br) const;
    void inverse_rdft(int32_t *x);

    uint32_t rate_ = 0, channels_ = 0;
    int frame_len_ = 0, overlap_ = 0, overlap_shift_ = 0, bands_count_ = 0;
    int raw_shift_ = 0;   // a raw value's scale: 2^(kFrac + 1) / sqrt(frame_len), as a shift
    bool raw_odd_ = false;   // ... and 1/sqrt(2) when frame_len is an odd power of two
    int bands_[26] = {};
    int64_t quant_table_[96] = {};   // one coefficient unit at each step, Q16 over the signal's units
    bool first_ = true;
    bool uses_dct_ = false;   // the DCT variant: planar channels, inverse DCT
    // RDFT: one frame. DCT: one frame per channel, one after the other (and
    // likewise for the previous block's tail kept for the crossfade).
    std::vector<int32_t> coeffs_, previous_;
    // FFT of frame_len / 2 points: twiddles e^(2 pi i k / m), Q30, and bit reversal.
    std::vector<int32_t> cos_, sin_, post_cos_, post_sin_;
    std::vector<uint16_t> bitrev_;
    std::vector<int32_t> re_, im_;
    // DCT only: cos and sin of the pre-rotation angle pi * k / (2 * frame_len)
    // for k < frame_len / 2, in Q30, and a frame_len scratch buffer that holds
    // the rotated spectrum while the FFT turns it into samples.
    std::vector<int32_t> dct_rotation_cos_, dct_rotation_sin_, dct_spectrum_;
};

}  // namespace bink

#endif
