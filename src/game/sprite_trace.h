// A trace of the sprite cache's traffic (issue #18), for replaying the
// cache offline under other limits and governors (tools/sprite_trace.py)
// instead of playing a two-hour session for each change. Off unless asked
// for: the INFO tab's "trace sprites" button on the console (the session's
// logs/sprites.trc, sent with its report), M4_SPRITE_TRACE in the m4 lane.
//
// Two stubs on the cache's paths, behind one flag byte the guest tests
// (sprite_trace.cpp): every sprite the game finds in the cache to draw, and
// every sprite the cache lets go of. While the trace is off they cost a
// compare and a jump.
//
// The file: a 32-byte header, then 16-byte records, little-endian.
//   header  "D2SPTRC1", u32 version (3), u32 record size (16),
//           u32 use window (ms), u32 start (Unix time), u32 0, u32 0
//   record  u32 type << 28 | ms since the start, u32 a, u32 b, u32 c
// The types (kUse... below):
//   use     a sprite found to draw: a key, b its bytes now, with bit 31
//           set if the game moved it to the recently used end of the
//           cache's list (it does not for every caller), c its cache entry.
//           A sprite's first use in each window, its first moved one, and
//           any use after it grew (its frames are decoded into it as they
//           are first drawn).
//   remove  a sprite let go of: a key, b its bytes, c the caller's return
//           address (which of the game's paths: an eviction for room, or
//           the cache emptied, or a sprite whose decode failed).
//   cache   each heartbeat: a the limit, b bytes in use, c sprites.
//   pool    each heartbeat: a pool free, b the spare used (0/1), c the
//           pool's largest free piece.
//   count   each heartbeat: a sprites taken since the start, b cells
//           decoded since the start (low 32 bits), c frames presented.
//   lost    a records not written (the file could not take them).
//   ident   (version 2) before a sprite's first use record (and again if
//           it was not seen for a while): the record's words +4 to +0x20,
//           two at a time in b and c of five records (A: +4 +8, B: +0xc
//           +0x10, C: +0x14 +0x18, D: +0x1c +0x20, E: the first word +0x20
//           points at), all with a = the key.
// The key is a hash of what the game's own sprite table (0x88db58) looks
// sprites up by, its record's +4 to +0x1c, so a sprite evicted and decoded
// again keeps its key (version 3). Version 2 hashed +0x20 as well: that is
// the sprite's own block in the cache, new each time it is decoded, so
// there a sprite decoded again had a new key, and differed from its earlier
// self only in +0x20 (issue #31; tools/sprite_trace.py keys such traces by
// their ident records).
#ifndef D2_GAME_SPRITE_TRACE_H
#define D2_GAME_SPRITE_TRACE_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

enum SpriteTraceType : uint32_t {
    kUse = 1, kRemove = 2, kCache = 3, kPool = 4, kCount = 5, kLost = 6,
    kIdentA = 7, kIdentB = 8, kIdentC = 9, kIdentD = 10, kIdentE = 11,
};

// The stubs, off. Returns false, changing nothing, on a different Game.exe.
bool install_sprite_trace(d2rt::Cpu &c);

// Starts writing to `path`: a new file the first time, the same one
// appended to after a stop (one trace per run, on one clock). False if the
// stubs are not installed, or the file or its buffers could not be had.
bool sprite_trace_start(const char *path);
void sprite_trace_stop();
bool sprite_trace_on();

// Each heartbeat while on: the cache, the pool and the counters, then the
// file flushed.
struct SpriteTraceBeat {
    uint32_t limit, used, sprites;
    uint32_t pool_free, spare_used, largest_free;
    uint32_t inserted, cells, frames;
};
void sprite_trace_beat(const SpriteTraceBeat &b);

}  // namespace game

#endif
