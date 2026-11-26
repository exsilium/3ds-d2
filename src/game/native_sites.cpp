// See native_sites.h.
#include "game/native_sites.h"

#include "game/aot_spike.h"
#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace game {

namespace {

struct Native {
    const char *name;
    uint32_t stack_dwords;
};
struct Site {
    const char *group;
    uint32_t va;
    const char *game, *written;
};

#define NATIVE(name, stack_dwords) {#name, stack_dwords},
#define GROUP(name, compiled)
#define SITE(group, va, game_bytes, bytes_written)
const Native kNatives[] = {
#include "game/native_sites_table.h"
};
#undef NATIVE
#undef SITE
#define NATIVE(name, stack_dwords)
#define SITE(group, va, game_bytes, bytes_written) {#group, va, game_bytes, bytes_written},
const Site kSites[] = {
#include "game/native_sites_table.h"
};
#undef NATIVE
#undef GROUP
#undef SITE
struct Group {
    const char *name;
    bool compiled;
};
#define NATIVE(name, stack_dwords)
#define GROUP(name, compiled) {#name, compiled != 0},
#define SITE(group, va, game_bytes, bytes_written)
const Group kGroups[] = {
#include "game/native_sites_table.h"
};
#undef NATIVE
#undef GROUP
#undef SITE

constexpr size_t kNativeCount = sizeof kNatives / sizeof kNatives[0];
uint32_t g_entry[kNativeCount];        // each native's entry, once a group that calls it is applied
bool g_applied[sizeof kGroups / sizeof kGroups[0]];
std::deque<std::vector<uint8_t>> g_canonical;   // the sites as compiled (set_aot_inline_equivalent keeps a pointer)

int native_index(const std::string &name)
{
    for (size_t k = 0; k < kNativeCount; k++)
        if (name == kNatives[k].name)
            return (int)k;
    return -1;
}

void put_rel32(std::vector<uint8_t> &b, uint32_t va, uint32_t target)
{
    const uint32_t r = target - (va + (uint32_t)b.size() + 4u);
    for (int k = 0; k < 4; k++)
        b.push_back((uint8_t)(r >> (8 * k)));
}

// The game's bytes of a row; false where the text isn't hex pairs.
bool parse_hex(const char *s, std::vector<uint8_t> &out)
{
    for (const char *p = s; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (!std::isxdigit((unsigned char)p[0]) || !std::isxdigit((unsigned char)p[1]))
            return false;
        out.push_back((uint8_t)std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    return true;
}

// The bytes a row writes at va, its natives' rel32s to `entry` (the run's) or
// to their kSiteNativeBase slots (as compiled, canonical).
bool build(const Site &s, const std::vector<uint32_t> &entry, bool canonical, std::vector<uint8_t> &out)
{
    for (const char *p = s.written; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (*p == '{') {
            const char *q = std::strchr(p, '}');
            if (!q)
                return false;
            const std::string name(p + 1, q);
            uint32_t target;
            if (name.size() > 1 && name[0] == '=') {
                target = (uint32_t)std::strtoul(name.c_str() + 1, nullptr, 0);
            } else {
                const int k = native_index(name);
                if (k < 0 || (!canonical && !entry[k]))
                    return false;
                target = canonical ? kSiteNativeBase + (uint32_t)k * kSiteNativeStep : entry[k];
            }
            put_rel32(out, s.va, target);
            p = q + 1;
            continue;
        }
        if (!std::isxdigit((unsigned char)p[0]) || !std::isxdigit((unsigned char)p[1]))
            return false;
        out.push_back((uint8_t)std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
        p += 2;
    }
    return true;
}

}  // namespace

bool apply_sites(d2rt::Cpu &c, const char *group, std::initializer_list<SiteNative> natives)
{
    std::vector<uint32_t> entry(kNativeCount, 0);
    for (const SiteNative &n : natives) {
        const int k = native_index(n.name);
        if (k < 0 || !n.entry) {
            rt::logf("native sites: %s for %s isn't in the table", n.name, group);
            return false;
        }
        entry[k] = n.entry;
    }
    struct Write {
        const Site *s;
        std::vector<uint8_t> bytes, canonical;
    };
    std::vector<Write> w;
    for (const Site &s : kSites) {
        if (std::strcmp(s.group, group))
            continue;
        std::vector<uint8_t> game, have;
        Write x{&s, {}, {}};
        if (!parse_hex(s.game, game) || !build(s, entry, false, x.bytes) || !build(s, entry, true, x.canonical) ||
            x.bytes.size() != game.size()) {
            rt::logf("native sites: %s at %08x: the row doesn't make %u bytes", group, s.va, (unsigned)game.size());
            return false;
        }
        have.resize(game.size());
        if (!c.read(s.va, have.data(), (uint32_t)have.size()) || have != game)
            return false;
        w.push_back(std::move(x));
    }
    if (w.empty())
        return false;
    for (Write &x : w) {
        c.write(x.s->va, x.bytes.data(), (uint32_t)x.bytes.size());
        c.invalidate_code(x.s->va, (uint32_t)x.bytes.size());
        g_canonical.push_back(std::move(x.canonical));
        set_aot_inline_equivalent(x.s->va, g_canonical.back().data(), (uint32_t)g_canonical.back().size());
    }
    for (size_t k = 0; k < kNativeCount; k++)
        if (entry[k])
            g_entry[k] = entry[k];
    for (size_t k = 0; k < sizeof kGroups / sizeof kGroups[0]; k++)
        if (!std::strcmp(kGroups[k].name, group))
            g_applied[k] = true;
    return true;
}

std::string compiled_sites_not_applied()
{
    std::string s;
    for (size_t k = 0; k < sizeof kGroups / sizeof kGroups[0]; k++)
        if (kGroups[k].compiled && !g_applied[k])
            s += (s.empty() ? "" : " ") + std::string(kGroups[k].name);
    return s;
}

uint32_t aot_site_target(uint32_t x)
{
    if (x < kSiteNativeBase || x >= kSiteNativeBase + kNativeCount * kSiteNativeStep)
        return x;
    return g_entry[(x - kSiteNativeBase) / kSiteNativeStep];
}

}  // namespace game
