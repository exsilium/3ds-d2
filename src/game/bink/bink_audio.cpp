// Bink audio decoder, RDFT and DCT variants, in fixed point (see bink_audio.h).
#include "game/bink/bink_audio.h"

#include "game/bink/bink_audio_tables.h"

#include <algorithm>
#include <cstring>

namespace bink {
namespace {

// Upper edges of the critical bands (Hz), the psychoacoustic band layout
// the format quantises by.
const int kBandEdges[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270, 1480,  1720, 2000,
                            2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};

// Lengths (in groups of 8 coefficients) of a run sharing one bit width.
const uint8_t kRunGroups[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

constexpr uint64_t kInvSqrt2Q24 = 11863283;   // round(2^24 / sqrt(2))
constexpr int32_t kSqrt2Q30 = 1518500250;     // round(2^30 * sqrt(2))
// cos and sin of 2 pi / 8192, in Q30: half a step of the sine table (which
// has 4096 steps per turn). The DCT's pre-rotation at frame_len 2048 needs
// angles on that finer grid; rotating a table angle by this half step gives
// them without any floating point. Values from Python's math module.
constexpr int32_t kHalfStepCosQ30 = 1073741508, kHalfStepSinQ30 = 823550;
// A coefficient's bound: 8 times full scale, far above what a sound file
// holds; a damaged packet's values stay short of 64-bit products overflowing.
constexpr int64_t kLimit = (int64_t)1 << 28;

// sin and cos of 2 pi a / 4096, Q30.
int32_t sin_q30(int a)
{
    a &= 4095;
    if (a < 1024)
        return kSinQ30[a];
    if (a < 2048)
        return kSinQ30[2048 - a];
    if (a < 3072)
        return -kSinQ30[a - 2048];
    return -kSinQ30[4096 - a];
}
int32_t cos_q30(int a) { return sin_q30(a + 1024); }

// v * 2^s, rounded to nearest when s < 0.
int64_t shift_round(int64_t v, int s)
{
    return s >= 0 ? v * ((int64_t)1 << s) : (v + ((int64_t)1 << (-s - 1))) >> -s;
}

int32_t clamp_coeff(int64_t v) { return (int32_t)std::max(-kLimit, std::min(kLimit, v)); }

// Sums wrap rather than overflow (only a damaged packet comes near).
int32_t add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
int32_t sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

// a * c - b * s and a * s + b * c with Q30 c and s: a rotation.
int32_t rot_re(int32_t a, int32_t b, int32_t c, int32_t s)
{
    return (int32_t)(((int64_t)a * c - (int64_t)b * s + (1 << 29)) >> 30);
}
int32_t rot_im(int32_t a, int32_t b, int32_t c, int32_t s)
{
    return (int32_t)(((int64_t)a * s + (int64_t)b * c + (1 << 29)) >> 30);
}

}  // namespace

bool AudioDecoder::init(uint32_t rate, uint32_t channels, bool dct)
{
    release();
    if (!rate || rate > 96000 || channels < 1 || channels > 2)
        return false;
    // RDFT: stereo is one interleaved signal at twice the rate, in frames
    // twice as long. DCT: each channel is coded apart at the file's rate.
    int bits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    if (channels == 2 && !dct)
        bits++;
    if (bits > 12)
        return false;   // the sine table covers frames up to 4096
    rate_ = rate;
    channels_ = channels;
    uses_dct_ = dct;
    const uint32_t signal_rate = dct ? rate : rate * channels;
    frame_len_ = 1 << bits;
    overlap_ = frame_len_ / 16;
    overlap_shift_ = bits - 4;
    // The format's scale, 2 / (sqrt(frame_len) * 32768) of a sample, is
    // 2^(kFrac + 1) / sqrt(frame_len) in the signal's units.
    raw_shift_ = kFrac + 1 - bits / 2;
    raw_odd_ = bits & 1;
    for (int i = 0; i < 96; i++) {   // steps of 0.6640 dB
        int64_t q = (int64_t)(kExpQ40[i] >> 24);   // Q16
        if (raw_odd_)
            q = (int64_t)(((uint64_t)q * kInvSqrt2Q24 + (1u << 23)) >> 24);
        quant_table_[i] = shift_round(q, raw_shift_);
    }
    const int half = (int)((signal_rate + 1) / 2);
    for (bands_count_ = 1; bands_count_ < 25; bands_count_++)
        if (half <= kBandEdges[bands_count_ - 1])
            break;
    bands_[0] = 2;
    for (int i = 1; i < bands_count_; i++)
        bands_[i] = (kBandEdges[i - 1] * frame_len_ / half) & ~1;
    bands_[bands_count_] = frame_len_;
    first_ = true;

    // The DCT variant keeps one frame (and one crossfade tail) per channel.
    const size_t frames_kept = dct ? channels : 1;
    coeffs_.assign((size_t)frame_len_ * frames_kept, 0);
    previous_.assign((size_t)overlap_ * frames_kept, 0);
    const int m = frame_len_ / 2;
    re_.assign((size_t)m, 0);
    im_.assign((size_t)m, 0);
    cos_.resize((size_t)m / 2);
    sin_.resize((size_t)m / 2);
    for (int j = 0; j < m / 2; j++) {   // 2 pi j / m
        cos_[(size_t)j] = cos_q30(j * (4096 / m));
        sin_[(size_t)j] = sin_q30(j * (4096 / m));
    }
    post_cos_.resize((size_t)m);
    post_sin_.resize((size_t)m);
    for (int k = 0; k < m; k++) {   // 2 pi k / frame_len
        post_cos_[(size_t)k] = cos_q30(k * (4096 / frame_len_));
        post_sin_[(size_t)k] = sin_q30(k * (4096 / frame_len_));
    }
    bitrev_.resize((size_t)m);
    for (int i = 0, lg = bits - 1; i < m; i++) {
        int r = 0;
        for (int b = 0; b < lg; b++)
            r |= ((i >> b) & 1) << (lg - 1 - b);
        bitrev_[(size_t)i] = (uint16_t)r;
    }
    if (dct) {
        // The pre-rotation angle for coefficient k is pi * k / (2 * frame_len),
        // that is 2 pi * k / (4 * frame_len): k * 1024 / frame_len steps of
        // the 4096-step sine table. At frame_len 512 and 1024 that is a whole
        // number of steps. At 2048 (44.1 kHz, the cinematics' rate) odd k
        // fall half way between two steps: those take the step below, rotated
        // by kHalfStep. frame_len is at most 2048 here (bits <= 11 without
        // the RDFT variant's stereo doubling).
        dct_rotation_cos_.resize((size_t)m);
        dct_rotation_sin_.resize((size_t)m);
        for (int k = 0; k < m; k++) {
            const int half_steps = k * 2048 / frame_len_;
            const int32_t table_cos = cos_q30(half_steps / 2), table_sin = sin_q30(half_steps / 2);
            if (half_steps % 2) {
                dct_rotation_cos_[(size_t)k] = rot_re(table_cos, table_sin, kHalfStepCosQ30, kHalfStepSinQ30);
                dct_rotation_sin_[(size_t)k] = rot_im(table_cos, table_sin, kHalfStepCosQ30, kHalfStepSinQ30);
            } else {
                dct_rotation_cos_[(size_t)k] = table_cos;
                dct_rotation_sin_[(size_t)k] = table_sin;
            }
        }
        dct_spectrum_.assign((size_t)frame_len_, 0);
    }
    return true;
}

void AudioDecoder::release()
{
    rate_ = channels_ = 0;
    uses_dct_ = false;
    for (auto *v : {&coeffs_, &previous_, &cos_, &sin_, &post_cos_, &post_sin_, &re_, &im_, &dct_rotation_cos_,
                    &dct_rotation_sin_, &dct_spectrum_})
        std::vector<int32_t>().swap(*v);
    std::vector<uint16_t>().swap(bitrev_);
}

// A raw spectral value, a 29-bit float (5-bit exponent, 23-bit mantissa,
// sign), at the format's scale.
int32_t AudioDecoder::read_raw(BitReader &br) const
{
    const int power = (int)br.bits(5);
    uint64_t v = (uint64_t)br.bits(23) << 16;   // Q16
    if (raw_odd_)
        v = (v * kInvSqrt2Q24 + (1u << 23)) >> 24;
    const int64_t x = clamp_coeff(shift_round(shift_round((int64_t)v, power - 23 + raw_shift_), -16));
    return br.bit() ? -(int32_t)x : (int32_t)x;
}

// x[0..n) from its spectrum in x: x[0] = DC, x[1] = Nyquist, then (re, im)
// of bins 1..n/2-1. Computes
//   x[t] = 1/2 (X0 + (-1)^t X(n/2)) + sum_k (re_k cos(2 pi k t / n) + im_k sin(2 pi k t / n))
// as one complex inverse FFT of n/2 points: the even samples come out as
// its real part, the odd ones as its imaginary part.
void AudioDecoder::inverse_rdft(int32_t *x)
{
    const int n = frame_len_, m = n / 2;
    int32_t *re = re_.data(), *im = im_.data();
    for (int k = 0; k < m; k++) {
        // Y_k = re_k - i im_k; A = Y_k + conj(Y_(m-k)); B = (Y_k - conj(Y_(m-k))) e^(2 pi i k / n).
        int32_t ar, ai, dr, di;
        if (k == 0) {
            ar = add(x[0], x[1]), ai = 0;
            dr = sub(x[0], x[1]), di = 0;
        } else {
            const int32_t yr = x[2 * k], yi = sub(0, x[2 * k + 1]);
            const int32_t zr = x[2 * (m - k)], zi = x[2 * (m - k) + 1];   // conj(Y_(m-k))
            ar = add(yr, zr), ai = add(yi, zi);
            dr = sub(yr, zr), di = sub(yi, zi);
        }
        const int32_t c = post_cos_[(size_t)k], s = post_sin_[(size_t)k];
        const int32_t br = rot_re(dr, di, c, s), bi = rot_im(dr, di, c, s);
        const int at = bitrev_[(size_t)k];
        re[at] = sub(ar, bi);   // Z = A + iB
        im[at] = add(ai, br);
    }
    for (int len = 2; len <= m; len <<= 1) {
        const int half = len / 2, step = m / len;
        for (int i = 0; i < m; i += len)
            for (int j = 0; j < half; j++) {
                const int32_t wr = cos_[(size_t)(j * step)], wi = sin_[(size_t)(j * step)];
                int32_t *ur = re + i + j, *ui = im + i + j, *vr = ur + half, *vi = ui + half;
                const int32_t tr = rot_re(*vr, *vi, wr, wi), ti = rot_im(*vr, *vi, wr, wi);
                *vr = sub(*ur, tr), *vi = sub(*ui, ti);
                *ur = add(*ur, tr), *ui = add(*ui, ti);
            }
    }
    for (int t = 0; t < m; t++) {
        x[2 * t] = re[t] >> 1;
        x[2 * t + 1] = im[t] >> 1;
    }
}

// One channel's spectrum: two raw values, the band quantisers and the
// run-length-grouped coefficients. Both variants code it the same way; only
// what the two raw values mean differs (RDFT: the DC and Nyquist bins; DCT:
// the first two cosine coefficients). Moved out of decode_block unchanged.
void AudioDecoder::read_channel_spectrum(BitReader &br, int32_t *c)
{
    c[0] = read_raw(br);
    c[1] = read_raw(br);
    // Steps, not their values: copying int64_t values, GCC goes through VFP
    // registers, and this decoder keeps off the FPU altogether.
    uint8_t quant[25];
    for (int i = 0; i < bands_count_; i++)
        quant[i] = (uint8_t)std::min<uint32_t>(br.bits(8), 95);
    int k = 0;
    int64_t q = quant_table_[quant[0]];
    for (int i = 2; i < frame_len_;) {
        int end = i + (br.bit() ? kRunGroups[br.bits(4)] * 8 : 8);
        end = std::min(end, frame_len_);
        const unsigned width = br.bits(4);
        if (!width) {
            std::fill(c + i, c + end, 0);
            i = end;
            while (bands_[k] < i)
                q = quant_table_[quant[k++]];
            continue;
        }
        for (; i < end; i++) {
            if (bands_[k] == i)
                q = quant_table_[quant[k++]];
            const int v = (int)br.bits(width);
            if (!v) {
                c[i] = 0;
                continue;
            }
            const int32_t x = clamp_coeff(shift_round((int64_t)v * q, -16));
            c[i] = br.bit() ? -x : x;
        }
    }
}

bool AudioDecoder::decode_block(BitReader &br, std::vector<int16_t> &out)
{
    int32_t *c = coeffs_.data();
    read_channel_spectrum(br, c);
    if (br.overrun())
        return false;
    inverse_rdft(c);
    if (!first_)
        for (int i = 0; i < overlap_; i++)
            c[i] = (int32_t)shift_round((int64_t)previous_[(size_t)i] * (overlap_ - i) + (int64_t)c[i] * i,
                                        -overlap_shift_);
    std::memcpy(previous_.data(), c + frame_len_ - overlap_, sizeof(int32_t) * (size_t)overlap_);
    first_ = false;
    const size_t at = out.size(), count = (size_t)(frame_len_ - overlap_);
    out.resize(at + count);
    for (size_t i = 0; i < count; i++) {
        const int32_t s = (int32_t)shift_round(c[i], -kFrac);
        out[at + i] = (int16_t)std::max<int32_t>(-32768, std::min<int32_t>(32767, s));
    }
    return true;
}

// The inverse DCT (DCT-III) of the DCT variant, in place on frame_len values:
//
//     sample[t] = sum over k of coefficient[k] * cos(pi * k * (2t + 1) / (2 * frame_len))
//
// Computed directly that would be frame_len^2 multiplications per channel
// (4 million per block at 2048 points). Instead, Makhoul's method reuses the
// RDFT variant's FFT (inverse_rdft, frame_len log frame_len work):
//
// 1. Rotate: give each coefficient the phase pi * k / (2 * frame_len),
//        rotated[k] = coefficient[k] * e^(i pi k / (2 * frame_len)).
//    The real part of the inverse DFT of `rotated` is then the samples in a
//    shuffled order: the even samples first, the odd ones after in reverse
//    (sample[2m] at position m, sample[2m + 1] at position frame_len - 1 - m).
// 2. Only the real part is wanted, so `rotated` can be replaced by its
//    Hermitian-symmetric half (pairing bin k with bin frame_len - k),
//        half[k] = e^(i pi k / (2 * frame_len)) * (coefficient[k] - i * coefficient[frame_len - k]) / 2,
//    which is exactly what an inverse real FFT takes. inverse_rdft's input
//    layout holds twice these values: bin 0 (2 * coefficient[0]), the middle
//    bin frame_len / 2 (where the rotation is 45 degrees: sqrt(2) *
//    coefficient[frame_len / 2], real), then the real part and the negated
//    imaginary part of each bin 1 .. frame_len / 2 - 1.
// 3. Run the inverse real FFT, then undo the shuffle of step 1.
//
// The arithmetic is the RDFT path's: values in the signal's units (kFrac
// fractional bits), twiddles in Q30, products in 64 bits and rounded back.
void AudioDecoder::inverse_dct(int32_t *coefficients)
{
    const int size = frame_len_, half_size = frame_len_ / 2;
    int32_t *spectrum = dct_spectrum_.data();

    // Step 2's layout: bin 0, the middle bin, then (real, -imaginary) pairs.
    spectrum[0] = add(coefficients[0], coefficients[0]);
    spectrum[1] = (int32_t)(((int64_t)coefficients[half_size] * kSqrt2Q30 + (1 << 29)) >> 30);
    for (int k = 1; k < half_size; k++) {
        const int32_t low = coefficients[k];           // coefficient[k]
        const int32_t high = coefficients[size - k];   // its pair, coefficient[frame_len - k]
        const int32_t angle_cos = dct_rotation_cos_[(size_t)k], angle_sin = dct_rotation_sin_[(size_t)k];
        // (low - i * high) * (cos + i * sin) = (low cos + high sin) + i (low sin - high cos)
        spectrum[2 * k] = rot_re(low, sub(0, high), angle_cos, angle_sin);   // low cos + high sin
        spectrum[2 * k + 1] = rot_re(high, low, angle_cos, angle_sin);       // high cos - low sin (negated imaginary)
    }

    // Step 3: the FFT, then even samples from the front, odd ones from the back.
    inverse_rdft(spectrum);
    for (int m = 0; m < half_size; m++) {
        coefficients[2 * m] = spectrum[m];
        coefficients[2 * m + 1] = spectrum[size - 1 - m];
    }
}

// One block of the DCT variant: 2 unused bits, one spectrum per channel,
// each turned into frame_len samples by the inverse DCT, crossfaded with the
// previous block's tail, and frame_len * 15/16 samples per channel output,
// interleaved.
bool AudioDecoder::decode_block_dct(BitReader &reader, std::vector<int16_t> &out)
{
    const int channel_count = (int)channels_;
    reader.bits(2);   // unused by the format's decoders
    for (int channel = 0; channel < channel_count; channel++)
        read_channel_spectrum(reader, coeffs_.data() + (size_t)channel * frame_len_);
    if (reader.overrun())
        return false;

    // The crossfade: over the first overlap_ samples, fade from the previous
    // block's last samples into this block's first ones. As in the format's
    // reference decoder, a sample's weight is its position in the
    // interleaved output, not in its own channel: for sample i of channel c
    // that is position = c + i * channel_count, out of
    // fade_length = overlap_ * channel_count. fade_length is a power of two
    // (overlap_ is frame_len / 16), so the division is a rounding shift.
    const int64_t fade_length = (int64_t)overlap_ * channel_count;
    const int fade_shift = overlap_shift_ + (channel_count - 1);   // log2(fade_length)
    for (int channel = 0; channel < channel_count; channel++) {
        int32_t *samples = coeffs_.data() + (size_t)channel * frame_len_;
        int32_t *previous_tail = previous_.data() + (size_t)channel * overlap_;
        inverse_dct(samples);
        if (!first_)
            for (int i = 0; i < overlap_; i++) {
                const int64_t position = channel + (int64_t)i * channel_count;
                const int64_t mixed = (int64_t)previous_tail[i] * (fade_length - position) + (int64_t)samples[i] * position;
                samples[i] = (int32_t)shift_round(mixed, -fade_shift);
            }
        // This block's last overlap_ samples are not output: the next block fades from them.
        std::memcpy(previous_tail, samples + frame_len_ - overlap_, sizeof(int32_t) * (size_t)overlap_);
    }
    first_ = false;

    // Output: drop the kFrac fractional bits (rounding), clip to 16 bits, and
    // interleave the channels (left, right, left, right ...).
    const size_t at = out.size(), samples_per_channel = (size_t)(frame_len_ - overlap_);
    out.resize(at + samples_per_channel * (size_t)channel_count);
    for (int channel = 0; channel < channel_count; channel++) {
        const int32_t *samples = coeffs_.data() + (size_t)channel * frame_len_;
        for (size_t i = 0; i < samples_per_channel; i++) {
            const int32_t value = (int32_t)shift_round(samples[i], -kFrac);
            out[at + i * (size_t)channel_count + (size_t)channel] =
                (int16_t)std::max<int32_t>(-32768, std::min<int32_t>(32767, value));
        }
    }
    return true;
}

bool AudioDecoder::decode(const uint8_t *data, size_t size, std::vector<int16_t> &out)
{
    if (!rate_ || size < 4)
        return false;
    BitReader br;
    br.init(data, size);
    br.skip(32);   // the decoded size, in bytes of 16-bit samples
    while (br.left()) {
        if (!(uses_dct_ ? decode_block_dct(br, out) : decode_block(br, out)))
            return false;
        br.align32();
    }
    return true;
}

}  // namespace bink
