// The sprite cache holds decoded DCC frames. 0x457300 sizes it from the
// physical RAM the game is told about: with U = RAM - 11 MiB, the cache gets
// (U - audio - 6 MiB - 2 MiB) / 3, clamped to [24 MiB, 64 MiB], and
// 0x5ffd40 hands it to the cache object at 0x89db60 (SpriteCache.cpp), which
// keeps it at +4, the bytes in use at +8 and the entry count at +0xc, and
// evicts within it. The cache grows into the game's pool system (blocks of
// VirtualAlloc'd memory, mostly 512 KiB, allocated at 0x409681) as new
// monsters and areas are seen, up to that size.
//
// On a New 3DS the guest heap and VirtualAlloc share one pool (rt_layout.h).
// VirtualAlloc climbed to ~52 MiB over a session reaching the Den of Evil
// and the heap's next block no longer fit (issue #15). The 64 MiB report
// already puts the cache at the game's floor; lowering the floor is the
// only lever left on it.
//
// The cache's limit is read on every insertion: 0x5ffc40 asks 0x5ff3f0 for
// room (refused when used + size > limit) and evicts the least recently
// used sprite (0x5ff4a0) until it gets it. A limit lowered while the game
// runs is therefore honoured from the next sprite on; it must stay above
// any single sprite (0x5ffc4b treats one that large as fatal).
//
// When the pool system's VirtualAlloc fails, it reports "Unrecoverable
// internal error" and calls exit(-1) (0x681e09), as do the game's ~2,000
// other fatal checks. exit runs the C runtime's teardown, which frees
// memory the DirectSound stream thread (0x516250) still walks: on the
// console that thread then faulted on a freed list node (issue #15, the
// same fault as issue #5). install_fatal_exit ends the run at exit(-1).
#include "game/d2_memory.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstring>

namespace game {
namespace {

// 0x4573e7: mov edx, 0x1800000 -- the floor, taken when the computed size
// is at or below it.
constexpr uint32_t kFloorInsn = 0x004573e7;
constexpr uint8_t kFloorOrig[] = {0xba, 0x00, 0x00, 0x80, 0x01};
constexpr uint32_t kSpriteCache = 0x0089db60;

// exit(code): push 0; push 0; push [esp+0xc]; call doexit (0x681d27);
// add esp, 0xc; ret. Rewritten in place as
//   cmp dword [esp+4], -1; jne A; jmp <native>
// with the original's body moved to two runs of int3 padding (no run holds
// all 17 bytes): A = push 0; push 0; push [esp+0xc]; jmp B, and
// B = call doexit; add esp, 0xc; ret.
constexpr uint32_t kExit = 0x00681e09;
constexpr uint8_t kExitOrig[] = {0x6a, 0x00, 0x6a, 0x00, 0xff, 0x74, 0x24, 0x0c, 0xe8,
                                 0x11, 0xff, 0xff, 0xff, 0x83, 0xc4, 0x0c, 0xc3};
constexpr uint32_t kDoExit = 0x00681d27;
constexpr uint32_t kPadA = 0x006814d1, kPadB = 0x00681531;   // 15 bytes of int3 each

uint32_t g_fatal_caller = 0;
uint32_t g_game_limit = 0;      // the cache size the game set
uint32_t g_written_limit = 0;   // the last limit govern_sprite_cache wrote

void put_rel32(uint8_t *at, uint32_t insn_end, uint32_t target)
{
    const uint32_t rel = target - insn_end;
    std::memcpy(at, &rel, 4);
}

bool all_int3(d2rt::Cpu &c, uint32_t va, uint32_t n)
{
    uint8_t b[16];
    if (n > sizeof b || !c.read(va, b, n))
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (b[i] != 0xcc)
            return false;
    return true;
}

}  // namespace

bool set_sprite_cache_floor(d2rt::Cpu &c, uint32_t bytes)
{
    uint8_t have[sizeof kFloorOrig];
    if (!c.read(kFloorInsn, have, sizeof have) || std::memcmp(have, kFloorOrig, sizeof have) != 0)
        return false;
    c.write_u32(kFloorInsn + 1, bytes);
    rt::logf("sprite cache floor: %u KiB (the game's own: 24576 KiB)", bytes >> 10);
    return true;
}

SpriteCacheStats sprite_cache_stats(d2rt::Cpu &c)
{
    return {c.read_u32(kSpriteCache + 8), c.read_u32(kSpriteCache + 4), c.read_u32(kSpriteCache + 0xc)};
}

uint32_t govern_sprite_cache(d2rt::Cpu &c, uint32_t pool_free, uint32_t reserve, uint32_t min_limit)
{
    const uint32_t cur = c.read_u32(kSpriteCache + 4);
    if (!cur)
        return 0;   // no cache yet
    if (cur != g_written_limit)
        g_game_limit = cur;   // the game (re)created it
    const uint32_t short_by = pool_free < reserve ? reserve - pool_free : 0;
    uint32_t want = g_game_limit > short_by + min_limit ? g_game_limit - short_by : min_limit;
    want = want < g_game_limit ? want & ~0x3FFFFu : g_game_limit;   // 256 KiB steps
    if (want == cur)
        return cur;
    c.write_u32(kSpriteCache + 4, want);
    g_written_limit = want;
    rt::logf("sprite cache limit %u KiB (pool free %u KiB)", want >> 10, pool_free >> 10);
    return want;
}

bool install_fatal_exit(d2rt::Cpu &c)
{
    uint8_t have[sizeof kExitOrig];
    if (!c.read(kExit, have, sizeof have) || std::memcmp(have, kExitOrig, sizeof have) != 0 ||
        !all_int3(c, kPadA, 15) || !all_int3(c, kPadB, 15))
        return false;
    // The native sees exit's own stack: the caller's return address, then the code.
    const uint32_t trap = rt::native_entry("FatalExit", 0, [](d2rt::Cpu &c) -> uint32_t {
        g_fatal_caller = c.read_u32(c.reg(d2rt::R_ESP));
        rt::logf("game: fatal exit(-1), called from %08x", g_fatal_caller);
        rt::request_exit(0xFFFFFFFFu);
        return 0;
    });
    if (!trap)
        return false;
    uint8_t a[13] = {0x6a, 0x00, 0x6a, 0x00, 0xff, 0x74, 0x24, 0x0c, 0xe9};
    put_rel32(a + 9, kPadA + 13, kPadB);
    uint8_t b[9] = {0xe8, 0, 0, 0, 0, 0x83, 0xc4, 0x0c, 0xc3};
    put_rel32(b + 1, kPadB + 5, kDoExit);
    uint8_t e[16] = {0x83, 0x7c, 0x24, 0x04, 0xff, 0x0f, 0x85, 0, 0, 0, 0, 0xe9};
    put_rel32(e + 7, kExit + 11, kPadA);
    put_rel32(e + 12, kExit + 16, trap);
    c.write(kPadA, a, sizeof a);
    c.write(kPadB, b, sizeof b);
    c.write(kExit, e, sizeof e);
    c.invalidate_code(kPadA, sizeof a);
    c.invalidate_code(kPadB, sizeof b);
    c.invalidate_code(kExit, sizeof e);
    rt::logf("fatal exit: exit(-1) at %08x ends the run", kExit);
    return true;
}

uint32_t fatal_exit_caller()
{
    return g_fatal_caller;
}

}  // namespace game
