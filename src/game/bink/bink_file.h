// Bink container: the header, the frame index and each frame's packet (one
// audio packet per track, then the video data). Reads from a host file at a
// base offset, so a video stored uncompressed inside an MPQ is read in place.
#ifndef D2_GAME_BINK_BINK_FILE_H
#define D2_GAME_BINK_BINK_FILE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bink {

struct Header {
    char revision = 0;            // signature "BIK" + revision
    uint32_t frames = 0;
    uint32_t largest_frame = 0;   // bytes
    uint32_t width = 0, height = 0;
    uint32_t fps_num = 0, fps_den = 0;
    uint32_t video_flags = 0;
    uint32_t audio_tracks = 0;
    // The first audio track (the only one the player uses).
    uint32_t audio_rate = 0;
    uint32_t audio_channels = 0;
    bool audio_dct = false;       // the DCT variant (else RDFT)
};

class File {
public:
    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() { close(); }

    // Opens the Bink file that starts `base` bytes into `path`.
    bool open(const std::string &path, uint64_t base);
    void close();
    const Header &header() const { return hdr_; }
    bool keyframe(uint32_t i) const { return i < index_.size() && (index_[i] & 1); }

    // Reads frame i. The spans point into a buffer owned by the File, valid
    // until the next read.
    struct Packet {
        const uint8_t *audio = nullptr;   // first track's packet (with its 4-byte size prefix)
        size_t audio_size = 0;
        const uint8_t *video = nullptr;
        size_t video_size = 0;
    };
    bool read_frame(uint32_t i, Packet &out);

private:
    FILE *f_ = nullptr;
    uint64_t base_ = 0;
    uint32_t file_size_ = 0;
    Header hdr_;
    std::vector<uint32_t> index_;   // frame offsets, bit 0 = keyframe
    std::vector<uint8_t> buf_;
};

}  // namespace bink

#endif
