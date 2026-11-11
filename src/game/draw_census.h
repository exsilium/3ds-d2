// The draw census (issue #80, step 1): which of a frame's pixels the draw
// natives write and which the game's own code writes, in what order. A draw
// queue can run the natives on another core only between the points where
// the game's code touches the frame buffer itself; the census finds them.
//
// Lane only: D2_DRAW_CENSUS=<n> takes every n-th frame. On those frames each
// draw native (d2_native.cpp) compares the frame buffer with a copy taken
// after the last one: a difference is the game's code drawing in between.
// Each frame logs its runs of natives with no game drawing between them:
//   census <frame> run <k> guest=<x0>,<y0>,<x1>,<y1>|- ret=<the first
//       native's return address> n=<natives> <name>:<calls>,...
//   census <frame> end guest=<box>|- natives <n> runs <k>
// `guest` is the box the game's code changed before the run (before the
// frame's blit for `end`). Two copies of the frame buffer a native: slow,
// and only for finding the structure, never for timing.
//
// The draw phases (drawphases=1 in settings.txt, D2_DRAW_PHASES on the
// lane) time the same natives with the host clock, two reads a call, on
// every frame. Every 250 frames:
//   draw phases: <n> frames of <ms>: floor run <ms> a frame (FloorTile <ms>,
//       the frame thread between them <ms>, <tiles> tiles); draw natives
//       <ms> (<name> <ms> ...)
// The floor run is a frame's FloorTile calls up to its first other draw
// native: the census finds no drawing by the game's code inside it, so a
// queue could draw it on another core while the frame thread does the
// work between the calls.
#ifndef D2_GAME_DRAW_CENSUS_H
#define D2_GAME_DRAW_CENSUS_H

#include <cstdint>
#include <functional>

namespace d2rt { struct Cpu; }

namespace game {

using NativeFn = std::function<uint32_t(d2rt::Cpu &)>;

// The draw phases' timing, before draw_census_init (the app's setting).
void draw_phases_set(bool on);
bool draw_phases_on();
// The app's present, in us, added to the phases' window: the automap's
// capture, the copy of the frame, the bottom screen's staging, and all of it.
void draw_phases_present(uint64_t automap_us, uint64_t copy_us, uint64_t bottom_us, uint64_t all_us);
// From the environment; installs the blit hook when either is on. `base`:
// the arena.
bool draw_census_init(uint8_t *base);
// `fn`, counted as the draw native `name` on census frames (as is when off).
NativeFn draw_census_wrap(const char *name, NativeFn fn);

}  // namespace game

#endif
