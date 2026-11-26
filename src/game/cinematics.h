// Diablo II's cinematics, played natively (cinematics.cpp). Install after
// linking, before the game runs (rt::RunOptions::install_native). Returns 1
// if installed; a different Game.exe is left as it was (no videos).
#ifndef D2_GAME_CINEMATICS_H
#define D2_GAME_CINEMATICS_H

namespace d2rt { struct Cpu; }

namespace game {

// play false: the code is installed (the AOT code is compiled with it, issue
// #138) but answers as the game's own stubs do: no video is played.
int install_cinematics(d2rt::Cpu &c, bool play = true);

// A cinematic is playing (from its open to its last frame or skip).
bool cinematic_playing();

}  // namespace game

#endif
