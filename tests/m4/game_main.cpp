// M4 game lane host (Linux, qemu-arm): runs Diablo II's Game.exe through
// 3ds-d2's own Win32 runtime on the ARMv6 Box86 backend, with the 3DS's
// settings (game layout, no-SIMD CPUID), and reports how far it got.
//
//   m4_host <game-dir> <write-dir> <max-ms> [command line]
//
// The game directory is read-only; everything the game writes goes to
// write-dir. Guest time is real time, as on the console (max-ms included);
// M4_VIRTUAL_CLOCK selects the scheduler's deterministic virtual clock, with
// 250 us of guest time per thread switch so the game's polling loop sees time
// pass (runs are then repeatable, and much faster than real time on qemu).
//
// Screenshots of what the game shows go to <write-dir>/../shots/ (t005.png at
// 5 s of guest time, then 15 s, 30 s and every 30 s, plus last.png).
//
// M4_VIDEO=1 plays the game's cinematics (game::install_cinematics); off by
// default so scripted runs keep reaching the menu at the same time.
// M4_VIDEO_WAV=<file> records their soundtrack, paced as a console would
// play it.
//
// M4_SCRIPT drives game::Controls (the 3DS control scheme) with scripted pad
// events (game::Autoplay, src/game/autoplay.h), ';'-separated, e.g.
//   "20000 tap 400 307;26000 tap 400 290;32000 text Tester;34000 press R;..."

#include "game/autoplay.h"
#include "game/automap.h"
#include "game/cinematics.h"
#include "game/controls.h"
#include "game/d2_native.h"
#include "game/d2_state.h"
#include "game/second_screen.h"
#include "runtime/rt_host.h"
#include "ctru_font.h"
#include "png.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
void dyn86_hotprof_reset(void);
void dyn86_hotprof_dump(FILE *f, int top);
}

static bool shot_now = false;   // M4_SCRIPT "shot": save the next frame

// The 3DS app's bottom screen (game::SecondScreen), driven by the scripted
// pad; each "shot" also saves it, as the console would show it, as b<ms>.png.
static game::SecondScreen second_screen;
static game::BottomView bottom_view;

