// Diablo II 1.14d's own memory budgets, adjusted for the 3DS, and the way it
// ends when it runs out (d2_memory.cpp). Install after linking, before the
// game runs (rt::RunOptions::install_native).
#ifndef D2_GAME_D2_MEMORY_H
#define D2_GAME_D2_MEMORY_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

// The 3DS app's floor: 8 MiB less than the game's, for the New 3DS's heap
// and VirtualAlloc pool (issue #15).
constexpr uint32_t kCtrSpriteCacheFloor = 16u << 20;

// Sets the sprite cache's floor to `bytes` (the game's own is 24 MiB). With
// the 64 MiB of RAM the 3DS app reports, the game sizes its cache at that
// floor. Returns false, changing nothing, on a different Game.exe.
bool set_sprite_cache_floor(d2rt::Cpu &c, uint32_t bytes);

// The sprite cache as the game set it up (all 0 before that).
struct SpriteCacheStats {
    uint32_t used, limit, sprites;   // bytes of decoded frames, its size, entries
};
SpriteCacheStats sprite_cache_stats(d2rt::Cpu &c);

// Holds the guest pool's free space near `reserve` by lowering the sprite
// cache's limit by what the pool is short of it, down to `min_limit` and
// never above the size the game gave the cache (it re-creates the cache
// between the menus and a game). The cache evicts down to a lowered limit
// as it takes new sprites, and the memory it lets go of stays in the game's
// pool system for its other data, which then stops taking more of the
// guest pool. Call it periodically with the pool's free bytes. Logs each
// change; returns the limit in force.
constexpr uint32_t kCtrPoolReserve = 10u << 20;
constexpr uint32_t kCtrSpriteCacheMin = 4u << 20;
uint32_t govern_sprite_cache(d2rt::Cpu &c, uint32_t pool_free, uint32_t reserve, uint32_t min_limit);

// Makes the game's fatal exit, exit(-1) after an internal error (an
// allocation that failed, a pool that overflowed, an assertion), end the
// run at once, as the process's end would on Windows, instead of running
// the C runtime's teardown while the other guest threads keep running. Any
// other exit code takes the original path. Returns false, changing nothing,
// on a different Game.exe.
bool install_fatal_exit(d2rt::Cpu &c);

// Where the game called exit(-1) from (its return address), 0 if it did not.
uint32_t fatal_exit_caller();

}  // namespace game

#endif
