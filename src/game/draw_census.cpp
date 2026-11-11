// The draw census: see draw_census.h.
#include "game/draw_census.h"

#include "runtime/cpu.h"
#include "runtime/host_clock.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace game {
namespace {

constexpr uint32_t kPitch = 0x007d544c;   // the frame buffer's row pitch (d2_native.cpp)
constexpr uint32_t kFbBase = 0x007d5448;  // where the natives draw

uint8_t *g_base = nullptr;
uint32_t g_every = 0, g_frame = 0;
bool g_active = false;   // this frame is a census frame
int g_depth = 0;         // natives inside natives (guest_call) are the outer one's
uint32_t g_fb = 0, g_bytes = 0;
std::vector<uint8_t> g_shadow;

struct Box {
    int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool empty() const { return x1 < x0; }
};

struct Run {
    Box guest;   // what the game's code drew before the run
    uint32_t ret = 0, n = 0;
    std::string chain;   // the callers above it, by the EBP chain
    std::vector<std::pair<const char *, uint32_t>> calls;
};
std::vector<Run> g_runs;

// The draw phases.
bool g_phases = false;
constexpr uint32_t kPhaseFrames = 250;
struct Phases {
    uint32_t frames = 0, tiles = 0;
    uint64_t first_blit = 0, floor_span = 0, floor_native = 0;
    uint64_t before = 0, after = 0, rest = 0;   // blit to floor run, floor run to the next native, to the blit
    uint64_t present[4] = {};                   // the app's present: automap, copy, bottom screen, all
    std::map<const char *, uint64_t> native;   // us by name
} g_ph;
uint64_t g_floor_start = 0, g_floor_end = 0, g_floor_us = 0, g_floor_next = 0;   // this frame's
uint64_t g_last_blit = 0;
uint32_t g_floor_tiles = 0;
bool g_floor_closed = false;

void phases_frame()
{
    const uint64_t now = wx86_now_us();
    if (g_floor_tiles) {
        g_ph.floor_span += g_floor_end - g_floor_start;
        g_ph.floor_native += g_floor_us;
        g_ph.tiles += g_floor_tiles;
        if (g_last_blit)
            g_ph.before += g_floor_start - g_last_blit;
        g_ph.after += (g_floor_next ? g_floor_next : now) - g_floor_end;
        g_ph.rest += now - g_floor_end;
    }
    g_last_blit = now;
    g_floor_next = 0;
    g_floor_tiles = 0;
    g_floor_us = 0;
    g_floor_closed = false;
    if (!g_ph.first_blit) {
        g_ph = Phases();
        g_ph.first_blit = now;
        return;
    }
    if (++g_ph.frames < kPhaseFrames)
        return;
    const double n = g_ph.frames, ms = 1000.0 * n;
    std::string names;
    uint64_t all = 0;
    for (const auto &kv : g_ph.native) {
        char b[48];
        std::snprintf(b, sizeof b, " %s %.2f", kv.first, kv.second / ms);
        names += b;
        all += kv.second;
    }
    rt::logf("draw phases: %u frames of %.1f ms: floor run %.2f ms a frame (FloorTile %.2f, the frame thread between "
             "them %.2f, %.0f tiles); draw natives %.2f ms (%s); from the last blit to the floor run %.2f, from it "
             "to the next draw native %.2f, to the blit %.2f; present %.2f (automap %.2f, copy %.2f, bottom screen "
             "%.2f)",
             g_ph.frames, (now - g_ph.first_blit) / ms, g_ph.floor_span / ms, g_ph.floor_native / ms,
             (g_ph.floor_span - g_ph.floor_native) / ms, g_ph.tiles / n, all / ms, names.c_str() + 1,
             g_ph.before / ms, g_ph.after / ms, g_ph.rest / ms, g_ph.present[3] / ms, g_ph.present[0] / ms,
             g_ph.present[1] / ms, g_ph.present[2] / ms);
    g_ph = Phases();
    g_ph.first_blit = now;
}

uint32_t U32(uint32_t va)
{
    uint32_t v;
    std::memcpy(&v, g_base + va, 4);
    return v;
}

// What changed since the copy, as a box, and the copy brought up to date.
Box sync()
{
    Box b;
    const uint8_t *fb = g_base + g_fb;
    if (!std::memcmp(fb, g_shadow.data(), g_bytes))
        return b;
    uint32_t pitch = U32(kPitch);
    if (!pitch || pitch > g_bytes)
        pitch = g_bytes;
    for (uint32_t y = 0; y * pitch < g_bytes; y++) {
        const uint8_t *r = fb + (size_t)y * pitch, *s = g_shadow.data() + (size_t)y * pitch;
        const uint32_t w = g_bytes - y * pitch < pitch ? g_bytes - y * pitch : pitch;
        if (!std::memcmp(r, s, w))
            continue;
        uint32_t l = 0, h = w;
        while (r[l] == s[l])
            l++;
        while (r[h - 1] == s[h - 1])
            h--;
        if (b.empty()) {
            b = {(int32_t)l, (int32_t)y, (int32_t)h - 1, (int32_t)y};
        } else {
            b.x0 = (int32_t)l < b.x0 ? (int32_t)l : b.x0;
            b.x1 = (int32_t)h - 1 > b.x1 ? (int32_t)h - 1 : b.x1;
            b.y1 = (int32_t)y;
        }
    }
    std::memcpy(g_shadow.data(), fb, g_bytes);
    return b;
}

std::string box_text(const Box &b)
{
    if (b.empty())
        return "-";
    return std::to_string(b.x0) + "," + std::to_string(b.y0) + "," + std::to_string(b.x1) + "," + std::to_string(b.y1);
}

void blit(uint32_t bits, uint32_t bytes)
{
    if (g_phases)
        phases_frame();
    if (!g_every)
        return;
    if (g_active) {
        const Box end = sync();
        uint32_t natives = 0;
        for (size_t k = 0; k < g_runs.size(); k++) {
            const Run &r = g_runs[k];
            natives += r.n;
            std::string calls;
            for (const auto &c : r.calls)
                calls += (calls.empty() ? "" : ",") + std::string(c.first) + ":" + std::to_string(c.second);
            rt::logf("census %u run %u guest=%s ret=%08x n=%u %s from %s", g_frame, (unsigned)k,
                     box_text(r.guest).c_str(), r.ret, r.n, calls.c_str(), r.chain.c_str());
        }
        rt::logf("census %u end guest=%s natives %u runs %u fb %08x bits %08x", g_frame, box_text(end).c_str(), natives,
                 (unsigned)g_runs.size(), U32(kFbBase), bits);
    }
    g_frame++;
    g_active = g_every && g_frame % g_every == 0;
    g_runs.clear();
    if (g_active) {
        g_fb = bits;
        g_bytes = bytes;
        g_shadow.assign(g_base + bits, g_base + bits + bytes);
    }
}

}  // namespace

