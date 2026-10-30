// Storm's reads from its archives, ordered for the drawing thread (issue #31).
// Install after linking, before the game runs (rt::RunOptions::install_native).
#ifndef D2_GAME_STORM_READS_H
#define D2_GAME_STORM_READS_H

namespace d2rt { struct Cpu; }

namespace game {

// Storm's read thread serves a waiting caller's read (a ReadFile without
// OVERLAPPED, as the drawing thread's) before the asynchronous loads queued
// ahead of it, instead of after them (storm_reads.cpp). Returns false,
// changing nothing, on a different Game.exe.
bool install_storm_read_priority(d2rt::Cpu &c);

}  // namespace game

#endif
