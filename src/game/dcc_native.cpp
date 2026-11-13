// See dcc_native.h.
#include "game/dcc_native.h"

#include "game/dcc_decode.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <atomic>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace game {

namespace {

// 0x60bff0's per-direction loop starts at 0x60c10a, once the group's frames
// are allocated (ESI) and the lock taken:
//   60c10a  8b 45 14            mov  eax, [ebp+0x14]      the first direction
//   60c10d  3b 85 c0 fc ff ff   cmp  eax, [ebp-0x340]     ... against the end
//   60c113  89 b5 8c fc ff ff   mov  [ebp-0x374], esi
//   60c119  89 b5 d0 fc ff ff   mov  [ebp-0x330], esi     the frames' cursor
//   60c11f  89 85 cc fc ff ff   mov  [ebp-0x334], eax
//   60c125  0f 8d 93 0b 00 00   jge  0x60ccbe             nothing to decode: the end
// It becomes a call to the native (which does the two stores) and the same
// compare: the native returns the end when it decoded the group, the first
// direction when the game's code should.
constexpr uint32_t kLoop = 0x0060c10a;
constexpr uint8_t kLoopOrig[21] = {0x8b, 0x45, 0x14, 0x3b, 0x85, 0xc0, 0xfc, 0xff, 0xff, 0x89, 0xb5,
                                   0x8c, 0xfc, 0xff, 0xff, 0x89, 0xb5, 0xd0, 0xfc, 0xff, 0xff};
constexpr uint32_t kLoopNext = 0x0060c11f;
// The lane's check: 0x5ff6e0, the load's callback, just after 0x60bff0
// returns:
//   5ff723  89 46 14      mov  [esi+0x14], eax   the group's frames
//   5ff726  83 67 0c fd   and  dword [edi+0xc], ~2
constexpr uint32_t kAfter = 0x005ff723;
constexpr uint8_t kAfterOrig[7] = {0x89, 0x46, 0x14, 0x83, 0x67, 0x0c, 0xfd};

std::atomic<uint32_t> g_mode{0};
d2rt::Cpu *g_cpu = nullptr;
uint8_t *g_base = nullptr;   // guest address 0's host address
DccScratch *g_scratch_game = nullptr, *g_scratch_worker = nullptr;
DccStats g_stats;
std::atomic<uint64_t> g_worker_us{0};
std::atomic<uint32_t> g_worker_refused{0};

// The check's expected bytes, from the native at kLoop to the callback.
uint32_t g_check_out = 0;
std::vector<uint8_t> g_check_bytes;
bool g_check_pending = false;

void put_rel32(uint8_t *p, uint32_t from_next, uint32_t to) { uint32_t r = to - from_next; std::memcpy(p, &r, 4); }

// Up to `want` bytes from `va` the host can read: the file's bytes reach
// the arena's end at the most.
size_t readable(uint32_t va, size_t want)
{
    while (want >= 4096) {
        if (g_cpu->hostptr(va, (uint32_t)want))
            return want;
        want >>= 1;
    }
    return g_cpu->hostptr(va, (uint32_t)want) ? want : 0;
}

// The decode, from an offload_job's arguments: a[0] the directions' bytes,
// a[1] how many are readable, a[2] directions, a[3] frames a direction,
// a[4] the frames' buffer, a[5] its size, a[6] the first direction, a[7]
// the end. Returns the end when decoded, the first direction otherwise.
uint32_t decode(const uint32_t *a, DccScratch &s)
{
    DccGroup g;
    g.src = g_base + a[0];
    g.src_len = a[1];
    g.dirs = a[2];
    g.frames = a[3];
    g.out = g_base + a[4];
    g.out_len = a[5];
    return dcc_decode_group(g, s) ? a[7] : a[6];
}

uint32_t worker_job(const uint32_t *a)
{
    const uint64_t t0 = rt::now_us();
    const uint32_t r = decode(a, *g_scratch_worker);
    g_worker_us.fetch_add(rt::now_us() - t0, std::memory_order_relaxed);
    if (r != a[7])
        g_worker_refused.fetch_add(1, std::memory_order_relaxed);
    return r;
}

uint32_t loop_native(d2rt::Cpu &c)
{
    const uint32_t ebp = c.reg(d2rt::R_EBP), esi = c.reg(d2rt::R_ESI);
    c.write_u32(ebp - 0x374, esi);
    c.write_u32(ebp - 0x330, esi);
    const uint32_t first = c.read_u32(ebp + 0x14), end = c.read_u32(ebp - 0x340);
    const DccMode mode = (DccMode)g_mode.load(std::memory_order_relaxed);
    if (mode == DccMode::Off || (int32_t)first >= (int32_t)end) {
        g_stats.game += (int32_t)first < (int32_t)end;
        return first;
    }
    const uint32_t src = c.read_u32(ebp - 0x320);
    const uint32_t frames = c.read_u32(ebp - 0x3f4);
    const uint32_t out_len = c.read_u32(c.read_u32(ebp + 0x1c));
    const size_t src_len = readable(src, 4u << 20);
    if (!src_len || !c.hostptr(esi, out_len)) {
        g_stats.refused++;
        g_stats.game++;
        return first;
    }
    const uint32_t a[8] = {src, (uint32_t)src_len, end - first, frames, esi, out_len, first, end};
    if (mode == DccMode::Check) {
        // Into a copy of the frames' buffer as it is now; the game's code
        // then decodes into the buffer itself, and the callback compares.
        g_check_bytes.assign(g_base + esi, g_base + esi + out_len);
        DccGroup g;
        g.src = g_base + src;
        g.src_len = src_len;
        g.dirs = end - first;
        g.frames = frames;
        g.out = g_check_bytes.data();
        g.out_len = out_len;
        const uint64_t t0 = rt::now_us();
        g_check_pending = dcc_decode_group(g, *g_scratch_game);
        g_stats.native_us += rt::now_us() - t0;
        g_check_out = esi;
        if (!g_check_pending)
            g_stats.refused++;
        g_stats.game++;
        return first;
    }
    const bool frame = rt::on_frame_thread();
    if (mode == DccMode::Offload && !frame) {
        uint32_t ret = 0;
        if (rt::offload_job(worker_job, a, 8, ret)) {
            g_stats.offloaded++;
            return ret;   // EAX becomes the job's result when the thread wakes
        }
        g_stats.worker_busy++;
    }
    if (frame) {
        g_stats.on_frame++;
        // Who decodes on the frame thread: each different EBP chain, once
        // (20 at the most).
        static std::vector<std::string> chains;
        if (chains.size() < 20) {
            char chain[160];
            int n = 0;
            uint32_t fp = c.read_u32(ebp);
            for (int k = 0; k < 8 && fp && n < (int)sizeof chain - 10; k++) {
                n += std::snprintf(chain + n, sizeof chain - n, " %06x", (unsigned)c.read_u32(fp + 4));
                fp = c.read_u32(fp);
            }
            chain[n] = 0;
            if (std::find(chains.begin(), chains.end(), std::string(chain)) == chains.end()) {
                chains.emplace_back(chain);
                rt::logf("dcc: decoded on the frame thread from%s", chain);
            }
        }
    }
    const uint64_t t0 = rt::now_us();
    const uint32_t r = decode(a, *g_scratch_game);
    g_stats.native_us += rt::now_us() - t0;
    if (r == end) {
        g_stats.native++;
    } else {
        g_stats.refused++;
        g_stats.game++;
    }
    return r;
}

uint32_t after_native(d2rt::Cpu &c)
{
    const uint32_t eax = c.reg(d2rt::R_EAX), esi = c.reg(d2rt::R_ESI), edi = c.reg(d2rt::R_EDI);
    c.write_u32(esi + 0x14, eax);
    c.write_u32(edi + 0xc, c.read_u32(edi + 0xc) & ~2u);
    if (g_check_pending && eax == g_check_out) {
        g_check_pending = false;
        g_stats.checked++;
        const uint8_t *game = g_base + eax;
        if (std::memcmp(game, g_check_bytes.data(), g_check_bytes.size()) != 0) {
            size_t at = 0;
            while (at < g_check_bytes.size() && game[at] == g_check_bytes[at])
                at++;
            if (++g_stats.differed <= 20)
                rt::logf("dcc check: group %08x (%u bytes) differs from byte %u: game %02x, native %02x",
                         (unsigned)eax, (unsigned)g_check_bytes.size(), (unsigned)at, game[at], g_check_bytes[at]);
        }
    }
    return eax;
}

}  // namespace

