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
//
// The sprites' decoded frames were the pool systems' worst fit: each group
// rounded up to a power of two (37% over their bytes), and a group over
// 256 KiB malloc'd from the heap, the requests of two `heap full` exits
// (issue #33). install_sprite_frames gives them an allocator of their own
// (sprite_frames.h) with finer classes, in blocks of the same shared pool.
#include "game/d2_memory.h"
#include "game/sprite_governor.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>
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
// 0x5ff3f0, the cache's room for a new sprite, counts it in once it has
// evicted enough: mov edx, [ebp+8]; add dword [edi+0xc], 1;
// add [edi+8], edx (the entry's own bytes: its frames are counted as they
// are decoded); then 0x5ff480 leaves the cache's lock. Every sprite the
// cache takes passes here, so with the entry count the heartbeat knows how
// many it let go of. The stub, in .text's slack after the block size's:
//   call <native>; <the 10 bytes>; jmp 0x5ff480
constexpr uint32_t kInsert = 0x005ff476, kInsertNext = 0x005ff480;
constexpr uint8_t kInsertOrig[] = {0x8b, 0x55, 0x08, 0x83, 0x47, 0x0c, 0x01, 0x01, 0x57, 0x08};
constexpr uint32_t kInsertStub = 0x006cb6c0, kInsertStubEnd = 0x006cb6e0;
// 0x5ff6e0 decodes a sprite's frames into the cache from its DCC file, once
// the game's asynchronous load of the file (0x40a620, queued at 0x5ffbbd and
// 0x5ff977) is done: the draw's poll of the load (0x40a7f0, from 0x600367)
// runs it on the game's thread. Every first decode, and every decode after
// an eviction, passes there; the file's read does not (Storm's read thread,
// 0x41a550, reads it and signals the load's event, while the game's thread
// goes on drawing without the sprite).
// It takes three stack arguments and returns with ret 0xc; its first bytes
// are push ebp; mov ebp, esp; push ebx; push esi. The stub, in .text's
// slack after the sprite trace's:
//   S+0   push eax; push ecx; push edx; call begin; pop edx; pop ecx; pop eax
//   S+11  push [esp+0xc] three times (the arguments again); call S+52
//   S+28  push eax; push ecx; push edx; call end; pop edx; pop ecx; pop eax
//   S+39  ret 0xc
//   S+52  <the 5 bytes>; jmp 0x5ff6e5
constexpr uint32_t kDecode = 0x005ff6e0, kDecodeNext = 0x005ff6e5;
constexpr uint8_t kDecodeOrig[] = {0x55, 0x8b, 0xec, 0x53, 0x56};
constexpr uint32_t kDecodeStub = 0x006cb740, kDecodeStubEnd = 0x006cb780;
// The frames of a decoded group (issue #44): 0x60bff0 asks the cache's pool
// system for their block at 0x60c103 (call 0x40b430: ecx the pool, edx the
// bytes, three stack arguments for the file and line). The call goes to a
// native instead, which falls back to 0x40b430 when it has no block.
constexpr uint32_t kFramesCall = 0x0060c103;
constexpr uint8_t kFramesCallOrig[] = {0xe8, 0x28, 0xf3, 0xdf, 0xff};
constexpr uint32_t kPoolAllocChecked = 0x0040b430;
constexpr uint32_t kPoolsOn = 0x00708000;   // 0: 0x40b430 and 0x40b480 pass pool 0 on
// Every free of the pool systems, 0x40b480's and the others', reaches
// 0x409ab0 (ecx the pool, edx where the pointer is, two stack arguments),
// whose first bytes are push ebp; mov ebp, esp; push ecx; push ebx. Its stub
// sends a pointer into a 64 KiB granule the frames hold (a byte map, after
// the stubs) to a native, and anything else on.
constexpr uint32_t kPoolFree = 0x00409ab0, kPoolFreeNext = 0x00409ab5;
constexpr uint8_t kPoolFreeOrig[] = {0x55, 0x8b, 0xec, 0x51, 0x53};
// 0x409c80 destroys a pool system ([esp+4], 0: the global one) and frees its
// blocks: push ebp; mov ebp, esp; sub esp, 8. The frames asked of it go too.
// It does nothing while [0x74f0e8] is 0.
constexpr uint32_t kPoolDestroy = 0x00409c80, kPoolDestroyNext = 0x00409c86;
constexpr uint8_t kPoolDestroyOrig[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08};
constexpr uint32_t kPoolsUp = 0x0074f0e8;
// 0x5ff6e0 counts the frames in the cache once decoded: mov ecx, [esi+0x18]
// (the group's OutSizeCoded); push 0; push ecx; then 0x5ff630. Counted as
// the block they hold instead ([esi+0x14] is the frames).
constexpr uint32_t kCountFrames = 0x005ff72d, kCountFramesNext = 0x005ff733;
constexpr uint8_t kCountFramesOrig[] = {0x8b, 0x4e, 0x18, 0x6a, 0x00, 0x51};
// 0x5fffa0 counts the bytes of the file 0x5ffa20 queued for the group:
// push eax; call 0x5ff630. For a DCC group (bit 1 of the flags its first
// argument points at: 0x5ffa20 queues the file with 0x5ff6e0 to decode it)
// they are freed once decoded (0x40a4c0) and are no part of the sprite; a
// DC6 file's bytes are the sprite.
constexpr uint32_t kCountFile = 0x0060002d, kCountFileNext = 0x00600033;
constexpr uint8_t kCountFileOrig[] = {0x50, 0xe8, 0xfd, 0xf5, 0xff, 0xff};
constexpr uint32_t kCacheCount = 0x005ff630;
// The four stubs and the byte map, in .text's zero slack after the native
// checks' (d2_native.cpp), up to .rdata.
constexpr uint32_t kFreeStub = 0x006cb820, kDestroyStub = 0x006cb860, kCountFramesStub = 0x006cb880,
                   kCountFileStub = 0x006cb8a0, kFramesMap = 0x006cb8c0, kFramesSlackEnd = 0x006cc000;

