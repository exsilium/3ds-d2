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
// freed: docs/memory-budget.md, issue #33; with the frames in blocks of
// their own, the class of their unit and no file bytes for a DCC group,
// issue #44) and its record at +8. An evicted
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
#include "game/d2_memory.h"

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
constexpr uint32_t kUseStub = 0x006cb6f0, kUseStubSize = 45;
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

// A record's key, kept for the rest of its window (issue #72): nearly every
// use repeats one made in the same 1 s window, and reading and hashing its
// 0x1c bytes was most of what a use cost after the call-out. Keyed by the
// record (a pool unit) and its cache entry, so a record reused for another
// sprite is hashed again. Direct-mapped; a miss only costs the hash.
struct RecKey {
    uint32_t rec, entry, window, key;
};
constexpr uint32_t kRecKeys = 1024;
RecKey g_rec_keys[kRecKeys];

uint8_t *g_flag = nullptr;   // host address of kFlag
FILE *g_file = nullptr;
char *g_buffer = nullptr;
Seen *g_seen = nullptr;
uint64_t g_start_ms = 0;
bool g_started = false;   // the file has its header: later starts append
uint32_t g_lost = 0;
SpriteTraceWrap g_wrap = nullptr;   // sprite_trace_set_wrap
bool g_each = false;      // sprite_trace_each: every record written out at once

void put_rel32(uint8_t *at, uint32_t insn_end, uint32_t target)
{
    const uint32_t rel = target - insn_end;
    std::memcpy(at, &rel, 4);
}

// The game's own clock (GetTickCount's, rt::tick_ms): taken for every use,
// and a host clock read costs ~1 us on the console (issue #72).
uint32_t elapsed_ms() { return rt::tick_ms() - (uint32_t)g_start_ms; }

