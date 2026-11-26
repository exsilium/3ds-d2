// src/game/pad_history.h (issue #118): presses shorter than a frame kept.
// A pad sampled every 4 ms, a pump once a frame; scripted presses of 80 and
// 150 ms at 25, 10 and 5 fps (D2Vita's bench for the same fix, bc968fb), the
// pump reading the pad once (as before) against the history. Run through
// `scripts/docker.sh test pad`.
#include "game/pad_history.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <vector>

using game::Pad;
using game::PadHistory;
using game::PadSample;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

// A script: the bits held at time t (ms).
struct Press {
    uint64_t at, len;
    uint32_t bits;
    bool touch;
};

static Pad pad_at(const std::vector<Press> &script, uint64_t t, uint32_t base = 0)
{
    Pad p;
    p.held = base;
    for (const Press &x : script)
        if (t >= x.at && t < x.at + x.len) {
            p.held |= x.bits;
            p.touching |= x.touch;
        }
    return p;
}

// Rising edges of `bit` (or the touch screen) in what the pump applied.
struct Seen {
    uint32_t presses = 0, touches = 0;
    uint32_t held = 0;
    bool touching = false;
    uint32_t with_r = 0;   // A pressed while R was held
    void apply(const Pad &p)
    {
        if ((p.held & game::kA) && !(held & game::kA)) {
            presses++;
            if (p.held & game::kR)
                with_r++;
        }
        if (p.touching && !touching)
            touches++;
        held = p.held;
        touching = p.touching;
    }
};

// Runs a script for `ms`, sampling every 4 ms and pumping every `frame` ms
// (from `phase` on); returns what the pump saw, both ways.
static void run(const std::vector<Press> &script, uint64_t ms, uint64_t frame, uint64_t phase, Seen &before,
                Seen &after, uint32_t base = 0)
{
    PadHistory h;
    std::vector<PadSample> out;
    for (uint64_t t = 0; t <= ms; t++) {
        if (t % 4 == 0)
            h.push(pad_at(script, t, base), t);
        if (t >= phase && (t - phase) % frame == 0) {
            before.apply(pad_at(script, t, base));   // the pump reading the pad itself
            h.take(out);
            for (const PadSample &s : out)
                after.apply(s.pad);
        }
    }
}

static void short_presses()
{
    for (uint64_t frame : {40u, 100u, 200u}) {
        for (uint64_t len : {80u, 150u}) {
            std::vector<Press> script;
            uint64_t t = 300;
            for (int k = 0; k < 12; k++) {
                script.push_back({t, len, game::kA, false});
                t += len + 230 + 37 * (uint64_t)k;   // gaps that fall differently against the frames
            }
            Seen before, after;
            run(script, t + 500, frame, 7, before, after);
            std::printf("  %3u fps, %3u ms presses: %2u of 12 kept before, %2u now\n", (unsigned)(1000 / frame),
                        (unsigned)len, before.presses, after.presses);
            CHECK(after.presses == 12);
        }
    }
}

// Nothing changes twice between two pumps: the latest reading alone, as the
// pump reading the pad itself would have had it.
static void unchanged_without_short_presses()
{
    std::vector<Press> script;
    for (int k = 0; k < 20; k++)
        script.push_back({100 + 300 * (uint64_t)k, 250, k % 2 ? game::kB : game::kA, k % 3 == 0});
    PadHistory h;
    std::vector<PadSample> out;
    bool same = true;
    size_t replays = 0;
    for (uint64_t t = 0; t <= 6500; t++) {
        if (t % 4 == 0)
            h.push(pad_at(script, t), t);
        if (t % 100 == 3) {   // a pump every 100 ms: every press spans one
            replays += h.take(out);
            const Pad want = pad_at(script, t - t % 4);
            same &= out.size() == 1 && out[0].pad.held == want.held && out[0].pad.touching == want.touching;
        }
    }
    CHECK(replays == 0);
    CHECK(same);
}

// A tap of the touch screen shorter than a frame, and A tapped while R is
// held (a modifier combination), at 5 fps.
static void touch_and_modifiers()
{
    Seen before, after;
    run({{500, 60, 0, true}, {1500, 60, 0, true}}, 2500, 200, 11, before, after);
    CHECK(after.touches == 2);
    Seen b2, a2;
    run({{300, 2000, game::kR, false}, {700, 70, game::kA, false}, {1400, 70, game::kA, false}}, 2600, 200, 3, b2, a2);
    CHECK(a2.presses == 2);
    CHECK(a2.with_r == 2);
}

// More changes than it keeps between two takes: the oldest dropped, counted;
// reset() forgets the changes, and the next take is the latest alone.
static void overflow_and_reset()
{
    PadHistory h;
    for (uint32_t k = 0; k < 200; k++) {
        Pad p;
        p.held = k & 1 ? (uint32_t)game::kA : 0u;
        h.push(p, k);
    }
    std::vector<PadSample> out;
    const size_t n = h.take(out);
    CHECK(n == PadHistory::kMax);
    CHECK(out.size() == PadHistory::kMax + 1);
    CHECK(h.dropped() == 200 - PadHistory::kMax);
    for (uint32_t k = 0; k < 6; k++) {
        Pad p;
        p.held = k & 1 ? (uint32_t)game::kB : 0u;
        h.push(p, 300 + k);
    }
    h.reset();
    CHECK(h.take(out) == 0);
    CHECK(out.size() == 1 && out[0].ms == 305);
}

// Controls (src/game/controls.cpp) with these stubs: the left button's
// downs and ups, each with the pump it was sent in.
static uint32_t g_pump = 0;
struct ButtonEvent {
    bool down;
    uint32_t pump;
};
static std::vector<ButtonEvent> g_left;
static bool g_left_on = false;
namespace rt {
void input_button(bool right, bool down)
{
    if (!right && down != g_left_on) {
        g_left.push_back({down, g_pump});
        g_left_on = down;
    }
}
void input_key(uint8_t, bool) {}
void input_mouse_move(int, int) {}
void logf(const char *, ...) {}
}  // namespace rt

// A pressed shorter than a pump, replayed: the click (left button) goes down
// in one pump and up in the next, never both in one.
static void clicks_across_pumps()
{
    game::Controls c;
    c.set_frame(800, 600);
    c.set_context(false, false, 0, false);   // the menus: A is the left button
    std::vector<Press> script;
    for (int k = 0; k < 6; k++)
        script.push_back({300 + 500 * (uint64_t)k, 60, game::kA, false});
    PadHistory h;
    std::vector<PadSample> out;
    for (uint64_t t = 0; t <= 3500; t++) {
        if (t % 4 == 0)
            h.push(pad_at(script, t), t);
        if (t % 200 == 9) {   // 5 fps
            g_pump++;
            c.begin_pump();
            h.take(out);
            for (const PadSample &s : out)
                c.update(s.pad, s.ms);
        }
    }
    uint32_t clicks = 0;
    bool apart = true;
    for (size_t k = 0; k + 1 < g_left.size(); k++)
        if (g_left[k].down && !g_left[k + 1].down) {
            clicks++;
            apart &= g_left[k + 1].pump > g_left[k].pump;
        }
    std::printf("  60 ms presses of A at 5 fps: %u of 6 clicks, down and up in different pumps: %s\n", clicks,
                apart ? "yes" : "no");
    CHECK(clicks == 6);
    CHECK(apart);
}

int main()
{
    std::printf("pad history (issue #118):\n");
    short_presses();
    unchanged_without_short_presses();
    touch_and_modifiers();
    overflow_and_reset();
    clicks_across_pumps();
    std::printf("RESULT pad.history %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
