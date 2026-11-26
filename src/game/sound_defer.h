// Sound effects read off the game thread (issue #153).
//
// A sound the game plays for the first time is read whole from its archive
// by the thread that plays it: 0x4e01b0 (a list entry's start) calls
// 0x482970 (the sound's load) with "now" unless the sound's record has
// sounds.txt's Defer Inst (the byte at +0x4d of its 0x8e bytes), and then
// 0x482970 queues the file with the game's asynchronous loader (0x40a620),
// 0x4ba020 (the list's tick) keeps the entry while the record is loading
// (+0x86 is 1) and the sound starts at the first tick after 0x482b40 (the
// client's sound tick) finds the file read. On the console the read is the
// frame's wait: 460 reads, 10.3 MiB and 1.6 s of the frame thread's 8.8 s
// on reads in 420 s of play (3DS-D2-1J).
//
// Every record that isn't a stream (+0x4f) gets Defer Inst when the table
// is loaded, by a native at 0x482173 in 0x481950 (the tables' load), in
// place of its "mov eax, [0x7bc9a4]" (the record count, which it returns).
// A sound's first play then starts when its file is in, some frames late.
//
// How late: the game finds a load finished in 0x482b40's scan of the
// records, which it runs every 25 of its ticks (over a second at the
// console's pace), and that was the lateness heard in a fight (3DS-D2-1J,
// build 1b5513de1b40-aot55b1de: the loader's reads waited 7 ms each). A
// native at 0x482b64 has the scan run every tick while sounds are deferred.
// Two more, where 0x482970 counts a load in (0x482b1d) and 0x481720 counts
// it out (0x481772), time each load for the heartbeat's line.
#ifndef D2_GAME_SOUND_DEFER_H
#define D2_GAME_SOUND_DEFER_H

#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// After linking. `on` false: the site alone, the records as sounds.txt has
// them. False, changing nothing, on a different Game.exe.
bool install_sound_defer(d2rt::Cpu &c, bool on = true);

// The heartbeat's line: the loads that came in since the last call, and how
// long from asked to the record's sound. None with nothing asked.
std::vector<std::string> sound_defer_lines();

}  // namespace game

#endif
