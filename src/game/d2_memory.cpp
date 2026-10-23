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
//
// The pool system keeps its blocks once they are empty: only 0x409b80, run
// when a game is joined or left, gives them back. Over a long session
// (40 minutes into Act I's second quest) it held 44 MiB of VirtualAlloc
// while the governor had evicted the cache down to 7 MiB, the pool's free
// space was in pieces of 274 KiB at most, and a 386 KiB block the pool
// system asks the heap for directly (0x409770, sizes above its largest
// class) did not fit. install_pool_release lets the game give empty blocks
// back while it plays. The global pool held only 17 of those MiB; the rest
// is in the pool systems the game creates for the client's and the
// server's game, which it releases too.
#include "game/d2_memory.h"

#include "runtime/cpu.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/rt_host.h"

#include <cstring>
#include <utility>
#include <vector>

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

// The global pool (0x74f104): its lock at +4, the number of size classes at
// +0x1c, the classes from +0x20 (0x30 bytes each: their lock at +0, units
// per block +0x1c, block bytes +0x20, blocks +0x24, list head +0x28 and tail
// +0x2c), and the blocks sorted by address for frees (count +0x7a0, an array
// of {header, address} at +0x7a8), whose bytes +0x17a8 counts. A block's
// header, from malloc: its VirtualAlloc'd memory +0, units in use +8, prev
// +0xc, next +0x10, class +0x14.
constexpr uint32_t kPool = 0x0074f104;
constexpr uint32_t kPoolMaxClasses = (0x7a0 - 0x20) / 0x30;
// The global pool is the first of 8 pool systems of 0x17cc bytes (its name
// at +0x17ac); 0x409dd0 hands out the next unused one (their high-water mark
// at 0x75af64) and 0x409c80 destroys one, zeroing it. The functions that set
// them up and tear them down lie in [0x409c80, 0x409ff0).
constexpr uint32_t kPoolSize = 0x17cc, kPoolSlots = 8;
constexpr uint32_t kPoolCount = 0x0075af64;
constexpr uint32_t kPoolSetup = 0x00409c80, kPoolSetupEnd = 0x00409ff0;
constexpr uint32_t kStackScan = 16u << 10;
// 0x40a080, the pool system's allocation: push ebp; mov ebp, esp; push ebx;
// mov ebx, ecx (the pool; edx the size).
constexpr uint32_t kPoolAlloc = 0x0040a080;
constexpr uint8_t kPoolAllocOrig[] = {0x55, 0x8b, 0xec, 0x53, 0x8b, 0xd9};
constexpr uint32_t kVirtualFreeIat = 0x006cc220;
constexpr uint32_t kFree = 0x00683fc0;
// The release stub and its flag, in .text's zero slack after the automap's.
constexpr uint32_t kReleaseStub = 0x006cb660, kReleaseStubSize = 56;
constexpr uint32_t kReleaseFlag = 0x006cb6a0;
constexpr uint32_t kReleaseSlackEnd = 0x006cb6b0;
// 0x409dd0 sizes each class's blocks: units of 1 KiB and more get
// 0x80000 / unit of them (mov eax, 0x80000 at 0x409fa8; 0x409fad divides
// it by the unit, esi, and multiplies back), 512 KiB blocks. Every pool the
// game creates tops out at 256 KiB units.
constexpr uint32_t kBlockSizeInsn = 0x00409fa8, kBlockSizeNext = 0x00409fad;
constexpr uint8_t kBlockSizeOrig[] = {0xb8, 0x00, 0x00, 0x08, 0x00};
constexpr uint32_t kBlockSizeStub = 0x006cb6b0, kBlockSizeStubEnd = 0x006cb6c0;

uint32_t g_fatal_caller = 0;
uint32_t g_game_limit = 0;      // the cache size the game set
uint32_t g_written_limit = 0;   // the last limit govern_sprite_cache wrote
bool g_release_wanted = false;
std::vector<std::pair<uint32_t, uint32_t>> g_released;   // {memory, header}, for the stub to free
uint8_t *g_release_flag = nullptr;                      // host address of kReleaseFlag

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

bool crit_free(uint32_t va)
{
    return (__atomic_load_n(&wx86_crit_for(va)->owner, __ATOMIC_SEQ_CST) & ~WX86_CRIT_WAITERS) == 0;
}

// Whether another guest thread is creating or destroying a pool system
// (0x409c80-0x409ff0): destruction walks the class lists and frees their
// blocks without taking any lock. A thread inside it, or inside a call it
// made (free), has the address in eip or a return address on its stack.
bool pool_setup_running(d2rt::Cpu &c)
{
    d2rt::ThreadScheduler *s = wx86_sched();
    if (!s)
        return false;
    const d2rt::GuestThread *self = s->current();
    for (uint32_t id : s->live_thread_ids()) {
        const d2rt::GuestThread *t = s->thread_by_id(id);
        if (!t || t == self || !t->started)
            continue;
        if (t->ctx.eip >= kPoolSetup && t->ctx.eip < kPoolSetupEnd)
            return true;
        const uint32_t esp = t->ctx.gpr[d2rt::R_ESP];
        const uint32_t top = t->stack_top - esp > kStackScan ? esp + kStackScan : t->stack_top;
        for (uint32_t a = esp; a + 4 <= top; a += 4) {
            const uint32_t v = c.read_u32(a);
            if (v > kPoolSetup && v <= kPoolSetupEnd)
                return true;
        }
    }
    return false;
}