void record(uint32_t type, uint32_t a, uint32_t b, uint32_t c)
{
    if (!g_file)
        return;
    const uint32_t r[4] = {type << 28 | (elapsed_ms() & 0x0fffffffu), a, b, c};
    if (std::fwrite(r, sizeof r, 1, g_file) != 1)
        g_lost++;
    else if (g_each)
        std::fflush(g_file);
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

d2rt::Cpu *g_cpu = nullptr;   // the guest's memory, for the in-line path

// A use: the stub's argument is the sprite's record (eax at 0x600350, a
// pool unit: 16-byte aligned), with bit 0 set when the caller's [ebp+0xc]
// is 0 (the game moves the sprite up its list). One argument: the in-line
// path sees the first argument only, not eax or ebp.
void on_use(d2rt::Cpu &c, uint32_t arg)
{
    const uint32_t rec = arg & ~1u;
    const bool touched = arg & 1;
    const uint32_t entry = rec ? c.read_u32(rec + 0x24) : 0;
    if (!entry || !g_seen)
        return;
    const uint32_t window = elapsed_ms() / kWindowMs + 1;
    RecKey &rk = g_rec_keys[(rec >> 4) % kRecKeys];
    if (rk.rec != rec || rk.entry != entry || rk.window != window)
        rk = {rec, entry, window, key_of(c, rec)};
    const uint32_t key = rk.key;
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
    const uint32_t use_native = rt::native_entry("SpriteTraceUse", 1, [](d2rt::Cpu &c) -> uint32_t {
        on_use(c, c.arg(0));
        return 0;
    });
    const uint32_t remove_native = rt::native_entry("SpriteTraceRemove", 0, [](d2rt::Cpu &c) -> uint32_t {
        on_remove(c);
        return 0;
    });
    if (!g_flag || !use_native || !remove_native)
        return false;
    // Served from inside the translated code, as the cache's own natives are
    // (issue #72): through the Bridge's trap, the use hook cost 6.2 ms of a
    // 59 ms fight frame on the console (3DS-D2-10, 2026-11-07), nearly all of
    // it the call-out for uses the window then drops. Both natives only read
    // guest memory and write to the trace's buffer: they neither call guest
    // code nor wait. Armed before the stubs are written, so before either is
    // translated.
    c.set_inline_shim(use_native);
    c.set_inline_shim(remove_native);
    // The use, as the lock calls are (issue #48): served before the Bridge,
    // without the GIL. Under the cooperative scheduler one guest thread runs
    // at a time and only the game's thread draws, so the trace's state needs
    // no lock; the trace's stream locks its own. Under the native scheduler
    // (IntrinFrame::thread set) it declines, and the shim above serves it.
    g_cpu = &c;
    const bool fast = c.set_intrinsic_nogil(use_native, [](d2rt::Cpu::IntrinFrame &f, uint32_t) {
        if (f.thread)
            return false;
        on_use(*g_cpu, f.arg0);
        f.pop = 4;
        return true;
    });

    // S+0  cmp byte [flag], 0; je S+35
    // S+9  push eax; push ecx; push edx
    // S+12 mov ecx, eax; cmp dword [ebp+0xc], 0; sete dl; movzx edx, dl; or ecx, edx
    // S+26 push ecx; S+27 call use (pops it)
    // S+32 pop edx; pop ecx; pop eax
    // S+35 mov ebx, [eax+0x20]; test ebx, ebx; jmp 0x600355 (its je)
    static const uint8_t kArg[] = {0x89, 0xc1, 0x83, 0x7d, 0x0c, 0x00, 0x0f, 0x94,
                                   0xc2, 0x0f, 0xb6, 0xd2, 0x09, 0xd1, 0x51};
    uint8_t u[kUseStubSize] = {0x80, 0x3d};
    std::memcpy(u + 2, &kFlag, 4);
    u[6] = 0x00, u[7] = 0x74, u[8] = 35 - 9;
    u[9] = 0x50, u[10] = 0x51, u[11] = 0x52;
    std::memcpy(u + 12, kArg, sizeof kArg);
    u[27] = 0xe8;
    put_rel32(u + 28, kUseStub + 32, use_native);
    u[32] = 0x5a, u[33] = 0x59, u[34] = 0x58;
    std::memcpy(u + 35, kUseOrig, sizeof kUseOrig);
    u[40] = 0xe9;
    put_rel32(u + 41, kUseStub + 45, kUseNext);
    static_assert(kUseStub + kUseStubSize <= kRemoveStub, "the use stub runs into the remove stub");

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
    rt::logf("sprite trace: stubs at %08x and %08x (in line%s), off", kUseAt, kRemoveAt,
             fast ? ", the use without the GIL" : "");
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
    // Again: at the file's end, without append mode (whose every write on
    // the console seeks to the end and asks the FS for the size).
    g_file = nullptr;
    if (g_buffer && g_seen && again) {
        g_file = std::fopen(path, "r+b");
        if (g_file && std::fseek(g_file, 0, SEEK_END) != 0) {
            std::fclose(g_file);
            g_file = nullptr;
        }
    }
    if (!g_file && g_buffer && g_seen)
        g_file = std::fopen(path, "wb");
    if (!g_file) {
        std::free(g_buffer);
        std::free(g_seen);
        g_buffer = nullptr;
        g_seen = nullptr;
        return false;
    }
    if (FILE *w = g_wrap ? g_wrap(g_file, g_buffer, kFileBuffer) : nullptr)
        g_file = w;   // its own buffer: g_buffer, freed after the stream is closed
    else
        std::setvbuf(g_file, g_buffer, _IOFBF, kFileBuffer);
    g_lost = 0;
    std::memset(g_rec_keys, 0, sizeof g_rec_keys);
    if (!again) {
        g_started = true;
        g_start_ms = rt::tick_ms();
        uint32_t header[8] = {0, 0, 4, 16, kWindowMs, (uint32_t)std::time(nullptr), sprite_frames_on() ? 1u : 0u, 0};
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

void sprite_trace_flush()
{
    if (g_file)
        std::fflush(g_file);
}

void sprite_trace_each(bool on) { g_each = on; }

void sprite_trace_set_wrap(SpriteTraceWrap wrap) { g_wrap = wrap; }

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
