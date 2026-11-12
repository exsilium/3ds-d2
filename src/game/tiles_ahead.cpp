// See tiles_ahead.h.
#include "game/tiles_ahead.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
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

// A DT1 in its archive, and its sector table once the worker has read it.
struct SectorTable {
    bool tried = false, ok = false;   // the worker's alone
    std::vector<uint32_t> offsets;
};
struct Dt1 {
    int pf = -1;   // rt::prefetch_file's id for its archive
    uint32_t block = 0, csize = 0, fsize = 0, flags = 0;
    std::shared_ptr<SectorTable> table;
};
std::unordered_map<std::string, Dt1> g_dt1;   // by name in upper case
std::unordered_set<uint32_t> g_sent;          // tiles sent to the worker
bool g_prefetch = false;
uint32_t g_sent_window = 0, g_unknown_window = 0;
uint64_t g_pf_mark_hits = 0, g_pf_mark_reads = 0, g_pf_mark_bytes = 0, g_pf_mark_us = 0;

constexpr uint32_t kSector = 4096;   // the archives' (512 << 3)
constexpr uint32_t kExists = 0x80000000, kCompressed = 0x00000200, kImplode = 0x00000100, kEncrypted = 0x00010000,
                   kSingle = 0x01000000, kCrc = 0x04000000;
constexpr uint32_t kSendMax = 48;   // tiles a scan sends at the most

std::string upper(const std::string &s)
{
    std::string u(s);
    for (char &ch : u)
        ch = (char)std::toupper((unsigned char)ch);
    return u;
}

// The worker's: the tile's sectors into the store.
void prefetch_tile(const Dt1 &d, uint32_t offset, uint32_t bytes)
{
    SectorTable &t = *d.table;
    if (!(d.flags & (kCompressed | kImplode))) {   // stored: the bytes as they are
        rt::prefetch_range(d.pf, d.block + offset, bytes);
        return;
    }
    if (!t.tried) {
        t.tried = true;
        const uint32_t n = (d.fsize + kSector - 1) / kSector + 1 + ((d.flags & kCrc) ? 1 : 0);
        t.offsets.resize(n);
        t.ok = rt::prefetch_raw(d.pf, d.block, t.offsets.data(), n * 4);
        if (t.ok)   // Storm reads it again before each read: keep it in the store too
            rt::prefetch_range(d.pf, d.block, n * 4);
    }
    const uint32_t s0 = offset / kSector, s1 = (offset + bytes - 1) / kSector;
    if (!t.ok || s1 + 1 >= t.offsets.size() || t.offsets[s1 + 1] <= t.offsets[s0])
        return;
    rt::prefetch_range(d.pf, d.block, (uint32_t)t.offsets.size() * 4);   // the table again, if it has gone since
    rt::prefetch_range(d.pf, d.block + t.offsets[s0], t.offsets[s1 + 1] - t.offsets[s0]);
}

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
    // Nearest first: the home room, then by the distance of their centres
    // (Room1 +0x4c: sub-tile x, y, width, height), so that a prefetch with
    // a budget takes the tiles drawn soonest.
    auto centre = [&](uint32_t r, int64_t &x, int64_t &y) {
        x = (int32_t)c.read_u32(r + 0x4c) + (int32_t)c.read_u32(r + 0x54) / 2;
        y = (int32_t)c.read_u32(r + 0x50) + (int32_t)c.read_u32(r + 0x58) / 2;
    };
    int64_t hx, hy;
    centre(home, hx, hy);
    auto dist = [&](uint32_t r) {
        if (r == home)
            return (int64_t)-1;
        int64_t x, y;
        centre(r, x, y);
        return (x - hx) * (x - hx) + (y - hy) * (y - hy);
    };
    std::sort(ring1.begin(), ring1.end(), [&](uint32_t a, uint32_t b) { return dist(a) < dist(b); });
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

void set_tile_prefetch(bool on) { g_prefetch = on; }

void tiles_ahead_note_file(d2rt::Cpu &c, const char *name, uint32_t file)
{
    std::string key = upper(name);
    if (g_dt1.count(key))
        return;
    const uint32_t arc = c.read_u32(file + 0x114), blk = c.read_u32(file + 0x118);
    if (!arc || !blk)
        return;
    Dt1 d;
    d.pf = rt::prefetch_file(rt::read_cstr(c, arc + 8, 260).c_str());
    d.block = c.read_u32(blk);
    d.csize = c.read_u32(blk + 4);
    d.fsize = c.read_u32(blk + 8);
    d.flags = c.read_u32(blk + 12);
    d.table = std::make_shared<SectorTable>();
    if (d.pf < 0 || !(d.flags & kExists) || (d.flags & (kEncrypted | kSingle)))
        d.pf = -1;
    g_dt1.emplace(std::move(key), std::move(d));
}

void tiles_ahead_scan(d2rt::Cpu &c)
{
    if (++g_presents % 4)
        return;
    const uint64_t now = rt::now_us();
    const std::vector<TileAhead> t = tiles_ahead(c);
    g_w.scans++;
    if (g_prefetch) {
        uint32_t sent = 0;
        for (const TileAhead &a : t) {
            if (a.ring != 1 || sent >= kSendMax || g_sent.count(a.tile))
                continue;
            auto it = g_dt1.find(upper(a.dt1));
            if (it == g_dt1.end() || it->second.pf < 0) {
                g_unknown_window++;
                continue;
            }
            const Dt1 d = it->second;
            const uint32_t offset = a.offset, bytes = a.bytes;
            if (!rt::io_background([d, offset, bytes] { prefetch_tile(d, offset, bytes); }))
                break;
            g_sent.insert(a.tile);
            sent++;
        }
        g_sent_window += sent;
        if (g_sent.size() > 4096)
            g_sent.clear();
    }
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
    if (g_seen.size() > 4096)   // tiles of rooms passed by and never drawn: the host heap is tight
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
    char line[400];
    const rt::FileStats &f = rt::file_stats();
    std::snprintf(line, sizeof line,
                  "[rt] tile prefetch: %s; %u tiles sent to the worker (%u of DT1s not known yet); it read %u ranges, "
                  "%u KiB in %u ms; %u reads served from it; %u KiB held",
                  g_prefetch ? "on" : "off", g_sent_window, g_unknown_window,
                  (unsigned)(f.prefetch_reads - g_pf_mark_reads), (unsigned)((f.prefetch_bytes - g_pf_mark_bytes) >> 10),
                  (unsigned)((f.prefetch_us - g_pf_mark_us) / 1000), (unsigned)(f.prefetch_hits - g_pf_mark_hits),
                  (unsigned)(f.prefetch_held >> 10));
    out.emplace_back(line);
    g_sent_window = g_unknown_window = 0;
    g_pf_mark_reads = f.prefetch_reads;
    g_pf_mark_bytes = f.prefetch_bytes;
    g_pf_mark_hits = f.prefetch_hits;
    g_pf_mark_us = f.prefetch_us;
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
