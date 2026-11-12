// The game thread's file reads, by file (issue #89): which of the files it
// reads itself it waits on, to tell which of the game's tables an area's
// prefetch must read.
//
// A hook at the entry of Storm's SFileReadFile (0x41aad0) notes, on the
// game thread, the file each read is of (Storm keeps the name at +8 of its
// file object). The game thread's reads are synchronous, so its blocked time
// (rt::frame_thread_us: critsec, event, fileio, multi) from one read's entry
// to the next is charged to the earlier read.
#ifndef D2_GAME_FRAME_READS_H
#define D2_GAME_FRAME_READS_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// After linking. False, changing nothing, on a different Game.exe.
bool install_frame_reads(d2rt::Cpu &c);

// The window's reads since the last call, as heartbeat lines: one by class,
// one with the files waited on most. Empty when there were none.
std::vector<std::string> frame_reads_lines();

}  // namespace game

#endif
