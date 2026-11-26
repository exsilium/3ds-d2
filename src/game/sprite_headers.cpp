// See sprite_headers.h.
#include "game/sprite_headers.h"
#include "game/native_sites.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace game {

namespace {

constexpr uint32_t kReadHeader = 0x0060bf20;   // ecx the name, edx the record: 1 with the record filled
constexpr uint32_t kMagic = 0x48533244, kVersion = 1;   // "D2SH"
constexpr size_t kKeptMost = 8192;             // about 100 bytes each
constexpr uint32_t kWriteAtLeast = 32;         // new ones, or kWriteAfterUs since the first of them
constexpr uint64_t kWriteAfterUs = 60ull * 1000 * 1000;

// What 0x60bf20 writes: the record's +4, +8, +0x14 (the directions) and
// +0x18, then a dword for each direction (32 at the most) and the file's
// size from +0x1c; +0xc is 0 and the record's first byte gets bit 1.
struct Header {
    uint32_t f4, f8, dirs, f18;
    uint32_t n;          // the dwords at +0x1c
    uint32_t at[33];
};

bool g_on = true;
std::string g_path;
uint64_t g_signature = 0;
bool g_file_ours = false;   // the file has this run's signature: new ones are appended
std::unordered_map<uint64_t, std::vector<uint32_t>> g_kept;   // the name's hash -> f4, f8, dirs, f18, at[]
std::vector<uint64_t> g_new;                                  // kept, not yet written
uint64_t g_new_since_us = 0;
uint32_t g_from_kept = 0, g_read = 0, g_read_us = 0, g_failed = 0, g_written = 0, g_loaded = 0;

uint64_t name_key(d2rt::Cpu &c, uint32_t name)
{
    char buf[261];
    if (!name || !c.read(name, buf, sizeof buf - 1))
        return 0;
    buf[sizeof buf - 1] = 0;
    uint64_t h = 1469598103934665603ull;
    for (const char *p = buf; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch >= 'A' && ch <= 'Z')
            ch |= 0x20;
        if (ch == '/')
            ch = '\\';
        h = (h ^ ch) * 1099511628211ull;
    }
    return h ? h : 1;
}

uint32_t on_header(d2rt::Cpu &c)
{
    const uint32_t name = c.reg(d2rt::R_ECX), rec = c.reg(d2rt::R_EDX);
    const uint64_t key = g_on ? name_key(c, name) : 0;
    if (key) {
        const auto it = g_kept.find(key);
        if (it != g_kept.end() && it->second.size() >= 5 && c.hostptr(rec, 0xa0)) {
            const std::vector<uint32_t> &h = it->second;
            c.write_u32(rec + 4, h[0]);
            c.write_u32(rec + 8, h[1]);
            c.write_u32(rec + 0xc, 0);
            c.write_u32(rec + 0x14, h[2]);
            c.write_u32(rec + 0x18, h[3]);
            for (size_t i = 4; i < h.size() && i < 4 + 33; i++)
                c.write_u32(rec + 0x1c + (uint32_t)(i - 4) * 4, h[i]);
            uint8_t flags = 0;
            c.read(rec, &flags, 1);
            flags |= 2;
            c.write(rec, &flags, 1);
            g_from_kept++;
            return 1;
        }
    }
    const uint64_t t0 = rt::now_us();
    c.set_reg(d2rt::R_ECX, name);
    c.set_reg(d2rt::R_EDX, rec);
    const uint32_t r = rt::guest_call(c, kReadHeader, {});
    c.set_reg(d2rt::R_ECX, name);
    c.set_reg(d2rt::R_EDX, rec);
    g_read_us += (uint32_t)(rt::now_us() - t0);
    if (!r) {
        g_failed++;
        return r;
    }
    g_read++;
    if (key && g_kept.size() < kKeptMost) {
        const uint32_t dirs = c.read_u32(rec + 0x14);
        const uint32_t n = (dirs > 32 ? 32 : dirs) + 1;
        std::vector<uint32_t> h(4 + n);
        h[0] = c.read_u32(rec + 4);
        h[1] = c.read_u32(rec + 8);
        h[2] = dirs;
        h[3] = c.read_u32(rec + 0x18);
        for (uint32_t i = 0; i < n; i++)
            h[4 + i] = c.read_u32(rec + 0x1c + i * 4);
        g_kept[key] = std::move(h);
        if (g_new.empty())
            g_new_since_us = rt::now_us();
        g_new.push_back(key);
    }
    return r;
}

