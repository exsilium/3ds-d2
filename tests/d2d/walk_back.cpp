// UnitWalk (d2_native.cpp, the game's 648120 with 648050's push-on) against
// d2decomped's cases: tests/walk_back_cases.inc in third_party/d2d, made by
// running the stock Game.exe's 648120 under Unicorn with 64d910 (the
// collision test) answering from random walls (its tools/emu/walk_back.py).
// Issue #108, part of #105.
//
//   walk_back <Game.exe>
//
// The native's own code runs: this file includes d2_native.cpp, so it calls
// unit_walk_plan with the case's walls as the collision test, where the game
// passes collision_cross. The plan reads the direction and step tables from
// Game.exe (0x6f1648, 0x6eb398, 0x6eb490), so the user's Game.exe is loaded
// into a small arena at its own addresses, never copied anywhere.
//
// The cases are GPL-3.0-or-later and are read from the submodule at build
// time (-I third_party/d2d/tests), never copied into this tree. This binary is
// a test only and is never distributed.
//
// d2d's records, mapped onto ours: its context is our path (+0 its start,
// which is our target; +4 its end, which is where our unit is and what the
// walk moves; +8 the room), its path is our unit (+0x30), and its unit is our
// inner record (the unit's +0x30), whose first dword is 0 for a player: the
// push-on. d2d's context leaves the size class 0, because it answers 64d910
// itself; ours is 1, a class the plan covers (the answer is the case's either
// way). Coordinates are offset by 1000, as the generator's are.
#include "../../src/game/d2_native.cpp"

#include <cstdio>
#include <set>
#include <utility>
#include <vector>

namespace game {
namespace {

struct WalkCase {
    int x, y, to_x, to_y, player;
    std::vector<std::pair<int, int>> walls;
    int found, end_x, end_y;
};
const WalkCase kWalkCases[] = {
#include "walk_back_cases.inc"
};

constexpr uint32_t kImageBase = 0x00400000;

// Game.exe's headers and sections at their addresses in a fresh arena, with
// room after the image for the test's records. Returns the first free address.
uint32_t load_image(const char *path, std::vector<uint8_t> &arena)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return 0;
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
        file.insert(file.end(), buf, buf + n);
    std::fclose(f);
    auto u16 = [&](size_t o) { return (uint32_t)file[o] | (uint32_t)file[o + 1] << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    if (file.size() < 0x400 || file[0] != 'M' || file[1] != 'Z')
        return 0;
    const uint32_t pe = u32(0x3c), count = u16(pe + 6), opt = u16(pe + 20);
    if (u32(pe + 24 + 28) != kImageBase)
        return 0;
    const uint32_t image = u32(pe + 24 + 56), headers = u32(pe + 24 + 60);
    arena.assign(image + (1u << 20), 0);
    std::memcpy(arena.data(), file.data(), headers);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t s = pe + 24 + opt + 40 * i;
        const uint32_t rva = u32(s + 12), raw_size = u32(s + 16), raw = u32(s + 20);
        if (raw + raw_size > file.size() || rva + raw_size > image)
            return 0;
        std::memcpy(arena.data() + rva, file.data() + raw, raw_size);
    }
    g_base = arena.data() - kImageBase;
    return kImageBase + image;
}

int walk_back(uint32_t free_va)
{
    const uint32_t path = free_va, unit = free_va + 0x100, inner = free_va + 0x200;
    int bad = 0, left = 0, unasked = 0;
    for (const WalkCase &one : kWalkCases) {
        std::memset(H(free_va), 0, 0x300);
        const uint16_t sx = (uint16_t)(1000 + one.x), sy = (uint16_t)(1000 + one.y);
        const uint16_t ex = (uint16_t)(1000 + one.to_x), ey = (uint16_t)(1000 + one.to_y);
        std::memcpy(H(path), &sx, 2);
        std::memcpy(H(path + 2), &sy, 2);
        std::memcpy(H(path + 4), &ex, 2);
        std::memcpy(H(path + 6), &ey, 2);
        put32(path + 8, 1);          // the room: passed to the test, not read
        put32(path + 0x28, 1);       // a size class of the cross test
        put32(path + 0x30, unit);
        put32(unit + 0x30, inner);
        put32(inner, one.player ? 0 : 1);

        std::set<std::pair<int, int>> walls, asked;
        for (const auto &w : one.walls)
            walls.insert({1000 + w.first, 1000 + w.second});
        auto cross = [&](uint32_t, int32_t x, int32_t y, uint16_t, uint32_t &r) {
            asked.insert({x, y});
            r = walls.count({x, y}) ? 1 : 0;
            return true;
        };
        bool moved;
        uint16_t nx = ex, ny = ey;
        const int r = unit_walk_plan(path, moved, nx, ny, cross);
        if (!moved) {
            nx = ex;
            ny = ey;
        }
        const int end_x = (int)nx - 1000, end_y = (int)ny - 1000;
        // Every wall the game asked about, the native asked about too (the
        // cases list only the asked points that were walls).
        bool missed = false;
        for (const auto &w : walls)
            missed |= !asked.count(w);
        unasked += missed;
        if (r < 0)
            left++;
        if (r != one.found || end_x != one.end_x || end_y != one.end_y || missed) {
            if (++bad <= 8)
                std::printf("DIFF end %d,%d player %d walls %zu: native %d %d,%d%s, game.exe %d %d,%d\n", one.to_x,
                            one.to_y, one.player, one.walls.size(), r, end_x, end_y,
                            missed ? " (a wall not asked about)" : "", one.found, one.end_x, one.end_y);
        }
    }
    const size_t n = sizeof kWalkCases / sizeof kWalkCases[0];
    std::printf("walk-back (648120, 648050's push-on): %zu cases, %d differ, %d left to the game's code, "
                "%d with a wall not asked about\n", n, bad, left, unasked);
    std::printf("RESULT d2d.walk_back %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

}  // namespace
}  // namespace game

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: walk_back <Game.exe>\n");
        return 2;
    }
    std::vector<uint8_t> arena;
    const uint32_t free_va = game::load_image(argv[1], arena);
    if (!free_va) {
        std::fprintf(stderr, "walk_back: %s is not a Game.exe this test can load\n", argv[1]);
        return 2;
    }
    return game::walk_back(free_va);
}
