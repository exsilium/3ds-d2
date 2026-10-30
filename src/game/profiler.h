// The profiler run (issue #19): what a session measures beyond what every
// session logs. Every heartbeat already carries the frame rate, the hitch
// counters, the sprite cache, the file reads and the frame thread's time;
// those cost nothing worth measuring. The options here cost something
// (time, SD card space, a pause), so they run only when asked: INFO's
// "profiler run" button opens a checklist of them, and its start and stop
// button runs the checked ones until it is tapped again. The checklist is
// a setting; it cannot change while a run is on.
//
// The platform starts and stops each option at its heartbeat and logs
//   profiler run: start <name>,<name>...
//   profiler run: stop
// in game.log. A session with a run is offered as a report when the game
// ends, and the report says what was profiled (report.h reads those lines),
// with the traces the run wrote.
#ifndef D2_GAME_PROFILER_H
#define D2_GAME_PROFILER_H

#include <cstdint>
#include <cstring>
#include <string>

namespace game {

enum ProfileOption : uint32_t {
    kProfSprites = 1u << 0,   // the sprite cache's traffic (sprite_trace.h): logs/sprites.trc
    kProfGuest = 1u << 1,     // the guest code profile (rt::set_profiling): game.log's "prof" lines
    kProfFiles = 1u << 2,     // every read of a game file (rt::set_file_trace): logs/files.trc
    kProfSdBench = 1u << 3,   // the SD card's read costs, measured once as the run starts
};

struct ProfileOptionInfo {
    uint32_t bit;
    const char *name;    // settings.txt, game.log and the report's tag
    const char *label;   // the checklist (at most 26 columns)
    const char *note;    // under it: what it writes, what it costs (at most 36)
};

inline constexpr ProfileOptionInfo kProfileOptions[] = {
    {kProfSprites, "sprites", "sprite trace", "cache traffic to logs/sprites.trc"},
    {kProfGuest, "guest", "guest code profile", "shims, threads, EIPs; ~12% slower"},
    {kProfFiles, "files", "file-read trace", "every game read to logs/files.trc"},
    {kProfSdBench, "sdbench", "SD card benchmark", "read costs; a 1 s pause at start"},
};
inline constexpr int kProfileOptionCount = sizeof kProfileOptions / sizeof kProfileOptions[0];
inline constexpr uint32_t kProfileAll = kProfSprites | kProfGuest | kProfFiles | kProfSdBench;

inline constexpr char kProfilerRunLine[] = "profiler run: ";

// "sprites,guest": the options in `bits`, in checklist order ("" for none).
inline std::string profile_names(uint32_t bits)
{
    std::string s;
    for (const ProfileOptionInfo &o : kProfileOptions)
        if (bits & o.bit)
            s += (s.empty() ? "" : ",") + std::string(o.name);
    return s;
}

// The bits named in a profile_names() list; unknown names are left out.
inline uint32_t profile_bits(const char *names)
{
    uint32_t bits = 0;
    while (*names) {
        const size_t n = std::strcspn(names, ", \r\n");
        for (const ProfileOptionInfo &o : kProfileOptions)
            if (n == std::strlen(o.name) && std::strncmp(names, o.name, n) == 0)
                bits |= o.bit;
        names += n;
        names += *names ? 1 : 0;
    }
    return bits;
}

}  // namespace game

#endif