uint32_t g_fatal_caller = 0;
SpriteGovernor g_governor;
uint32_t g_inserted = 0;   // since the start, wrapping
uint32_t g_decodes = 0;    // sprites decoded since the start, and the time it took
uint64_t g_decode_us = 0;
uint64_t g_decode_from = 0;   // the decode under way began (0: none)
struct {
    uint32_t inserted, sprites, decodes;
    uint64_t decode_us;
    bool valid;
} g_window_mark{};
bool g_release_wanted = false;
std::vector<std::pair<uint32_t, uint32_t>> g_released;   // {memory, header}, for the stub to free
uint8_t *g_release_flag = nullptr;                      // host address of kReleaseFlag
FrameHeap *g_frames = nullptr;
uint8_t *g_frames_map = nullptr;   // host address of kFramesMap: a byte per 64 KiB granule from g_map_base
uint32_t g_map_base = 0;
uint32_t g_frames_to_pool = 0, g_frames_dropped = 0, g_frames_strays = 0;

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

bool install_sprite_cache_counter(d2rt::Cpu &c)
{
    uint8_t have[sizeof kInsertOrig];
    uint8_t slack[kInsertStubEnd - kInsertStub];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kInsert, have, sizeof have) || std::memcmp(have, kInsertOrig, sizeof have) != 0 ||
        !c.read(kInsertStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    // eax and ecx are dead there, edx is loaded again after.
    const uint32_t count = rt::native_entry("SpriteCacheInsert", 0, [](d2rt::Cpu &) -> uint32_t {
        __atomic_fetch_add(&g_inserted, 1u, __ATOMIC_RELAXED);
        return 0;
    });
    if (!count)
        return false;
    uint8_t s[5 + sizeof kInsertOrig + 5] = {0xe8};
    put_rel32(s + 1, kInsertStub + 5, count);
    std::memcpy(s + 5, kInsertOrig, sizeof kInsertOrig);
    s[15] = 0xe9;
    put_rel32(s + 16, kInsertStub + sizeof s, kInsertNext);
    uint8_t jump[sizeof kInsertOrig] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    put_rel32(jump + 1, kInsert + 5, kInsertStub);
    c.write(kInsertStub, s, sizeof s);
    c.write(kInsert, jump, sizeof jump);
    c.invalidate_code(kInsertStub, sizeof s);
    c.invalidate_code(kInsert, sizeof jump);
    rt::logf("sprite cache: insertions counted at %08x", kInsert);
    return true;
}