struct ReleaseTally {
    uint32_t blocks = 0, held = 0, in_use = 0, freed = 0;
};

// Takes one pool system's empty blocks out of it, as 0x409b80 does, into
// g_released. Native code runs without a thread switch, so with the pool's
// lock and every class's lock free no other thread is inside the pool
// system; one that is skips this pool until the next request (taking the
// locks here would not do: 0x409b80 takes the pool's before a class's,
// allocation the reverse). A block whose units are all free has no
// pointer into it left to free.
void take_empty_blocks(d2rt::Cpu &c, uint32_t pool, ReleaseTally &all)
{
    const uint32_t classes = c.read_u32(pool + 0x1c);
    if (!classes || classes > kPoolMaxClasses)
        return;
    if (!crit_free(pool + 4)) {
        rt::logf("pool release: pool %08x busy, skipped", pool);
        return;
    }
    for (uint32_t i = 0; i < classes; i++)
        if (!crit_free(pool + 0x20 + 0x30 * i)) {
            rt::logf("pool release: pool %08x class %u busy, skipped", pool, i);
            return;
        }
    ReleaseTally t;
    for (uint32_t i = 0; i < classes; i++) {
        const uint32_t cls = pool + 0x20 + 0x30 * i;
        const uint32_t units = c.read_u32(cls + 0x1c), bytes = c.read_u32(cls + 0x20);
        const uint32_t header_bytes = 0x18 + ((units + 31) >> 5) * 4;
        for (uint32_t h = c.read_u32(cls + 0x28), n = 0; h && n < 0x10000; n++) {
            const uint32_t next = c.read_u32(h + 0x10), used = c.read_u32(h + 8);
            t.blocks++;
            t.held += bytes;
            if (used) {
                t.in_use += units ? (uint32_t)((uint64_t)bytes * used / units) : 0;
                h = next;
                continue;
            }
            const uint32_t prev = c.read_u32(h + 0xc), mem = c.read_u32(h);
            if (prev)
                c.write_u32(prev + 0x10, next);
            if (next)
                c.write_u32(next + 0xc, prev);
            if (c.read_u32(cls + 0x28) == h)
                c.write_u32(cls + 0x28, next);
            if (c.read_u32(cls + 0x2c) == h)
                c.write_u32(cls + 0x2c, prev);
            c.write_u32(cls + 0x24, c.read_u32(cls + 0x24) - 1);
            // Out of the sorted array, as 0x409490 does (it also shrinks the
            // array, which the game's next removal still will).
            const uint32_t count = c.read_u32(pool + 0x7a0), arr = c.read_u32(pool + 0x7a8);
            for (uint32_t k = 0; k < count; k++)
                if (c.read_u32(arr + 8 * k) == h) {
                    for (uint32_t m = k; m + 1 < count; m++) {
                        c.write_u32(arr + 8 * m, c.read_u32(arr + 8 * (m + 1)));
                        c.write_u32(arr + 8 * m + 4, c.read_u32(arr + 8 * (m + 1) + 4));
                    }
                    c.write_u32(pool + 0x7a0, count - 1);
                    c.write_u32(pool + 0x17a8, c.read_u32(pool + 0x17a8) - bytes - header_bytes);
                    break;
                }
            g_released.emplace_back(mem, h);
            t.freed += bytes;
            h = next;
        }
    }
    char name[33] = {};
    c.read(pool + 0x17ac, name, 32);
    rt::logf("pool release: %08x %s: %u KiB released, %u KiB held in %u blocks, ~%u KiB in use", pool,
             name[0] ? name : "-", t.freed >> 10, (t.held - t.freed) >> 10, t.blocks, t.in_use >> 10);
    all.blocks += t.blocks, all.held += t.held, all.in_use += t.in_use, all.freed += t.freed;
}

