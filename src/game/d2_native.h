// Native replacements for Diablo II 1.14d's hottest drawing routines
// (d2_native.cpp). Install after linking, before the game runs
// (rt::RunOptions::install_native). Returns how many were hooked; a
// different Game.exe gets none.
#ifndef D2_GAME_D2_NATIVE_H
#define D2_GAME_D2_NATIVE_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

int install_d2_native(d2rt::Cpu &c);

// The floor queue (issue #80, d2_native.cpp): 0 off, 1 on, 2 a lane check
// (drawn in reverse). Before install_d2_native; D2_FLOOR_QUEUE overrides.
void set_floor_queue(int mode);
// Its worker: `start` runs `worker` on a host thread of its own, on the
// second core (false: none; the frame thread then draws the queue at
// 476ce1); the worker blocks in `wait` until `wake`. Before install_d2_native.
struct FloorWorker {
    bool (*start)(void (*worker)()) = nullptr;
    void (*wait)() = nullptr;
    void (*wake)() = nullptr;
};
void set_floor_worker(const FloorWorker &w);
// Ends the worker's loop (it returns from `worker`): before the app joins it.
void stop_floor_worker();

// The cells DccCell has decoded since the start (0 without it).
uint64_t dcc_cells_decoded();

}  // namespace game

#endif
