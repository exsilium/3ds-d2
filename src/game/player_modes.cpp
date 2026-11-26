// See player_modes.h.
#include "game/player_modes.h"

#include "game/native_sites.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstdio>

namespace game {

namespace {

constexpr uint32_t kWalk = 0x00470070;     // the walk of the units near the player (eax its argument)
constexpr uint32_t kAskMode = 0x0046fb60;  // eax the unit, ebx the mode; (background)
constexpr uint32_t kPlayer = 0x007a6a70;   // the client's player unit
constexpr uint64_t kEvery = 300 * 1000;
// The game's own eight first (0x712aa0): hit, the first attack, run,
// walk, neutral, cast, town neutral and walk. Then the others: the second
// attack, block, kick, throw, the first skill, death. Under Box86 one of
// those others never came back loaded and the game stopped on an
// assertion soon after (the x86 CIA in Azahar, 6 runs of 7: 0x46e5a0,
// 0x6018c0, 0x6018f0, 0x647540), where the compiled code had them loaded
// and ran on; why is not known. So the others are for the AOT build.
constexpr uint32_t kModes[] = {4, 7, 3, 2, 1, 10, 5, 6, 8, 9, 12, 11, 13, 0};
constexpr unsigned kOwn = 8, kAll = sizeof kModes / sizeof kModes[0];
unsigned g_count = kOwn;

bool g_on = true;
unsigned g_next = 0;
uint64_t g_last_us = 0;
struct Window {
    uint32_t asked = 0, loaded = 0, loading = 0, none = 0;
    uint64_t us = 0;
};
Window g_w;

uint32_t on_walk(d2rt::Cpu &c)
{
    const uint32_t eax = c.reg(d2rt::R_EAX), ebx = c.reg(d2rt::R_EBX), ecx = c.reg(d2rt::R_ECX),
                   edx = c.reg(d2rt::R_EDX), esi = c.reg(d2rt::R_ESI), edi = c.reg(d2rt::R_EDI);
    rt::guest_call(c, kWalk, {});
    const uint64_t now = rt::now_us();
    const uint32_t player = c.read_u32(kPlayer);
    if (g_on && player && now - g_last_us >= kEvery) {
        g_last_us = now;
        const uint32_t mode = kModes[g_next++ % g_count];
        c.set_reg(d2rt::R_EAX, player);
        c.set_reg(d2rt::R_EBX, mode);
        const uint32_t r = rt::guest_call(c, kAskMode, {1});
        g_w.asked++;
        g_w.loaded += r == 1;
        g_w.loading += r == 0;
        g_w.none += r != 0 && r != 1;
        g_w.us += rt::now_us() - now;
    }
    c.set_reg(d2rt::R_EBX, ebx);
    c.set_reg(d2rt::R_ECX, ecx);
    c.set_reg(d2rt::R_EDX, edx);
    c.set_reg(d2rt::R_ESI, esi);
    c.set_reg(d2rt::R_EDI, edi);
    return eax;
}

}  // namespace

bool install_player_modes(d2rt::Cpu &c, int level)
{
    g_on = level > 0;
    g_count = level > 1 ? kAll : kOwn;
    const uint32_t native = rt::native_entry("PlayerModes", 0, on_walk);
    if (!native || !apply_sites(c, "player_modes", {{"PlayerModes", native}}))
        return false;
    if (g_on)
        rt::logf("player modes: one of the player's %u modes asked for every 300 ms, in the background", g_count);
    else
        rt::logf("player modes: off, the game's eight modes asked for once");
    return true;
}

std::vector<std::string> player_modes_lines()
{
    std::vector<std::string> out;
    if (!g_w.asked)
        return out;
    char line[200];
    std::snprintf(line, sizeof line,
                  "[rt] player modes: %u asked for in %u ms, %u loaded, %u had sprites to load, %u not there",
                  g_w.asked, (unsigned)(g_w.us / 1000), g_w.loaded, g_w.loading, g_w.none);
    out.emplace_back(line);
    g_w = Window{};
    return out;
}

}  // namespace game
