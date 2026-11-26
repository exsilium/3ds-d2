// The AOT spike (issue #113, docs/aot.md): Game.exe functions compiled to C
// by tools/aot/recompile from the user's own Game.exe, linked in when the
// build is given them (build/aot/, never committed), and hooked at their
// entries as natives that aren't served in line.
#ifndef D2_GAME_AOT_SPIKE_H
#define D2_GAME_AOT_SPIKE_H

#include <cstdint>
#include <string>
#include <vector>

namespace d2rt { struct Cpu; }

namespace game {

enum class AotMode : int {
    Off = 0,
    Sig = 1,        // per-function register signatures
    Ctx = 2,        // all eight registers through a context at every call
    SigCalls = 3,   // as Sig, no compiled function inlined into another
    CtxCalls = 4,   // as Ctx, likewise
    Check = 5,      // Sig journaled, memory put back, Box86 runs the original, compared
    Bench = 6,      // each call by one of Box86, the four builds and the native, in turn, timed
};

// Whether this build has the compiled code (aot_roots linked).
bool aot_linked();
// The mode to start in (the app, before install_d2_native); D2_AOT on the
// lane overrides it.
void set_aot_wanted(AotMode mode);
// Only the hookable functions with these indexes (settings.txt's aotfns=, as
// the lane's D2_AOT_FNS; issue #115): before install_aot.
void set_aot_fns(uint32_t first, uint32_t last);
// The probe (issue #115): a compiled function whose calls from Box86 take
// less than this each (its first few hundred, timed) is left to Box86, its
// trap costing more than it saves. Settings.txt's aotmin=, the lane's
// D2_AOT_MIN_US; 0 keeps every one hooked.
void set_aot_min_us(double us);
// From install_d2_native, before its hooks: with a mode other than Off the
// roots are hooked, and a native on the same entry (PathSearch) then isn't.
// Returns how many roots there are.
int install_aot(d2rt::Cpu &c);
// After every native's hook (the lane, issue #115): D2_AOT's Sig or Check
// turned on, as the console's OPTIONS row turns it on once they're in; what
// they changed stays with Box86 (set_aot_mode).
void aot_after_natives(d2rt::Cpu &c);
// The console's switch (the OPTIONS page): Bench or Off, at once, from
// the game's thread between frames. Off puts back what was at each entry
// (the game's bytes or a native's jump).
void set_aot_mode(d2rt::Cpu &c, AotMode mode);
AotMode aot_mode();
// The heartbeat's lines ("aot: ..."), each window's calls and times; empty
// when nothing ran.
std::vector<std::string> aot_lines();
void aot_sample();   // AOT_SAMPLE_ON builds: one sample of where compiled code is, from a host thread every 2 ms
// D2_AOT_STRICT=1 (the lane, issue #138): what compiled code left to Box86
// over the whole run, by cause and address ("aot strict: ..."); empty without it.
std::vector<std::string> aot_strict_lines();

// A native that hands back to the game's code (issue #115: UnitPickFilter's
// units it doesn't answer): the trap that enters the function's compiled
// code instead, for the native to go on at with the registers and stack as
// at its hook (resume_original). 0 when there's none, or compiled code is off.
uint32_t aot_trap_for(uint32_t va);

// A native's hand-back at va (resume_original, issue #138), with the
// registers in c and esp there: run in C when the recompiler compiled an
// entry there (Sig only), then on at the function's return, or at an exit's
// target. c's registers are then as the bridge wants them for
// redirect_next's target, its pop of the return address and `stack_args`
// dwords included. False when there's no such entry: Box86 runs it.
bool aot_resume(d2rt::Cpu &c, uint32_t va, uint32_t esp, uint32_t stack_args);
// Whether aot_resume would run va's entry.
bool aot_resumable(uint32_t va);

// Whether compiled code runs the function at va (Sig or Check, the lane's
// D2_AOT included before the natives are in): a native that only speeds up
// Box86 there can leave it be (issue #138: _ftol2's).
bool aot_compiled(uint32_t va);
// A build without Box86, at its last code check: the compiled functions, how
// many of them had other bytes in the image, the natives bound by address.
struct AotStatus {
    uint32_t functions = 0, differ = 0, natives = 0;
};
AotStatus aot_status();

// A native that does what the game's function at va does, its registers
// included (the check lane with it: 0 differ): compiled code calls the
// function's compiled original, not the native.
void set_aot_equivalent(uint32_t va);

// A native patched in line at va (n bytes, `orig` the game's) that does what
// the code it replaced does, only faster under Box86 (issue #138: the
// natives' loop steps): the function holding it counts as unchanged, so it
// stays compiled and hooked, and its C runs the original loop. The check
// lane compares the two.
void set_aot_inline_equivalent(uint32_t va, const uint8_t *orig, uint32_t n);

// The same for a native that can call it outright (Sig): the compiled
// function run in C with regs (the eight general registers, esp at the
// return address), as natives' guest_call would; false when it can't be
// (compiled code off or checked, or it has hard exits: then aot_trap_for).
bool aot_direct(d2rt::Cpu &c, uint32_t va, uint32_t *regs);

// The natives' entry for Bench: called as the hook would call it.
using AotNativeFn = uint32_t (*)(d2rt::Cpu &);
void aot_set_native(uint32_t va, AotNativeFn fn);

}  // namespace game

#endif
