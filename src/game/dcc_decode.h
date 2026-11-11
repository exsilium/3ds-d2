// A DCC sprite group's directions, decoded natively (issue #83): what
// Diablo II 1.14d's 0x60bff0 does between allocating the group's frames and
// leaving its lock (0x60c10a to 0x60ccbe), byte for byte. It reads the
// directions' bit streams from the file's bytes and writes each frame's
// header and run-length rows (0x60bdb0's) into the group's buffer, using its
// own scratch instead of the game's globals (0x969020's frame records,
// 0x94d8a8's cells, 0x90d8a8's pixel entries, 0x8f03e8's direction image).
//
// It touches nothing else, so it can run off the game's threads. A file the
// game's code would handle differently (a frame past the game's own buffer
// sizes, a run that does not fill the frame's coded bytes, the stream
// running off the file) makes it return false having written nothing past
// what the caller allows; the caller then runs the game's own code.
#ifndef D2_GAME_DCC_DECODE_H
#define D2_GAME_DCC_DECODE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace game {

struct DccScratch;

struct DccGroup {
    const uint8_t *src = nullptr;   // the first direction's bytes (0x60bff0's ecx)
    size_t src_len = 0;             // bytes readable from src
    uint32_t dirs = 0;              // directions to decode (0x60bff0's 5th argument)
    uint32_t frames = 0;            // frames per direction (the group's +0x18)
    uint8_t *out = nullptr;         // the group's frames, as 0x40b430 allocated them
    size_t out_len = 0;             // their size (the directions' OutSizeCoded, added up)
};

// The scratch is the caller's, one per thread that decodes at once.
DccScratch *dcc_scratch_new();
void dcc_scratch_free(DccScratch *s);

// True when every direction decoded as the game's code would have.
bool dcc_decode_group(const DccGroup &g, DccScratch &s);

}  // namespace game

#endif
