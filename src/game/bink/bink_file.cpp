// Bink container (see bink_file.h). Header layout, all little-endian 32-bit
// unless noted:
//   0 "BIK" + revision, 4 file size - 8, 8 frames, 12 largest frame,
//   16 frames again, 20 width, 24 height, 28/32 frame rate (num/den),
//   36 video flags, 40 audio tracks; then per track a 32-bit maximum
//   decoded size, then per track 16-bit sample rate + 16-bit flags
//   (0x2000 stereo, 0x1000 DCT), then per track a 32-bit id; then
//   frames + 1 offsets (bit 0: keyframe).
#include "game/bink/bink_file.h"

#include <cstring>

namespace bink {
namespace {

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

}  // namespace

bool File::open(const std::string &path, uint64_t base)
{
    close();
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_)
        return false;
    base_ = base;
    uint8_t h[44];
    if (std::fseek(f_, (long)base, SEEK_SET) != 0 || std::fread(h, 1, sizeof h, f_) != sizeof h ||
        std::memcmp(h, "BIK", 3) != 0) {
        close();
        return false;
    }
    hdr_ = Header();
    hdr_.revision = (char)h[3];
    file_size_ = le32(h + 4) + 8;
    hdr_.frames = le32(h + 8);
    hdr_.largest_frame = le32(h + 12);
    hdr_.width = le32(h + 20);
    hdr_.height = le32(h + 24);
    hdr_.fps_num = le32(h + 28);
    hdr_.fps_den = le32(h + 32);
    hdr_.video_flags = le32(h + 36);
    hdr_.audio_tracks = le32(h + 40);
    if (!hdr_.frames || hdr_.frames > 1000000 || !hdr_.fps_num || !hdr_.fps_den || hdr_.audio_tracks > 256 ||
        hdr_.largest_frame > file_size_ || hdr_.revision == 'k') {   // 'k' has extra header fields
        close();
        return false;
    }
    const size_t tracks = hdr_.audio_tracks;
    std::vector<uint8_t> t(tracks * 12);
    if (tracks && std::fread(t.data(), 1, t.size(), f_) != t.size()) {
        close();
        return false;
    }
    if (tracks) {
        const uint8_t *fmt = t.data() + 4 * tracks;   // after the maximum sizes
        hdr_.audio_rate = le16(fmt);
        const uint16_t flags = le16(fmt + 2);
        hdr_.audio_channels = flags & 0x2000 ? 2 : 1;
        hdr_.audio_dct = (flags & 0x1000) != 0;
    }
    std::vector<uint8_t> idx((size_t)(hdr_.frames + 1) * 4);
    if (std::fread(idx.data(), 1, idx.size(), f_) != idx.size()) {
        close();
        return false;
    }
    index_.resize(hdr_.frames + 1);
    for (uint32_t i = 0; i <= hdr_.frames; i++)
        index_[i] = le32(&idx[(size_t)i * 4]);
    index_[hdr_.frames] = file_size_;   // the last frame ends at the end of the file
    for (uint32_t i = 0; i < hdr_.frames; i++)
        if ((index_[i + 1] & ~1u) <= (index_[i] & ~1u)) {
            close();
            return false;
        }
    buf_.assign(hdr_.largest_frame, 0);
    return true;
}

void File::close()
{
    if (f_)
        std::fclose(f_);
    f_ = nullptr;
    index_.clear();
    std::vector<uint8_t>().swap(buf_);
}

bool File::read_frame(uint32_t i, Packet &out)
{
    if (!f_ || i >= hdr_.frames)
        return false;
    const uint32_t pos = index_[i] & ~1u, size = (index_[i + 1] & ~1u) - pos;
    if (size > hdr_.largest_frame || std::fseek(f_, (long)(base_ + pos), SEEK_SET) != 0 ||
        std::fread(buf_.data(), 1, size, f_) != size)
        return false;
    out = Packet();
    size_t at = 0;
    for (uint32_t t = 0; t < hdr_.audio_tracks; t++) {
        if (size - at < 4)
            return false;
        const uint32_t n = le32(&buf_[at]);
        at += 4;
        if (n > size - at)
            return false;
        if (t == 0 && n >= 4) {
            out.audio = &buf_[at];
            out.audio_size = n;
        }
        at += n;
    }
    out.video = &buf_[at];
    out.video_size = size - at;
    return true;
}

}  // namespace bink
