// See frame_reads.h.
#include "game/frame_reads.h"
#include "game/native_sites.h"
#include "game/sprites_ahead.h"
#include "game/tiles_ahead.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
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
constexpr uint32_t kAt = 0x0041aad0;

// rt::frame_thread_us's kinds the game thread waits for a read in.
constexpr size_t kCritsec = 4, kEvent = 5, kFileio = 6, kMulti = 7;

enum Class { kTiles, kPresets, kMonsters, kObjects, kPlayer, kMissiles, kItems, kOverlays, kSound, kUi, kOther, kClasses };
constexpr const char *kClassNames[kClasses] = {"tiles",    "presets", "monsters", "objects", "player", "missiles",
                                               "items",    "overlays", "sound",   "ui",      "other"};

struct Use {
    uint32_t reads = 0;
    uint64_t bytes = 0, wait_us = 0;
    uint64_t kind_us[4] = {};   // of wait_us: critsec, event, fileio, multi
};

std::unordered_map<std::string, Use> g_files;   // the window's, by name
std::string g_last;                              // the game thread's last read, its wait not yet charged
uint64_t g_last_kind[4] = {};
// The same by who read (issue #153): the read's caller and four frames up
// its EBP chain, by the file's class.
struct Chain {
    uint32_t ra[5] = {};
    bool operator<(const Chain &o) const { return std::memcmp(ra, o.ra, sizeof ra) < 0; }
};
std::map<Chain, Use> g_chains[kClasses];
Chain g_last_chain;
int g_last_chain_of = -1;
// The reads sprites_ahead answered 1 (a file's first) and 2 (a later one).
Use g_ahead[3];
int g_last_ahead = 0;

// The wait since the last read's entry, to that read, by kind.
void charge()
{
    uint64_t t[rt::kFrameThreadKinds];
    rt::frame_thread_us(t);
    const uint64_t now[4] = {t[kCritsec], t[kEvent], t[kFileio], t[kMulti]};
    if (!g_last.empty()) {
        Use &u = g_files[g_last];
        Use *by = g_last_chain_of >= 0 ? &g_chains[g_last_chain_of][g_last_chain] : nullptr;
        for (int k = 0; k < 4; k++)
            if (now[k] > g_last_kind[k]) {
                u.kind_us[k] += now[k] - g_last_kind[k];
                u.wait_us += now[k] - g_last_kind[k];
                if (by) {
                    by->kind_us[k] += now[k] - g_last_kind[k];
                    by->wait_us += now[k] - g_last_kind[k];
                }
                g_ahead[g_last_ahead].wait_us += now[k] - g_last_kind[k];
            }
    }
    for (int k = 0; k < 4; k++)
        g_last_kind[k] = now[k];
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
    charge();
    char name[261];
    if (!file || !c.read(file + 8, name, sizeof name - 1))
        std::strcpy(name, "(no name)");
    name[sizeof name - 1] = 0;
    g_last = name;
    // A DT1 (issue #89): where it is in its archive, for the tile prefetch;
    // and a tile's graphics read at its first draw, which 0x5fdea0 reads
    // into the buffer it has just put at the tile's +0x54.
    const size_t len = std::strlen(name);
    if (file && len > 4 && (name[len - 1] | 0x20) == '1' && (name[len - 2] | 0x20) == 't' &&
        (name[len - 3] | 0x20) == 'd') {
        tiles_ahead_note_file(c, name, file);
        tiles_ahead_loaded(c, c.read_u32(esp + 0x0c));
    }
    Use &u = g_files[g_last];
    u.reads++;
    u.bytes += bytes;
    const Class cls = class_of(g_last);
    // A missile's, an overlay's or an object's sprite (issue #153), which
    // the draw loads a direction at a time: the rest of the file read ahead.
    if ((cls == kMissiles || cls == kOverlays || cls == kObjects) && file && len > 4 &&
        (name[len - 1] | 0x20) == 'c' && (name[len - 2] | 0x20) == 'c' && (name[len - 3] | 0x20) == 'd')
        g_last_ahead = sprites_ahead_read(c, name, file);
    else
        g_last_ahead = 0;
    // Any sprite's file: what the card gives of it is kept (issue #153).
    if (file && len > 4 && (name[len - 1] | 0x20) == 'c' && (name[len - 2] | 0x20) == 'c' && (name[len - 3] | 0x20) == 'd')
        sprites_keep_read(c, file);
    g_ahead[g_last_ahead].reads++;
    g_last_chain_of = cls;
    {
        Chain ch;
        ch.ra[0] = c.read_u32(esp + 4);
        uint32_t ebp = c.reg(d2rt::R_EBP);
        for (int i = 1; i < 5 && ebp && c.hostptr(ebp, 8); i++) {
            ch.ra[i] = c.read_u32(ebp + 4);
            ebp = c.read_u32(ebp);
        }
        g_last_chain = ch;
        Use &by = g_chains[g_last_chain_of][ch];
        by.reads++;
        by.bytes += bytes;
    }
    return 0;
}

}  // namespace

