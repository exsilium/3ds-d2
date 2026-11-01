// Diablo II 1.14d's cinematics, decoded natively (src/game/bink).
//
// How the game plays a video. Its renderers share one function table layout,
// and three entries of it play cinematics: +0x34 "enter video mode", +0x38
// "play" (fastcall: ecx = file name, edx = scaling mode, one stack argument:
// a callback run every loop iteration) and +0x3c "is a video playing". The
// DirectDraw, Direct3D and Glide renderers implement them with binkw32 and a
// modal message loop. The runtime runs the GDI renderer (-w), whose table
// (0x74c4a8) has stubs there: "enter video mode" fails, so the game skips
// every cinematic without calling binkw32 at all. Those imports stay
// unimplemented (rt_shims_misc.cpp).
//
// Here the three GDI entries are replaced. "Play" does what the DirectDraw
// player does: open the file (stored uncompressed in d2video.mpq /
// d2xvideo.mpq, so it is read in place), decode a frame ahead of the one on
// screen, present each at its time, and own the message loop while the
// video runs. Each frame goes to the platform as YUV through
// RunOptions::present, the sound through RunOptions::video_audio. The game's
// callback (an integrity check whose result the player ignores) is not run.
//
// The loop itself is guest code, a few bytes written over the GDI stubs it
// replaces (only their table slots refer to them):
//     if (VideoOpen(name, mode)) while ((ms = VideoStep()) != -1) Sleep(ms);
// Waits must happen in guest code: the scheduler switches threads when a
// shim returns, not inside one, so a native loop could never let the
// game's other threads run (nor, on the virtual clock, let time pass).
// VideoStep does at most one frame's work and says how long until the next.
//
// Timing: the soundtrack is the clock. Audio packets are read and decoded
// up to ~250 ms ahead of what is heard, so the sound keeps going through a
// slow video frame; a frame that is a whole frame late is decoded but not
// shown. Without sound the guest clock paces playback.
//
// Skipping follows the game's rule (its DirectDraw loop): a character key
// (Esc and Enter: Start, R + Start) or a mouse button (A, B, a tap) ends the
// video. The press that ended it is swallowed together with its release, so
// the next screen never sees half of it, and the game's own Escape handling
// never sees it at all: one press skips one video.
#include "game/cinematics.h"

#include "game/bink/bink_audio.h"
#include "game/bink/bink_file.h"
#include "game/bink/bink_video.h"
#include "game/mpq.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/host_clock.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace d2rt;

namespace game {
namespace {

constexpr uint32_t kGdiRenderer = 0x0074c4a8;   // the GDI renderer's function table
constexpr uint32_t kVideoSlots = kGdiRenderer + 0x34;
// What 1.14d keeps there: "return 0", "ret 4", "return 0", each padded with
// int3 to 16 bytes, one after the other from kLoopAt. The play loop goes
// over them.
constexpr uint32_t kLoopAt = 0x006c7cc0, kLoopRoom = 48;
const uint8_t kStubBytes[3][3] = {{0x33, 0xc0, 0xc3}, {0xc2, 0x04, 0x00}, {0x33, 0xc0, 0xc3}};

// The play loop (fastcall: ecx = name, edx = mode; one stack argument, the
// callback, popped on return). Operands at kOpen, kStep, kSleep: addresses.
const uint8_t kLoop[] = {
    0x52,                          //  0 push edx
    0x51,                          //  1 push ecx
    0xB8, 0, 0, 0, 0,              //  2 mov eax, VideoOpen
    0xFF, 0xD0,                    //  7 call eax
    0x85, 0xC0,                    //  9 test eax, eax
    0x74, 0x16,                    // 11 jz done
    0xB8, 0, 0, 0, 0,              // 13 loop: mov eax, VideoStep
    0xFF, 0xD0,                    // 18 call eax
    0x83, 0xF8, 0xFF,              // 20 cmp eax, -1
    0x74, 0x0A,                    // 23 je done
    0x50,                          // 25 push eax
    0xB8, 0, 0, 0, 0,              // 26 mov eax, Sleep
    0xFF, 0xD0,                    // 31 call eax
    0xEB, 0xEA,                    // 33 jmp loop
    0x31, 0xC0,                    // 35 done: xor eax, eax
    0xC2, 0x04, 0x00,              // 37 ret 4
};
constexpr int kOpen = 3, kStep = 14, kSleep = 27;
static_assert(sizeof kLoop <= kLoopRoom, "the play loop must fit over the stubs");

constexpr uint32_t WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
                   WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205,
                   WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208;

bool g_playing = false;

// The video file: loose in the game folder, else stored in an archive.
bool find_video(const std::string &name, std::string &host, uint64_t &offset)
{
    if (rt::host_path(name, false, host)) {
        offset = 0;
        return true;
    }
    for (const char *archive : {"patch_d2.mpq", "d2xvideo.mpq", "d2video.mpq"}) {
        uint32_t size;
        if (rt::host_path(archive, false, host) && mpq_find_stored(host, name, offset, size))
            return true;
    }
    return false;
}

class Session {
public:
    bool open(const std::string &host, uint64_t offset);
    // One turn of the loop: milliseconds to sleep, or -1 when finished.
    int step();
    ~Session()
    {
        if (audio_out_)
            audio_out_->close();
    }

private:
    static constexpr uint32_t kAhead = 16;          // video packets read ahead, at most
    static constexpr uint64_t kAudioLeadUs = 250000;

