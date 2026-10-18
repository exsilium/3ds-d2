// Diablo II's cinematics, played natively (cinematics.cpp). Install after
// linking, before the game runs (rt::RunOptions::install_native). Returns 1
// if installed; a different Game.exe is left as it was (no videos).
#ifndef D2_GAME_CINEMATICS_H
#define D2_GAME_CINEMATICS_H

namespace d2rt { struct Cpu; }

namespace game {

int install_cinematics(d2rt::Cpu &c);

}  // namespace game

#endif
