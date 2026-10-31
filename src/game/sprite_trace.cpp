// See sprite_trace.h. The cache's code (0x89db60, d2_memory.cpp):
//
// 0x6001f0 finds a sprite to draw in the game's sprite table (0x88db58,
// 0x60adc0 makes its records): +4 to +0x1c what the table looks sprites up
// by (0x60ace0 compares them: token, component, armour class, mode and
// weapon class at +8 to +0x18, and at +0x1c 0x60ab80's group of directions
// the direction is in), +0x20 the sprite's own
// block in the cache (0x5fff32 passes the entry's memory), +0x24 its cache
// entry. An entry holds that block at +0, its bytes at +4 (0xb8 for the
// block; 0x5ff630 adds the group's frames once decoded and, for a sprite
// made by 0x5fffa0, the file's bytes read for them, which stay counted once
// freed: docs/memory-budget.md, issue #33) and its record at +8. An evicted
// sprite's record goes (0x5fe170), so the next lookup misses and the sprite is
// decoded again into a new block. Found, it reaches 0x600350 with the
// record in eax:
//   mov ebx, [eax+0x20]; test ebx, ebx; je ...
// Only when its caller's second argument ([ebp+0xc]) is 0 does it go on to
// 0x60038b, which moves the entry to the recently used end of the cache's
// list (0x5ff5b0); with it set, the sprite is drawn and left where it was.
// The use records say which: a sprite drawn every frame that way ages in
// the cache as if it were not drawn at all.
//
// 0x5ff4a0 lets an entry go: the least recently used one (edx 0), or the
// one in edx. Both ways reach 0x5ff53c with the entry in edi, under the
// cache's lock:
//   mov ecx, [edi+4]; sub [esi+8], ecx
// and then free its record (0x5ff550), so the record is still there to key.
// Its callers: 0x5ff678 (room for a sprite that grew), 0x5ffbf6 (the
// cache emptied), 0x5ffcca (room for a new sprite), 0x600097 (a sprite
// whose decode failed); ebp is 0x5ff4a0's frame, [ebp+4] the return address.
//
// Each stub, in .text's zero slack after the insertion counter's:
//   cmp byte [flag], 0; je over; <save>; call <native>; <restore>;
//   over: <the original bytes>; jmp back
// The natives run on the guest's thread (the scheduler is cooperative), as
// does the heartbeat, so the file needs no lock of its own.
#include "game/sprite_trace.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace game {
namespace {

constexpr uint32_t kUseAt = 0x00600350, kUseNext = 0x00600355;
constexpr uint8_t kUseOrig[] = {0x8b, 0x58, 0x20, 0x85, 0xdb};
constexpr uint32_t kRemoveAt = 0x005ff53c, kRemoveNext = 0x005ff542;
constexpr uint8_t kRemoveOrig[] = {0x8b, 0x4f, 0x04, 0x29, 0x4e, 0x08};
constexpr uint32_t kFlag = 0x006cb6e0;
constexpr uint32_t kUseStub = 0x006cb6f0, kUseStubSize = 30;
constexpr uint32_t kRemoveStub = 0x006cb720, kRemoveStubSize = 31;
constexpr uint32_t kSlackEnd = 0x006cb740;

constexpr uint32_t kTouched = 1u << 31;   // a use's bytes: the game moved it up its list
constexpr uint32_t kWindowMs = 1000;    // a sprite's uses: the first in each window
constexpr uint32_t kSeen = 4096;        // sprites remembered for that (direct-mapped)
constexpr size_t kFileBuffer = 64u << 10;

struct Seen {
    uint32_t key, window, bytes;
    bool touched;   // a touched use written in this window
};

uint8_t *g_flag = nullptr;   // host address of kFlag
FILE *g_file = nullptr;
char *g_buffer = nullptr;
Seen *g_seen = nullptr;
uint64_t g_start_ms = 0;
bool g_started = false;   // the file has its header: later starts append
uint32_t g_lost = 0;

void put_rel32(uint8_t *at, uint32_t insn_end, uint32_t target)
{
    const uint32_t rel = target - insn_end;
    std::memcpy(at, &rel, 4);
}

uint32_t elapsed_ms() { return (uint32_t)(rt::now_ms() - g_start_ms); }

void record(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    if (!g_file)
        return;
    const uint32_t r[4] = {type << 28 | (elapsed_ms() & 0x0fffffffu), a, b, c};
    if (std::fwrite(r, sizeof r, 1, g_file) != 1)
        g_lost++;
}

// FNV-1a over the record's lookup fields, +4 to +0x1c; never 0 (0: no
// record). Version 2 hashed +0x20 too, the sprite's block, which is new each
// time it is decoded: a sprite evicted and drawn again had a new key.
uint32_t key_of(d2rt::Cpu &c, uint32_t rec)
{
    uint8_t k[0x1c];
    if (!rec || !c.read(rec + 4, k, sizeof k))
        return 0;
    uint32_t h = 2166136261u;
    for (uint8_t b : k)
        h = (h ^ b) * 16777619u;
    return h ? h : 1;
}

void on_use(d2rt::Cpu &c)
{
    const uint32_t rec = c.reg(d2rt::R_EAX);
    const uint32_t entry = rec ? c.read_u32(rec + 0x24) : 0;
    if (!entry || !g_seen)
        return;
    const bool touched = c.read_u32(c.reg(d2rt::R_EBP) + 0xc) == 0;
    const uint32_t key = key_of(c, rec), window = elapsed_ms() / kWindowMs + 1;
    const uint32_t bytes = c.read_u32(entry + 4) & ~kTouched;
    Seen &s = g_seen[key % kSeen];
    if (s.key != key) {
        // Not seen lately (or its slot taken since): what the key is a hash of.
        uint32_t f[9] = {};
        c.read(rec + 4, f, 0x20);
        if (f[7])
            c.read(f[7], &f[8], 4);   // +0x20 points at the sprite's block: its first word
        record(kIdentA, key, f[0], f[1]);
        record(kIdentB, key, f[2], f[3]);
        record(kIdentC, key, f[4], f[5]);
        record(kIdentD, key, f[6], f[7]);
        record(kIdentE, key, f[8], 0);
    }
    if (s.key != key || s.window != window)
        s = {key, window, bytes, false};
    else if (s.bytes == bytes && (s.touched || !touched))
        return;   // again in this window, neither grown nor touched for the first time
    s.bytes = bytes;
    s.touched |= touched;
    record(kUse, key, bytes | (touched ? kTouched : 0), entry);
}

void on_remove(d2rt::Cpu &c)
{
    const uint32_t entry = c.reg(d2rt::R_EDI);
    const uint32_t caller = c.read_u32(c.reg(d2rt::R_EBP) + 4);
    record(kRemove, key_of(c, c.read_u32(entry + 8)), c.read_u32(entry + 4), caller);
}

}  // namespace

