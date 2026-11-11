// The tiles the game will load soon (issue #89, step 2): the rooms near the
// player, and the tiles of theirs not yet loaded.
//
// A floor, wall or roof tile's graphics are read the first time it is drawn
// (0x5fdea0, from the renderer's tile draw), on the game thread, one Storm
// read each. The rooms the draw walks are the player's room's near list
// (Room1 +0x00, count +0x24); a room's tiles are its records (0x30 bytes,
// the tile's header at +0x18) in Room1 +0x08's lists: walls +0x00/+0x04,
// floors +0x08/+0x0c, roofs +0x10/+0x14. A tile's header (the DT1's, as the
// act's load read it) has its graphics at +0x48 (offset in the DT1) and +0x4c
// (bytes), the DT1's name at +0x58, and +0x5c set once loaded.
#ifndef D2_GAME_TILES_AHEAD_H
#define D2_GAME_TILES_AHEAD_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// A tile not yet loaded, in a room near the player.
struct TileAhead {
    uint32_t tile;          // its header (guest)
    std::string dt1;        // the DT1's name, as the game opens it
    uint32_t offset, bytes; // its graphics in the DT1
    int ring;               // 1: a room near the player's; 2: near one of those
};

// From the game thread, while no guest code runs (the present): the tiles
// not yet loaded in the rooms around the player. Empty out of a game.
std::vector<TileAhead> tiles_ahead(d2rt::Cpu &c);

// The measurement: scan (every few presents) notes what tiles_ahead finds;
// loaded (frame_reads, at a tile's read on the game thread) checks a tile
// against it. lines: the window's figures for the heartbeat.
void tiles_ahead_scan(d2rt::Cpu &c);
// The prefetch: each scan sends the ring-1 tiles not yet loaded to the I/O
// worker, which reads their sectors of the archive into the runtime's
// prefetch store (rt::prefetch_blocks). The DT1s' places in their archives
// come from Storm's file objects as the game reads them (the act's load
// reads every DT1 of its level types): frame_reads calls note_file on each
// DT1 read with the object (+0x08 name, +0x114 archive, name at +0x08;
// +0x118 the file's block entry: offset, compressed and full sizes, flags).
void set_tile_prefetch(bool on);
void tiles_ahead_note_file(d2rt::Cpu &c, const char *name, uint32_t file);
void tiles_ahead_loaded(d2rt::Cpu &c, uint32_t buffer);   // the DT1 read's buffer
std::vector<std::string> tiles_ahead_lines();

}  // namespace game

#endif
