// See monster_pace.h.
#include "game/monster_pace.h"

#include "game/native_sites.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <unordered_map>

namespace game {

namespace {

constexpr uint32_t kSkip = 0xffffffff;
constexpr uint64_t kEvery = 100 * 1000;         // a mode let through, at the most
constexpr uint64_t kForget = 60 * 1000 * 1000;  // a class not asked for this long starts again
constexpr uint32_t kNeutral = 1, kWalk = 2, kHit = 3, kAttack = 4;

// A monster class: the modes let through (a bit each), and when it was
// last asked for.
struct Class {
    uint32_t through = 0, wanted = 0;
    uint64_t first_us = 0, last_us = 0;
};
std::unordered_map<uint32_t, Class> g_classes;
bool g_on = true;
uint64_t g_last_us = 0;
struct Window {
    uint32_t asked = 0, through = 0, later = 0, unpaced = 0;
};
Window g_w;

// 0: at once; 1: once the class's rank-0 modes are through; 2: after those.
int rank(uint32_t mode, uint32_t own)
{
    if (mode == own || mode == kNeutral || mode == kWalk)
        return 0;
    return mode == kAttack || mode == kHit ? 1 : 2;
}

// At the stub: eax the mode, esi the unit, [ebp+8] set for a load in the
// background (0x46feb0's frame).
uint32_t on_mode(d2rt::Cpu &c)
{
    const uint32_t mode = c.reg(d2rt::R_EAX), unit = c.reg(d2rt::R_ESI);
    if (!g_on || mode >= 32 || !unit || !c.read_u32(c.reg(d2rt::R_EBP) + 8)) {
        g_w.unpaced++;
        return mode;
    }
    const uint32_t cls = c.read_u32(unit + 4), own = c.read_u32(unit + 0x10);
    const uint64_t now = rt::now_us();
    Class &k = g_classes[cls];
    if (now - k.last_us > kForget) {
        k.through = k.wanted = 0;
        k.first_us = now;
    }
    k.last_us = now;
    const uint32_t bit = 1u << mode;
    if (k.through & bit)   // the game's own table says whether it is done
        return mode;
    g_w.asked += !(k.wanted & bit);
    k.wanted |= bit;
    // A mode of a lower rank still to go through keeps this one back; and
    // the class's first walk, which has not yet said which modes it has,
    // lets only the modes drawn first through.
    const int r = rank(mode, own);
    bool first = r == 0 || now - k.first_us >= kEvery;
    for (uint32_t m = 0; m < 32 && first; m++)
        if ((k.wanted & ~k.through & (1u << m)) && rank(m, own) < r)
            first = false;
    if (!first || now - g_last_us < kEvery) {
        g_w.later++;
        return kSkip;
    }
    g_last_us = now;
    k.through |= bit;
    g_w.through++;
    if (g_classes.size() > 256)   // the host heap is tight
        g_classes.clear();
    return mode;
}

}  // namespace

bool install_monster_pace(d2rt::Cpu &c, bool on)
{
    g_on = on;
    const uint32_t native = rt::native_entry("MonsterMode", 0, on_mode);
    if (!native || !apply_sites(c, "monster_pace", {{"MonsterMode", native}}))
        return false;
    rt::logf("monster pace: %s", on ? "a monster's modes asked for one every 100 ms, the ones drawn first ahead"
                                    : "off, a monster's modes asked for at once");
    return true;
}

std::vector<std::string> monster_pace_lines()
{
    std::vector<std::string> out;
    if (!g_w.asked && !g_w.through && !g_w.unpaced)
        return out;
    char line[200];
    std::snprintf(line, sizeof line,
                  "[rt] monster pace: %u modes asked for, %u let through, %u calls left for later, %u not paced",
                  g_w.asked, g_w.through, g_w.later, g_w.unpaced);
    out.emplace_back(line);
    g_w = Window{};
    return out;
}

}  // namespace game
