// What Diablo II 1.14d is showing, read from its memory: in the game world or
// in the menus, and whether a panel (inventory, character sheet, a shop...)
// is open. The control scheme uses it to decide what the circle pad does.
//
// Facts for Game.exe 1.14d at its preferred base (0x00400000), as documented
// by D2Vita's reverse engineering (behavioural reference; no code from it):
//   0x007A6A70  player unit pointer: non-null while a game is running
//   0x007A27C0  UI state table, one dword per panel id; non-zero = open
//   0x007D55C8  D2Win's focused control (0: none); a control's first dword
//               is its type, 1 = edit box. The text itself (control+0x5C) is
//               never read: on Battle.net it can be a password.
//   0x0045B440  a function entry starting with push ebp (0x55): a cheap check
//               that this really is the 1.14d layout before trusting the rest
#ifndef D2_GAME_D2_STATE_H
#define D2_GAME_D2_STATE_H

#include <cstdint>

namespace game {

struct GameState {
    bool known = false;       // the 1.14d layout check passed
    bool in_game = false;     // a game is running (player unit exists)
    bool panel_open = false;  // a panel covers the world (the cursor is for it)
    uint32_t edit_focus = 0;  // the focused edit box (character name, chat...), 0 if none
    uint64_t ui = 0;          // in game: bit i set = UI table entry i is non-zero (i < 0x28)
};

// Cheap: a check byte and up to 42 dwords. Call once per input poll.
GameState read_game_state();

}  // namespace game

#endif