// Every pool system's empty blocks: the global one and those the game
// creates (one for the client's game and one for each server-side game).
void take_all_empty_blocks(d2rt::Cpu &c)
{
    if (pool_setup_running(c)) {
        rt::logf("pool release: a pool is being set up or torn down, skipped");
        return;
    }
    uint32_t pools = c.read_u32(kPoolCount);
    if (pools > kPoolSlots)
        pools = kPoolSlots;
    ReleaseTally all;
    for (uint32_t i = 0; i < pools; i++)
        take_empty_blocks(c, kPool + kPoolSize * i, all);
    rt::logf("pool release: all pools: %u KiB released, %u KiB held, ~%u KiB in use", all.freed >> 10,
             (all.held - all.freed) >> 10, all.in_use >> 10);
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

bool install_pool_release(d2rt::Cpu &c)
{
    uint8_t have[sizeof kPoolAllocOrig];
    uint8_t slack[kReleaseSlackEnd - kReleaseStub];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kPoolAlloc, have, sizeof have) || std::memcmp(have, kPoolAllocOrig, sizeof have) != 0 ||
        !c.read(kReleaseStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    g_release_flag = (uint8_t *)c.hostptr(kReleaseFlag, 1);
    // The next block to free: its memory in eax (0: none left), its header in
    // edx. The first call after a request takes the empty blocks out.
    const uint32_t next = rt::native_entry("PoolReleaseNext", 0, [](d2rt::Cpu &c) -> uint32_t {
        if (g_released.empty() && g_release_wanted) {
            g_release_wanted = false;
            take_all_empty_blocks(c);
        }
        if (g_released.empty()) {
            *g_release_flag = 0;
            return 0;
        }
        const auto b = g_released.back();
        g_released.pop_back();
        c.set_reg(d2rt::R_EDX, b.second);
        return b.first;
    });
    if (!next || !g_release_flag)
        return false;
    // S+0  cmp byte [flag], 0; je S+45
    // S+9  pushad
    // S+10 call next; test eax, eax; je S+44
    // S+19 push edx; push 0x8000; push 0; push eax; call [VirtualFree]
    // S+34 call free; add esp, 4; jmp S+10
    // S+44 popad
    // S+45 push ebp; mov ebp, esp; push ebx; mov ebx, ecx; jmp 0x40a086
    uint8_t s[kReleaseStubSize] = {0x80, 0x3d};
    std::memcpy(s + 2, &kReleaseFlag, 4);
    s[6] = 0x00, s[7] = 0x74, s[8] = 45 - 9;
    s[9] = 0x60;
    s[10] = 0xe8;
    put_rel32(s + 11, kReleaseStub + 15, next);
    s[15] = 0x85, s[16] = 0xc0, s[17] = 0x74, s[18] = 44 - 19;
    s[19] = 0x52;
    s[20] = 0x68, s[21] = 0x00, s[22] = 0x80, s[23] = 0x00, s[24] = 0x00;
    s[25] = 0x6a, s[26] = 0x00;
    s[27] = 0x50;
    s[28] = 0xff, s[29] = 0x15;
    std::memcpy(s + 30, &kVirtualFreeIat, 4);
    s[34] = 0xe8;
    put_rel32(s + 35, kReleaseStub + 39, kFree);
    s[39] = 0x83, s[40] = 0xc4, s[41] = 0x04;
    s[42] = 0xeb, s[43] = (uint8_t)(10 - 44);
    s[44] = 0x61;
    std::memcpy(s + 45, kPoolAllocOrig, sizeof kPoolAllocOrig);
    s[51] = 0xe9;
    put_rel32(s + 52, kReleaseStub + 56, kPoolAlloc + sizeof kPoolAllocOrig);
    uint8_t jump[sizeof kPoolAllocOrig] = {0xe9, 0, 0, 0, 0, 0x90};
    put_rel32(jump + 1, kPoolAlloc + 5, kReleaseStub);
    c.write(kReleaseStub, s, sizeof s);
    c.write(kPoolAlloc, jump, sizeof jump);
    c.invalidate_code(kReleaseStub, sizeof s);
    c.invalidate_code(kPoolAlloc, sizeof jump);
    rt::logf("pool release: stub at %08x in front of %08x", kReleaseStub, kPoolAlloc);
    return true;
}

bool set_pool_block_size(d2rt::Cpu &c, uint32_t bytes)
{
    uint8_t have[sizeof kBlockSizeOrig];
    uint8_t slack[kBlockSizeStubEnd - kBlockSizeStub];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kBlockSizeInsn, have, sizeof have) || std::memcmp(have, kBlockSizeOrig, sizeof have) != 0 ||
        !c.read(kBlockSizeStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    // mov eax, bytes; cmp esi, eax; jle +2; mov eax, esi; jmp 0x409fad:
    // a block of `bytes`, or of one unit where a unit is larger.
    uint8_t s[16] = {0xb8, 0, 0, 0, 0, 0x3b, 0xf0, 0x7e, 0x02, 0x8b, 0xc6, 0xe9};
    std::memcpy(s + 1, &bytes, 4);
    put_rel32(s + 12, kBlockSizeStub + 16, kBlockSizeNext);
    uint8_t jump[sizeof kBlockSizeOrig] = {0xe9};
    put_rel32(jump + 1, kBlockSizeInsn + 5, kBlockSizeStub);
    c.write(kBlockSizeStub, s, sizeof s);
    c.write(kBlockSizeInsn, jump, sizeof jump);
    c.invalidate_code(kBlockSizeStub, sizeof s);
    c.invalidate_code(kBlockSizeInsn, sizeof jump);
    rt::logf("pool blocks: %u KiB (the game's own: 512 KiB)", bytes >> 10);
    return true;
}

void request_pool_release()
{
    if (!g_release_flag || g_release_wanted)
        return;
    g_release_wanted = true;
    *g_release_flag = 1;
}

}  // namespace game