bool install_sprite_trace(d2rt::Cpu &c)
{
    uint8_t use[sizeof kUseOrig], remove[sizeof kRemoveOrig], slack[kSlackEnd - kFlag];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kUseAt, use, sizeof use) || std::memcmp(use, kUseOrig, sizeof use) != 0 ||
        !c.read(kRemoveAt, remove, sizeof remove) || std::memcmp(remove, kRemoveOrig, sizeof remove) != 0 ||
        !c.read(kFlag, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    g_flag = (uint8_t *)c.hostptr(kFlag, 1);
    // Both trap natives may change eax, ecx and edx.
    const uint32_t use_native = rt::native_entry("SpriteTraceUse", 0, [](d2rt::Cpu &c) -> uint32_t {
        on_use(c);
        return 0;
    });
    const uint32_t remove_native = rt::native_entry("SpriteTraceRemove", 0, [](d2rt::Cpu &c) -> uint32_t {
        on_remove(c);
        return 0;
    });
    if (!g_flag || !use_native || !remove_native)
        return false;

    // S+0  cmp byte [flag], 0; je S+20
    // S+9  push eax; push ecx; push edx; call use; pop edx; pop ecx; pop eax
    // S+20 mov ebx, [eax+0x20]; test ebx, ebx; jmp 0x600355 (its je)
    uint8_t u[kUseStubSize] = {0x80, 0x3d};
    std::memcpy(u + 2, &kFlag, 4);
    u[6] = 0x00, u[7] = 0x74, u[8] = 20 - 9;
    u[9] = 0x50, u[10] = 0x51, u[11] = 0x52;
    u[12] = 0xe8;
    put_rel32(u + 13, kUseStub + 17, use_native);
    u[17] = 0x5a, u[18] = 0x59, u[19] = 0x58;
    std::memcpy(u + 20, kUseOrig, sizeof kUseOrig);
    u[25] = 0xe9;
    put_rel32(u + 26, kUseStub + 30, kUseNext);

    // S+0  cmp byte [flag], 0; je S+20
    // S+9  push eax; push ecx; push edx; call remove; pop edx; pop ecx; pop eax
    // S+20 mov ecx, [edi+4]; sub [esi+8], ecx; jmp 0x5ff542
    uint8_t r[kRemoveStubSize] = {0x80, 0x3d};
    std::memcpy(r + 2, &kFlag, 4);
    r[6] = 0x00, r[7] = 0x74, r[8] = 20 - 9;
    r[9] = 0x50, r[10] = 0x51, r[11] = 0x52;
    r[12] = 0xe8;
    put_rel32(r + 13, kRemoveStub + 17, remove_native);
    r[17] = 0x5a, r[18] = 0x59, r[19] = 0x58;
    std::memcpy(r + 20, kRemoveOrig, sizeof kRemoveOrig);
    r[26] = 0xe9;
    put_rel32(r + 27, kRemoveStub + 31, kRemoveNext);

    uint8_t use_jump[sizeof kUseOrig] = {0xe9};
    put_rel32(use_jump + 1, kUseAt + 5, kUseStub);
    uint8_t remove_jump[sizeof kRemoveOrig] = {0xe9, 0, 0, 0, 0, 0x90};
    put_rel32(remove_jump + 1, kRemoveAt + 5, kRemoveStub);

    c.write(kUseStub, u, sizeof u);
    c.write(kRemoveStub, r, sizeof r);
    c.write(kUseAt, use_jump, sizeof use_jump);
    c.write(kRemoveAt, remove_jump, sizeof remove_jump);
    c.invalidate_code(kUseStub, sizeof u);
    c.invalidate_code(kRemoveStub, sizeof r);
    c.invalidate_code(kUseAt, sizeof use_jump);
    c.invalidate_code(kRemoveAt, sizeof remove_jump);
    rt::logf("sprite trace: stubs at %08x and %08x, off", kUseAt, kRemoveAt);
    return true;
}

