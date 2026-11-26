// A DCC file's header kept, on the card too (issue #153).
//
// 0x5fe990 (a sprite's file record, by name) fills a record the first time
// a file is asked for: for a DCC file 0x60bf20 (ecx the name, edx the
// record) opens it, reads its 15 bytes and its directions' offsets, and
// takes its size. The record is then in a table that lets its oldest go.
// The fetch (0x6001f0) and the asynchronous request (0x6004e0) both start
// there, so a monster whose sprites are asked of the loader a mode at a time
// still has each file's header read by the frame thread: 209 reads of a few
// bytes and 1.6 s of the 2.4 s it waited on monsters' files in 200 s of the
// Cold Plains in Azahar.
//
// What 0x60bf20 leaves in the record depends on the file alone. A native in
// place of the call at 0x5fea79 keeps it by the file's name, fills a record
// from what it has kept, and has the game's function read what it hasn't.
// The kept ones are written to a file on the card and read at the start, so
// a file's header is read from its archive once, not once a session.
#ifndef D2_GAME_SPRITE_HEADERS_H
#define D2_GAME_SPRITE_HEADERS_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// After linking. `path`: the file they are kept in (null: this run's only);
// `signature`: what the archives are, so that another copy's are not used.
// `on` false: the site alone, every header read. False, changing nothing, on
// a different Game.exe.
bool install_sprite_headers(d2rt::Cpu &c, const char *path, uint64_t signature, bool on = true);

// The heartbeat's line, since the last call; and the new ones written.
std::vector<std::string> sprite_headers_lines();

}  // namespace game

#endif