    struct Queued {
        std::vector<uint8_t> data;
        size_t size = 0;
    };

    bool read_ahead();
    uint64_t audio_decoded_us() const
    {
        return (audio_written_ + (pcm_.size() - pcm_at_) / audio_.channels()) * 1000000 / audio_.rate();
    }
    void feed_audio();
    uint64_t clock_us();
    bool decode_next();
    void present();
    bool poll_skip();
    int finish(const char *how);

    bink::File file_;
    bink::VideoDecoder video_;
    bink::AudioDecoder audio_;
    rt::VideoAudio *audio_out_ = nullptr;
    uint32_t frames_ = 0;
    uint64_t period_us_ = 0;   // one frame

    std::vector<Queued> ring_;   // video packets read ahead
    uint32_t ring_head_ = 0, ring_count_ = 0;
    uint32_t next_read_ = 0, next_show_ = 0;
    bool decoded_ = false;       // frame next_show_ is decoded, waiting for its time

    std::vector<float> decoded_audio_;
    std::vector<int16_t> pcm_;   // decoded, not yet taken by the platform
    size_t pcm_at_ = 0;
    uint64_t audio_written_ = 0; // frames given to the platform
    uint64_t start_us_ = 0;

    uint32_t last_key_ = 0;
    uint32_t release_msg_ = 0, release_key_ = 0;
    uint64_t release_deadline_ms_ = 0;   // nonzero: skipped, swallowing input until the release
    bool started_ = false;