bool sprite_trace_start(const char *path)
{
    if (!g_flag || g_file)
        return false;
    g_buffer = (char *)std::malloc(kFileBuffer);
    g_seen = (Seen *)std::calloc(kSeen, sizeof(Seen));
    // Started again in the same run: on in the same file, on the same clock.
    const bool again = g_started;
    g_file = g_buffer && g_seen ? std::fopen(path, again ? "ab" : "wb") : nullptr;
    if (!g_file) {
        std::free(g_buffer);
        std::free(g_seen);
        g_buffer = nullptr;
        g_seen = nullptr;
        return false;
    }
    std::setvbuf(g_file, g_buffer, _IOFBF, kFileBuffer);
    g_lost = 0;
    if (!again) {
        g_started = true;
        g_start_ms = rt::now_ms();
        uint32_t header[8] = {0, 0, 3, 16, kWindowMs, (uint32_t)std::time(nullptr), 0, 0};
        std::memcpy(header, "D2SPTRC1", 8);
        std::fwrite(header, sizeof header, 1, g_file);
    }
    *g_flag = 1;
    rt::logf("sprite trace: on, %s", path);
    return true;
}

void sprite_trace_stop()
{
    if (!g_file)
        return;
    *g_flag = 0;
    if (g_lost)
        record(kLost, g_lost, 0, 0);
    std::fclose(g_file);
    g_file = nullptr;
    std::free(g_buffer);
    std::free(g_seen);
    g_buffer = nullptr;
    g_seen = nullptr;
    rt::logf("sprite trace: off");
}

bool sprite_trace_on() { return g_file != nullptr; }

void sprite_trace_beat(const SpriteTraceBeat &b)
{
    if (!g_file)
        return;
    record(kCache, b.limit, b.used, b.sprites);
    record(kPool, b.pool_free, b.spare_used, b.largest_free);
    record(kCount, b.inserted, b.cells, b.frames);
    if (g_lost) {
        record(kLost, g_lost, 0, 0);
        g_lost = 0;
    }
    std::fflush(g_file);
}

}  // namespace game
