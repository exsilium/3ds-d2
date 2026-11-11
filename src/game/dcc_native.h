// The native DCC decoder in the game (issue #83, dcc_decode.h): 0x60bff0's
// directions decoded by native code, on the I/O worker's core when the
// thread decoding is not the frame thread (Storm's read thread, in a load's
// callback), in place otherwise.
//
// 0x60bff0 still takes its lock and allocates the group's frames; at
// 0x60c10a, a call to the native replaces its per-direction loop. When the
// native decodes the group, it returns the loop's end so the game's own
// compare skips to the function's end (0x60ccbe); when it cannot (the
// decoder off, or a file dcc_decode_group leaves to the game), it returns
// the loop's start and the game's code decodes as before.
#ifndef D2_GAME_DCC_NATIVE_H
#define D2_GAME_DCC_NATIVE_H

#include <cstdint>

namespace d2rt { struct Cpu; }

namespace game {

enum class DccMode : uint32_t {
    Off = 0,       // the game's code
    InPlace = 1,   // native, on the thread decoding
    Offload = 2,   // native, on the I/O worker when off the frame thread
    Check = 3,     // the lane: native into a copy, the game's code, then compared
};

// After linking (beside install_d2_native). D2_DCC=0..3 overrides `mode`.
// False on a different Game.exe.
bool install_dcc_native(d2rt::Cpu &c, DccMode mode);
// Switched at any time (the OPTIONS page).
void set_dcc_mode(DccMode mode);
DccMode dcc_mode();

struct DccStats {
    uint32_t native = 0, offloaded = 0, game = 0;   // groups decoded by each
    uint32_t refused = 0;                           // groups the native left to the game's code
    uint64_t native_us = 0;                         // the native's time, wherever it ran
    uint32_t checked = 0, differed = 0;             // DccMode::Check
    uint32_t on_frame = 0, worker_busy = 0;         // decoded in place: on the frame thread, the worker full
};
DccStats dcc_stats();

}  // namespace game

#endif