void load()
{
    FILE *f = std::fopen(g_path.c_str(), "rb");
    if (!f)
        return;
    uint32_t head[2];
    uint64_t sig = 0;
    if (std::fread(head, 4, 2, f) == 2 && head[0] == kMagic && head[1] == kVersion && std::fread(&sig, 8, 1, f) == 1 &&
        sig == g_signature) {
        g_file_ours = true;
        uint64_t key;
        uint32_t n;
        while (g_kept.size() < kKeptMost && std::fread(&key, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1 && n >= 5 &&
               n <= 4 + 33) {
            std::vector<uint32_t> h(n);
            if (std::fread(h.data(), 4, n, f) != n)
                break;
            g_kept[key] = std::move(h);
        }
        g_loaded = (uint32_t)g_kept.size();
    }
    std::fclose(f);
}

// The new ones, at the file's end; the file begun again if it was another
// copy's or none.
void write_new()
{
    if (g_path.empty() || g_new.empty())
        return;
    FILE *f = std::fopen(g_path.c_str(), g_file_ours ? "ab" : "wb");
    if (!f) {
        g_new.clear();
        return;
    }
    if (!g_file_ours) {
        const uint32_t head[2] = {kMagic, kVersion};
        std::fwrite(head, 4, 2, f);
        std::fwrite(&g_signature, 8, 1, f);
        g_file_ours = true;
    }
    for (const uint64_t key : g_new) {
        const std::vector<uint32_t> &h = g_kept[key];
        const uint32_t n = (uint32_t)h.size();
        std::fwrite(&key, 8, 1, f);
        std::fwrite(&n, 4, 1, f);
        std::fwrite(h.data(), 4, n, f);
    }
    std::fclose(f);
    g_written += (uint32_t)g_new.size();
    g_new.clear();
}

}  // namespace

bool install_sprite_headers(d2rt::Cpu &c, const char *path, uint64_t signature, bool on)
{
    g_on = on;
    g_path = path && on ? path : "";
    g_signature = signature;
    const uint32_t native = rt::native_entry("SpriteHeader", 0, on_header);
    // native_sites_table.h's sprite_headers row, which the AOT recompiler
    // reads too (issue #138).
    if (!native || !apply_sites(c, "sprite_headers", {{"SpriteHeader", native}}))
        return false;
    if (!g_path.empty())
        load();
    rt::logf("sprite headers: %u kept from the card%s", g_loaded, on ? "" : "; off");
    return true;
}

std::vector<std::string> sprite_headers_lines()
{
    std::vector<std::string> out;
    if (!g_new.empty() && (g_new.size() >= kWriteAtLeast || rt::now_us() - g_new_since_us >= kWriteAfterUs))
        write_new();
    if (!g_from_kept && !g_read && !g_failed)
        return out;
    char buf[200];
    std::snprintf(buf, sizeof buf,
                  "[rt] sprite headers: %u from the kept ones, %u read in %u ms, %u not there; %u kept, %u written%s",
                  g_from_kept, g_read, g_read_us / 1000, g_failed, (unsigned)g_kept.size(), g_written, g_on ? "" : "; off");
    out.push_back(buf);
    g_from_kept = g_read = g_read_us = g_failed = 0;
    return out;
}

}  // namespace game
