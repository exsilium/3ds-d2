// Bit reader for Bink packets: bits are taken least significant first from
// little-endian bytes. A read is one unaligned 32-bit load and a shift, which
// the ARM11 does in a few cycles. Reads past the end return zeros and set
// overrun() (the decoders check it once per block row or audio block), so a
// damaged file can never make the reader leave its buffer.
#ifndef D2_GAME_BINK_BINK_BITS_H
#define D2_GAME_BINK_BINK_BITS_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bink {

class BitReader {
public:
    void init(const uint8_t *data, size_t bytes)
    {
        data_ = data;
        bits_ = bytes * 8;
        pos_ = 0;
    }

    // n = 0..25 bits.
    uint32_t peek(unsigned n) const
    {
        const size_t byte = pos_ >> 3;
        uint32_t w;
        if (byte + 4 <= (bits_ >> 3)) {
            std::memcpy(&w, data_ + byte, 4);   // little-endian host (ARM, x86)
        } else {
            w = 0;
            for (size_t i = 0; i < 4 && byte + i < (bits_ >> 3); i++)
                w |= (uint32_t)data_[byte + i] << (8 * i);
        }
        return (w >> (pos_ & 7)) & ((1u << n) - 1);
    }
    void skip(size_t n) { pos_ += n; }
    uint32_t bits(unsigned n)
    {
        const uint32_t v = peek(n);
        pos_ += n;
        return v;
    }
    uint32_t bit() { return bits(1); }
    // Two's complement sign applied the format's way: a set bit negates v.
    int signed_by_bit(int v) { return bit() ? -v : v; }

    void align32() { pos_ = (pos_ + 31) & ~(size_t)31; }
    size_t position() const { return pos_; }
    size_t size() const { return bits_; }
    size_t left() const { return pos_ < bits_ ? bits_ - pos_ : 0; }
    bool overrun() const { return pos_ > bits_; }

private:
    const uint8_t *data_ = nullptr;
    size_t bits_ = 0, pos_ = 0;
};

}  // namespace bink

#endif
