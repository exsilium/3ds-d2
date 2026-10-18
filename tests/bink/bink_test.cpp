// Bink decoder test driver: decodes a .bik with src/game/bink and writes what
// FFmpeg writes for the same file, so the two can be compared byte for byte
// (tests/bink/compare.sh):
//
//   bink_test <file.bik> [-v <out.yuv>] [-a <out.f32>] [-n <frames>]
//
// -v: every frame as raw yuv420p, cropped to the picture size
//     (ffmpeg -i f.bik -f rawvideo -pix_fmt yuv420p).
// -a: the first audio track as interleaved 32-bit float
//     (ffmpeg -i f.bik -map 0:a:0 -f f32le).
// Always prints the header and the decode time per frame (video, audio:
// average and worst), which on the qemu ARM11 lane is the CPU-bound measure.
#include "game/bink/bink_audio.h"
#include "game/bink/bink_file.h"
#include "game/bink/bink_video.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.bik> [-v out.yuv] [-a out.f32] [-n frames]\n", argv[0]);
        return 2;
    }
    const char *yuv_path = nullptr, *pcm_path = nullptr;
    uint32_t limit = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-v"))
            yuv_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "-a"))
            pcm_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "-n"))
            limit = (uint32_t)std::strtoul(argv[i + 1], nullptr, 0);
    }
    bink::File file;
    if (!file.open(argv[1], 0)) {
        std::fprintf(stderr, "%s: not a Bink file this decoder reads\n", argv[1]);
        return 1;
    }
    const bink::Header &h = file.header();
    std::printf("%s: BIK%c %ux%u %u frames at %u/%u fps, flags %08x, %u audio tracks (%u Hz, %u ch, %s)\n", argv[1],
                h.revision, h.width, h.height, h.frames, h.fps_num, h.fps_den, h.video_flags, h.audio_tracks,
                h.audio_rate, h.audio_channels, h.audio_dct ? "DCT" : "RDFT");
    bink::VideoDecoder video;
    if (!video.init(h.width, h.height, h.revision, h.video_flags)) {
        std::fprintf(stderr, "video: unsupported\n");
        return 1;
    }
    bink::AudioDecoder audio;
    const bool has_audio = h.audio_tracks && audio.init(h.audio_rate, h.audio_channels, h.audio_dct);
    FILE *yuv = yuv_path ? std::fopen(yuv_path, "wb") : nullptr;
    FILE *pcm = pcm_path ? std::fopen(pcm_path, "wb") : nullptr;
    const uint32_t frames = limit && limit < h.frames ? limit : h.frames;
    double video_ms = 0, video_worst = 0, audio_ms = 0, audio_worst = 0;
    std::vector<float> samples;
    int failures = 0;
    for (uint32_t i = 0; i < frames; i++) {
        bink::File::Packet p;
        if (!file.read_frame(i, p)) {
            std::fprintf(stderr, "frame %u: read failed\n", i);
            return 1;
        }
        if (has_audio && p.audio) {
            samples.clear();
            const double t0 = now_ms();
            if (!audio.decode(p.audio, p.audio_size, samples) && failures++ < 10)
                std::fprintf(stderr, "frame %u: audio packet damaged\n", i);
            const double dt = now_ms() - t0;
            audio_ms += dt;
            audio_worst = dt > audio_worst ? dt : audio_worst;
            if (pcm)
                std::fwrite(samples.data(), sizeof(float), samples.size(), pcm);
        }
        const double t0 = now_ms();
        if (!video.decode(p.video, p.video_size) && failures++ < 10)
            std::fprintf(stderr, "frame %u: video data damaged\n", i);
        const double dt = now_ms() - t0;
        video_ms += dt;
        video_worst = dt > video_worst ? dt : video_worst;
        if (yuv) {
            for (int pl = 0; pl < 3; pl++) {
                const uint32_t w = pl ? (h.width + 1) / 2 : h.width, rows = pl ? (h.height + 1) / 2 : h.height;
                for (uint32_t y = 0; y < rows; y++)
                    std::fwrite(video.plane(pl) + (size_t)y * video.stride(pl), 1, w, yuv);
            }
        }
    }
    std::printf("video: %.3f ms/frame average, %.3f worst (%u frames)\n", video_ms / frames, video_worst, frames);
    if (has_audio)
        std::printf("audio: %.3f ms/frame average, %.3f worst\n", audio_ms / frames, audio_worst);
    if (yuv)
        std::fclose(yuv);
    if (pcm)
        std::fclose(pcm);
    return failures ? 1 : 0;
}
