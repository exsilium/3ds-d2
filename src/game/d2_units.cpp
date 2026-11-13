// See d2_units.h.
#include "game/d2_units.h"

#include "runtime/rt_host.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
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
            s.unit = u;
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

// The level's preset units, loaded or not: the player's room (its path's
// +0x1c) has its level's room (+0x10), whose level (+0x58) lists them all
// (+0x10, next +0x24); a room's place in tiles (+0x34, +0x38) and its
// presets (+0x5c, next +0xc: +4 class, +8 x, +0x14 type, +0x18 y, in
// subtiles from the room's corner). f(type, class, x, y), places in subtiles.
template <class F> bool each_preset(F f)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t room = path ? rd32(path + 0x1c) : 0;
    const uint32_t room2 = room ? rd32(room + 0x10) : 0;
    const uint32_t level = room2 ? rd32(room2 + 0x58) : 0;
    if (!level)
        return false;
    int rooms = 0;
    for (uint32_t r = rd32(level + 0x10); r && rooms < 4096; r = rd32(r + 0x24), rooms++) {
        if (!rooms)
            rt::logf("units: level %08x (no. %u), first room %08x", level, rd32(level + 0x1d0), r);
        const int32_t rx = (int32_t)rd32(r + 0x34) * 5, ry = (int32_t)rd32(r + 0x38) * 5;
        int n = 0;
        for (uint32_t p = rd32(r + 0x5c); p && n < 512; p = rd32(p + 0xc), n++)
            f(rd32(p + 0x14), rd32(p + 4), rx + (int32_t)rd32(p + 8), ry + (int32_t)rd32(p + 0x18));
    }
    rt::logf("units: %d rooms in the level", rooms);
    return true;
}

std::string object_name(uint32_t unit);   // below, by log_units

bool find_object_named(const char *name, UnitSpot &out)
{
    bool found = false;
    each_unit(kObject, [&](const UnitSpot &s) {
        if (strcasecmp(object_name(s.unit).c_str(), name) == 0 && (!found || s.distance < out.distance)) {
            out = s;
            found = true;
        }
    });
    return found;
}

bool frontier_room(const std::vector<uint32_t> &skip, uint32_t &room, double &dx, double &dy, double &distance)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t r1 = path ? rd32(path + 0x1c) : 0;
    const uint32_t r2 = r1 ? rd32(r1 + 0x10) : 0;
    const uint32_t level = r2 ? rd32(r2 + 0x58) : 0;
    if (!level || !place(player, px, py))
        return false;
    bool found = false;
    int n = 0;
    for (uint32_t r = rd32(level + 0x10); r && n < 4096; r = rd32(r + 0x24), n++) {
        if (rd32(r + 0x30) || std::find(skip.begin(), skip.end(), r) != skip.end())
            continue;   // loaded (its room is there), or given up on
        const double cx = ((int32_t)rd32(r + 0x34) + (int32_t)rd32(r + 0x3c) / 2.0) * 5;
        const double cy = ((int32_t)rd32(r + 0x38) + (int32_t)rd32(r + 0x40) / 2.0) * 5;
        const double ex = cx - px, ey = cy - py, d = std::sqrt(ex * ex + ey * ey);
        if (!found || d < distance) {
            room = r;
            dx = ex;
            dy = ey;
            distance = d;
            found = true;
        }
    }
    return found;
}

bool find_preset(uint32_t cls, uint32_t type, double &dx, double &dy, double &distance)
{
    const uint32_t player = rd32(kPlayerUnitVa);
    double px, py;
    if (!player || !place(player, px, py))
        return false;
    bool found = false;
    each_preset([&](uint32_t t, uint32_t c, int32_t x, int32_t y) {
        if (t != type || c != cls)
            return;
        const double ex = x - px, ey = y - py, d = std::sqrt(ex * ex + ey * ey);
        if (!found || d < distance) {
            dx = ex;
            dy = ey;
            distance = d;
            found = true;
        }
    });
    return found;
}

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

bool holding_item()
{
    // The player's inventory (+0x60; its first dword the 0x01020304
    // signature) holds the item on the cursor at +0x20.
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t inv = player ? rd32(player + 0x60) : 0;
    return inv && rd32(inv) == 0x01020304u && rd32(inv + 0x20) != 0;
}

int player_level()
{
    const uint32_t player = rd32(kPlayerUnitVa);
    const uint32_t path = player ? rd32(player + kPath) : 0;
    const uint32_t room = path ? rd32(path + 0x1c) : 0;
    const uint32_t room2 = room ? rd32(room + 0x10) : 0;
    const uint32_t level = room2 ? rd32(room2 + 0x58) : 0;
    return level ? (int)rd32(level + 0x1d0) : 0;
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

// An object's name, from its objects.txt record (the unit's data, +0x14,
// starts with a pointer to it; the record starts with the name), or "".
std::string object_name(uint32_t unit)
{
    char name[33] = {};
    const uint32_t data = rd32(unit + 0x14), txt = data ? rd32(data) : 0;
    if (!txt || !rt::guest_read(txt, name, sizeof name - 1))
        return "";
    for (char &ch : name)
        if (ch && (ch < 0x20 || ch > 0x7e))
            return "";
    return name;
}

void log_units(bool all)
{
    int n = 0;
    const bool ok = each_unit(kMonster, [&](const UnitSpot &s) {
        if (all || s.distance <= kNear) {
            rt::logf("units: class %u id %u mode %u, %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode, s.distance,
                     s.x, s.y);
            n++;
        }
    });
    each_unit(kObject, [&](const UnitSpot &s) {
        if (all || s.distance <= kNear)
            rt::logf("units: object class %u id %u mode %u \"%s\", %.1f subtiles away, at %d,%d", s.cls, s.id, s.mode,
                     object_name(s.unit).c_str(), s.distance, s.x, s.y);
    });
    if (all) {   // and the level's preset objects (type 2), loaded or not
        double px = 0, py = 0;
        const uint32_t player = rd32(kPlayerUnitVa);
        if (player)
            place(player, px, py);
        int counts[8] = {};
        each_preset([&](uint32_t t, uint32_t c, int32_t x, int32_t y) {
            counts[t & 7]++;
            if (t == 2 || t == 1)
                rt::logf("units: preset %s class %u at %d,%d, %.1f subtiles away", t == 2 ? "object" : "monster", c,
                         x, y, std::sqrt((x - px) * (x - px) + (y - py) * (y - py)));
        });
        rt::logf("units: presets by type: %d %d %d %d %d %d", counts[0], counts[1], counts[2], counts[3], counts[4],
                 counts[5]);
    }
    rt::logf(ok ? "units: %d near the player" : "units: not in a game", n);
}

}  // namespace game
