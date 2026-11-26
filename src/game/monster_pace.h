// A monster's sprites asked for a mode at a time (issue #153).
//
// When a monster arrives on the client, 0x470b70 (from the monster's
// assign, 0x45f190) calls 0x46feb0, which walks the modes monstats2 gives
// the monster (the list at 0x712ac4: death and dead first, then neutral,
// walk and the rest) and calls 0x46fda0 for each: every component's file is
// asked of the sprite cache (0x46fb60), and one not there is queued with the
// game's asynchronous loader (0x6004e0). The load is in the background, but
// the request opens the file and reads its header on the game's thread: for
// a monster of 12 modes, some 30 to 60 files in the one frame. In Azahar's
// Cold Plains fight a 10 s window had 127 such reads and 751 ms of the
// frame thread's wait, against 10 sprite groups it decoded itself. 0x470350
// (each frame from 0x44c790, at most every 600 ms) walks the units of the
// rooms near the player and asks again for the modes not done, so a mode
// left out is asked for later.
//
// The call to 0x46fda0 at 0x46ff31 goes through a stub in .text's slack
// that asks a native first: the mode to go on with, or -1 to leave it to a
// later walk. The native lets a class's modes through one at a time, one
// every 100 ms: the monster's own mode, neutral and walk first, then the
// attack and the hit, the others after those. 0x470350 walks every 100 ms
// (its 600 at 0x47036d). A load that isn't in the background (the loading
// screen's, 0x470b10) goes through as it is.
#ifndef D2_GAME_MONSTER_PACE_H
#define D2_GAME_MONSTER_PACE_H

#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

// After linking. `on` false: the sites alone, every mode let through at
// once. False, changing nothing, on a different Game.exe.
bool install_monster_pace(d2rt::Cpu &c, bool on = true);
// The heartbeat's line (none when no mode was asked for in the window).
std::vector<std::string> monster_pace_lines();

}  // namespace game

#endif
