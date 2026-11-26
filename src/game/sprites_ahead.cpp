// See sprites_ahead.h.
#include "game/sprites_ahead.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace game {

namespace {

constexpr uint32_t kSector = 4096;   // the archives' (512 << 3)
constexpr uint32_t kExists = 0x80000000, kCompressed = 0x00000200, kImplode = 0x00000100, kEncrypted = 0x00010000,
                   kSingle = 0x01000000, kCrc = 0x04000000;
constexpr uint32_t kFileMax = 384u << 10;   // a file larger than this is left to its own reads
constexpr uint32_t kPiece = 48u << 10;      // a read of the worker's, at the most: a read waits behind it
constexpr uint64_t kAgain = 30 * 1000 * 1000;   // the store is a ring: a file read after this long is sent again

bool g_on = false;
std::unordered_map<uint64_t, uint64_t> g_sent;   // by the name's hash: when
struct Window {
    uint32_t files = 0, bytes = 0, skipped = 0;
};
Window g_w;
std::atomic<uint32_t> g_pieces{0}, g_piece_bytes{0};   // the worker's

uint64_t hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)(*s | 0x20)) * 0x100000001b3ull;
    return h;
}

// The worker's: the file's extent into the store, in pieces that end at a
// sector's end where the file has a sector table.
void ahead(int pf, uint32_t block, uint32_t csize, uint32_t fsize, uint32_t flags)
{
    auto piece = [&](uint32_t from, uint32_t to) {
        if (to > from && rt::prefetch_range(pf, block + from, to - from)) {
            g_pieces++;
            g_piece_bytes += to - from;
        }
    };
    if (!(flags & (kCompressed | kImplode))) {
        for (uint32_t at = 0; at < csize; at += kPiece)
            piece(at, at + kPiece < csize ? at + kPiece : csize);
        return;
    }
    const uint32_t n = (fsize + kSector - 1) / kSector + 1 + ((flags & kCrc) ? 1 : 0);
    std::vector<uint32_t> offsets(n);
    if (!rt::prefetch_raw(pf, block, offsets.data(), n * 4))
        return;
    for (uint32_t i = 0; i < n; i++)
        if (offsets[i] > csize || (i && offsets[i] < offsets[i - 1]))
            return;
    piece(0, n * 4);   // Storm reads the table again before each read
    uint32_t from = 1;
    for (uint32_t i = 2; i <= n; i++)
        if (i == n || offsets[i] - offsets[from - 1] > kPiece) {
            piece(offsets[from - 1], offsets[i - 1]);
            from = i;
        }
}

void send(d2rt::Cpu &c, uint32_t file)
{
    const uint32_t arc = c.read_u32(file + 0x114), blk = c.read_u32(file + 0x118);
    if (!arc || !blk)
        return;
    const int pf = rt::prefetch_file(rt::read_cstr(c, arc + 8, 260).c_str());
    const uint32_t block = c.read_u32(blk), csize = c.read_u32(blk + 4), fsize = c.read_u32(blk + 8),
                   flags = c.read_u32(blk + 12);
    if (pf < 0 || !(flags & kExists) || (flags & (kEncrypted | kSingle)) || !csize || csize > kFileMax ||
        csize <= 2 * kSector) {   // one read or two as it is
        g_w.skipped++;
        return;
    }
    if (!rt::io_background([pf, block, csize, fsize, flags] { ahead(pf, block, csize, fsize, flags); }))
        return;
    g_w.files++;
    g_w.bytes += csize;
}

}  // namespace

void set_sprites_ahead(bool on) { g_on = on; }

int sprites_ahead_read(d2rt::Cpu &c, const char *name, uint32_t file)
{
    if (!file)
        return 0;
    const uint64_t now = rt::now_us(), key = hash(name);
    auto it = g_sent.find(key);
    if (it != g_sent.end() && now - it->second < kAgain)
        return now - it->second >= 100 * 1000 ? 2 : 0;
    if (g_sent.size() > 2048)   // the host heap is tight
        g_sent.clear();
    g_sent[key] = now;
    if (g_on)
        send(c, file);
    return 1;
}

void sprites_keep_read(d2rt::Cpu &c, uint32_t file)
{
    if (!file)
        return;
    const uint32_t arc = c.read_u32(file + 0x114), blk = c.read_u32(file + 0x118);
    if (!arc || !blk)
        return;
    const uint32_t block = c.read_u32(blk), csize = c.read_u32(blk + 4), flags = c.read_u32(blk + 12);
    if (!(flags & kExists) || !csize || csize > (4u << 20))
        return;
    rt::keep_region(rt::prefetch_file(rt::read_cstr(c, arc + 8, 260).c_str()), block, csize);
}

std::vector<std::string> sprites_ahead_lines()
{
    std::vector<std::string> out;
    {
        const rt::KeepStats k = rt::keep_stats();
        if (k.hits || k.puts) {
            char line[200];
            std::snprintf(line, sizeof line,
                          "[rt] sprites kept: %u reads served from the store (%u KiB), %u put in it (%u KiB); it holds %u of %u KiB",
                          k.hits, k.hit_bytes >> 10, k.puts, k.put_bytes >> 10, k.held >> 10, k.size >> 10);
            out.emplace_back(line);
        }
    }
    const uint32_t pieces = g_pieces.exchange(0), bytes = g_piece_bytes.exchange(0);
    if (!g_w.files && !g_w.skipped && !pieces)
        return out;
    char line[200];
    std::snprintf(line, sizeof line,
                  "[rt] sprites ahead: %u files sent (%u KiB), %u not sent; the worker read %u pieces, %u KiB",
                  g_w.files, g_w.bytes >> 10, g_w.skipped, pieces, bytes >> 10);
    out.emplace_back(line);
    g_w = Window{};
    return out;
}

}  // namespace game