bool install_frame_reads(d2rt::Cpu &c)
{
    const uint32_t native = rt::native_entry("FrameRead", 0, on_read);
    // native_sites_table.h's frame_reads row, which the AOT recompiler reads
    // too (issue #138).
    if (!native || !apply_sites(c, "frame_reads", {{"FrameRead", native}}))
        return false;
    rt::logf("frame reads: the game thread's reads named at %08x", kAt);
    return true;
}

std::vector<std::string> frame_reads_lines()
{
    std::vector<std::string> out;
    charge();
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
        for (int k = 0; k < 4; k++)
            u.kind_us[k] += f.second.kind_us[k];
        all.reads += f.second.reads;
        all.bytes += f.second.bytes;
        all.wait_us += f.second.wait_us;
    }
    if (!all.reads && all.wait_us < 1000)
        return out;
    char line[640];
    int n = std::snprintf(line, sizeof line, "[rt] frame reads: %u reads of %u files, %u KiB, waited %u ms;",
                          all.reads, (unsigned)files.size(), (unsigned)(all.bytes >> 10), (unsigned)(all.wait_us / 1000));
    for (int k = 0; k < kClasses && n < (int)sizeof line - 48; k++)
        if (by[k].reads)
            n += std::snprintf(line + n, sizeof line - n, " %s %u (%u KiB, %u ms)", kClassNames[k], by[k].reads,
                               (unsigned)(by[k].bytes >> 10), (unsigned)(by[k].wait_us / 1000));
    out.emplace_back(line);
    // Tiles' waits by kind (issue #89): the card (fileio), Storm's read thread
    // (event), its lock (critsec).
    if (by[kTiles].reads) {
        const Use &t = by[kTiles];
        std::snprintf(line, sizeof line, "[rt] frame reads, tiles waited: lock %u ms, read thread %u ms, file %u ms, multi %u ms",
                      (unsigned)(t.kind_us[0] / 1000), (unsigned)(t.kind_us[1] / 1000), (unsigned)(t.kind_us[2] / 1000),
                      (unsigned)(t.kind_us[3] / 1000));
        out.emplace_back(line);
    }
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
    // A class's callers, where it waited 20 ms or more in the window.
    for (int w = 0; w < kClasses; w++) {
        std::vector<std::pair<Chain, Use>> v(g_chains[w].begin(), g_chains[w].end());
        g_chains[w].clear();
        if (v.empty() || by[w].wait_us < 20000)
            continue;
        std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
            return a.second.wait_us != b.second.wait_us ? a.second.wait_us > b.second.wait_us : a.second.reads > b.second.reads;
        });
        n = std::snprintf(line, sizeof line, "[rt] frame reads, %s by caller:", kClassNames[w]);
        for (size_t i = 0; i < v.size() && i < 4 && n < (int)sizeof line - 120; i++) {
            const Use &u = v[i].second;
            n += std::snprintf(line + n, sizeof line - n, "%s %ux %u KiB %u ms (lock %u, read thread %u, file %u) from", i ? ";" : "",
                               u.reads, (unsigned)(u.bytes >> 10), (unsigned)(u.wait_us / 1000), (unsigned)(u.kind_us[0] / 1000),
                               (unsigned)(u.kind_us[1] / 1000), (unsigned)(u.kind_us[2] / 1000));
            for (int k = 0; k < 5; k++)
                n += std::snprintf(line + n, sizeof line - n, " %x", v[i].first.ra[k]);
        }
        out.emplace_back(line);
    }
    if (g_ahead[1].reads || g_ahead[2].reads) {
        std::snprintf(line, sizeof line,
                      "[rt] frame reads, sprites ahead: a file's first read %ux %u ms, its later reads %ux %u ms",
                      g_ahead[1].reads, (unsigned)(g_ahead[1].wait_us / 1000), g_ahead[2].reads,
                      (unsigned)(g_ahead[2].wait_us / 1000));
        out.emplace_back(line);
    }
    for (Use &u : g_ahead)
        u = Use{};
    g_files.clear();
    if (!g_last.empty())
        g_files[g_last];   // its wait still to come
    return out;
}

}  // namespace game