void draw_phases_set(bool on) { g_phases = on; }
bool draw_phases_on() { return g_phases; }

void draw_phases_present(uint64_t automap_us, uint64_t copy_us, uint64_t bottom_us, uint64_t all_us)
{
    g_ph.present[0] += automap_us;
    g_ph.present[1] += copy_us;
    g_ph.present[2] += bottom_us;
    g_ph.present[3] += all_us;
}

bool draw_census_init(uint8_t *base)
{
    const char *e = std::getenv("D2_DRAW_CENSUS");
    g_every = e ? (uint32_t)std::strtoul(e, nullptr, 0) : 0;
    if (std::getenv("D2_DRAW_PHASES"))
        g_phases = true;
    if (!g_every && !g_phases)
        return false;
    g_base = base;
    rt::set_blit_hook(blit);
    if (g_every)
        rt::logf("draw census: every %u frames", g_every);
    if (g_phases)
        rt::logf("draw phases: timed, every %u frames", kPhaseFrames);
    return true;
}

NativeFn draw_census_wrap(const char *name, NativeFn fn)
{
    if (!g_every && !g_phases)
        return fn;
    const bool floor = !std::strcmp(name, "FloorTile");
    return [name, fn, floor](d2rt::Cpu &c) -> uint32_t {
        if (g_phases && !g_depth) {
            const uint64_t t0 = wx86_now_us();
            if (!floor && g_floor_tiles && !g_floor_closed) {
                g_floor_closed = true;
                g_floor_next = t0;
            }
            g_depth++;
            const uint32_t v = fn(c);
            g_depth--;
            const uint64_t t1 = wx86_now_us();
            g_ph.native[name] += t1 - t0;
            if (floor && !g_floor_closed) {
                if (!g_floor_tiles++)
                    g_floor_start = t0;
                g_floor_end = t1;
                g_floor_us += t1 - t0;
            }
            return v;
        }
        if (!g_active || g_depth)
            return fn(c);
        const Box guest = sync();
        if (g_runs.empty() || !guest.empty()) {
            Run r;
            r.guest = guest;
            r.ret = U32(c.reg(d2rt::R_ESP));
            uint32_t ebp = c.reg(d2rt::R_EBP);
            for (int i = 0; i < 6 && ebp > 0x10000 && ebp < 0x7ffff000u; i++) {
                char b[12];
                std::snprintf(b, sizeof b, "%s%08x", i ? "," : "", U32(ebp + 4));
                r.chain += b;
                const uint32_t up = U32(ebp);
                if (up <= ebp)
                    break;
                ebp = up;
            }
            g_runs.push_back(r);
        }
        Run &r = g_runs.back();
        r.n++;
        // The run's calls by name, in the order first seen (a line stays short).
        auto it = r.calls.begin();
        while (it != r.calls.end() && it->first != name)
            ++it;
        if (it == r.calls.end())
            it = r.calls.insert(it, {name, 0});
        it->second++;
        g_depth++;
        const uint32_t v = fn(c);
        g_depth--;
        sync();   // the native's own pixels
        return v;
    };
}

}  // namespace game
