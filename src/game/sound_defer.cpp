// See sound_defer.h.
#include "game/sound_defer.h"
#include "game/native_sites.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace game {

namespace {

constexpr uint32_t kTable = 0x007bc9a0, kCount = 0x007bc9a4;   // the records and how many
constexpr uint32_t kRecord = 0x8e, kDefer = 0x4d, kStream = 0x4f;
constexpr uint32_t kCountMax = 0x10000;

bool g_on = true;

uint32_t on_table(d2rt::Cpu &c)
{
    const uint32_t count = c.read_u32(kCount);
    const uint32_t table = c.read_u32(kTable);
    if (!g_on || !table || !count || count > kCountMax)
        return count;
    auto *rec = (uint8_t *)c.hostptr(table, count * kRecord);
    if (!rec)
        return count;
    uint32_t set = 0, had = 0, streams = 0;
    for (uint32_t i = 0; i < count; i++, rec += kRecord) {
        if (rec[kStream])
            streams++;
        else if (rec[kDefer])
            had++;
        else {
            rec[kDefer] = 1;
            set++;
        }
    }
    rt::logf("sound: %u of %u sounds read off the game thread at their first play (%u more as sounds.txt has them, %u streams)",
             set, count, had, streams);
    return count;
}

// The loads the game has asked its asynchronous loader for, by record, and
// how long each took to be the record's sound: 0x482970 counts one in at
// 0x482b1d ([0x7bc9c8], esi the record), 0x481720 counts it out at 0x481772,
// called by the tick's scan (0x482bf0) or, the load waited for, by a play
// that wants the sound now (0x4829d9).
constexpr uint32_t kLoading = 0x007bc9c8, kLastScan = 0x007bc9c0;
constexpr uint32_t kWaitedFor = 0x004829de;   // 0x481720's return address in 0x482970
constexpr uint32_t kScanEvery = 25;           // the game's: its ticks between two scans
constexpr uint32_t kScanAsked = 0x00482bd8;   // 0x482970's return address in the scan: a load ahead of any play
std::unordered_map<uint32_t, uint64_t> g_asked;   // the record -> when, bit 0 set for the scan's
std::vector<uint32_t> g_took_ms, g_ahead_ms;
uint32_t g_waited_for = 0;

uint32_t on_asked(d2rt::Cpu &c)
{
    c.write_u32(kLoading, c.read_u32(kLoading) + c.reg(d2rt::R_EDI));
    if (g_asked.size() > 4096)
        g_asked.clear();
    g_asked[c.reg(d2rt::R_ESI)] = (rt::now_us() & ~1ull) | (c.read_u32(c.reg(d2rt::R_EBP) + 4) == kScanAsked);
    return c.reg(d2rt::R_EAX);
}

uint32_t on_loaded(d2rt::Cpu &c)
{
    c.write_u32(kLoading, c.read_u32(kLoading) - 1);
    const auto it = g_asked.find(c.reg(d2rt::R_ESI));
    if (it != g_asked.end()) {
        std::vector<uint32_t> &v = it->second & 1 ? g_ahead_ms : g_took_ms;
        if (v.size() < 4096)
            v.push_back((uint32_t)((rt::now_us() - it->second) / 1000));
        g_asked.erase(it);
    }
    if (c.arg(0) == kWaitedFor)
        g_waited_for++;
    return c.reg(d2rt::R_EAX);
}

// 0x482b40's "25 ticks since the last scan" (ecx the tick, 0 for a scan
// now): 1 for a scan. The scan is what finds a load finished, so a sound
// asked for waited up to 25 ticks more than its file took, over a second
// at the console's pace; with the sounds deferred it is every tick's.
uint32_t on_scan_due(d2rt::Cpu &c)
{
    return c.reg(d2rt::R_ECX) - c.read_u32(kLastScan) >= (g_on ? 1u : kScanEvery);
}

}  // namespace

std::vector<std::string> sound_defer_lines()
{
    std::vector<std::string> out;
    if (g_took_ms.empty() && g_ahead_ms.empty() && g_asked.empty())
        return out;
    std::sort(g_took_ms.begin(), g_took_ms.end());
    std::sort(g_ahead_ms.begin(), g_ahead_ms.end());
    const size_t n = g_took_ms.size(), m = g_ahead_ms.size();
    uint32_t late = 0, very = 0;
    for (const uint32_t ms : g_took_ms) {
        late += ms > 250;
        very += ms > 1000;
    }
    char buf[240];
    std::snprintf(buf, sizeof buf,
                  "[rt] sound loads: %u for a play, median %u ms, 9 in 10 within %u ms, longest %u ms, %u over 250 ms, %u "
                  "over 1 s; %u ahead of a play, median %u ms, longest %u ms; %u waited for, %u loading",
                  (unsigned)n, n ? g_took_ms[n / 2] : 0, n ? g_took_ms[n * 9 / 10] : 0, n ? g_took_ms[n - 1] : 0, late, very,
                  (unsigned)m, m ? g_ahead_ms[m / 2] : 0, m ? g_ahead_ms[m - 1] : 0, g_waited_for, (unsigned)g_asked.size());
    out.push_back(buf);
    g_took_ms.clear();
    g_ahead_ms.clear();
    g_waited_for = 0;
    return out;
}

bool install_sound_defer(d2rt::Cpu &c, bool on)
{
    g_on = on;
    const uint32_t native = rt::native_entry("SoundTable", 0, on_table);
    // native_sites_table.h's sound_defer row, which the AOT recompiler reads
    // too (issue #138).
    const uint32_t asked = rt::native_entry("SoundAsked", 0, on_asked);
    const uint32_t loaded = rt::native_entry("SoundLoaded", 0, on_loaded);
    const uint32_t due = rt::native_entry("SoundScanDue", 0, on_scan_due);
    return native && asked && loaded && due &&
           apply_sites(c, "sound_defer",
                       {{"SoundTable", native}, {"SoundAsked", asked}, {"SoundLoaded", loaded}, {"SoundScanDue", due}});
}

}  // namespace game
