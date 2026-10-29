// src/game/sprite_governor.cpp, the sprite cache governor's policy (issue
// #18): hysteresis on the way down, steps on the way up, the working set as
// a floor, the spare as an emergency. Run through `scripts/docker.sh test governor`.
#include "game/sprite_governor.h"

#include <cstdio>

using game::SpriteGovernor;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

constexpr uint32_t KiB = 1u << 10, MiB = 1u << 20;
constexpr uint32_t kReserve = 6 * MiB, kMin = 4 * MiB, kGame = 16 * MiB;

static SpriteGovernor::Beat beat(uint32_t limit, uint32_t used, uint32_t evicted, uint32_t pool_free, bool spare = false)
{
    return {limit, used, evicted, pool_free, spare};
}

// A governor that has seen the game's cache: its first beat, at the game's size.
static SpriteGovernor started()
{
    SpriteGovernor g;
    g.step(beat(kGame, 1 * MiB, 0, 30 * MiB), kReserve, kMin);
    return g;
}

int main()
{
    {   // The game's own limit, never changed yet: two short beats lower it. (In the
        // 4 h 25 min session, every beat before the first change counted as a new
        // cache, and the pool reached the spare with the cache at 16 MiB.)
        SpriteGovernor g;
        CHECK(g.step(beat(kGame, 16 * MiB - 5 * KiB, 0, 4 * MiB), kReserve, kMin) == kGame);
        CHECK(g.game_limit() == kGame);
        CHECK(g.step(beat(kGame, 16 * MiB - 5 * KiB, 0, 4 * MiB), kReserve, kMin) == 13 * MiB + 768 * KiB);
    }
    {   // One short heartbeat changes nothing; the second lowers by the shortfall, from what the cache holds.
        SpriteGovernor g = started();
        CHECK(g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin) == kGame);
        CHECK(g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin) == 10 * MiB);
        // Counting starts again after acting.
        CHECK(g.step(beat(10 * MiB, 10 * MiB, 5, 5 * MiB), kReserve, kMin) == 10 * MiB);
        CHECK(g.step(beat(10 * MiB, 10 * MiB, 5, 5 * MiB), kReserve, kMin) == 9 * MiB);
    }
    {   // A short heartbeat between two good ones is noise.
        SpriteGovernor g = started();
        CHECK(g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin) == kGame);
        CHECK(g.step(beat(kGame, 12 * MiB, 5, kReserve + 100 * KiB), kReserve, kMin) == kGame);
        CHECK(g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin) == kGame);
    }
    {   // Up in 256 KiB steps, with the pool two steps above the reserve and the cache at its limit.
        SpriteGovernor g = started();
        g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin);
        CHECK(g.step(beat(kGame, 12 * MiB, 5, 4 * MiB), kReserve, kMin) == 10 * MiB);
        CHECK(g.step(beat(10 * MiB, 10 * MiB, 5, kReserve + 256 * KiB), kReserve, kMin) == 10 * MiB);
        CHECK(g.step(beat(10 * MiB, 10 * MiB, 5, kReserve + 512 * KiB), kReserve, kMin) == 10 * MiB + 256 * KiB);
        // Not while the cache has room: in town it would drift up to the game's size.
        CHECK(g.step(beat(10 * MiB + 256 * KiB, 6 * MiB, 0, 20 * MiB), kReserve, kMin) == 10 * MiB + 256 * KiB);
        // Never above the game's size.
        SpriteGovernor h = started();
        h.step(beat(kGame, kGame - 100 * KiB, 5, 4 * MiB), kReserve, kMin);
        const uint32_t low = h.step(beat(kGame, kGame - 100 * KiB, 5, 4 * MiB), kReserve, kMin);
        CHECK(low < kGame);
        uint32_t limit = low;
        for (int i = 0; i < 40; i++)
            limit = h.step(beat(limit, limit, 5, 20 * MiB), kReserve, kMin);
        CHECK(limit == kGame);
    }
    {   // The working set: what the cache held with room and no evictions holds the limit there...
        SpriteGovernor g = started();
        g.step(beat(kGame, 12 * MiB, 0, 11 * MiB), kReserve, kMin);
        for (int i = 0; i < 5; i++)
            g.step(beat(kGame, 12 * MiB, 3, 11 * MiB), kReserve, kMin);
        CHECK(g.working_set() == 12 * MiB);
        // 12.5 MiB held, 768 KiB short: 11.75 MiB, held at 12.
        CHECK(g.step(beat(kGame, 12 * MiB + 512 * KiB, 3, kReserve - 768 * KiB), kReserve, kMin) == kGame);
        CHECK(g.step(beat(kGame, 12 * MiB + 512 * KiB, 3, kReserve - 768 * KiB), kReserve, kMin) == 12 * MiB);
        // ...but not with the pool more than 1 MiB short (the 2026-10-29
        // sessions: held there, the pool stayed near the spare).
        CHECK(g.step(beat(12 * MiB, 12 * MiB, 3, 4 * MiB), kReserve, kMin) == 12 * MiB);
        CHECK(g.step(beat(12 * MiB, 12 * MiB, 3, 4 * MiB), kReserve, kMin) == 10 * MiB);
        // It is forgotten after 60 heartbeats with evictions.
        for (uint32_t i = 0; i < SpriteGovernor::kHistory; i++)
            g.step(beat(10 * MiB, 10 * MiB, 3, 6 * MiB + 100 * KiB), kReserve, kMin);
        CHECK(g.working_set() == 0);
    }
    {   // A cache at its limit says nothing of what it needs: not counted. (Counted,
        // it held the limit at 7.9 MiB for three and a half hours.)
        SpriteGovernor g;
        for (int i = 0; i < 10; i++)
            CHECK(g.step(beat(kGame, kGame - 10 * KiB, 0, 7 * MiB), kReserve, kMin) == kGame);
        CHECK(g.working_set() == 0);
    }
    {   // The floor never raises a limit already below it.
        SpriteGovernor g = started();
        g.step(beat(kGame, 12 * MiB, 0, 5 * MiB), kReserve, kMin);
        CHECK(g.step(beat(kGame, 12 * MiB, 3, 5 * MiB), kReserve, kMin) == 12 * MiB);
        g.step(beat(12 * MiB, 12 * MiB, 3, 5 * MiB), kReserve, kMin);
        CHECK(g.step(beat(12 * MiB, 12 * MiB, 3, 5 * MiB), kReserve, kMin) == 12 * MiB);
    }
    {   // Not below the minimum, in 256 KiB steps.
        SpriteGovernor g = started();
        g.step(beat(kGame, 5 * MiB, 3, 2 * MiB), kReserve, kMin);
        CHECK(g.step(beat(kGame, 5 * MiB, 3, 2 * MiB), kReserve, kMin) == kMin);
        SpriteGovernor h = started();
        h.step(beat(kGame, 12 * MiB, 3, kReserve - 300 * KiB), kReserve, kMin);
        CHECK(h.step(beat(kGame, 12 * MiB, 3, kReserve - 300 * KiB), kReserve, kMin) == 11 * MiB + 512 * KiB);
    }
    {   // A request served from the spare: the minimum at once, working set or not.
        SpriteGovernor g = started();
        g.step(beat(kGame, 12 * MiB, 0, 11 * MiB), kReserve, kMin);
        CHECK(g.step(beat(kGame, 12 * MiB, 3, 4 * MiB, true), kReserve, kMin) == kMin);
    }
    {   // The game re-creating its cache (a limit step() did not set): a new start.
        SpriteGovernor g = started();
        g.step(beat(kGame, 12 * MiB, 0, 11 * MiB), kReserve, kMin);
        g.step(beat(kGame, 12 * MiB, 3, 4 * MiB), kReserve, kMin);
        CHECK(g.working_set() == 12 * MiB);
        CHECK(g.step(beat(8 * MiB, 0, 0, 30 * MiB), kReserve, kMin) == 8 * MiB);
        CHECK(g.game_limit() == 8 * MiB);
        CHECK(g.working_set() == 0);
        // A short beat before it does not count towards the new cache's two.
        CHECK(g.step(beat(8 * MiB, 8 * MiB, 3, 4 * MiB), kReserve, kMin) == 8 * MiB);
    }
    {   // A run of a session's shape: the pool swinging around the reserve. The
        // limit goes down at most once per two short beats, and up a step at a time.
        SpriteGovernor g = started();
        uint32_t limit = kGame, downs = 0, shorts = 0;
        const uint32_t frees[] = {5, 4, 3, 7, 5, 4, 7, 4, 5, 7, 3, 4, 7, 5, 3, 4};
        for (uint32_t f : frees) {
            const uint32_t want = g.step(beat(limit, limit, 3, f * MiB), kReserve, kMin);
            shorts += f < 6;
            downs += want < limit;
            CHECK(want <= limit + SpriteGovernor::kStep);
            CHECK(want <= limit || f >= 6);
            limit = want;
        }
        CHECK(2 * downs <= shorts);
        CHECK(limit >= kMin);
    }
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("governor: all passed\n");
    return 0;
}
