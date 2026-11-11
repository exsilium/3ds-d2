// See tiles_ahead.h.
#include "game/tiles_ahead.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace game {

namespace {

constexpr uint32_t kPlayer = 0x007a6a70;   // the player's unit (0x4646a0)
constexpr uint32_t kRecord = 0x30;         // a room's tile record
constexpr uint32_t kMaxRooms = 64, kMaxNear = 64, kMaxTiles = 4096;

// What a scan saw of a tile: when first, and in which ring.
struct Seen {
    uint64_t first_us;
    int ring;
};
std::unordered_map<uint32_t, Seen> g_seen;   // tiles seen not loaded, since the level's tiles were last cleared
struct Window {
    uint32_t loads = 0, ring1 = 0, ring2 = 0, unseen = 0;
    uint64_t lead1_us = 0, lead2_us = 0;
    uint32_t scans = 0, pending1 = 0, pending2 = 0;   // the last scan's not-loaded tiles
    uint64_t pending_bytes1 = 0, pending_bytes2 = 0;
};
Window g_w;
uint32_t g_presents = 0;

uint32_t room_of_player(d2rt::Cpu &c)
{
    const uint32_t unit = c.read_u32(kPlayer);
    if (!unit || c.read_u32(unit) != 0)   // a player
        return 0;
    const uint32_t path = c.read_u32(unit + 0x2c);
    return path ? c.read_u32(path + 0x1c) : 0;
}

void near_of(d2rt::Cpu &c, uint32_t room, std::vector<uint32_t> &out)
{
    const uint32_t list = c.read_u32(room), n = c.read_u32(room + 0x24);
    if (!list || n > kMaxNear)
        return;
    for (uint32_t i = 0; i < n; i++)
        if (const uint32_t r = c.read_u32(list + 4 * i))
            out.push_back(r);
}

}  // namespace

std::vector<TileAhead> tiles_ahead(d2rt::Cpu &c)
{
    std::vector<TileAhead> out;
    const uint32_t home = room_of_player(c);
    if (!home)
        return out;
    // Ring 1: the home room's near list (it includes the home room); ring 2:
    // theirs, less ring 1.
    std::vector<uint32_t> ring1, ring2;
    near_of(c, home, ring1);
    if (std::find(ring1.begin(), ring1.end(), home) == ring1.end())
        ring1.push_back(home);
    for (const uint32_t r : ring1)
        near_of(c, r, ring2);
    std::sort(ring2.begin(), ring2.end());
    ring2.erase(std::unique(ring2.begin(), ring2.end()), ring2.end());
    ring2.erase(std::remove_if(ring2.begin(), ring2.end(),
                               [&](uint32_t r) { return std::find(ring1.begin(), ring1.end(), r) != ring1.end(); }),
                ring2.end());
    std::unordered_set<uint32_t> done;
    auto room_tiles = [&](uint32_t room, int ring) {
        const uint32_t tiles = c.read_u32(room + 0x08);
        if (!tiles)
            return;
        for (int kind = 0; kind < 3; kind++) {   // walls, floors, roofs
            const uint32_t recs = c.read_u32(tiles + 8 * kind), n = c.read_u32(tiles + 8 * kind + 4);
            if (!recs || n > kMaxTiles)
                continue;
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t tile = c.read_u32(recs + i * kRecord + 0x18);
                if (!tile || c.read_u32(tile + 0x5c) || !done.insert(tile).second)
                    continue;
                const uint32_t bytes = c.read_u32(tile + 0x4c);
                if (!bytes || !c.read_u32(tile + 0x50))
                    continue;
                TileAhead t;
                t.tile = tile;
                t.offset = c.read_u32(tile + 0x48);
                t.bytes = bytes;
                t.ring = ring;
                t.dt1 = rt::read_cstr(c, c.read_u32(tile + 0x58), 260);
                out.push_back(std::move(t));
            }
        }
    };
    for (size_t i = 0; i < ring1.size() && i < kMaxRooms; i++)
        room_tiles(ring1[i], 1);
    for (size_t i = 0; i < ring2.size() && i < kMaxRooms; i++)
        room_tiles(ring2[i], 2);
    return out;
}

void tiles_ahead_scan(d2rt::Cpu &c)
{
    if (++g_presents % 4)
        return;
    const uint64_t now = rt::now_us();
    const std::vector<TileAhead> t = tiles_ahead(c);
    g_w.scans++;
    g_w.pending1 = g_w.pending2 = 0;
    g_w.pending_bytes1 = g_w.pending_bytes2 = 0;
    for (const TileAhead &a : t) {
        auto it = g_seen.find(a.tile);
        if (it == g_seen.end())
            g_seen.emplace(a.tile, Seen{now, a.ring});
        else if (a.ring < it->second.ring)
            it->second.ring = a.ring;
        (a.ring == 1 ? g_w.pending1 : g_w.pending2)++;
        (a.ring == 1 ? g_w.pending_bytes1 : g_w.pending_bytes2) += a.bytes;
    }
    if (g_seen.size() > 65536)
        g_seen.clear();
}

void tiles_ahead_loaded(d2rt::Cpu &c, uint32_t buffer)
{
    // The tile whose graphics buffer this is (+0x54, set just before).
    auto it = g_seen.begin();
    for (; it != g_seen.end(); ++it)
        if (c.read_u32(it->first + 0x54) == buffer)
            break;
    g_w.loads++;
    if (it == g_seen.end()) {
        g_w.unseen++;
        return;
    }
    const uint64_t lead = rt::now_us() - it->second.first_us;
    if (it->second.ring == 1) {
        g_w.ring1++;
        g_w.lead1_us += lead;
    } else {
        g_w.ring2++;
        g_w.lead2_us += lead;
    }
    g_seen.erase(it);
}

std::vector<std::string> tiles_ahead_lines()
{
    std::vector<std::string> out;
    if (!g_w.scans && !g_w.loads)
        return out;
    char line[256];
    std::snprintf(line, sizeof line,
                  "[rt] tiles ahead: %u loads, seen before in ring 1 %u (%u ms ahead on average), ring 2 %u (%u ms), "
                  "not seen %u; the last scan's not loaded: ring 1 %u (%u KiB), ring 2 %u (%u KiB)",
                  g_w.loads, g_w.ring1, g_w.ring1 ? (unsigned)(g_w.lead1_us / g_w.ring1 / 1000) : 0, g_w.ring2,
                  g_w.ring2 ? (unsigned)(g_w.lead2_us / g_w.ring2 / 1000) : 0, g_w.unseen, g_w.pending1,
                  (unsigned)(g_w.pending_bytes1 >> 10), g_w.pending2, (unsigned)(g_w.pending_bytes2 >> 10));
    out.emplace_back(line);
    g_w = Window{};
    return out;
}

}  // namespace game
