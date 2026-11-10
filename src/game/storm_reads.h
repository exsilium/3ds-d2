// Storm's reads from its archives, ordered for the drawing thread (issue #31).
// Install after linking, before the game runs (rt::RunOptions::install_native).
#ifndef D2_GAME_STORM_READS_H
#define D2_GAME_STORM_READS_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

// Storm's read thread serves a waiting caller's read (a ReadFile without
// OVERLAPPED, as the drawing thread's) before the asynchronous loads queued
// ahead of it, instead of after them (storm_reads.cpp). Returns false,
// changing nothing, on a different Game.exe.
bool install_storm_read_priority(d2rt::Cpu &c);

// A caller's read without OVERLAPPED (the drawing thread's: a sound, a
// sprite's file) is read on its own thread under Storm's lock, as on an
// archive opened for synchronous reads, whenever the read thread is not
// itself reading, instead of queued behind what the read thread does next:
// its completion callbacks decode whole sprite groups in guest code, tens of
// ms each (issue #76). False, changing nothing, on a different Game.exe.
bool install_storm_direct_reads(d2rt::Cpu &c);
// Reads taken that way, and those queued because the read thread was reading.
void storm_direct_counts(uint32_t &direct, uint32_t &queued);

// The read thread's rate limit, in bytes a second: the game sets 256 KiB/s
// (0x40a3b8, through Storm's 0x4164a0), and the read thread then waits
// between the requests it picks by position (not those a caller waits for)
// so that its reads keep under it. A card that reads ~10 MB/s had the
// loads of a new area's sprites arrive late, the game then waiting for them
// (0x40a9bc). Before the game starts (install_native). False, changing
// nothing, on a different Game.exe.
bool set_storm_read_rate(d2rt::Cpu &c, uint32_t bytes_per_s);

}  // namespace game

#endif