    // Statistics for the log.
    uint32_t shown_ = 0, dropped_ = 0;
    double decode_ms_ = 0, decode_worst_ms_ = 0;
};

bool Session::open(const std::string &host, uint64_t offset)
{
    if (!file_.open(host, offset))
        return false;
    const bink::Header &h = file_.header();
    if (!video_.init(h.width, h.height, h.revision, h.video_flags))
        return false;
    frames_ = h.frames;
    period_us_ = (uint64_t)1000000 * h.fps_den / h.fps_num;
    ring_.resize(kAhead);
    for (Queued &q : ring_)
        q.data.resize(h.largest_frame);
    audio_out_ = rt::options().video_audio;
    if (audio_out_ && h.audio_tracks && audio_.init(h.audio_rate, h.audio_channels, h.audio_dct)) {
        if (!audio_out_->open(h.audio_rate, h.audio_channels))
            audio_out_ = nullptr;
    } else {
        audio_out_ = nullptr;
    }
    rt::logf("cinematic: %ux%u, %u frames at %u/%u fps, sound %s", h.width, h.height, h.frames, h.fps_num, h.fps_den,
             audio_out_ ? "on" : "off");
    return true;
}

// Reads frame next_read_: its audio is decoded at once, its video queued.
bool Session::read_ahead()
{
    bink::File::Packet p;
    if (ring_count_ == kAhead || next_read_ >= frames_ || !file_.read_frame(next_read_, p))
        return false;
    if (audio_out_ && p.audio) {
        decoded_audio_.clear();
        if (audio_.decode(p.audio, p.audio_size, decoded_audio_)) {
            pcm_.erase(pcm_.begin(), pcm_.begin() + (ptrdiff_t)pcm_at_);   // drop what the platform took
            pcm_at_ = 0;
            for (float s : decoded_audio_) {
                const float v = s * 32768.0f;
                pcm_.push_back((int16_t)(v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (int)(v + (v < 0 ? -0.5f : 0.5f))));
            }
        }
    }
    Queued &q = ring_[(ring_head_ + ring_count_) % kAhead];
    std::memcpy(q.data.data(), p.video, p.video_size);
    q.size = p.video_size;
    ring_count_++;
    next_read_++;
    return true;
}

void Session::feed_audio()
{
    if (!audio_out_ || pcm_at_ == pcm_.size())
        return;
    const uint32_t ch = audio_.channels();
    const uint32_t frames = (uint32_t)((pcm_.size() - pcm_at_) / ch);
    const uint32_t took = audio_out_->write(&pcm_[pcm_at_], frames);
    pcm_at_ += (size_t)took * ch;
    audio_written_ += took;
}

// Media time: what has been heard, or the guest clock without sound (and
// once the soundtrack has run out or stalls for more than 200 ms).
uint64_t Session::clock_us()
{
    const uint64_t wall = rt::now_us() - start_us_;
    if (!audio_out_)
        return wall;
    const uint64_t heard = audio_out_->played() * 1000000 / audio_.rate();
    return std::max(heard, wall > 200000 ? wall - 200000 : 0);
}

bool Session::decode_next()
{
    Queued &q = ring_[ring_head_];
    const uint64_t t0 = wx86_now_us();   // host time: the guest clock may be virtual
    const bool ok = video_.decode(q.data.data(), q.size);
    const double ms = (double)(wx86_now_us() - t0) / 1000.0;
    decode_ms_ += ms;
    decode_worst_ms_ = std::max(decode_worst_ms_, ms);
    ring_head_ = (ring_head_ + 1) % kAhead;
    ring_count_--;
    return ok;
}

void Session::present()
{
    if (!rt::options().present)
        return;
    rt::Frame f;
    f.pixels = nullptr;
    f.palette = nullptr;
    f.width = video_.width();
    f.height = video_.height();
    f.stride = 0;
    for (int i = 0; i < 3; i++) {
        f.yuv[i] = video_.plane(i);
        f.yuv_stride[i] = video_.stride(i);
    }
    // 640x146 and 640x240 are stored at half height and shown doubled.
    f.display_height = f.height <= 240 ? 2 * f.height : f.height;
    rt::options().present(f);
    shown_++;
}

bool Session::poll_skip()
{
    rt::InputMessage m;
    while (rt::take_input_message(m)) {
        switch (m.message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            last_key_ = m.wparam;
            break;
        case WM_CHAR:
            release_msg_ = WM_KEYUP, release_key_ = last_key_;
            return true;
        case WM_LBUTTONDOWN:
            release_msg_ = WM_LBUTTONUP;
            return true;
        case WM_RBUTTONDOWN:
            release_msg_ = WM_RBUTTONUP;
            return true;
        case WM_MBUTTONDOWN:
            release_msg_ = WM_MBUTTONUP;
            return true;
        default:
            break;   // moves, releases, other keys: swallowed
        }
    }
    return false;
}

int Session::finish(const char *how)
{
    const uint32_t decoded = next_show_ + (decoded_ ? 1 : 0);
    rt::logf("cinematic: %s at frame %u/%u: %u shown, %u dropped, decode %.2f ms/frame (worst %.2f)", how,
             next_show_, frames_, shown_, dropped_, decoded ? decode_ms_ / decoded : 0.0, decode_worst_ms_);
    return -1;
}

int Session::step()
{
    if (rt::exit_requested())
        return finish("stopped");
    if (!started_) {
        started_ = true;
        start_us_ = rt::now_us();
    }
    // Skipped: swallow input until the press that skipped is released (at most 5 s).
    if (release_deadline_ms_) {
        rt::InputMessage m;
        while (rt::take_input_message(m))
            if (m.message == release_msg_ && (m.message != WM_KEYUP || m.wparam == release_key_))
                return finish("skipped");
        return rt::now_ms() >= release_deadline_ms_ ? finish("skipped") : 10;
    }
    if (poll_skip()) {
        release_deadline_ms_ = rt::now_ms() + 5000;
        return 0;
    }
    // Keep the next frame's packet at hand, and the sound ahead.
    while (next_read_ < frames_ && ring_count_ < kAhead &&
           (next_read_ <= next_show_ + 1 || (audio_out_ && audio_decoded_us() < clock_us() + kAudioLeadUs)))
        if (!read_ahead())
            break;
    feed_audio();
    if (!decoded_) {
        if (next_show_ >= frames_) {
            // All shown: let the sound finish (or its last frame's time pass).
            if (clock_us() >= (uint64_t)frames_ * period_us_ ||
                (audio_out_ && audio_out_->played() >= audio_written_ && pcm_at_ == pcm_.size()))
                return finish("ended");
            return 10;
        }
        if (!ring_count_ || !decode_next()) {
            rt::logf("cinematic: frame %u could not be %s", next_show_, ring_count_ ? "decoded" : "read");
            return finish("stopped");
        }
        decoded_ = true;
    }
    const uint64_t now = clock_us(), due = next_show_ * period_us_;
    if (now < due)
        return (int)std::min<uint64_t>((due - now) / 1000 + 1, 10);   // input is polled at least every 10 ms
    // A whole frame late, with another one to show: skip this one.
    if (now >= due + period_us_ && next_show_ + 1 < frames_)
        dropped_++;
    else
        present();
    next_show_++;
    decoded_ = false;
    return 0;
}

std::unique_ptr<Session> g_session;

// VideoOpen(name, mode): 1 if the video plays (VideoStep follows), 0 to skip it.
uint32_t video_open(Cpu &c)
{
    const std::string name = rt::read_cstr(c, c.arg(0), 260);
    rt::logf("cinematic: %s (mode %u)", name.c_str(), c.arg(1));
    std::string host;
    uint64_t offset = 0;
    if (!find_video(name, host, offset)) {
        rt::logf("cinematic: not found, skipped");
        return 0;
    }
    if (rt::options().video_event)
        rt::options().video_event("open");
    g_session.reset(new Session);
    if (!g_session->open(host, offset)) {
        rt::logf("cinematic: cannot be decoded, skipped");
        g_session.reset();
        if (rt::options().video_event)
            rt::options().video_event("close");
        return 0;
    }
    g_playing = true;
    return 1;
}

uint32_t video_step(Cpu &)
{
    const int ms = g_session ? g_session->step() : -1;
    if (ms < 0) {
        g_session.reset();   // the decoder's memory goes with it
        g_playing = false;
        if (rt::options().video_event)
            rt::options().video_event("close");
    }
    return (uint32_t)ms;
}

}  // namespace

int install_cinematics(Cpu &c)
{
    uint8_t expect[kLoopRoom], have[kLoopRoom];
    std::memset(expect, 0xCC, sizeof expect);
    bool same = c.read(kLoopAt, have, sizeof have);
    for (uint32_t i = 0; i < 3; i++) {
        std::memcpy(expect + 16 * i, kStubBytes[i], 3);
        same = same && c.read_u32(kVideoSlots + 4 * i) == kLoopAt + 16 * i;
    }
    if (!same || std::memcmp(have, expect, sizeof have) != 0) {
        rt::logf("cinematics: the GDI renderer's video entries differ, not installed");
        return 0;
    }
    const uint32_t begin = rt::native_entry("VideoBegin", 0, [](Cpu &) { return 1u; });
    const uint32_t playing = rt::native_entry("VideoPlaying", 0, [](Cpu &) { return g_playing ? 1u : 0u; });
    const uint32_t open = rt::native_entry("VideoOpen", 2, video_open);
    const uint32_t step = rt::native_entry("VideoStep", 0, video_step);
    const uint32_t sleep = rt::bridge().shim_trap("KERNEL32.dll", "Sleep");
    if (!begin || !playing || !open || !step || !sleep)
        return 0;
    uint8_t loop[kLoopRoom];
    std::memset(loop, 0xCC, sizeof loop);
    std::memcpy(loop, kLoop, sizeof kLoop);
    std::memcpy(loop + kOpen, &open, 4);
    std::memcpy(loop + kStep, &step, 4);
    std::memcpy(loop + kSleep, &sleep, 4);
    c.write(kLoopAt, loop, sizeof loop);
    c.invalidate_code(kLoopAt, sizeof loop);
    c.write_u32(kVideoSlots, begin);
    c.write_u32(kVideoSlots + 4, kLoopAt);
    c.write_u32(kVideoSlots + 8, playing);
    rt::logf("cinematics: native player installed");
    return 1;
}

}  // namespace game
