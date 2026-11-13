// The game's units near the player, read from Diablo II 1.14d's memory, for
// scripted input (autoplay.h's tapunit, issue #66): an NPC found by its
// class, and the game pixel to click it at.
//
// Facts for Game.exe 1.14d (checked on the Linux lane against screenshots):
//   0x007A6A70  the player's unit (d2_state.h)
//   0x007A5E70  the client's unit tables: 128 buckets a unit type (0 players,
//               1 monsters and NPCs, ...), unit id & 127; a bucket's units
//               chained through +0xE4
//   unit +0x00 type, +0x04 class (monstats row), +0x0C id, +0x10 mode,
//        +0x18 act (0 to 4), +0x2C path: +0x00 x fraction (1/65536), +0x02 x, +0x04 y fraction,
//        +0x06 y, in subtiles
// The view keeps the player at the centre of the 640x480 frame; a subtile is
// 32x16 pixels on screen: x - y across, (x + y) / 2 down.
#ifndef D2_GAME_D2_UNITS_H
#define D2_GAME_D2_UNITS_H

#include <cstdint>
#include <vector>

namespace game {

struct UnitSpot {
    uint32_t unit = 0, cls = 0, id = 0, mode = 0;   // unit: its guest address
    int x = 0, y = 0;          // the game pixel of its body (may be off the screen)
    float distance = 0;        // from the player, in subtiles
};

// The unit of class `cls` nearest the player: a monster or NPC (type 1), or
// an object (type 2: a waypoint is 119 in the Rogue Encampment). False if
// none, or not in a game.
bool find_unit(uint32_t cls, UnitSpot &out, uint32_t type = 1);

// The act the player is in, 1 to 5; 0 when not in a game.
int player_act();

// The game pixel of line `line` (1: the first under the NPC's name) of the
// menu an NPC opens when clicked, drawn over its head: from where the unit
// is now (1.14d at 640x480: the name's middle 138 pixels above its body, a
// line every 15, centred 12 to the left).
void npc_menu_line(const UnitSpot &npc, int line, int &x, int &y);

// A town NPC's class by name, case-insensitive ("warriv", "kashya"...), or a
// number as it is; 0 if neither.
uint32_t unit_class(const char *name);

// An item is on the cursor (picked up in the inventory, a shop...).
bool holding_item();

// The level the player is in (levels.txt's number: 1 the Rogue Encampment,
// 40 Lut Gholein, 43 the Far Oasis), 0 if not in a game.
int player_level();

// The nearest loaded object whose objects.txt name is `name` (any case).
bool find_object_named(const char *name, UnitSpot &out);
// The nearest of the level's rooms not loaded yet (and not in `skip`): its
// offset from the player in subtiles, to its centre.
bool frontier_room(const std::vector<uint32_t> &skip, uint32_t &room, double &dx, double &dy, double &distance);

// The nearest of the level's preset units of `type` (2 objects) and class
// `cls`, loaded or not: its offset from the player in subtiles. False if
// none, or not in a game.
bool find_preset(uint32_t cls, uint32_t type, double &dx, double &dy, double &distance);

// Every monster and object near the player (all: every one in the game's
// tables), to the runtime log: class, id, mode, an object's name, place and
// its spot on the screen ("units:" lines; autoplay's "units [all]").
void log_units(bool all = false);

}  // namespace game

#endif
