// See d2_units.h.
#include "game/d2_units.h"

#include "runtime/rt_host.h"

#include <cmath>
#include <cstdlib>
#include <strings.h>

namespace game {
namespace {

constexpr uint32_t kPlayerUnitVa = 0x007A6A70;
constexpr uint32_t kUnitTablesVa = 0x007A5E70;
constexpr uint32_t kBuckets = 128;
constexpr uint32_t kMonster = 1, kObject = 2;
constexpr uint32_t kListNext = 0xE4;
constexpr uint32_t kPath = 0x2C;
// The player's body on the screen (the view's centre, above the feet), and
// how far above its feet a unit's body is.
constexpr int kCentreX = 320, kCentreY = 197;
constexpr int kNear = 40;   // subtiles: the units logged

// Names for the NPCs a script talks to, as the lane showed them (a class
// not here is given by its number).
const struct { const char *name; uint32_t cls; } kNpcs[] = {
    {"warriv", 155},   // Act I, the Rogue Encampment
};

uint32_t rd32(uint32_t va)
{
    uint32_t v = 0;
    rt::guest_read(va, &v, 4);
    return v;
}

// A unit's place in subtiles, with the fraction. An object (types 2, 4, 5)
// has a static path: +0x0C x, +0x10 y (0x620b11).
bool place(uint32_t unit, double &x, double &y)
{
    const uint32_t path = rd32(unit + kPath);
    if (!path)
        return false;
    const uint32_t type = rd32(unit);
    if (type == 2 || type == 4 || type == 5) {
        x = (int32_t)rd32(path + 0x0C);
        y = (int32_t)rd32(path + 0x10);
        return true;
    }
    uint16_t p[4] = {};
    if (!rt::guest_read(path, p, sizeof p))
        return false;
    x = p[1] + p[0] / 65536.0;
    y = p[3] + p[2] / 65536.0;
    return true;
}

// Each unit of a type (1 monsters and NPCs, 2 objects) in the client's
// table, with its offset from the player.
template <class F> bool each_unit(uint32_t type, F f)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    if (!player || !place(player, px, py))
        return false;
    for (uint32_t b = 0; b < kBuckets; b++) {
        uint32_t u = rd32(kUnitTablesVa + (type * kBuckets + b) * 4);
        for (int n = 0; u && n < 64; n++, u = rd32(u + kListNext)) {
            double x, y;
            if (rd32(u) != type || !place(u, x, y))
                continue;
            UnitSpot s;
            s.cls = rd32(u + 0x04);
            s.id = rd32(u + 0x0C);
            s.mode = rd32(u + 0x10);
            const double dx = x - px, dy = y - py;
            s.x = kCentreX + (int)std::lround((dx - dy) * 16);
            s.y = kCentreY + (int)std::lround((dx + dy) * 8);
            s.distance = (float)std::sqrt(dx * dx + dy * dy);
            f(s);
        }
    }
    return true;
}

}  // namespace

bool find_unit(uint32_t cls, UnitSpot &out, uint32_t type)
{
    bool found = false;
    each_unit(type, [&](const UnitSpot &s) {
        if (s.cls == cls && (!found || s.distance < out.distance)) {
            out = s;
            found = true;
        }
    });
    return found;
}

int player_act()
{
    const uint32_t player = rd32(kPlayerUnitVa);
    return player ? (int)rd32(player + 0x18) + 1 : 0;
}

void npc_menu_line(const UnitSpot &npc, int line, int &x, int &y)
{
    x = npc.x - 12;
    y = npc.y - 138 + 15 * line;
}

uint32_t unit_class(const char *name)
{
    for (const auto &n : kNpcs)
        if (strcasecmp(name, n.name) == 0)
            return n.cls;
    char *end = nullptr;
    const unsigned long v = std::strtoul(name, &end, 10);
    return end != name && !*end ? (uint32_t)v : 0;
}

void log_units()
{
    int n = 0;
    const bool ok = each_unit(kMonster, [&](const UnitSpot &s) {
        if (s.distance <= kNear) {
            rt::logf("units: class %u id %u mode %u, %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode, s.distance,
                     s.x, s.y);
            n++;
        }
    });
    each_unit(kObject, [&](const UnitSpot &s) {
        if (s.distance <= kNear)
            rt::logf("units: object class %u id %u mode %u, %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode,
                     s.distance, s.x, s.y);
    });
    rt::logf(ok ? "units: %d near the player" : "units: not in a game", n);
}

}  // namespace game
