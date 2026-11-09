#include "game/game_exe.h"

namespace game {
namespace {

uint32_t u16_at(const uint8_t *p, size_t at) { return (uint32_t)p[at] | (uint32_t)p[at + 1] << 8; }

uint32_t u32_at(const uint8_t *p, size_t at)
{
    return (uint32_t)p[at] | (uint32_t)p[at + 1] << 8 | (uint32_t)p[at + 2] << 16 | (uint32_t)p[at + 3] << 24;
}

}  // namespace

GameExe check_game_exe(const uint8_t *data, size_t size)
{
    GameExe r;
    // MZ header, its e_lfanew at 0x3c, then "PE\0\0", the 20-byte file header,
    // and the PE32 optional header (magic 0x10b): AddressOfEntryPoint at +16,
    // ImageBase at +28.
    if (!data || size < 0x40 || u16_at(data, 0) != 0x5a4d)
        return r;
    const uint32_t pe = u32_at(data, 0x3c);
    const uint32_t opt = pe + 24;
    if (pe > size || size - pe < 24 + 32 || u32_at(data, pe) != 0x00004550u || u16_at(data, opt) != 0x10b)
        return r;
    r.entry = u32_at(data, opt + 28) + u32_at(data, opt + 16);
    if (r.entry == kGameExeEntry)
        r.kind = GameExe::kOk;
    else
        r.kind = size < kGameExeMinSize ? GameExe::kBeforeV114 : GameExe::kOtherBuild;
    return r;
}

}  // namespace game
