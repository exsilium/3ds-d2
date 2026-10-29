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

// The cells DccCell has decoded since the start (0 without it).
uint64_t dcc_cells_decoded();

}  // namespace game

#endif
