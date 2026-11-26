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
#include "game/native_sites.h"
#include "game/sprite_governor.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace game {
namespace {

// 0x4573e7: mov edx, 0x1800000 -- the floor, taken when the computed size
// is at or below it.
constexpr uint32_t kFloorAt = 0x006cb644;    // the data dword the stub reads, in .text's slack
constexpr uint32_t kFloorOwn = 0x01800000;   // the game's own floor
constexpr uint32_t kSpriteCache = 0x0089db60;

// exit(code): push 0; push 0; push [esp+0xc]; call doexit (0x681d27);
// add esp, 0xc; ret. Rewritten in place as
//   cmp dword [esp+4], -1; jne A; jmp <native>
// with the original's body moved to two runs of int3 padding (no run holds
// all 17 bytes): A = push 0; push 0; push [esp+0xc]; jmp B, and
// B = call doexit; add esp, 0xc; ret.
constexpr uint32_t kExit = 0x00681e09;
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
constexpr uint32_t kReleaseStub = 0x006cb660;
constexpr uint32_t kReleaseFlag = 0x006cb6a0;
// 0x409dd0 sizes each class's blocks: units of 1 KiB and more get
// 0x80000 / unit of them (mov eax, 0x80000 at 0x409fa8; 0x409fad divides
// it by the unit, esi, and multiplies back), 512 KiB blocks. Every pool the
// game creates tops out at 256 KiB units.
constexpr uint32_t kBlockSize = 0x006cb6a4;   // the data dword the stub reads
// 0x5ff3f0, the cache's room for a new sprite, counts it in once it has
// evicted enough: mov edx, [ebp+8]; add dword [edi+0xc], 1;
// add [edi+8], edx (the entry's own bytes: its frames are counted as they
// are decoded); then 0x5ff480 leaves the cache's lock. Every sprite the
// cache takes passes here, so with the entry count the heartbeat knows how
// many it let go of. The stub, in .text's slack after the block size's:
//   call <native>; <the 10 bytes>; jmp 0x5ff480
constexpr uint32_t kInsert = 0x005ff476;
// 0x5ff6e0 decodes a sprite's frames into the cache from its DCC file, once
// the game's asynchronous load of the file (0x40a620, queued at 0x5ffbbd and
// 0x5ff977) is done: the draw's poll of the load (0x40a7f0, from 0x600367)
// runs it on the game's thread. Every first decode, and every decode after
// an eviction, passes there; the file's read does not (Storm's read thread,
// 0x41a550, reads it and signals the load's event, while the game's thread
// goes on drawing without the sprite).
// It takes three stack arguments and returns with ret 0xc; its first bytes
// are push ebp; mov ebp, esp; push ebx; push esi. The stub, in .text's
// slack after the sprite trace's (native_sites_table.h, decode_timer):
//   S+0   push eax; push ecx; push edx; call begin; pop edx; pop ecx; pop eax
//   S+11  push [esp+0xc] three times (the arguments again); call S+52
//   S+28  push eax; push ecx; push edx; call end; pop edx; pop ecx; pop eax
//   S+39  ret 0xc
//   S+52  <the 5 bytes>; jmp 0x5ff6e5
constexpr uint32_t kDecode = 0x005ff6e0;
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
constexpr uint32_t kPoolFree = 0x00409ab0;
constexpr uint8_t kPoolFreeOrig[] = {0x55, 0x8b, 0xec, 0x51, 0x53};
// 0x409c80 destroys a pool system ([esp+4], 0: the global one) and frees its
// blocks: push ebp; mov ebp, esp; sub esp, 8. The frames asked of it go too.
// It does nothing while [0x74f0e8] is 0.
constexpr uint32_t kPoolDestroy = 0x00409c80;
constexpr uint8_t kPoolDestroyOrig[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08};
constexpr uint32_t kPoolsUp = 0x0074f0e8;
// 0x5ff6e0 counts the frames in the cache once decoded: mov ecx, [esi+0x18]
// (the group's OutSizeCoded); push 0; push ecx; then 0x5ff630. Counted as
// the block they hold instead ([esi+0x14] is the frames).
constexpr uint32_t kCountFrames = 0x005ff72d;
constexpr uint8_t kCountFramesOrig[] = {0x8b, 0x4e, 0x18, 0x6a, 0x00, 0x51};
// 0x5fffa0 counts the bytes of the file 0x5ffa20 queued for the group:
// push eax; call 0x5ff630. For a DCC group (bit 1 of the flags its first
// argument points at: 0x5ffa20 queues the file with 0x5ff6e0 to decode it)
// they are freed once decoded (0x40a4c0) and are no part of the sprite; a
// DC6 file's bytes are the sprite.
constexpr uint32_t kCountFile = 0x0060002d;
constexpr uint8_t kCountFileOrig[] = {0x50, 0xe8, 0xfd, 0xf5, 0xff, 0xff};
// The four stubs and the byte map, in .text's zero slack after the native
// checks' (d2_native.cpp), up to .rdata.
constexpr uint32_t kFreeStub = 0x006cb820, kFramesBase = 0x006cb8b8, kFramesLen = 0x006cb8bc, kFramesMap = 0x006cb8c0,
                   kFramesSlackEnd = 0x006cc000;

uint32_t g_fatal_caller = 0;
SpriteGovernor g_governor;
uint32_t g_inserted = 0;   // since the start, wrapping
uint32_t g_decodes = 0;    // sprites decoded since the start, and the time it took
uint64_t g_decode_us = 0;
uint64_t g_decode_from[16] = {};   // by guest thread: its decode under way began (0: none)
uint32_t g_frame_decodes = 0;      // ...of them the frame thread's, for the frame trace
uint64_t g_frame_decode_us = 0;
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
// The groups in blocks of their own (over 256 KiB), by their frames: the
// cache entry each belongs to (SpriteFramesHeld's, at 0x5ff72d with the
// entry in edi), and how many were let go of for another's room.
std::unordered_map<uint32_t, uint32_t> g_big_entries;
uint32_t g_big_let_go = 0, g_big_made_room = 0;
bool g_big_test = false;
uint32_t g_big_min = FrameHeap::kPoolLargest;   // the test: 32 KiB, so that the lane's scripts have such groups

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
    // native_sites_table.h's sprite_cache_floor rows (issue #138): the floor
    // is the data dword's, so the bytes written are the same on every run and
    // the AOT recompiler compiles the code with them. Once: a later call
    // changes the dword alone.
    static bool applied = false;
    if (!bytes)
        bytes = kFloorOwn;
    if (!applied) {
        uint32_t slot = 0;
        if (!c.read(kFloorAt, &slot, 4) || slot != 0)
            return false;
        c.write_u32(kFloorAt, bytes);
        if (!apply_sites(c, "sprite_cache_floor", {})) {
            c.write_u32(kFloorAt, 0);
            return false;
        }
        applied = true;
    } else {
        c.write_u32(kFloorAt, bytes);
    }
    if (bytes != kFloorOwn)
        rt::logf("sprite cache floor: %u KiB (the game's own: 24576 KiB)", bytes >> 10);
    return true;
}