// The cinematics' sound on the lane: a consumer that plays in real time on
// the guest clock (so the player paces itself as on the console), with
// 300 ms of queue; M4_VIDEO_WAV keeps what it "played" in a WAV file.
class LaneVideoAudio : public rt::VideoAudio {
public:
    bool open(uint32_t rate, uint32_t channels) override
    {
        rate_ = rate, channels_ = channels, written_ = 0, start_us_ = 0;
        if (const char *path = std::getenv("M4_VIDEO_WAV")) {
            wav_ = std::fopen(path, created_ ? "r+b" : "wb");
            if (wav_ && !created_)
                header(0);
            if (wav_)
                std::fseek(wav_, 0, SEEK_END);
            created_ = wav_ != nullptr;
        }
        return true;
    }
    uint32_t write(const int16_t *pcm, uint32_t frames) override
    {
        if (!start_us_)
            start_us_ = rt::now_us();
        const uint64_t queued = written_ - played(), cap = rate_ * 3 / 10;
        const uint32_t n = queued >= cap ? 0 : (uint32_t)std::min<uint64_t>(frames, cap - queued);
        if (wav_ && n) {
            std::fwrite(pcm, 2 * channels_, n, wav_);
            data_bytes_ += 2 * channels_ * n;
        }
        written_ += n;
        return n;
    }
    uint64_t played() override
    {
        return start_us_ ? std::min<uint64_t>(written_, (rt::now_us() - start_us_) * rate_ / 1000000) : 0;
    }
    void close() override
    {
        if (wav_) {   // every video appends to one file; the header covers them all
            std::fflush(wav_);
            std::fseek(wav_, 0, SEEK_SET);
            header(data_bytes_);
            std::fclose(wav_);
            wav_ = nullptr;
        }
    }

private:
    void header(uint32_t data)
    {
        const uint32_t byte_rate = rate_ * channels_ * 2;
        const uint16_t align = (uint16_t)(channels_ * 2), bits = 16, fmt = 1, ch = (uint16_t)channels_;
        const uint32_t riff = 36 + data, fmt_len = 16;
        std::fwrite("RIFF", 1, 4, wav_), std::fwrite(&riff, 4, 1, wav_), std::fwrite("WAVEfmt ", 1, 8, wav_);
        std::fwrite(&fmt_len, 4, 1, wav_), std::fwrite(&fmt, 2, 1, wav_), std::fwrite(&ch, 2, 1, wav_);
        std::fwrite(&rate_, 4, 1, wav_), std::fwrite(&byte_rate, 4, 1, wav_), std::fwrite(&align, 2, 1, wav_);
        std::fwrite(&bits, 2, 1, wav_), std::fwrite("data", 1, 4, wav_), std::fwrite(&data, 4, 1, wav_);
    }
    uint32_t rate_ = 0, channels_ = 0, data_bytes_ = 0;
    uint64_t written_ = 0, start_us_ = 0;
    FILE *wav_ = nullptr;
    bool created_ = false;
};

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <game-dir> <write-dir> <max-ms> [command line]\n", argv[0]);
        return 2;
    }
    rt::RunOptions opt;
    // M4_LAYOUT=ctr: the 3DS's compact layout and its 64 MiB RAM report, to
    // check on Linux exactly what the console app will run.
    const std::string lay = std::getenv("M4_LAYOUT") ? std::getenv("M4_LAYOUT") : "";
    const bool ctr = lay == "ctr" || lay == "ctr-n3";   // ctr-n3: the CIA's layout on a New 3DS
    opt.layout = lay == "ctr-n3" ? &rt::kCtrGameLayoutN3 : ctr ? &rt::kCtrGameLayout : &rt::kGameLayout;
    if (ctr)
        opt.total_phys_mb = 64;
    if (const char *mb = std::getenv("M4_PHYS_MB"))   // override the RAM the game is told about
        opt.total_phys_mb = (uint32_t)std::atoi(mb);
    opt.virtual_tick_us = 250;   // used only under M4_VIRTUAL_CLOCK
    // Native drawing routines, as in the 3DS app; M4_NATIVE=0 runs the
    // translated originals (the A/B for byte-exactness and speed).
    // M4_NATIVE=late installs them on arrival in the game world instead: the
    // scheduler preempts by instruction count and the clock advances per
    // switch, so faster menu code changes the moment the game is created and
    // with it the map seed; installed late, both runs share the same world.
    static d2rt::Cpu *g_cpu = nullptr;
    static const bool native_late = std::getenv("M4_NATIVE") && std::string(std::getenv("M4_NATIVE")) == "late";
    static const bool native_now = !native_late && (!std::getenv("M4_NATIVE") || std::string(std::getenv("M4_NATIVE")) != "0");
    static const bool video = std::getenv("M4_VIDEO") && std::string(std::getenv("M4_VIDEO")) != "0";
    opt.install_native = [](d2rt::Cpu &c) {
        if (native_late)
            g_cpu = &c;
        if (native_now)
            game::install_d2_native(c);
        if (video)
            game::install_cinematics(c);
        game::install_automap_capture(c);
    };
    static LaneVideoAudio video_audio;
    opt.video_audio = &video_audio;
    if (std::getenv("WX86_SON"))   // e.g. WX86_SON=wav WX86_SONDUMP=<file>: sound to a WAV file
        opt.audio = true;
    if (std::getenv("M4_FAST"))   // the 3DS app's video settings (src/app/ctr_game.cpp), for A/B runs
        opt.registry_dwords = {{"Resolution", 0}, {"Light Quality", 0}, {"Blended Shadows", 0}, {"Perspective", 0}};
    opt.data_root = argv[1];
    opt.write_root = argv[2];
    opt.max_ms = (uint32_t)std::strtoul(argv[3], nullptr, 0);
    opt.command_line = argc > 4 ? argv[4] : "game.exe -w";
    opt.cpuid_edx = rt::kCpuidNoSimd;
    opt.trap_report = 250;
    opt.heartbeat_ms = 10000;

    std::vector<uint8_t> exe;
    opt.exe_name = "Game.exe";
    if (FILE *f = std::fopen((opt.data_root + "/Game.exe").c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            exe.insert(exe.end(), buf, buf + n);
        std::fclose(f);
    }
    if (exe.empty()) {
        std::fprintf(stderr, "cannot read %s/Game.exe\n", argv[1]);
        return 2;
    }
    opt.out = [](const char *s, size_t n) { std::fwrite(s, 1, n, stdout); std::fflush(stdout); };
    opt.log = [](const char *s) {
        std::fprintf(stderr, "[rt] %s\n", s);
        second_screen.log(s);
    };
    if (!std::getenv("M4_VIRTUAL_CLOCK")) {
        opt.real_clock = true;
        opt.sleep_ms = [](uint32_t ms) { usleep(ms * 1000); };
    }
    // Screenshots: the frame is converted only when one is due; the latest is
    // kept (indices + palette) for last.png.
    const std::string shots = std::string(argv[2]) + "/../shots";
    std::system(("mkdir -p '" + shots + "'").c_str());
    static std::vector<uint8_t> last_idx;
    static uint32_t last_pal[256], last_w = 0, last_h = 0;
    static bool last_yuv = false;   // a cinematic's frame: last_idx holds its Y, U and V planes
    static uint64_t next_shot = 5000;
    auto save = [shots](const char *name) {
        if (!last_w)
            return;
        std::vector<uint8_t> rgb((size_t)last_w * last_h * 3);
        const uint32_t cw = (last_w + 1) / 2, ch = (last_h + 1) / 2;
        const uint8_t *py = last_idx.data(), *pu = py + (size_t)last_w * last_h, *pv = pu + (size_t)cw * ch;
        auto clip = [](int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (size_t i = 0; i < (size_t)last_w * last_h; i++) {
            if (last_yuv) {   // BT.601, limited range
                const size_t x = i % last_w, y = i / last_w, c = (y / 2) * cw + x / 2;
                const int Y = 298 * (py[i] - 16), U = pu[c] - 128, V = pv[c] - 128;
                rgb[3 * i] = clip((Y + 409 * V + 128) >> 8);
                rgb[3 * i + 1] = clip((Y - 100 * U - 208 * V + 128) >> 8);
                rgb[3 * i + 2] = clip((Y + 516 * U + 128) >> 8);
                continue;
            }
            const uint32_t p = last_pal[last_idx[i]];
            rgb[3 * i] = (uint8_t)(p >> 16);
            rgb[3 * i + 1] = (uint8_t)(p >> 8);
            rgb[3 * i + 2] = (uint8_t)p;
        }
        png::write_rgb(shots + "/" + name, rgb.data(), last_w, last_h);
    };
    // M4_FRAMEHASH=<file>: one line per presented frame, "<n> <hash>" (FNV-1a
    // over the pixels and the palette). Two runs of the same script on the
    // virtual clock must match line for line; a native replacement of game
    // code must not change a single one.
    static FILE *framehash = std::getenv("M4_FRAMEHASH") ? std::fopen(std::getenv("M4_FRAMEHASH"), "w") : nullptr;
    opt.present = [save, shots](const rt::Frame &f) {
        if (f.yuv[0]) {
            // A cinematic's frame: hashed (Y, U, V) and kept for the screenshots.
            const uint32_t cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
            last_w = f.width;
            last_h = f.height;
            last_yuv = true;
            last_idx.resize((size_t)f.width * f.height + 2 * (size_t)cw * ch);
            uint8_t *o = last_idx.data();
            for (int p = 0; p < 3; p++) {
                const uint32_t w = p ? cw : f.width, rows = p ? ch : f.height;
                for (uint32_t y = 0; y < rows; y++, o += w)
                    std::memcpy(o, f.yuv[p] + (size_t)y * f.yuv_stride[p], w);
            }
            if (framehash) {
                static unsigned n = 0;
                uint32_t h = 2166136261u;
                for (uint8_t b : last_idx)
                    h = (h ^ b) * 16777619u;
                std::fprintf(framehash, "video %u %08x %ux%u\n", n++, h, f.width, f.height);
            }
            const uint64_t now = rt::now_ms();
            if (shot_now) {
                shot_now = false;
                char name[32];
                std::snprintf(name, sizeof name, "s%06llu.png", (unsigned long long)now);
                save(name);
            }
            if (now >= next_shot) {
                char name[32];
                std::snprintf(name, sizeof name, "t%03llu.png", (unsigned long long)(now / 1000));
                save(name);
                next_shot = next_shot < 15000 ? 15000 : next_shot < 30000 ? 30000 : next_shot + 30000;
            }
            return;
        }
        last_yuv = false;
        game::automap_frame(f.pixels, f.width, f.height, f.stride);
        if (framehash) {
            static unsigned n = 0;
            uint32_t h = 2166136261u;
            for (uint32_t y = 0; y < f.height; y++) {
                const uint8_t *row = f.pixels + (ptrdiff_t)y * f.stride;
                for (uint32_t x = 0; x < f.width; x++)
                    h = (h ^ row[x]) * 16777619u;
            }
            for (int i = 0; i < 256; i++)
                h = (h ^ f.palette[i]) * 16777619u;
            // M4_HASHMEM=<va>:<bytes>: a guest memory range hashed with each
            // frame, for native code whose result the pixels may not show
            // (the light grid, 0x7b0e68:0x4800).
            static const char *hm = std::getenv("M4_HASHMEM");
            if (hm) {
                char *end = nullptr;
                const uint32_t va = (uint32_t)std::strtoul(hm, &end, 0);
                const uint32_t len = end && *end == ':' ? (uint32_t)std::strtoul(end + 1, nullptr, 0) : 0;
                std::vector<uint8_t> mem(len);
                uint32_t mh = 2166136261u;
                if (len && rt::guest_read(va, mem.data(), len))
                    for (uint8_t b : mem)
                        mh = (mh ^ b) * 16777619u;
                std::fprintf(framehash, "%u %08x %ux%u mem %08x\n", n, h, f.width, f.height, mh);
            } else {
                std::fprintf(framehash, "%u %08x %ux%u\n", n, h, f.width, f.height);
            }
            // M4_DUMPFRAME=<n>: that frame's raw indices, for comparing two runs.
            static const char *dump = std::getenv("M4_DUMPFRAME");
            if (dump && n == (unsigned)std::atoi(dump)) {
                if (FILE *o = std::fopen((shots + "/frame.raw").c_str(), "wb")) {
                    for (uint32_t y = 0; y < f.height; y++)
                        std::fwrite(f.pixels + (ptrdiff_t)y * f.stride, 1, f.width, o);
                    std::fwrite(f.palette, 4, 256, o);
                    std::fclose(o);
                }
            }
            n++;
        }
        // CPU time per frame (all of the host process: translated game code,
        // runtime, qemu): the lane's measure of how much work a frame is,
        // since qemu on a fast host never runs short of time the way the 3DS
        // does. Printed every 250 frames.
        static unsigned frames = 0;
        static double cpu0 = 0;
        timespec ts;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        const double cpu = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
        if (++frames % 250 == 0) {
            if (cpu0)
                std::fprintf(stderr, "[rt] cpu %.2f ms/frame (%ux%u) at frame %u\n", (cpu - cpu0) / 250, f.width,
                             f.height, frames);
            cpu0 = cpu;
        }
        last_w = f.width;
        last_h = f.height;
        last_idx.resize((size_t)f.width * f.height);
        for (uint32_t y = 0; y < f.height; y++)
            std::memcpy(&last_idx[(size_t)y * f.width], f.pixels + (ptrdiff_t)y * f.stride, f.width);
        std::memcpy(last_pal, f.palette, sizeof last_pal);
        const uint64_t now = rt::now_ms();
        second_screen.set_frame((int)f.width, (int)f.height);
        second_screen.stage(bottom_view, now);
        game::set_automap_capture(second_screen.wants_automap(), second_screen.map_ms(), second_screen.map_scale());
        if (shot_now) {
            shot_now = false;
            char name[32];
            std::snprintf(name, sizeof name, "s%06llu.png", (unsigned long long)now);
            save(name);
            // The bottom screen as the console would show it now.
            static uint16_t fb[320 * 240];
            uint16_t pal[256];
            for (int i = 0; i < 256; i++) {
                const uint32_t p = f.palette[i];
                pal[i] = (uint16_t)((((p >> 16) & 0xFF) >> 3 << 11) | (((p >> 8) & 0xFF) >> 2 << 5) | ((p & 0xFF) >> 3));
            }
            game::draw_bottom(fb, bottom_view, last_idx.data(), (int)f.width, (int)f.height, pal, kCtruFont);
            std::vector<uint8_t> rgb(320 * 240 * 3);
            for (int y = 0; y < 240; y++)
                for (int x = 0; x < 320; x++) {
                    const uint16_t c = fb[x * 240 + 239 - y];
                    uint8_t *o = &rgb[(y * 320 + x) * 3];
                    o[0] = (uint8_t)((c >> 11) << 3), o[1] = (uint8_t)(((c >> 5) & 63) << 2), o[2] = (uint8_t)((c & 31) << 3);
                }
            std::snprintf(name, sizeof name, "b%06llu.png", (unsigned long long)now);
            png::write_rgb(shots + "/" + name, rgb.data(), 320, 240);
        }
        if (now >= next_shot) {
            char name[32];
            std::snprintf(name, sizeof name, "t%03llu.png", (unsigned long long)(now / 1000));
            save(name);
            next_shot = next_shot < 15000 ? 15000 : next_shot < 30000 ? 30000 : next_shot + 30000;
        }
    };
    // M4_BOTTOM=map|help|keys|zoom|info: the bottom screen's tab at the start.
    if (const char *t = std::getenv("M4_BOTTOM")) {
        game::BottomTab tab;
        if (game::bottom_tab_from_name(t, tab))
            second_screen.set_tab(tab);
        second_screen.set_map_help(std::strcmp(t, "help") == 0);
    }
    static game::Autoplay autoplay;
    if (const char *sc = std::getenv("M4_SCRIPT"))
        autoplay.load(sc);
    static game::Controls controls;
    opt.poll_input = [] {
        const uint64_t now = rt::now_ms();
        bool shot = false;
        const game::GameState gs = game::read_game_state();
        // WX86_HOTPROF: profile only the game world, from 10 s after arriving.
        static uint64_t in_game_since = 0;
        static bool reset_done = false;
        if (gs.in_game && !in_game_since) {
            in_game_since = now;
            if (g_cpu)
                game::install_d2_native(*g_cpu);
        }
        if (in_game_since && !reset_done && now - in_game_since > 10000 && std::getenv("WX86_HOTPROF")) {
            dyn86_hotprof_reset();
            reset_done = true;
            std::fprintf(stderr, "[rt] hotprof: counting from here (in game)\n");
        }
        game::Pad pad = autoplay.step(now, controls, shot, gs);
        if (shot)
            shot_now = true;
        second_screen.update(pad, gs, controls.cursor_x(), controls.cursor_y(), now);
        controls.set_context(gs.in_game, gs.panel_open);
        controls.update(pad, now);
        second_screen.request(controls.take_bottom_request());
    };
    // M4_ASYNCIO=1 (real clock only): asynchronous file reads with a worker
    // thread, as on the console. Not deterministic; off by default.
    static std::mutex io_mutex;
    static std::condition_variable io_cv;
    static bool io_signaled = false;
    static std::thread io_thread;
    if (std::getenv("M4_ASYNCIO") && !std::getenv("M4_VIRTUAL_CLOCK")) {
        opt.io_start = [](void (*worker)()) {
            io_thread = std::thread(worker);
            return true;
        };
        opt.io_wait = [] {
            std::unique_lock<std::mutex> lock(io_mutex);
            io_cv.wait(lock, [] { return io_signaled; });
            io_signaled = false;
        };
        opt.io_wake = [] {
            {
                std::lock_guard<std::mutex> lock(io_mutex);
                io_signaled = true;
            }
            io_cv.notify_one();
        };
        static std::mutex done_mutex;
        static std::condition_variable done_cv;
        static bool done = false;
        opt.io_done = [] {
            {
                std::lock_guard<std::mutex> lock(done_mutex);
                done = true;
            }
            done_cv.notify_one();
        };
        opt.io_idle_wait = [](uint32_t ms) {
            std::unique_lock<std::mutex> lock(done_mutex);
            done_cv.wait_for(lock, std::chrono::milliseconds(ms), [] { return done; });
            done = false;
        };
    }
#ifdef PROF_COUNTERS
    // M4_PROF: the time-based profile sampler, as on the console (2 ms).
    static std::atomic<bool> sampler_stop{false};
    std::thread sampler([] {
        while (!sampler_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            rt::profile_tick();
        }
    });
#endif
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    if (io_thread.joinable())
        io_thread.join();
#ifdef PROF_COUNTERS
    sampler_stop = true;
    sampler.join();
#endif
    dyn86_hotprof_dump(stderr, 60);
    save("last.png");
    if (!r.loaded) {
        std::fprintf(stderr, "[rt] FATAL %s\n", r.error.c_str());
        return 3;
    }
    std::fprintf(stderr, "[rt] RESULT exited=%d timed_out=%d code=%u %s\n", r.exited, r.timed_out, r.exit_code,
                 r.error.c_str());
    std::fflush(stdout);
    std::_Exit(0);
}