bool install_sprite_decode_timer(d2rt::Cpu &c)
{
    uint8_t have[sizeof kDecodeOrig];
    uint8_t slack[kDecodeStubEnd - kDecodeStub];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kDecode, have, sizeof have) || std::memcmp(have, kDecodeOrig, sizeof have) != 0 ||
        !c.read(kDecodeStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    // The game's thread alone decodes, and the guest's threads take turns:
    // no lock. A decode does not nest in another.
    const uint32_t begin = rt::native_entry("SpriteDecodeBegin", 0, [](d2rt::Cpu &) -> uint32_t {
        g_decode_from = rt::now_us();
        return 0;
    });
    const uint32_t end = rt::native_entry("SpriteDecodeEnd", 0, [](d2rt::Cpu &) -> uint32_t {
        if (g_decode_from) {
            g_decode_us += rt::now_us() - g_decode_from;
            g_decodes++;
            g_decode_from = 0;
        }
        return 0;
    });
    if (!begin || !end)
        return false;
    uint8_t s[kDecodeStubEnd - kDecodeStub] = {};
    const uint8_t save[] = {0x50, 0x51, 0x52}, restore[] = {0x5a, 0x59, 0x58};
    const uint8_t arg[] = {0xff, 0x74, 0x24, 0x0c};   // push dword [esp+0xc]
    uint32_t at = 0;
    auto put = [&](const uint8_t *b, uint32_t n) {
        std::memcpy(s + at, b, n);
        at += n;
    };
    auto call = [&](uint8_t op, uint32_t target) {
        s[at] = op;
        put_rel32(s + at + 1, kDecodeStub + at + 5, target);
        at += 5;
    };
    put(save, 3), call(0xe8, begin), put(restore, 3);
    put(arg, 4), put(arg, 4), put(arg, 4);
    call(0xe8, kDecodeStub + 52);
    put(save, 3), call(0xe8, end), put(restore, 3);
    const uint8_t ret[] = {0xc2, 0x0c, 0x00};
    put(ret, 3);
    at = 52;
    put(kDecodeOrig, sizeof kDecodeOrig);
    call(0xe9, kDecodeNext);
    uint8_t jump[sizeof kDecodeOrig] = {0xe9};
    put_rel32(jump + 1, kDecode + 5, kDecodeStub);
    c.write(kDecodeStub, s, sizeof s);
    c.write(kDecode, jump, sizeof jump);
    c.invalidate_code(kDecodeStub, sizeof s);
    c.invalidate_code(kDecode, sizeof jump);
    rt::logf("sprite cache: decodes timed at %08x", kDecode);
    return true;
}

SpriteCacheWindow sprite_cache_window(d2rt::Cpu &c)
{
    const uint32_t inserted = __atomic_load_n(&g_inserted, __ATOMIC_RELAXED);
    const uint32_t sprites = c.read_u32(kSpriteCache + 0xc);
    SpriteCacheWindow w{};
    if (g_window_mark.valid) {
        w.inserted = inserted - g_window_mark.inserted;
        w.decodes = g_decodes - g_window_mark.decodes;
        w.decode_ms = (uint32_t)((g_decode_us - g_window_mark.decode_us) / 1000);
        // Out: what came in less what the count grew by. A cache the game
        // re-created (its count back to 0) gives less than 0: nothing known.
        const int64_t out = (int64_t)w.inserted - ((int64_t)sprites - g_window_mark.sprites);
        w.evicted = out > 0 ? (uint32_t)out : 0;
    }
    g_window_mark = {inserted, sprites, g_decodes, g_decode_us, true};
    return w;
}