SpriteCacheStats sprite_cache_stats(d2rt::Cpu &c)
{
    return {c.read_u32(kSpriteCache + 8), c.read_u32(kSpriteCache + 4), c.read_u32(kSpriteCache + 0xc)};
}

bool install_sprite_cache_counter(d2rt::Cpu &c)
{
    // eax and ecx are dead there, edx is loaded again after.
    const uint32_t count = rt::native_entry("SpriteCacheInsert", 0, [](d2rt::Cpu &) -> uint32_t {
        __atomic_fetch_add(&g_inserted, 1u, __ATOMIC_RELAXED);
        return 0;
    });
    // call count; the 10 bytes; jmp 0x5ff480 (native_sites_table.h, sprite_cache_counter).
    if (!count || !apply_sites(c, "sprite_cache_counter", {{"SpriteCacheInsert", count}}))
        return false;
    rt::logf("sprite cache: insertions counted at %08x", kInsert);
    return true;
}

bool install_sprite_decode_timer(d2rt::Cpu &c)
{
    // The game's thread decodes, and Storm's read thread in its loads'
    // callbacks (issue #76): a start by thread, the guest's threads taking
    // turns (no lock). A decode does not nest in another on one thread. The
    // frame trace counts the frame thread's alone: the read thread's cost
    // the frames only the turns it takes, counted as their ready time.
    const uint32_t begin = rt::native_entry("SpriteDecodeBegin", 0, [](d2rt::Cpu &) -> uint32_t {
        const int t = rt::current_thread_id();
        if (t >= 0 && t < 16)
            g_decode_from[t] = rt::now_us();
        return 0;
    });
    const uint32_t end = rt::native_entry("SpriteDecodeEnd", 0, [](d2rt::Cpu &) -> uint32_t {
        const int t = rt::current_thread_id();
        if (t >= 0 && t < 16 && g_decode_from[t]) {
            const uint64_t us = rt::now_us() - g_decode_from[t];
            g_decode_us += us;
            g_decodes++;
            if (rt::on_frame_thread()) {
                g_frame_decode_us += us;
                g_frame_decodes++;
            }
            g_decode_from[t] = 0;
        }
        return 0;
    });
    // The stub, the function's first bytes after it, and the jump to it
    // (native_sites_table.h, decode_timer).
    if (!begin || !end || !apply_sites(c, "decode_timer", {{"SpriteDecodeBegin", begin}, {"SpriteDecodeEnd", end}}))
        return false;
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

void sprite_decode_totals(uint32_t &decodes, uint64_t &us)
{
    decodes = g_frame_decodes;
    us = g_frame_decode_us;
}

uint32_t govern_sprite_cache(d2rt::Cpu &c, uint32_t pool_free, bool spare_taken, uint32_t evicted, uint32_t reserve,
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
    const SpriteGovernor::Beat beat{cur, c.read_u32(kSpriteCache + 8), evicted, pool_free, spare_taken};
    const uint32_t want = g_governor.step(beat, reserve, min_limit);
    if (want == cur)
        return cur;
    c.write_u32(kSpriteCache + 4, want);
    rt::logf("sprite cache limit %u KiB (pool free %u KiB%s, working set %u KiB)", want >> 10, pool_free >> 10,
             spare_taken ? ", spare taken" : "", g_governor.working_set() >> 10);
    return want;
}

bool install_fatal_exit(d2rt::Cpu &c)
{
    if (!all_int3(c, kPadA, 15) || !all_int3(c, kPadB, 15))
        return false;
    // The native sees exit's own stack: the caller's return address, then the code.
    const uint32_t trap = rt::native_entry("FatalExit", 0, [](d2rt::Cpu &c) -> uint32_t {
        g_fatal_caller = c.read_u32(c.reg(d2rt::R_ESP));
        rt::logf("game: fatal exit(-1), called from %08x", g_fatal_caller);
        rt::request_exit(0xFFFFFFFFu);
        return 0;
    });
    // native_sites_table.h, fatal_exit.
    if (!trap || !apply_sites(c, "fatal_exit", {{"FatalExit", trap}}))
        return false;
    rt::logf("fatal exit: exit(-1) at %08x ends the run", kExit);
    return true;
}

uint32_t fatal_exit_caller()
{
    return g_fatal_caller;
}

bool install_pool_release(d2rt::Cpu &c)
{
    g_release_flag = (uint8_t *)c.hostptr(kReleaseFlag, 1);
    if (!g_release_flag || *g_release_flag)
        return false;
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
    // The stub in front of 0x40a080 (native_sites_table.h, pool_release):
    // while the flag is set, VirtualFree and free each block `next` hands it.
    if (!next || !apply_sites(c, "pool_release", {{"PoolReleaseNext", next}})) {
        g_release_flag = nullptr;
        return false;
    }
    rt::logf("pool release: stub at %08x in front of %08x", kReleaseStub, kPoolAlloc);
    return true;
}

bool set_pool_block_size(d2rt::Cpu &c, uint32_t bytes)
{
    // 0x409fa8 reads the size from the data dword (native_sites_table.h,
    // pool_block_size): a block of `bytes`, or of one unit where a unit is
    // larger.
    static bool applied = false;
    if (!applied) {
        if (c.read_u32(kBlockSize) != 0)
            return false;
        c.write_u32(kBlockSize, bytes);
        if (!apply_sites(c, "pool_block_size", {})) {
            c.write_u32(kBlockSize, 0);
            return false;
        }
        applied = true;
    } else
        c.write_u32(kBlockSize, bytes);
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

// A group over 256 KiB that found no block (3DS-D2-1F: 390 KiB wanted with
// 5.4 MiB of the pool free in pieces of 320 KiB at the most, the zone for
// such blocks holding five): the cache lets go of sprites that hold a block
// of that kind, the least recently used first, until this one has room.
// What the cache lets go of otherwise is small sprites, whose units leave
// no piece that large. 0x5ff4a0 (ecx the cache, edx the entry, one stack
// argument) lets an entry go as 0x5ff630 has it do for a sprite that grew,
// a moment after this in the same callback; it refuses one in use (+0xc
// bit 1), as the sprite being decoded is.
constexpr uint32_t kCacheLetGo = 0x005ff4a0;
constexpr uint32_t kBigUnused = 1000;   // ms since its last use, for a sprite to be let go of here
uint32_t big_room(d2rt::Cpu &c, uint32_t n, uint32_t pool)
{
    if (n <= g_big_min || g_big_entries.empty())
        return 0;
    // The cache's entries are in two lists (+0x14 the used ones' head, +0x1c
    // the head of those not drawn since they came in; an entry's next at
    // +0x14), each with the tick of its last use at +0x10 (0x5ff61a). Ours
    // among them, the longest unused first, and none used in the last
    // second: a draw up the stack may hold the frames of one it has just
    // fetched (the lane's test, letting go in any order, ended in the
    // loader's check of a handle freed under it, 0x40a9e0).
    struct Old {
        uint32_t frames, tick;
    };
    std::vector<Old> old;
    uint32_t newest = 0, recent = 0;
    for (const uint32_t head : {0x1cu, 0x14u})
        for (uint32_t e = c.read_u32(kSpriteCache + head), k = 0; e && k < 8192; e = c.read_u32(e + 0x14), k++) {
            const uint32_t tick = c.read_u32(e + 0x10);
            if ((int32_t)(tick - newest) > 0 || !newest)
                newest = tick;
            for (const auto &b : g_big_entries)
                if (b.second == e)
                    old.push_back({b.first, tick});
        }
    std::vector<uint32_t> order;
    std::sort(old.begin(), old.end(), [&](const Old &x, const Old &y) { return newest - x.tick > newest - y.tick; });
    for (const Old &o : old) {
        if (newest - o.tick >= kBigUnused)
            order.push_back(o.frames);
        else
            recent++;
    }
    const uint32_t ecx = c.reg(d2rt::R_ECX), edx = c.reg(d2rt::R_EDX);
    uint32_t va = 0, gone = 0, stale = 0, refused = 0;
    for (size_t i = 0; i < order.size() && !va; i++) {
        const auto it = g_big_entries.find(order[i]);
        if (it == g_big_entries.end())
            continue;
        const uint32_t entry = it->second;
        if (!c.read_u32(entry) || (c.read_u32(entry + 0xc) & 2) || !g_frames->unit_bytes(order[i])) {
            stale++;
            continue;
        }
        c.set_reg(d2rt::R_ECX, kSpriteCache);
        c.set_reg(d2rt::R_EDX, entry);
        const uint32_t let = rt::guest_call(c, kCacheLetGo, {0});
        c.set_reg(d2rt::R_ECX, ecx);
        c.set_reg(d2rt::R_EDX, edx);
        g_big_entries.erase(order[i]);   // freed (SpriteFramesFree has it too), or refused: not asked again
        if (!let) {
            refused++;
            continue;
        }
        gone++;
        va = g_frames->alloc(n, pool);
    }
    g_big_let_go += gone;
    g_big_made_room += va != 0;
    rt::logf("sprite frames: %u KiB had no block; %u sprites with blocks of their own let go of (%u used in the last "
             "second, %u in use or gone, %u refused), %s",
             n >> 10, gone, recent, stale, refused, va ? "and it has one" : "and it still has none");
    return va;
}

// 0x60c103: the group's frames, edx bytes, of the pool system in ecx; or,
// with no block to be had, 0x40b430 as the game called it.
uint32_t frames_alloc(d2rt::Cpu &c)
{
    const uint32_t n = c.reg(d2rt::R_EDX), ecx = c.reg(d2rt::R_ECX);
    const uint32_t pool = c.read_u32(kPoolsOn) && ecx ? ecx : kPool;
    // The test (set_big_room_test): every group over 256 KiB first has the
    // cache let go of another, as if it had found no block.
    if (g_big_test)
        if (const uint32_t va = big_room(c, n, pool))
            return va;
    if (const uint32_t va = n ? g_frames->alloc(n, pool) : 0)
        return va;
    if (const uint32_t va = big_room(c, n, pool))
        return va;
    if (g_frames_to_pool++ < 8) {
        rt::logf("sprite frames: %u bytes to the pool system", n);
        // No block of that size in the pool: what its free space is in
        // (issue #70; in 3DS-D2-10 these came with the spare's takes).
        rt::log_pool_pieces("sprite frames to the pool system");
    }
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
        if (g_frames->free(c.arg(0))) {
            g_big_entries.erase(c.arg(0));
            return 1;
        }
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
        if (unit > g_big_min) {
            if (g_big_entries.size() > 256)   // stale ones, of pools destroyed
                g_big_entries.clear();
            g_big_entries[frames] = c.reg(d2rt::R_EDI);
        }
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
    // The stubs and their entries: native_sites_table.h, sprite_frames. The
    // free stub reads the granules' base and length from data.
    c.write_u32(kFramesBase, g_map_base);
    c.write_u32(kFramesLen, map_len);
    if (!apply_sites(c, "sprite_frames",
                     {{"SpriteFramesAlloc", alloc}, {"SpriteFramesFree", free}, {"SpriteFramesPoolGone", destroy},
                      {"SpriteFramesHeld", held}})) {
        c.write_u32(kFramesBase, 0);
        c.write_u32(kFramesLen, 0);
        delete g_frames;
        g_frames = nullptr;
        return false;
    }
    rt::logf("sprite frames: their own blocks, from %08x; the cache counts what they hold", kFramesCall);
    return true;
}

bool sprite_frames_on() { return g_frames != nullptr; }
void set_big_room_test(bool on)
{
    g_big_test = on;
    g_big_min = on ? 32u << 10 : FrameHeap::kPoolLargest;
}

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
    if (g_big_let_go) {
        const size_t at = std::strlen(buf);
        std::snprintf(buf + at, n - at, ", %u let go of for the room of %u", g_big_let_go, g_big_made_room);
    }
    return true;
}

}  // namespace game
