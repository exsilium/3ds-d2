// Native replacements for Diablo II 1.14d's hottest drawing routines
// (d2_native.cpp). Install after linking, before the game runs
// (rt::RunOptions::install_native). Returns how many were hooked; a
// different Game.exe gets none.
#ifndef D2_GAME_D2_NATIVE_H
#define D2_GAME_D2_NATIVE_H

namespace d2rt { struct Cpu; }

namespace game {

int install_d2_native(d2rt::Cpu &c);

}  // namespace game

#endif
