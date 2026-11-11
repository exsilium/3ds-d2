// See frame_reads.h.
#include "game/frame_reads.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace game {

namespace {

// SFileReadFile(file, buffer, bytes, &read, overlapped, ...), stdcall, 7
// arguments:
//   41aad0  55                 push ebp
//   41aad1  8b ec              mov  ebp, esp
//   41aad3  8b 45 14           mov  eax, [ebp+0x14]     &read
//   41aad6  83 ec 2c           sub  esp, 0x2c
//   41aad9  85 c0              test eax, eax
//   41aadb  74 06              je   0x41aae3
//   41aadd  c7 00 00 00 00 00  mov  dword [eax], 0
//   41aae3  ...                (eax is loaded again before it is read)
// becomes a call to the native, which clears *read itself, and the frame:
//   call native; push ebp; mov ebp, esp; sub esp, 0x2c; nops to 0x41aae3
constexpr uint32_t kAt = 0x0041aad0, kNext = 0x0041aae3;
constexpr uint8_t kOrig[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x14, 0x83, 0xec, 0x2c, 0x85,
                             0xc0, 0x74, 0x06, 0xc7, 0x00, 0x00, 0x00, 0x00, 0x00};
static_assert(kAt + sizeof kOrig == kNext, "the patch ends where the game's code goes on");

// rt::frame_thread_us's kinds the game thread waits for a read in.
constexpr size_t kCritsec = 4, kEvent = 5, kFileio = 6, kMulti = 7;

enum Class { kTiles, kPresets, kMonsters, kObjects, kPlayer, kMissiles, kItems, kOverlays, kSound, kUi, kOther, kClasses };
constexpr const char *kClassNames[kClasses] = {"tiles",    "presets", "monsters", "objects", "player", "missiles",
                                               "items",    "overlays", "sound",   "ui",      "other"};

struct Use {
    uint32_t reads = 0;
    uint64_t bytes = 0, wait_us = 0;
};

std::unordered_map<std::string, Use> g_files;   // the window's, by name
std::string g_last;                              // the game thread's last read, its wait not yet charged
uint64_t g_last_blocked = 0;

uint64_t blocked_us()
{
    uint64_t t[rt::kFrameThreadKinds];
    rt::frame_thread_us(t);
    return t[kCritsec] + t[kEvent] + t[kFileio] + t[kMulti];
}

// The wait since the last read's entry, to that read.
void charge(uint64_t now)
{
    if (!g_last.empty() && now > g_last_blocked)
        g_files[g_last].wait_us += now - g_last_blocked;
    g_last_blocked = now;
}

Class class_of(const std::string &name)
{
    std::string n(name);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    auto has = [&](const char *s) { return n.find(s) != std::string::npos; };
    auto ends = [&](const char *s) {
        const size_t k = std::strlen(s);
        return n.size() >= k && n.compare(n.size() - k, k, s) == 0;
    };
    if (ends(".ds1"))
        return kPresets;
    if (has("\\tiles\\") || ends(".dt1"))
        return kTiles;
    if (has("\\monsters\\"))
        return kMonsters;
    if (has("\\objects\\"))
        return kObjects;
    if (has("\\chars\\"))
        return kPlayer;
    if (has("\\missiles\\"))
        return kMissiles;
    if (has("\\items\\"))
        return kItems;
    if (has("\\overlays\\"))
        return kOverlays;
    if (has("\\sfx\\") || has("\\music\\") || ends(".wav"))
        return kSound;
    if (has("\\ui\\"))
        return kUi;
    return kOther;
}

uint32_t on_read(d2rt::Cpu &c)
{
    const uint32_t esp = c.reg(d2rt::R_ESP);
    // [esp] the patch's return, [esp+4] the caller's, then the arguments.
    const uint32_t file = c.read_u32(esp + 8), bytes = c.read_u32(esp + 0x10), read = c.read_u32(esp + 0x14);
    if (read)
        c.write_u32(read, 0);
    if (!rt::on_frame_thread())
        return 0;
    charge(blocked_us());
    char name[261];
    if (!file || !c.read(file + 8, name, sizeof name - 1))
        std::strcpy(name, "(no name)");
    name[sizeof name - 1] = 0;
    g_last = name;
    Use &u = g_files[g_last];
    u.reads++;
    u.bytes += bytes;
    return 0;
}

}  // namespace

bool install_frame_reads(d2rt::Cpu &c)
{
    uint8_t have[sizeof kOrig];
    if (!c.read(kAt, have, sizeof have) || std::memcmp(have, kOrig, sizeof have) != 0)
        return false;
    const uint32_t native = rt::native_entry("FrameRead", 0, on_read);
    if (!native)
        return false;
    uint8_t p[sizeof kOrig];
    std::memset(p, 0x90, sizeof p);
    p[0] = 0xe8;
    const uint32_t rel = native - (kAt + 5);
    std::memcpy(p + 1, &rel, 4);
    const uint8_t frame[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c};
    std::memcpy(p + 5, frame, sizeof frame);
    c.write(kAt, p, sizeof p);
    c.invalidate_code(kAt, sizeof p);
    rt::logf("frame reads: the game thread's reads named at %08x", kAt);
    return true;
}

std::vector<std::string> frame_reads_lines()
{
    std::vector<std::string> out;
    charge(blocked_us());
    if (g_files.empty())
        return out;
    Use by[kClasses];
    Use all;
    std::vector<std::pair<std::string, Use>> files(g_files.begin(), g_files.end());
    for (const auto &f : files) {
        Use &u = by[class_of(f.first)];
        u.reads += f.second.reads;
        u.bytes += f.second.bytes;
        u.wait_us += f.second.wait_us;
        all.reads += f.second.reads;
        all.bytes += f.second.bytes;
        all.wait_us += f.second.wait_us;
    }
    if (!all.reads && all.wait_us < 1000)
        return out;
    char line[512];
    int n = std::snprintf(line, sizeof line, "[rt] frame reads: %u reads of %u files, %u KiB, waited %u ms;",
                          all.reads, (unsigned)files.size(), (unsigned)(all.bytes >> 10), (unsigned)(all.wait_us / 1000));
    for (int k = 0; k < kClasses && n < (int)sizeof line - 48; k++)
        if (by[k].reads)
            n += std::snprintf(line + n, sizeof line - n, " %s %u (%u KiB, %u ms)", kClassNames[k], by[k].reads,
                               (unsigned)(by[k].bytes >> 10), (unsigned)(by[k].wait_us / 1000));
    out.emplace_back(line);
    // The files waited on most.
    std::sort(files.begin(), files.end(),
              [](const auto &a, const auto &b) { return a.second.wait_us > b.second.wait_us; });
    if (files[0].second.wait_us >= 1000) {
        n = std::snprintf(line, sizeof line, "[rt] frame reads, waited on most:");
        for (size_t i = 0; i < files.size() && i < 6 && files[i].second.wait_us >= 1000 && n < (int)sizeof line - 100;
             i++)
            n += std::snprintf(line + n, sizeof line - n, "%s %s %ux %u ms", i ? ";" : "", files[i].first.c_str(),
                               files[i].second.reads, (unsigned)(files[i].second.wait_us / 1000));
        out.emplace_back(line);
    }
    g_files.clear();
    if (!g_last.empty())
        g_files[g_last];   // its wait still to come
    return out;
}

}  // namespace game
