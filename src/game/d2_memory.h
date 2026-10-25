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
// cache's limit by twice what the pool is short of it, down to `min_limit`
// and never above the size the game gave the cache (it re-creates the cache
// between the menus and a game). The cache evicts down to a lowered limit
// as it takes new sprites, and the memory it lets go of stays in the game's
// pool system for its other data, which then stops taking more of the
// guest pool. Call it periodically with the pool's free bytes. Logs each
// change; returns the limit in force.
constexpr uint32_t kCtrPoolReserve = 10u << 20;
constexpr uint32_t kCtrSpriteCacheMin = 4u << 20;
// Of the pool, kept back in one piece for a request that fits nowhere else
// (rt::RunOptions::pool_spare): the heap blocks the console was refused at
// the end of long sessions were 262 to 656 KiB.
constexpr uint32_t kCtrPoolSpare = 1u << 20;
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

// Lets the game's pool systems (the global one and those of the client's and
// the server's game) give their empty blocks back to VirtualAlloc while a
// game runs (the game only does so when one is joined or left): a stub in
// front of their allocation (0x40a080) frees them, through the game's own
// VirtualFree and free, on the first allocation after
// request_pool_release, from each pool no other thread is inside then. Logs what each release finds. Returns false, changing nothing, on a
// different Game.exe.
bool install_pool_release(d2rt::Cpu &c);

// Makes the pool systems' blocks for units of 1 KiB and more `bytes` long
// (one unit where a unit is larger) instead of 512 KiB, so that a new block
// still fits in a pool whose free space is in pieces, and blocks go empty,
// to be released, more often. Install before the game creates its pools.
// Returns false, changing nothing, on a different Game.exe.
constexpr uint32_t kCtrPoolBlock = 128u << 10;
bool set_pool_block_size(d2rt::Cpu &c, uint32_t bytes);
void request_pool_release();

}  // namespace game

#endif
