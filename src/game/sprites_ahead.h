// The rest of a sprite's file read ahead, after its first read (issue #153).
//
// A DCC sprite is read a direction at a time: the game thread's fetch of a
// direction not yet loaded (0x60048d, through 0x5fff1f) waits for one Storm
// read of that direction's sectors. A missile's or a monster's directions
// are met one after another, so a file costs a read on the game thread per
// direction: on the console 22 reads for one missile and about 9 ms each
// (3DS-D2-1J, build 2bf5f286f381-aotb38ca6, 101 such reads of missiles and
// 880 ms in 240 s).
//
// At the first read on the game thread of a missile's, an overlay's or an
// object's sprite file, its whole extent in
// the archive goes to the I/O worker, which reads it into the runtime's
// prefetch store (rt::prefetch_range, the tile prefetch's, issue #89) when
// it has no read waiting. The reads of its other directions are then
// served from the store. Nothing of the game's is changed: the file's
// place comes from Storm's file object as frame_reads sees it (+0x114 the
// archive, +0x118 its block entry), as tiles_ahead does for a DT1.
#ifndef D2_GAME_SPRITES_AHEAD_H
#define D2_GAME_SPRITES_AHEAD_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// Off until set: it needs the runtime's prefetch store.
void set_sprites_ahead(bool on);
// From frame_reads, at a read of a .dcc on the game thread: `file` Storm's
// file object. 1: the file's first read (sent now, or would be); 2: a read
// of a file first read at least 100 ms ago, which the store may serve; 0:
// between the two. Off, it sends nothing and answers the same, to compare.
int sprites_ahead_read(d2rt::Cpu &c, const char *name, uint32_t file);
// The heartbeat's line (none when no file was sent in the window).
// The game thread's read of a sprite's file (any class's): its extent in
// the archive named to the sprites' store (rt::keep_region), which keeps
// what the card gives of it for the next read of the same bytes. Nothing
// without a store (rt::set_sprite_keep).
void sprites_keep_read(d2rt::Cpu &c, uint32_t file);
// The heartbeat's lines: the read-ahead's and the store's.
std::vector<std::string> sprites_ahead_lines();

}  // namespace game

#endif
