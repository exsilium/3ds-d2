// Bink audio decoder, RDFT variant (see bink_audio.h).
#include "game/bink/bink_audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bink {
namespace {

// Upper edges of the critical bands (Hz), the psychoacoustic band layout
// the format quantises by.
const int kBandEdges[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270, 1480,  1720, 2000,
                            2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};

// Lengths (in groups of 8 coefficients) of a run sharing one bit width.
const uint8_t kRunGroups[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

// A 29-bit float: 5-bit exponent, 23-bit mantissa, sign.
float read_float(BitReader &br)
{
    const int power = (int)br.bits(5);
    const float f = std::ldexp((float)br.bits(23), power - 23);
    return br.bit() ? -f : f;
}

}  // namespace

bool AudioDecoder::init(uint32_t rate, uint32_t channels, bool dct)
{
    release();
    if (dct || !rate || rate > 96000 || channels < 1 || channels > 2)
        return false;
    rate_ = rate;
    channels_ = channels;
    // Stereo is one interleaved signal at twice the rate, in frames twice as long.
    int bits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    if (channels == 2)
        bits++;
    const uint32_t signal_rate = rate * channels;
    frame_len_ = 1 << bits;
    overlap_ = frame_len_ / 16;
    const float root = (float)(2.0 / (std::sqrt((double)frame_len_) * 32768.0));
    for (int i = 0; i < 96; i++)   // steps of 0.6640 dB
        quant_table_[i] = std::exp((float)i * 0.15289164787221953823f) * root;
    const int half = (int)((signal_rate + 1) / 2);
    for (bands_count_ = 1; bands_count_ < 25; bands_count_++)
        if (half <= kBandEdges[bands_count_ - 1])
            break;
    bands_[0] = 2;
    for (int i = 1; i < bands_count_; i++)
        bands_[i] = (kBandEdges[i - 1] * frame_len_ / half) & ~1;
    bands_[bands_count_] = frame_len_;
    first_ = true;

    coeffs_.assign((size_t)frame_len_, 0.0f);
    previous_.assign((size_t)overlap_, 0.0f);
    const int m = frame_len_ / 2;
    re_.assign((size_t)m, 0.0f);
    im_.assign((size_t)m, 0.0f);
    cos_.resize((size_t)m / 2);
    sin_.resize((size_t)m / 2);
    for (int j = 0; j < m / 2; j++) {
        cos_[(size_t)j] = (float)std::cos(2.0 * M_PI * j / m);
        sin_[(size_t)j] = (float)std::sin(2.0 * M_PI * j / m);
    }
    post_cos_.resize((size_t)m);
    post_sin_.resize((size_t)m);
    for (int k = 0; k < m; k++) {
        post_cos_[(size_t)k] = (float)std::cos(2.0 * M_PI * k / frame_len_);
        post_sin_[(size_t)k] = (float)std::sin(2.0 * M_PI * k / frame_len_);
    }
    bitrev_.resize((size_t)m);
    for (int i = 0, lg = bits - 1; i < m; i++) {
        int r = 0;
        for (int b = 0; b < lg; b++)
            r |= ((i >> b) & 1) << (lg - 1 - b);
        bitrev_[(size_t)i] = (uint16_t)r;
    }
    return true;
}

void AudioDecoder::release()
{
    rate_ = channels_ = 0;
    for (auto *v : {&coeffs_, &previous_, &cos_, &sin_, &post_cos_, &post_sin_, &re_, &im_})
        std::vector<float>().swap(*v);
    std::vector<uint16_t>().swap(bitrev_);
}

// x[0..n) from its spectrum in x: x[0] = DC, x[1] = Nyquist, then (re, im)
// of bins 1..n/2-1. Computes
//   x[t] = 1/2 (X0 + (-1)^t X(n/2)) + sum_k (re_k cos(2 pi k t / n) + im_k sin(2 pi k t / n))
// as one complex inverse FFT of n/2 points: the even samples come out as
// its real part, the odd ones as its imaginary part.
void AudioDecoder::inverse_rdft(float *x)
{
    const int n = frame_len_, m = n / 2;
    float *re = re_.data(), *im = im_.data();
    for (int k = 0; k < m; k++) {
        // Y_k = re_k - i im_k; A = Y_k + conj(Y_(m-k)); B = (Y_k - conj(Y_(m-k))) e^(2 pi i k / n).
        float ar, ai, dr, di;
        if (k == 0) {
            ar = x[0] + x[1], ai = 0.0f;
            dr = x[0] - x[1], di = 0.0f;
        } else {
            const float yr = x[2 * k], yi = -x[2 * k + 1];
            const float zr = x[2 * (m - k)], zi = x[2 * (m - k) + 1];   // conj(Y_(m-k))
            ar = yr + zr, ai = yi + zi;
            dr = yr - zr, di = yi - zi;
        }
        const float c = post_cos_[(size_t)k], s = post_sin_[(size_t)k];
        const float br = dr * c - di * s, bi = dr * s + di * c;
        const int at = bitrev_[(size_t)k];
        re[at] = ar - bi;   // Z = A + iB
        im[at] = ai + br;
    }
    for (int len = 2; len <= m; len <<= 1) {
        const int half = len / 2, step = m / len;
        for (int i = 0; i < m; i += len)
            for (int j = 0; j < half; j++) {
                const float wr = cos_[(size_t)(j * step)], wi = sin_[(size_t)(j * step)];
                float *ur = re + i + j, *ui = im + i + j, *vr = ur + half, *vi = ui + half;
                const float tr = *vr * wr - *vi * wi, ti = *vr * wi + *vi * wr;
                *vr = *ur - tr, *vi = *ui - ti;
                *ur += tr, *ui += ti;
            }
    }
    for (int t = 0; t < m; t++) {
        x[2 * t] = 0.5f * re[t];
        x[2 * t + 1] = 0.5f * im[t];
    }
}

bool AudioDecoder::decode_block(BitReader &br, std::vector<float> &out)
{
    float *c = coeffs_.data();
    const float root = quant_table_[0];
    c[0] = read_float(br) * root;
    c[1] = read_float(br) * root;
    float quant[25];
    for (int i = 0; i < bands_count_; i++)
        quant[i] = quant_table_[std::min<uint32_t>(br.bits(8), 95)];
    int k = 0;
    float q = quant[0];
    for (int i = 2; i < frame_len_;) {
        int end = i + (br.bit() ? kRunGroups[br.bits(4)] * 8 : 8);
        end = std::min(end, frame_len_);
        const unsigned width = br.bits(4);
        if (!width) {
            std::fill(c + i, c + end, 0.0f);
            i = end;
            while (bands_[k] < i)
                q = quant[k++];
            continue;
        }
        for (; i < end; i++) {
            if (bands_[k] == i)
                q = quant[k++];
            const int v = (int)br.bits(width);
            c[i] = v ? (br.bit() ? -q * (float)v : q * (float)v) : 0.0f;
        }
    }
    if (br.overrun())
        return false;
    inverse_rdft(c);
    if (!first_)
        for (int i = 0; i < overlap_; i++)
            c[i] = (previous_[(size_t)i] * (float)(overlap_ - i) + c[i] * (float)i) / (float)overlap_;
    std::memcpy(previous_.data(), c + frame_len_ - overlap_, sizeof(float) * (size_t)overlap_);
    first_ = false;
    out.insert(out.end(), c, c + frame_len_ - overlap_);
    return true;
}

bool AudioDecoder::decode(const uint8_t *data, size_t size, std::vector<float> &out)
{
    if (!rate_ || size < 4)
        return false;
    BitReader br;
    br.init(data, size);
    br.skip(32);   // the decoded size, in bytes of 16-bit samples
    while (br.left()) {
        if (!decode_block(br, out))
            return false;
        br.align32();
    }
    return true;
}

}  // namespace bink
