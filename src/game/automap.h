// The automap for the bottom screen (automap.cpp): Diablo II 1.14d draws it
// into the game frame, over the world, from its UI pass. This takes over that
// call site so the map can also be drawn into an off-screen buffer of the
// frame's size, which the second screen shows on its own.
//
// Facts for Game.exe 1.14d (from its disassembly):
//   0x00456f93  the UI pass: if (ui[0x0A] && 0x45ae90() != 3) 0x45ad60();
//               23 bytes, replaced by a call to a stub of guest code
//   0x0045ad60  draws the automap (no arguments), centred on the player
//   0x007a27e8  ui[0x0A], the automap toggle (Tab)
//   0x006cb5b0  zero slack after .text's last byte (0x6cb5a1), in its last
//               mapped page: room for the stub
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

// Whether the platform wants the off-screen map, and how often: every
// `every`-th UI pass draws it (the game's own map on the top screen is not
// affected). Game thread.
void set_automap_capture(bool on, int every = 3);

// The latest off-screen map, the frame's size: 8-bit, the frame's palette,
// index 0 = nothing.
// `pixels` points into guest memory and stays valid until the next capture;
// `seq` counts captures. False before the first one.
struct AutomapImage {
    const uint8_t *pixels;
    int32_t stride;
    uint32_t width, height;
    uint32_t seq;
};
bool automap_image(AutomapImage &out);

}  // namespace game

#endif
