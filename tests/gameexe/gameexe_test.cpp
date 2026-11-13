// src/game/game_exe.cpp, the Game.exe the app agrees to start (issue #77):
// 1.14d's by its entry point, a loader from before 1.14 and another whole
// build told apart, anything else not a PE. Run through
// `scripts/docker.sh test gameexe`.
#include "game/game_exe.h"

#include <cstdio>
#include <vector>

using game::GameExe;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static void put16(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    b[at] = (uint8_t)v;
    b[at + 1] = (uint8_t)(v >> 8);
}

static void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        b[at + i] = (uint8_t)(v >> (8 * i));
}

// The headers of a PE32 with this image base and entry RVA, padded to size.
static std::vector<uint8_t> pe(size_t size, uint32_t base, uint32_t entry_rva, uint32_t lfanew = 0xe8)
{
    std::vector<uint8_t> b(size, 0);
    put16(b, 0, 0x5a4d);   // "MZ"
    put32(b, 0x3c, lfanew);
    if (lfanew + 24 + 32 > size)
        return b;
    put32(b, lfanew, 0x00004550);   // "PE\0\0"
    put16(b, lfanew + 4, 0x14c);    // i386
    put16(b, lfanew + 24, 0x10b);   // PE32
    put32(b, lfanew + 24 + 16, entry_rva);
    put32(b, lfanew + 24 + 28, base);
    return b;
}

static GameExe check(const std::vector<uint8_t> &b) { return game::check_game_exe(b.data(), b.size()); }

int main()
{
    {   // 1.14d: image base 00400000, entry RVA 00282985.
        const GameExe r = check(pe(3600000, 0x00400000, 0x00282985));
        CHECK(r.kind == GameExe::kOk);
        CHECK(r.entry == game::kGameExeEntry);
    }
    {   // 3DS-D2-16's Game.exe: 309,379 bytes, entry 00416852.
        const GameExe r = check(pe(309379, 0x00400000, 0x00016852));
        CHECK(r.kind == GameExe::kBeforeV114);
        CHECK(r.entry == 0x00416852);
    }
    {   // A whole game that isn't 1.14d (1.14c's size, another entry point).
        const GameExe r = check(pe(3600000, 0x00400000, 0x00282000));
        CHECK(r.kind == GameExe::kOtherBuild);
    }
    {   // 1.14d's entry RVA under another image base is not 1.14d.
        CHECK(check(pe(3600000, 0x10000000, 0x00282985)).kind == GameExe::kOtherBuild);
    }
    {   // Not a PE: empty, too short, no MZ, no PE signature, a PE32+ header.
        CHECK(game::check_game_exe(nullptr, 0).kind == GameExe::kNotPe);
        CHECK(check(std::vector<uint8_t>(0x20, 0)).kind == GameExe::kNotPe);
        std::vector<uint8_t> b = pe(4096, 0x00400000, 0x00282985);
        b[0] = 'X';
        CHECK(check(b).kind == GameExe::kNotPe);
        b = pe(4096, 0x00400000, 0x00282985);
        b[0xe8] = 'X';
        CHECK(check(b).kind == GameExe::kNotPe);
        b = pe(4096, 0x00400000, 0x00282985);
        put16(b, 0xe8 + 24, 0x20b);
        CHECK(check(b).kind == GameExe::kNotPe);
        CHECK(check(b).entry == 0);
    }
    {   // e_lfanew past the end, or headers cut short: refused, never read past the file.
        CHECK(check(pe(4096, 0x00400000, 0x00282985, 0xfffffff0)).kind == GameExe::kNotPe);
        CHECK(check(pe(0x100, 0x00400000, 0x00282985, 0x100 - 40)).kind == GameExe::kNotPe);
        CHECK(check(pe(0x100, 0x00400000, 0x00282985, 0x100 - 56)).kind == GameExe::kOk);
    }
    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("game_exe: all passed\n");
    return 0;
}