uint32_t sprite_cache_taken() { return __atomic_load_n(&g_inserted, __ATOMIC_RELAXED); }

uint32_t govern_sprite_cache(d2rt::Cpu &c, uint32_t pool_free, bool spare_used, uint32_t evicted, uint32_t reserve,
                             uint32_t min_limit)
{
    const uint32_t cur = c.read_u32(kSpriteCache + 4);
    if (!cur)
        return 0;   // no cache yet
    // What the cache evicts goes back to the frames' blocks, which the pool
    // has back only once they empty: their free units count with its free
    // space, as the pool systems' would if they could be told apart.
    if (g_frames)
        pool_free += g_frames->stats().held - g_frames->stats().in_use;
    const SpriteGovernor::Beat beat{cur, c.read_u32(kSpriteCache + 8), evicted, pool_free, spare_used};
    const uint32_t want = g_governor.step(beat, reserve, min_limit);
    if (want == cur)
        return cur;
    c.write_u32(kSpriteCache + 4, want);
    rt::logf("sprite cache limit %u KiB (pool free %u KiB%s, working set %u KiB)", want >> 10, pool_free >> 10,
             spare_used ? ", spare used" : "", g_governor.working_set() >> 10);
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

namespace {

bool bytes_are(d2rt::Cpu &c, uint32_t va, const uint8_t *want, uint32_t n)
{
    uint8_t have[16];
    return n <= sizeof have && c.read(va, have, n) && std::memcmp(have, want, n) == 0;
}

void mark_granules(uint32_t va, uint32_t bytes, uint8_t v)
{
    for (uint32_t g = (va - g_map_base) >> 16, end = g + (bytes >> 16); g < end; g++)
        g_frames_map[g] = v;
}

// 0x60c103: the group's frames, edx bytes, of the pool system in ecx; or,
// with no block to be had, 0x40b430 as the game called it.
uint32_t frames_alloc(d2rt::Cpu &c)
{
    const uint32_t n = c.reg(d2rt::R_EDX), ecx = c.reg(d2rt::R_ECX);
    const uint32_t pool = c.read_u32(kPoolsOn) && ecx ? ecx : kPool;
    if (const uint32_t va = n ? g_frames->alloc(n, pool) : 0)
        return va;
    if (g_frames_to_pool++ < 8)
        rt::logf("sprite frames: %u bytes to the pool system", n);
    // The bridge pops the return address and the three arguments from the
    // ESP set here, then goes to 0x40b430 with the call's own frame.
    c.set_reg(d2rt::R_ESP, c.reg(d2rt::R_ESP) - 16);
    rt::bridge().redirect_next(kPoolAllocChecked);
    return c.reg(d2rt::R_EAX);
}

}  // namespace

bool install_sprite_frames(d2rt::Cpu &c)
{
    // The pool the blocks come from: the map covers it in 64 KiB granules.
    const rt::Layout &L = rt::layout();
    const bool shared = L.heap_base + L.heap_size == L.va_base;
    g_map_base = (shared ? L.heap_base : L.va_base) & ~0xffffu;
    const uint32_t map_len = L.va_base + L.va_size - g_map_base;
    static const uint8_t kZero[kFramesSlackEnd - kFreeStub] = {};
    if (!bytes_are(c, kFramesCall, kFramesCallOrig, sizeof kFramesCallOrig) ||
        !bytes_are(c, kPoolFree, kPoolFreeOrig, sizeof kPoolFreeOrig) ||
        !bytes_are(c, kPoolDestroy, kPoolDestroyOrig, sizeof kPoolDestroyOrig) ||
        !bytes_are(c, kCountFrames, kCountFramesOrig, sizeof kCountFramesOrig) ||
        !bytes_are(c, kCountFile, kCountFileOrig, sizeof kCountFileOrig) || (map_len >> 16) > kFramesSlackEnd - kFramesMap)
        return false;
    std::vector<uint8_t> slack(sizeof kZero);
    if (!c.read(kFreeStub, slack.data(), (uint32_t)slack.size()) || std::memcmp(slack.data(), kZero, sizeof kZero))
        return false;
    // Data: written through the host view (no code is translated from it).
    g_frames_map = (uint8_t *)c.hostptr(kFramesMap, map_len >> 16);
    if (!g_frames_map)
        return false;
    const uint32_t alloc = rt::native_entry("SpriteFramesAlloc", 3, frames_alloc);
    // A pointer in a granule the frames hold that is not one of their units
    // (none should be) goes on to the pool system.
    const uint32_t free = rt::native_entry("SpriteFramesFree", 1, [](d2rt::Cpu &c) -> uint32_t {
        if (g_frames->free(c.arg(0)))
            return 1;
        if (g_frames_strays++ < 8)
            rt::logf("sprite frames: %08x freed is not a unit, to the pool system", c.arg(0));
        return 0;
    });
    const uint32_t destroy = rt::native_entry("SpriteFramesPoolGone", 1, [](d2rt::Cpu &c) -> uint32_t {
        // As 0x409c80: nothing is destroyed while the pool systems are off,
        // nor a pool without classes.
        const uint32_t pool = c.arg(0) ? c.arg(0) : kPool;
        if (!c.read_u32(kPoolsUp) || !c.read_u32(pool + 0x1c))
            return 0;
        if (const uint32_t bytes = g_frames->drop_owner(pool)) {
            g_frames_dropped += bytes;
            rt::logf("sprite frames: pool %08x destroyed, %u KiB of blocks with it", pool, bytes >> 10);
        }
        return 0;
    });
    // The bytes the cache counts for a decoded group: the class of its unit,
    // or what the pool system took for it.
    const uint32_t held = rt::native_entry("SpriteFramesHeld", 2, [](d2rt::Cpu &c) -> uint32_t {
        const uint32_t frames = c.arg(0), n = c.arg(1);
        if (!frames)
            return n;
        const uint32_t unit = g_frames->unit_bytes(frames);
        return unit ? unit : FrameHeap::pool_bytes(n);
    });
    if (!alloc || !free || !destroy || !held)
        return false;
    g_frames = new FrameHeap({[](uint32_t bytes) -> uint32_t {
                                  const uint32_t va = rt::va_alloc(bytes, kFramesCall + 5);
                                  if (va)
                                      mark_granules(va, bytes, 1);
                                  return va;
                              },
                              [](uint32_t va, uint32_t bytes) {
                                  mark_granules(va, bytes, 0);
                                  rt::va_free(va);
                              }});
    uint8_t s[kFramesSlackEnd - kFreeStub] = {};
    uint32_t at = 0;
    auto put = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t x : b)
            s[at++] = x;
    };
    auto put32 = [&](uint32_t v) {
        std::memcpy(s + at, &v, 4);
        at += 4;
    };
    auto rel = [&](uint8_t op, uint32_t target) {
        s[at] = op;
        put_rel32(s + at + 1, kFreeStub + at + 5, target);
        at += 5;
    };
    // kFreeStub:
    //   +0  mov eax, [edx]; sub eax, base; cmp eax, len; jae +44
    //   +14 shr eax, 16; cmp byte [eax+map], 0; je +44
    //   +26 push ecx; push edx; push dword [edx]; call free; pop edx; pop ecx
    //   +37 test eax, eax; je +44; ret 8
    //   +44 push ebp; mov ebp, esp; push ecx; push ebx; jmp 0x409ab5
    put({0x8b, 0x02, 0x2d}), put32(g_map_base), put({0x3d}), put32(map_len), put({0x73, 44 - 14});
    put({0xc1, 0xe8, 0x10, 0x80, 0xb8}), put32(kFramesMap), put({0x00, 0x74, 44 - 26});
    put({0x51, 0x52, 0xff, 0x32}), rel(0xe8, free), put({0x5a, 0x59});
    put({0x85, 0xc0, 0x74, 44 - 41, 0xc2, 0x08, 0x00});
    put({0x55, 0x8b, 0xec, 0x51, 0x53}), rel(0xe9, kPoolFreeNext);
    // kDestroyStub: push ecx; push edx; push dword [esp+0xc]; call destroy;
    // pop edx; pop ecx; the 6 bytes; jmp 0x409c86
    at = kDestroyStub - kFreeStub;
    put({0x51, 0x52, 0xff, 0x74, 0x24, 0x0c}), rel(0xe8, destroy), put({0x5a, 0x59});
    put({0x55, 0x8b, 0xec, 0x83, 0xec, 0x08}), rel(0xe9, kPoolDestroyNext);
    // kCountFramesStub: push dword [esi+0x18]; push dword [esi+0x14];
    // call held; push 0; push eax; jmp 0x5ff733 (ecx and edx are loaded
    // again there).
    at = kCountFramesStub - kFreeStub;
    put({0xff, 0x76, 0x18, 0xff, 0x76, 0x14}), rel(0xe8, held), put({0x6a, 0x00, 0x50}), rel(0xe9, kCountFramesNext);
    // kCountFileStub: push eax; mov eax, [ebp+8]; test byte [eax], 2; pop eax;
    // je +12; xor eax, eax; push eax; call 0x5ff630; jmp 0x600033
    at = kCountFileStub - kFreeStub;
    put({0x50, 0x8b, 0x45, 0x08, 0xf6, 0x00, 0x02, 0x58, 0x74, 0x02, 0x33, 0xc0, 0x50});
    rel(0xe8, kCacheCount), rel(0xe9, kCountFileNext);
    c.write(kFreeStub, s, kFramesMap - kFreeStub);
    c.invalidate_code(kFreeStub, kFramesMap - kFreeStub);
    auto jump = [&](uint32_t va, uint32_t n, uint32_t stub) {
        uint8_t j[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
        put_rel32(j + 1, va + 5, stub);
        c.write(va, j, n);
        c.invalidate_code(va, n);
    };
    uint8_t call[5] = {0xe8};
    put_rel32(call + 1, kFramesCall + 5, alloc);
    c.write(kFramesCall, call, sizeof call);
    c.invalidate_code(kFramesCall, sizeof call);
    jump(kPoolFree, sizeof kPoolFreeOrig, kFreeStub);
    jump(kPoolDestroy, sizeof kPoolDestroyOrig, kDestroyStub);
    jump(kCountFrames, sizeof kCountFramesOrig, kCountFramesStub);
    jump(kCountFile, sizeof kCountFileOrig, kCountFileStub);
    rt::logf("sprite frames: their own blocks, from %08x; the cache counts what they hold", kFramesCall);
    return true;
}

bool sprite_frames_on() { return g_frames != nullptr; }

SpriteFramesStats sprite_frames_stats()
{
    SpriteFramesStats s{};
    if (g_frames)
        s.heap = g_frames->stats();
    s.to_pool = g_frames_to_pool;
    s.dropped = g_frames_dropped;
    return s;
}

bool sprite_frames_line(char *buf, size_t n)
{
    if (!g_frames)
        return false;
    const FrameHeap::Stats &s = g_frames->stats();
    std::snprintf(buf, n,
                  "sprite frames %u/%u KiB in %u blocks, %u units asking %u KiB, blocks taken %u given %u, "
                  "over 256 KiB %u, to the pool %u",
                  s.in_use >> 10, s.held >> 10, s.blocks, s.units, s.asked >> 10, s.taken, s.given, s.big,
                  g_frames_to_pool);
    return true;
}

}  // namespace game
