// The automap for the bottom screen (automap.cpp). Diablo II 1.14d draws its
// map into the game frame, over the world, from its UI pass. The bottom
// screen draws the map's tiles itself, from the game's map data, at the
// player's exact position (smooth scrolling, cheap); the units' crosses and
// names are captured now and then by letting the game draw them alone, at a
// call site taken over in the UI pass.
//
// Facts for Game.exe 1.14d (from its disassembly):
//   0x00456f93  the UI pass: if (ui[0x0A] && 0x45ae90() != 3) 0x45ad60();
//               23 bytes, replaced by a call to a stub of guest code
//   0x0045ad60  draws the map (no arguments), centred on the player:
//               0x457520 setup, 0x459700 the tiles (0x459440 per layer
//               tree), 0x45ac90 the units (0x45a860 per unit), then labels
//   0x0045a860  one unit's cross and name (eax = unit); the stub's filter
//               goes in front of it
//   0x007a5164  the level's map layer: +8 floors, +0x0c walls, +0x10
//               objects, +0x14 extras (trees of cells: +4 frame, +6 x, +8 y
//               in map pixels, +0x0c smaller y, +0x10 larger y)
//   0x007a5168  the map's cell file (DC6: +0x10 directions, +0x14 frames,
//               +0x18 frame pointers); 0x7a516c.. the extras' by the layer's
//               kind (+4)
//   0x007a5198  the map pixel at the view's corner (x, y)
//   0x007a51c8  the units' bounds in the view (x0, y0, x1, y1)
//   0x007a51b0  the player's cross colour
//   0x007a27e8  ui[0x0A], the map toggle (Tab)
//   0x006cb5b0  zero slack after .text's last byte (0x6cb5a1), in its last
//               mapped page: room for the stub, the filter and a flag
#ifndef D2_GAME_AUTOMAP_H
#define D2_GAME_AUTOMAP_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

// Install after linking, before the game runs (beside install_d2_native).
// False on a different Game.exe.
bool install_automap_capture(d2rt::Cpu &c);

// The game's frame (the present callback's rt::Frame): the buffer the game
// draws into. Call on every present; the capture needs one first.
void automap_frame(const uint8_t *pixels, uint32_t width, uint32_t height, int32_t stride);

// Whether the bottom map is shown, how often its units are captured (at most
// one every `move_ms` while the player moves, every two seconds or `move_ms`
// standing still), and its scale (1 or 2: half size). The game's own map on
// the top screen is not affected. Game thread.
void set_automap_capture(bool on, uint32_t move_ms = 500, int scale = 1);

// Draws the map around the player into `out` (w x h, 8-bit, the frame's
// palette, 0 = nothing; cleared by the caller): the tiles at the player's
// position now, the units from the last capture, the player's cross at the
// centre. `scale` 2: half size. False when there is no map to draw (not in
// a game, loading). Game thread.
bool automap_draw(uint8_t *out, int w, int h, int scale);
// Changes whenever automap_draw's picture would (the player moved a pixel at
// that scale, new units, another level); 0 when there is no map.
uint64_t automap_state(int scale);

}  // namespace game

#endif
