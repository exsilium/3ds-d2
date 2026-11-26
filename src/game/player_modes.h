// The player's sprites asked for again, ahead of their next draw (issue #153).
//
// The game's walk of the units near the player (0x470070, from 0x470350)
// asks for a player's sprites in the background a mode at a time, through
// 0x46ff50, for eight modes: town neutral and walk, neutral, walk, run,
// the first attack, cast and hit (the table at 0x712aa0). A mode's
// "loaded" mark (0x7a7490, by class and mode) is never taken back, so a
// mode asked for once is not asked for again when the sprite cache has
// let its sprites go, or the player's equipment names other files. Those
// are then loaded by the frame that first draws them: on the console 20
// or so files and 100 to 350 ms in one frame (3DS-D2-1J, build
// 83013c43b592-aotc76663, where the hit mode's files with a one-handed
// weapon were read in play).
//
// The other modes (second attack, block, kick, throw, skill, death) the
// game does not ask for at all.
//
// A native at the walk's call (0x470377) asks for one of the player's
// modes every 300 ms, in turn and for as long as the game runs, through
// the function the walk itself uses under its marks (0x46fb60: eax the
// unit, ebx the mode, one argument, set for a load in the background).
// Level 1 is the game's eight again; level 2 the other six too, which is
// for the AOT build alone: see player_modes.cpp.
#ifndef GAME_PLAYER_MODES_H
#define GAME_PLAYER_MODES_H

#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// After linking. Level 0: the site alone; 1: the game's eight modes; 2:
// fourteen. False, changing nothing, on a different Game.exe.
bool install_player_modes(d2rt::Cpu &c, int level);
// The heartbeat's line (none when no mode was asked for in the window).
std::vector<std::string> player_modes_lines();

}  // namespace game

#endif
