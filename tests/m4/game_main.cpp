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
// M4_SCRIPT drives game::Controls (the 3DS control scheme) with scripted pad
// events (game::Autoplay, src/game/autoplay.h), ';'-separated, e.g.
//   "20000 tap 400 307;26000 tap 400 290;32000 text Tester;34000 press R;..."

#include "game/autoplay.h"
#include "game/controls.h"
#include "game/d2_native.h"
#include "game/d2_state.h"
#include "runtime/rt_host.h"
#include "png.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
void dyn86_hotprof_reset(void);
void dyn86_hotprof_dump(FILE *f, int top);
}

static bool shot_now = false;   // M4_SCRIPT "shot": save the next frame

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
    if (native_late)
        opt.install_native = [](d2rt::Cpu &c) { g_cpu = &c; };
    else if (!std::getenv("M4_NATIVE") || std::string(std::getenv("M4_NATIVE")) != "0")
        opt.install_native = [](d2rt::Cpu &c) { game::install_d2_native(c); };
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
    opt.log = [](const char *s) { std::fprintf(stderr, "[rt] %s\n", s); };
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
    static uint64_t next_shot = 5000;
    auto save = [shots](const char *name) {
        if (!last_w)
            return;
        std::vector<uint8_t> rgb((size_t)last_w * last_h * 3);
        for (size_t i = 0; i < (size_t)last_w * last_h; i++) {
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
            std::fprintf(framehash, "%u %08x %ux%u\n", n, h, f.width, f.height);
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
    };
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
        const game::Pad &pad = autoplay.step(now, controls, shot, gs);
        if (shot)
            shot_now = true;
        controls.set_context(gs.in_game, gs.panel_open);
        controls.update(pad, now);
    };
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
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
