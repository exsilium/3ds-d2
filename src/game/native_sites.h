// The natives' sites (issue #138): applying native_sites_table.h's rows.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace d2rt {
class Cpu;
}

namespace game {

// Where the recompiler puts native k (its NATIVE row, from 0) in the code it
// compiles: a call there is a callout, which aot_site_target maps to the
// native's entry.
constexpr uint32_t kSiteNativeBase = 0xfee00000u;
constexpr uint32_t kSiteNativeStep = 16;

struct SiteNative {
    const char *name;
    uint32_t entry;   // rt::native_entry's
};

// Writes the group's sites, the natives named in their bytes from `natives`.
// False, and nothing written, when a site's bytes aren't the game's (another
// Game.exe, or a native there already) or a native isn't given. The compiled
// code's hash reads each site as the recompiler compiled it.
bool apply_sites(d2rt::Cpu &c, const char *group, std::initializer_list<SiteNative> natives);

// The groups compiled as changed (GROUP's compiled 1) that this run didn't
// apply, space-separated: the functions with their sites run as the game's
// code is, in Box86 (the lane runs as the console app does with
// M4_LAYOUT=ctr).
std::string compiled_sites_not_applied();

// The native entry for a target the compiled code has (kSiteNativeBase + k *
// kSiteNativeStep), 0 if its group wasn't applied; anything else as it is.
uint32_t aot_site_target(uint32_t x);

}  // namespace game