bool install_dcc_native(d2rt::Cpu &c, DccMode mode)
{
    if (const char *e = std::getenv("D2_DCC"))
        mode = (DccMode)std::atoi(e);
    uint8_t have[sizeof kLoopOrig];
    if (!c.read(kLoop, have, sizeof have) || std::memcmp(have, kLoopOrig, sizeof have) != 0)
        return false;
    g_cpu = &c;
    g_base = (uint8_t *)c.hostptr(kLoop) - kLoop;
    g_scratch_game = dcc_scratch_new();
    g_scratch_worker = dcc_scratch_new();
    const uint32_t native = rt::native_entry("DccDecode", 0, loop_native);
    if (!native)
        return false;
    // call native; cmp eax, [ebp-0x340]; nops to 0x60c11f.
    uint8_t p[sizeof kLoopOrig];
    std::memset(p, 0x90, sizeof p);
    p[0] = 0xe8;
    put_rel32(p + 1, kLoop + 5, native);
    const uint8_t cmp[6] = {0x3b, 0x85, 0xc0, 0xfc, 0xff, 0xff};
    std::memcpy(p + 5, cmp, sizeof cmp);
    static_assert(kLoop + sizeof kLoopOrig == kLoopNext, "the patch ends where the game's code goes on");
    c.write(kLoop, p, sizeof p);
    c.invalidate_code(kLoop, sizeof p);
    if (mode == DccMode::Check) {
        uint8_t a[sizeof kAfterOrig];
        if (c.read(kAfter, a, sizeof a) && std::memcmp(a, kAfterOrig, sizeof a) == 0) {
            const uint32_t after = rt::native_entry("DccCheck", 0, after_native);
            uint8_t q[sizeof kAfterOrig] = {0xe8, 0, 0, 0, 0, 0x90, 0x90};
            put_rel32(q + 1, kAfter + 5, after);
            c.write(kAfter, q, sizeof q);
            c.invalidate_code(kAfter, sizeof q);
        }
    }
    g_mode.store((uint32_t)mode, std::memory_order_relaxed);
    rt::logf("dcc: the native decoder at %08x, %s", kLoop,
             mode == DccMode::Off       ? "off"
             : mode == DccMode::InPlace ? "in place"
             : mode == DccMode::Offload ? "on the I/O worker off the frame thread"
                                        : "checked against the game's code");
    return true;
}

void set_dcc_mode(DccMode mode)
{
    if (mode != DccMode::Check)   // the check needs its hook, installed or not at the start
        g_mode.store((uint32_t)mode, std::memory_order_relaxed);
}

DccMode dcc_mode() { return (DccMode)g_mode.load(std::memory_order_relaxed); }

DccStats dcc_stats()
{
    DccStats s = g_stats;
    s.native_us += g_worker_us.load(std::memory_order_relaxed);
    const uint32_t refused = g_worker_refused.load(std::memory_order_relaxed);
    s.refused += refused;
    s.game += refused;
    s.offloaded -= refused;
    return s;
}

}  // namespace game
