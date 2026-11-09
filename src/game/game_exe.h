// The Game.exe on the SD card (or in the RomFS), told apart from the 1.14d
// build this project runs before the game is started (issue #77). An older
// Game.exe (before 1.14, a small loader for the game's own DLLs) was started
// anyway: its imports from those DLLs stayed empty, and the first call through
// one crashed the app (Sentry 3DS-D2-16).
#pragma once

#include <cstddef>
#include <cstdint>

namespace game {

// Where 1.14d's Game.exe starts: its image base plus its entry point's RVA
// (docs/evidence/m4-linux-menu.txt). Its SHA-256 is in the README.
constexpr uint32_t kGameExeEntry = 0x00682985;

// 1.14 put the game into Game.exe, about 3.6 MB. The loaders before it are
// a few hundred KiB.
constexpr size_t kGameExeMinSize = 1u << 20;

struct GameExe {
    enum Kind {
        kOk,          // the 1.14d build
        kNotPe,       // not a 32-bit Windows executable at all
        kBeforeV114,  // a loader from before 1.14: needs DLLs the app does not have
        kOtherBuild,  // a whole game, but not 1.14d (1.14c, 1.14b, ...)
    };
    Kind kind = kNotPe;
    uint32_t entry = 0;   // image base + entry point, 0 if not a PE
};

GameExe check_game_exe(const uint8_t *data, size_t size);

}  // namespace game
