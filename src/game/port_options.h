// The port's options (issue #80): how the engine runs on the console, as
// opposed to what a profiler run measures. They live on the INFO tab's
// OPTIONS page (second_screen.h): a box per option, applied at once (a run
// on or not, so a profiler run can compare them), and saved in settings.txt
// as "port=" with the names of those that are on.
#ifndef D2_GAME_PORT_OPTIONS_H
#define D2_GAME_PORT_OPTIONS_H

#include <cstdint>
#include <cstring>
#include <string>

namespace game {

enum PortOption : uint32_t {
    // The second core draws the floor beside the game's thread, and copies
    // each frame out of the game's buffer (d2_native.h's floor queue and
    // renderer guard). Only with core 2 (a New 3DS).
    kPortCore2Draw = 1u << 0,
    // The top screen refreshes at 50 Hz instead of 59.83, so that each of
    // the game's 25 frames a second stays on it for exactly two refreshes
    // (40 ms) instead of two or three (33 or 50). ctr_game.cpp's screen_hz:
    // one LCD timing register, put back for the HOME menu, sleep and exit.
    kPortScreen50 = 1u << 1,
};

struct PortOptionInfo {
    uint32_t bit;
    const char *name;    // in settings.txt
    const char *label;   // on the page
    const char *note;    // under it: what it does
};

inline constexpr PortOptionInfo kPortOptions[] = {
    {kPortCore2Draw, "core2draw", "draw on core 2", "floor and frame copy on core 2"},
    {kPortScreen50, "screen50", "screen at 50 Hz", "each frame 40 ms on the screen"},
};
inline constexpr int kPortOptionCount = sizeof kPortOptions / sizeof kPortOptions[0];
inline constexpr uint32_t kPortDefault = kPortCore2Draw;
inline constexpr uint32_t kPortAll = kPortCore2Draw | kPortScreen50;

// "core2draw ..." <-> bits; unknown names are left out.
inline uint32_t port_bits(const char *names)
{
    uint32_t bits = 0;
    while (*names) {
        const size_t n = std::strcspn(names, ", \r\n");
        for (const PortOptionInfo &o : kPortOptions)
            if (n == std::strlen(o.name) && std::strncmp(names, o.name, n) == 0)
                bits |= o.bit;
        names += n;
        names += *names ? 1 : 0;
    }
    return bits;
}

inline std::string port_names(uint32_t bits)
{
    std::string s;
    for (const PortOptionInfo &o : kPortOptions)
        if (bits & o.bit) {
            if (!s.empty())
                s += ',';
            s += o.name;
        }
    return s;
}

}  // namespace game

#endif
