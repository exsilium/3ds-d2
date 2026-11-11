// The port's options (issue #80): how the engine runs on the console, as
// opposed to what a profiler run measures. They live on the INFO tab's
// OPTIONS page (second_screen.h): a box per option, applied at once (a run
// on or not, so a profiler run can compare them), and saved in settings.txt
// as "port=" (port_names).
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
    // Sprites (DCC groups) decoded by native code instead of the game's, on
    // the I/O worker's core when Storm's read thread decodes them
    // (dcc_native.h, issue #83).
    kPortNativeDcc = 1u << 2,
    // At 50 Hz, the game's clock kept in step with the screen's refreshes,
    // so that its frames reach the screen at the same point of a refresh
    // (ctr_game.cpp's frame lock, issue #86). Only with core 2 draw's
    // render thread.
    kPortFrameLock = 1u << 3,
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
    {kPortNativeDcc, "dcc", "native sprite decoder", "new sprites decoded off core 0"},
    {kPortFrameLock, "framelock", "frame lock at 50 Hz", "the game's frames timed by the screen"},
};
inline constexpr int kPortOptionCount = sizeof kPortOptions / sizeof kPortOptions[0];
inline constexpr uint32_t kPortDefault = kPortCore2Draw | kPortScreen50 | kPortNativeDcc;
inline constexpr uint32_t kPortAll = kPortCore2Draw | kPortScreen50 | kPortNativeDcc | kPortFrameLock;
// The options of the first "port=" lines, which named only those on.
inline constexpr uint32_t kPortFirst = kPortCore2Draw;

// settings.txt's "port=": every option, "-name" for one that is off
// ("core2draw,-screen50"). A line with no "-" is the first form, which named
// only the options on: those it could name are off unless named, the newer
// ones keep their defaults. Unknown names are left out.
inline uint32_t port_bits(const char *names)
{
    const bool first_form = std::strchr(names, '-') == nullptr;
    uint32_t bits = first_form ? kPortDefault & ~kPortFirst : kPortDefault;
    while (*names) {
        const size_t n = std::strcspn(names, ", \r\n");
        const bool off = n > 0 && *names == '-';
        const char *name = names + (off ? 1 : 0);
        const size_t len = n - (off ? 1 : 0);
        for (const PortOptionInfo &o : kPortOptions)
            if (len == std::strlen(o.name) && std::strncmp(name, o.name, len) == 0)
                bits = off ? bits & ~o.bit : bits | o.bit;
        names += n;
        names += *names ? 1 : 0;
    }
    return bits;
}

inline std::string port_names(uint32_t bits)
{
    std::string s;
    for (const PortOptionInfo &o : kPortOptions) {
        if (!s.empty())
            s += ',';
        if (!(bits & o.bit))
            s += '-';
        s += o.name;
    }
    return s;
}

}  // namespace game

#endif
