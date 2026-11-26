// The AOT spike's runtime side (issue #113, aot_spike.h, aot_rt.h). The
// compiled functions come from build/aot/ (tools/aot/recompile, the
// user's Game.exe), linked only when the build is given them: their tables
// are weak here.
#include "game/aot_spike.h"
#include "runtime/rt_cpu_aot.h"
#include "game/native_sites.h"
#include "game/aot_rt.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_thread.h"
#include "runtime/rt_host.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <map>
#include <set>
#include <unordered_map>

#ifdef __3DS__
extern "C" {
#include <3ds/svc.h>   // svcGetSystemTick (a C header without its own extern "C")
}
#endif

// A stop from compiled code: CpuAot's (the app's own end on the console,
// issue #139); a Box86 build has no such end and aborts as before.
[[noreturn]] static void aot_stop(const char *what, uint32_t at)
{
#ifdef D2RT_CPU_AOT
    d2rt::cpu_aot_stop(what, at);
#else
    (void)what;
    (void)at;
    std::abort();
#endif
}

extern "C" {
uint8_t *aot_mem = nullptr;
uint32_t aot_eflags = 0x202;   // aot_rt.h: pushfd's word (IF set, as a thread starts)
uint32_t aot_xf = 0;          // aot_rt.h: the flags a call hands on or back
static AotXmm g_xmm0[8];      // a build with Box86 compiles no SSE2: nothing reads these
AotXmm *aot_xmm = g_xmm0;     // aot_rt.h: the running thread's xmm0..7 (CpuAot's, after its x87 state)
AotFpu *aot_fpu = nullptr;   // aot_rt.h: the running emu's x87 state (Cpu::x87_state)
// Box86's 80-bit conversions (x87emu_private.c), for fld/fstp m80.
void LD2D(void *ld, void *d);
void D2LD(void *d, void *ld);
void aot_ld2d(const void *ld, double *d) { LD2D(const_cast<void *>(ld), d); }
void aot_d2ld(const double *d, void *ld) { D2LD(const_cast<double *>(d), ld); }
uint8_t aot_stale[16384];   // aot_rt.h's AOT_LIVE: by the functions' table's rows
// From the generated file, compiled three ways (aot_rt.h): absent in a build
// without it.
extern const AotRoot aot_roots[] __attribute__((weak));
extern const AotRoot aotn_roots[] __attribute__((weak));
extern const AotFunc aot_funcs[] __attribute__((weak));
extern const AotFunc aotj_funcs[] __attribute__((weak));
extern const AotResume aot_resumes[] __attribute__((weak));
extern const AotReent aot_reents[] __attribute__((weak));
extern const AotTramp aot_tramps[] __attribute__((weak));
}

using d2rt::Cpu;

// CpuAot (issue #138, rt_cpu_aot.h): the run has no Box86; the dispatcher and
// a call from compiled code are below, by aot_call_out.
static bool g_cpu_aot = false;
#ifdef AOT_SAMPLE_ON
// The sampled profile (aot_rt.h): the last compiled function entered, or a
// trap's address inside a shim, or 1 while the thread is switched away. The
// dispatcher puts back what was there when a trap, a switch or a function it
// ran returns, so a sample after it is the caller's again (the last function
// entered before, which is the caller unless a callee of its own returned).
extern "C" {
volatile uint32_t aot_at = 0;
}
#define AOT_AT_SAVE const uint32_t at_was = aot_at
#define AOT_AT_SET(v) (aot_at = (v))
#define AOT_AT_BACK (aot_at = at_was)
namespace {
constexpr uint32_t kSampleSlots = 4096;   // open addressing; the sampler thread alone writes
struct SampleSlot {
    uint32_t va, n;
};
SampleSlot g_samples[kSampleSlots];
volatile uint32_t g_sample_n = 0, g_sample_lost = 0;
}
namespace game {
void aot_sample()
{
    const uint32_t va = aot_at;
    uint32_t h = (va * 2654435761u) >> 20;
    for (uint32_t k = 0; k < 64; k++, h = (h + 1) % kSampleSlots) {
        SampleSlot &s = g_samples[h];
        if (s.n && s.va != va)
            continue;
        s.va = va;
        s.n++;
        g_sample_n = g_sample_n + 1;
        return;
    }
    g_sample_lost = g_sample_lost + 1;
}
// The window's samples, the most first, and the table cleared.
static void aot_sample_lines(std::vector<std::string> &out)
{
    static SampleSlot top[kSampleSlots];
    uint32_t n = 0, all = 0;
    for (SampleSlot &s : g_samples) {
        if (s.n)
            top[n++] = s, all += s.n;
        s.n = 0;
    }
    if (!all)
        return;
    std::sort(top, top + n, [](const SampleSlot &a, const SampleSlot &b) { return a.n > b.n; });
    std::string line = "aot samples: " + std::to_string(all) + " (" + std::to_string(n) + " places, " +
                       std::to_string((unsigned)g_sample_lost) + " lost; 1: switched away, 003xxxxx: a shim):";
    char w[40];
    for (uint32_t k = 0; k < n && k < 16; k++) {
        std::snprintf(w, sizeof w, " %08x %.1f%%", (unsigned)top[k].va, 100.0 * top[k].n / all);
        line += w;
    }
    out.push_back(line);
}
}
#else
#define AOT_AT_SAVE (void)0
#define AOT_AT_SET(v) (void)0
#define AOT_AT_BACK (void)0
#endif
// The lane's fault report: the last compiled function the dispatcher entered,
// the last trap, the last hand-back (aot_resume), each a word written as it
// happens.
static volatile uint32_t g_at_fn = 0, g_at_trap = 0, g_at_resume = 0;
#ifndef __3DS__
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void cpuaot_fault(int sig, siginfo_t *si, void *uc)
{
#if defined(__arm__) && defined(__linux__)
    {
        const ucontext_t *u = static_cast<const ucontext_t *>(uc);
        char w[200];
        const int m = std::snprintf(w, sizeof w, "[rt] cpu_aot: pc %08lx lr %08lx sp %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx\n",
                                    u->uc_mcontext.arm_pc, u->uc_mcontext.arm_lr, u->uc_mcontext.arm_sp,
                                    u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1, u->uc_mcontext.arm_r2,
                                    u->uc_mcontext.arm_r3);
        if (m > 0)
            (void)!write(2, w, (size_t)m);
    }
#else
    (void)uc;
#endif
    char b[200];
    const int n = std::snprintf(b, sizeof b,
                                "[rt] cpu_aot: signal %d at host %p (guest %08lx): last function %08x, trap %08x, hand-back %08x\n",
                                sig, si->si_addr, (unsigned long)((uint8_t *)si->si_addr - aot_mem), (unsigned)g_at_fn,
                                (unsigned)g_at_trap, (unsigned)g_at_resume);
    if (n > 0)
        (void)!write(2, b, (size_t)n);
    _Exit(86);
}
#endif
static std::unordered_map<uint32_t, uint32_t> g_all_at;   // every compiled function: va -> its row
// g_all_at for the callouts (cpuaot_call): open addressing, a multiply and a
// mask (the ARM11 has no divide, and unordered_map's bucket is one).
struct FastAt {
    static constexpr uint32_t kNone = UINT32_MAX;
    std::vector<uint32_t> va, row;
    uint32_t mask = 0;
    void build(const std::unordered_map<uint32_t, uint32_t> &m)
    {
        uint32_t n = 16;
        while (n < m.size() * 2 + 1)
            n <<= 1;
        va.assign(n, 0), row.assign(n, kNone), mask = n - 1;
        for (const auto &kv : m) {
            uint32_t k = (kv.first * 2654435761u) >> 8 & mask;
            while (row[k] != kNone)
                k = (k + 1) & mask;
            va[k] = kv.first, row[k] = kv.second;
        }
    }
    uint32_t find(uint32_t x) const
    {
        if (!mask)
            return kNone;
        for (uint32_t k = (x * 2654435761u) >> 8 & mask;; k = (k + 1) & mask) {
            if (row[k] == kNone)
                return kNone;
            if (va[k] == x)
                return row[k];
        }
    }
};
static FastAt g_fast_at;
static const bool g_fast_on = std::getenv("D2_AOT_NOFAST") == nullptr;   // the lane's: the callouts as before, to compare
static const bool g_co_on = std::getenv("D2_AOT_CALLOUTS") != nullptr;   // count CpuAot's callouts by target (the lane)
static std::unordered_map<uint32_t, uint32_t> g_co_by;
static std::vector<uint32_t> g_native_slot;               // by row: the trap of the native bound to it, else 0
extern "C" {   // defined among aot_rt.h's functions
static bool cpuaot_dispatch(d2rt::Cpu &c, uint32_t eip, uint32_t until);
static void cpuaot_call(uint32_t x, AotRegs *t);
}

namespace game {
uint8_t *aot_guest_base();   // d2_native.cpp: its g_base
namespace {

// Per host thread (issue #115): on the console each guest thread runs on a
// host thread of its own, and one blocked in a shim lets another run, which
// may enter compiled code too; the first's calls to Box86 must still go
// through its own Cpu.
__thread Cpu *g_cpu = nullptr;
uint32_t g_fns_first = 0, g_fns_last = UINT32_MAX;   // set_aot_fns
AotMode g_mode = AotMode::Off, g_wanted = AotMode::Off;

uint64_t ticks()
{
#ifdef __3DS__
    return svcGetSystemTick();   // 268,111,856 Hz
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
#ifdef __3DS__
constexpr double kTicksPerUs = 268.111856;
#else
constexpr double kTicksPerUs = 1000.0;
#endif

// What Bench times: Box86 (the original from its trampoline, nested), the
// four builds of the compiled code, and our native where there is one.
// kHandedBack: the native's turns it handed back (they then ran nested under
// Box86, the attempt included), kept apart from Box86's own.
enum Variant { kBox86, kSig, kCtx, kSigCalls, kCtxCalls, kNative, kHandedBack, kVariants };
constexpr const char *kVariantNames[kVariants] = {"box86", "sig", "ctx", "sig-calls", "ctx-calls", "native",
                                                  "handed-back"};

struct Root {
    const AotRoot *r = nullptr, *calls = nullptr;
    uint32_t tramp = 0;          // the game's first instructions, then a jump back
    uint32_t trap = 0;           // our native entry
    uint8_t before[5] = {};      // what was at the entry before our jump
    bool hooked = false;
    AotNativeFn native = nullptr;
    // Per heartbeat window.
    uint32_t calls_n[kVariants] = {};
    uint64_t ticks_n[kVariants] = {};
    uint32_t checked = 0, differ = 0, nested = 0;
    // Since the start (the check's totals).
    uint32_t checked_all = 0, differ_all = 0;
    uint32_t turn = 0;
};
Root g_roots[8];
int g_root_count = 0;
uint32_t g_empty = 0;            // a ret, for the cost of a nested run
uint32_t g_empty_n = 0;
uint64_t g_empty_ticks = 0;
uint32_t g_nested = 0;           // nested runs from compiled code (callees not compiled)
std::map<uint32_t, uint32_t> g_nested_by;   // ... by callee, this window

struct NativeAt {
    uint32_t va;
    AotNativeFn fn;
};
NativeAt g_natives[8];
int g_native_count = 0;

uint32_t g_nested_yields = 0;   // shims inside a nested run that asked to yield (logged)
// Compiled calls to a function a native changed (aot_call_stale): the
// trap slot its entry jumps to (0: none, so Box86's call), by function;
// found again each time compiled code is turned on. And this window's: the
// native's shim called in C, of those handed back to the game's code (run
// nested to the return) or a wait done in place, and Box86's calls.
std::unordered_map<uint32_t, uint32_t> g_stale_slot;
uint32_t g_stale_shims = 0, g_stale_handed = 0, g_stale_waits = 0, g_stale_guest = 0;
std::map<uint32_t, uint32_t> g_stale_by;

// The registers of a nested run, and the guest call itself.
//
// run() returns at every stop, not only at the sentinel: on the console the
// profile sampler and the stall probe zero the block budget from their own
// threads (rt_host.cpp's nudge_thread_budget), and a shim may ask to yield.
// Each comes back with EIP at the resume point, so this runs on until EIP
// is the sentinel, as the backend's own nested calls do (cpu_box86.cpp, the
// SEH dispatch). Taking the first stop for the end would leave the game's
// thread with registers from the middle of the callee. A nudge taken here
// is sent again after, so the outer slice still stops for it. (A nested
// run must never come from a trap served in line, inside the dynarec's own
// run: its stop at the sentinel would end that run too. install_aot names
// the roots' traps apart for this.) A yield can't be honoured inside a
// nested run (the scheduler can't switch away from it): an advisory one is
// resumed; a wait that blocked the thread is done in place (rt::
// finish_nested_block), else it would go on with the wait's placeholder for a
// result. A wait on another guest thread can't end there: the recompiler
// leaves to Box86 what could reach one (the recompiler's may_block).
// Box86 from eip until it returns to the sentinel (in place of the return
// address at the caller's esp), every stop resumed as above; va names it.
const char *tramp_name(uint32_t va);   // the function whose trampoline va is, if any

void run_to_sentinel(Cpu &c, uint32_t eip, uint32_t va)
{
    const uint32_t sentinel = rt::bridge().sentinel();
    const char *fault = nullptr;
    bool stopped = false;
    for (;;) {
        if (!c.run(eip, &fault)) {
            rt::logf("aot: nested run of %08x faulted: %s at %08x", va, fault ? fault : "?", c.fault_addr());
            break;
        }
        eip = c.reg(d2rt::R_EIP);
        if (eip == sentinel)
            break;
        stopped = true;
        const bool lim = c.take_limit_hit();
        const bool yielded = rt::bridge().take_yield();
        // A shim that blocked the thread (the I/O worker's read on the
        // console) left a placeholder in EAX for the scheduler's wake: the
        // wait is done here, in place (issue #115: Storm's reads got 0).
        if (yielded) {   // each callee's first wait named before it's done (one that never ends, too)
            static std::set<uint32_t> named;
            if (named.insert(va).second) {
                const char *of = tramp_name(va);   // a hooked function's trampoline: the check's run of the original
                rt::logf("aot: a nested run of %08x%s%s waits, at %08x, in place", va, of ? ", the original of " : "",
                         of ? of : "", eip);
            }
        }
        const uint32_t waited_us = yielded ? rt::finish_nested_block(c) : 0;
        if (yielded)
            g_nested_yields++;
        static uint32_t logged = 0;
        if (++logged <= 10)
            rt::logf("aot: a nested run of %08x stopped at %08x (%s), resumed", va, eip,
                     lim ? "slice" : waited_us ? "a wait, done in place" : yielded ? "a shim's yield" : "no reason given");
    }
    if (stopped)
        c.nudge_thread_budget(nullptr);
}

// The trap's general registers and eip, kept across a run of our own (a
// nested run, a shim called in C): not the full context, whose FPU blob
// (768 bytes each way) cost more than a short callee, and whose FPU is the
// callee's to leave, as when Box86 calls it (compiled x87 code works on that
// same state, issue #136).
struct TrapRegs {
    uint32_t r[9];
    explicit TrapRegs(Cpu &c) { c.regs_gp(r); r[8] = c.reg(d2rt::R_EIP); }
    void put(Cpu &c) const
    {
        for (int k = 0; k < 8; k++)
            c.set_reg(k, r[k]);
        c.set_reg(d2rt::R_EIP, r[8]);
    }
};

void nested_run(Cpu &c, uint32_t va, uint32_t *r)
{
    const TrapRegs saved(c);
    const uint32_t esp = r[4];
    const uint32_t ra = c.read_u32(esp);   // the compiled caller's return address
    c.write_u32(esp, rt::bridge().sentinel());
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    run_to_sentinel(c, va, va);
    c.regs_gp(r);
    c.write_u32(esp, ra);   // as Box86 would have left it
    saved.put(c);
}

// The journal of AOT_JOURNAL's stores: where, and what was there.
struct Store {
    uint32_t va, n, at;   // at: its old bytes in g_old
};
std::vector<Store> g_journal;
std::vector<uint8_t> g_old;

// D2_AOT=5, the check (issue #114): every compiled function whose entry can
// be hooked is, and each of its calls, from the game's code or from inside
// another check's run of the original, runs twice. First its compiled code,
// journaled, with its writes put back after; then the original from its
// trampoline under Box86, from the same registers and esp. Compared: the
// registers its compiled callers read after it (out), those it keeps
// (preserved), and every byte the compiled code wrote, except the stack below
// the entry's esp + 4 (dead once the call returns). A byte only Box86 wrote
// isn't seen here; the host cases (Unicorn) compare every byte. The game
// keeps Box86's result, so a call inside the original's run is checked in its
// turn, at every depth.
//
// The compiled code's own calls to compiled functions are C calls, checked
// as part of it. A call it makes to Box86 (a callee not compiled) writes
// memory the journal doesn't see: that call keeps the compiled result,
// nothing is put back or run twice, and it counts as unchecked.
struct Fn {
    const AotFunc *f = nullptr;    // aot_funcs': what Sig runs
    const AotFunc *fj = nullptr;   // aotj_funcs' (journaled): what Check runs
    uint32_t tramp = 0, trap = 0;
    uint32_t trap_in_line = 0;     // a leaf's (AotFunc::leaf), served in line: what Sig hooks
    uint8_t before[5] = {};
    bool hooked = false;
    uint32_t checked = 0, differ = 0, unchecked = 0;           // this window
    uint32_t entered = 0;                                      // traps from Box86, this window
    uint64_t ticks_in = 0;                                     // D2_AOT_PROF: time inside, this window
    uint32_t probe = 0;                                        // entries seen since turned on (Sig)
    uint64_t probe_ticks = 0;                                  // ... the timed ones' time inside
    uint64_t probe_nested_ticks = 0;                           // ... of it, in nested runs (Box86's)
    uint32_t probe_nested = 0;                                 // ... and their number
    bool dropped = false;                                      // left to Box86 after its probe
    uint32_t direct = 0;                                       // natives' guest_calls run in C, this window
    uint32_t checked_all = 0, differ_all = 0, unchecked_all = 0;
};
std::vector<Fn> g_fns;           // sized once at install: entries keep their address
// By the functions' table's rows (issue #138): 1 where it reaches, in C, a
// function a native changed that may exit. Its call to the native exits to
// Box86 where the native hands back, so a native's direct call (no trap to
// exit to) leaves it to Box86.
std::vector<uint8_t> g_stale_hard;
std::unordered_map<uint32_t, uint32_t> g_fn_at;   // va -> index in g_fns
// The compiled functions whose entry can't be hooked (a 1-byte ret, 6c8030,
// in the renderer's table) but whose code is the game's, and reaches none
// changed (issue #138): va -> index in aot_funcs, for a callout to one.
std::unordered_map<uint32_t, uint32_t> g_unhooked_at;

const char *tramp_name(uint32_t va)
{
    for (const Fn &G : g_fns)
        if (G.tramp == va)
            return G.f->name;
    return nullptr;
}
__thread int g_journaling = 0;      // inside a journaled compiled run (this thread's)
__thread bool g_impure = false;     // ... which called Box86
// ... and the registers a compiled callee it called through a pointer
// (aot_call_out) left as garbage (neither out nor kept: a library call's
// leftovers, which Box86 has its own of): the caller can't know they are,
// and may pass them on as its own, so the check doesn't compare them.
__thread uint8_t g_garbage = 0;
uint32_t g_differ_logged = 0;

const char *reg_name(int k)
{
    static const char *const n[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    return n[k & 7];
}

__thread int g_prof_depth = 0;   // D2_AOT_PROF: compiled code entered from Box86, inside one another

// An exit (issue #115, aot_exit): compiled code hands the rest of its call
// to Box86 at a call that may block, an import or an indirect call. It
// comes back here, to the trap that entered it, with Box86's target and the
// registers there; the C frames between are compiled code's, dropped.
struct ExitJump {   // on the stack at each trap: left uninitialised
    std::jmp_buf jb;
    uint32_t target;
    uint32_t r[8];
};
__thread ExitJump *g_exit_jump = nullptr;
uint32_t g_exits = 0;                       // this window
std::map<uint32_t, uint32_t> g_exits_to;    // ... by Box86's target
// Callouts (aot_call_out), this window: our shims and compiled functions
// called in C; those that came back other than assumed (an exit at the
// return), by callee.
bool g_callouts = true;   // D2_AOT_NOCALLOUT (the lane): every callout an exit
// Compiled code entered where there's no trap to exit to (a native's
// guest_call, aot_run_compiled): only functions without hard exits, and a
// callout's fallback a nested run, as Box86's own call would be.
__thread int g_no_exit = 0;
constexpr size_t kMaxStale = sizeof aot_stale;
std::set<uint32_t> g_equivalent;   // set_aot_equivalent: natives compiled code may call the originals of
struct InlinePatch {
    uint32_t va, n;
    const uint8_t *orig;
};
std::vector<InlinePatch> g_inline_equivalent;   // set_aot_inline_equivalent: read as the game's bytes
// D2_AOT_STALE=1: compiled callers of a changed function call its native
// (aot_call_stale) and stay hooked. Off by default: on the lane, every
// caller that reaches one with Box86 did better (Act II 63.7 s against
// 67.2, fight 54.5 against 58.4): the natives' calls from C cost more than
// the callers gain.
bool g_stale_calls = false;
// The natives' hand-backs compiled (aot_resume; D2_AOT_RESUME=0, the lane:
// Box86 runs them), by address; this window's runs and their exits.
std::unordered_map<uint32_t, const AotResume *> g_resume_at;
uint32_t g_resumed = 0, g_resume_exits = 0;
std::map<uint32_t, uint32_t> g_resumed_by;
// The return sites' entries (issue #138; reent_<va>, D2_AOT_REENT=0: none):
// a shim's yield at a callout exits to g_reent_trap, not to the return, with
// the site kept by the thread's esp there; when the scheduler runs the thread
// again the trap runs the function's compiled code from the site, then each
// compiled caller's from its return site, in C, until one isn't. This
// window's: the trap's runs, the functions run, and the exits from them.
std::unordered_map<uint32_t, const AotReent *> g_reent_at;
std::unordered_map<uint32_t, uint32_t> g_reent_pending;
uint32_t g_reent_trap = 0;
uint32_t g_reent_runs = 0, g_reent_fns = 0, g_reent_exits = 0;
std::map<uint32_t, uint32_t> g_reent_ended;   // where Box86 took over, by address
uint32_t g_handed = 0;   // natives' hand-backs to compiled code (aot_trap_for), this window
uint32_t g_handed_direct = 0;   // ... and those run in C (aot_direct)
uint32_t g_callout_shims = 0, g_callout_fns = 0, g_callout_back = 0;
std::map<uint32_t, uint32_t> g_callout_back_by;

// D2_AOT_STRICT=1 (the lane, issue #138): everything compiled code hands to
// Box86 over the whole run, by cause and address (aot_strict_lines): what a
// build with no Box86 has nowhere to send. The cause is set by the runtime's
// own exits; an exit with none is one the recompiler wrote into the C.
enum : int {
    kLeftExit,       // an exit in the compiled code: the address it exits to
    kLeftNotCompiled,   // a callout to code that isn't compiled
    kLeftNotHooked,  // a callout to a compiled function not hooked
    kLeftChanged,    // a function a native changed, with no shim at its entry
    kLeftBack,       // a callout that came back with another esp or registers, or handed back: the callee
    kLeftBackWait,   // a callout whose shim waited, the return not a site with an entry: the callee
    kLeftTrap,       // a wait at a callout, on through the return sites' trap: the callee
    kLeftReentEnd,   // a thread past its last return site: the address Box86 goes on at
    kLeftNested,     // a nested run: the callee
    kLeftCauses
};
const char *const kLeftNames[kLeftCauses] = {
    "an exit in the compiled code, to",
    "a callout to code that isn't compiled:",
    "a callout to a compiled function that isn't hooked:",
    "a function a native changed, with no shim:",
    "a callout that came back with another esp or registers, or handed back, from",
    "a callout whose shim waited, its return no site, from",
    "a wait at a callout, on through the return sites' trap, from",
    "a thread past its last return site, Box86 on at",
    "a nested run of",
};
const bool g_strict = std::getenv("D2_AOT_STRICT") != nullptr;
int g_cause = kLeftExit;
uint32_t g_cause_at = 0;
std::map<std::pair<int, uint32_t>, uint64_t> g_left;
inline void left(int cause, uint32_t at)
{
    if (g_strict)
        g_left[{cause, at}]++;
}

// fn(regs), an exit caught: false when the code exited (into x).
bool run_catching(AotRootFn fn, uint32_t *regs, ExitJump &x)
{
    ExitJump *volatile prev = g_exit_jump;
    const volatile int depth = g_prof_depth, journaling = g_journaling, no_exit = g_no_exit;
    g_exit_jump = &x;
    g_no_exit = 0;   // a trap of its own (in a nested run under one that can't exit)
    if (setjmp(x.jb)) {
        g_exit_jump = prev;
        g_prof_depth = depth;
        g_journaling = journaling;
        g_no_exit = no_exit;
        g_exits++;
        g_exits_to[x.target]++;
        return false;
    }
    fn(regs);
    g_exit_jump = prev;
    g_no_exit = no_exit;
    return true;
}

// Whether F's compiled code may exit: by its own code, or through a function
// a native changed that exits (g_stale_hard; issue #138: _ftol2, a leaf, once
// its in-line patch made it Box86's).
inline bool may_exit_now(const AotFunc *f)
{
    const size_t k = (size_t)(f - aot_funcs);
    return f->may_exit || (k < g_stale_hard.size() && g_stale_hard[k]);
}

// F's compiled code on regs: true when it returned, false when it exited.
// Only a function that may exit pays for the catch.
inline bool run_fn(const AotFunc *f, uint32_t *regs, ExitJump &x)
{
    if (!may_exit_now(f)) {
        f->sig(regs);
        return true;
    }
    return run_catching(f->sig, regs, x);
}

// The x87 state after a call (issue #136), the compiled code's against
// Box86's: the control word, the status word's condition codes, the depth
// and each value on the stack, from each one's own top (the dynarec writes
// top 0 when its cache held all eight). False when they agree.
bool fpu_differs(const AotFpu &a, const AotFpu &b, char *what, size_t n)
{
    if (a.cw != b.cw) {
        std::snprintf(what, n, "x87 cw %04x, Box86 %04x", a.cw, b.cw);
        return true;
    }
    const uint16_t cc = C87_C0 | C87_C1 | C87_C2 | C87_C3;
    if ((a.sw ^ b.sw) & cc) {
        std::snprintf(what, n, "x87 sw %04x, Box86 %04x", a.sw & cc, b.sw & cc);
        return true;
    }
    if (a.stack != b.stack) {
        std::snprintf(what, n, "x87 depth %d, Box86 %d", (int)a.stack, (int)b.stack);
        return true;
    }
    for (int k = 0; k < a.stack && k < 8; k++) {
        uint64_t x, y;
        std::memcpy(&x, a.st[(a.top + k) & 7], 8);
        std::memcpy(&y, b.st[(b.top + k) & 7], 8);
        if (x != y) {
            double dx, dy;
            std::memcpy(&dx, &x, 8);
            std::memcpy(&dy, &y, 8);
            std::snprintf(what, n, "st(%d) %016llx (%.17g), Box86 %016llx (%.17g)", k, (unsigned long long)x, dx,
                          (unsigned long long)y, dy);
            return true;
        }
    }
    return false;
}

// Check: false when the compiled code exited; Box86 goes on at x.target with
// x.r (from the function's entry, x.target its trampoline, when the
// journal put back all it wrote).
bool check_fn(Fn &F, Cpu &c, uint32_t *regs, ExitJump &x)
{
    if (g_journaling) {   // from Box86 inside a journaled run: the original, unchecked
        g_impure = true;
        nested_run(c, F.tramp, regs);
        return true;
    }
    uint32_t a[8], b[8];
    std::memcpy(a, regs, sizeof a);
    std::memcpy(b, regs, sizeof b);
    AotFpu fpu_in, fpu_mine;   // issue #136: the x87 state at the entry, and the compiled code's after
    std::memcpy(&fpu_in, aot_fpu, sizeof fpu_in);
    g_journal.clear();
    g_old.clear();
    g_impure = false;
    g_garbage = 0;
    g_journaling++;
    const bool done =
        F.fj->may_exit || may_exit_now(F.f) ? run_catching(F.fj->sig, a, x) : (F.fj->sig(a), true);
    g_journaling = 0;
    const uint8_t garbage = g_garbage;
    if (!done) {
        // Exited: unchecked. With nothing but its own stores to put back,
        // Box86 runs the whole call from the entry; else on from the exit.
        F.unchecked++;
        F.unchecked_all++;
        if (!g_impure) {
            for (size_t k = g_journal.size(); k-- > 0;)
                std::memcpy(aot_mem + g_journal[k].va, &g_old[g_journal[k].at], g_journal[k].n);
            x.target = F.tramp;
            std::memcpy(x.r, b, sizeof b);
            std::memcpy(aot_fpu, &fpu_in, sizeof fpu_in);
        }
        return false;
    }
    if (g_impure) {
        F.unchecked++;
        F.unchecked_all++;
        std::memcpy(regs, a, sizeof a);
        return true;
    }
    // This check's journal, apart: a check inside the original's run below
    // starts one of its own.
    std::vector<Store> journal;
    std::vector<uint8_t> old, now(g_old.size());
    journal.swap(g_journal);
    old.swap(g_old);
    for (const Store &st : journal)
        std::memcpy(&now[st.at], aot_mem + st.va, st.n);
    for (size_t k = journal.size(); k-- > 0;)
        std::memcpy(aot_mem + journal[k].va, &old[journal[k].at], journal[k].n);
    std::memcpy(&fpu_mine, aot_fpu, sizeof fpu_mine);
    std::memcpy(aot_fpu, &fpu_in, sizeof fpu_in);
    nested_run(c, F.tramp, b);
    const uint32_t esp = regs[4];
    int bad_reg = -1;
    for (int k = 0; k < 8 && bad_reg < 0; k++)
        if (k != 4 && ((F.f->out | F.f->preserved) & ~garbage) >> k & 1 && a[k] != b[k])
            bad_reg = k;
    uint32_t first = 0;
    uint8_t mine = 0;
    bool bad_mem = false;
    for (size_t j = 0; j < journal.size() && !bad_mem; j++) {
        const Store &st = journal[j];
        for (uint32_t k = 0; k < st.n && !bad_mem; k++) {
            const uint32_t va = st.va + k;
            if (va < esp + 4 && va + 0x40000 > esp)
                continue;   // the dead stack
            if (aot_mem[va] == now[st.at + k])
                continue;
            bool later = false;   // a later store to the same byte wins: compare against the last
            for (size_t t = j + 1; t < journal.size() && !later; t++)
                later = va >= journal[t].va && va < journal[t].va + journal[t].n;
            if (!later) {
                bad_mem = true;
                first = va;
                mine = now[st.at + k];
            }
        }
    }
    char fpu_what[128];
    const bool bad_fpu = bad_reg < 0 && !bad_mem && fpu_differs(fpu_mine, *aot_fpu, fpu_what, sizeof fpu_what);
    F.checked++;
    F.checked_all++;
    if (bad_reg >= 0 || bad_mem || bad_fpu) {
        F.differ++;
        F.differ_all++;
        if (++g_differ_logged <= 20) {
            char what[128];
            if (bad_fpu)
                std::snprintf(what, sizeof what, "%s", fpu_what);
            else if (bad_reg >= 0)
                std::snprintf(what, sizeof what, "%s %08x, Box86 %08x", reg_name(bad_reg), a[bad_reg], b[bad_reg]);
            else
                std::snprintf(what, sizeof what, "the byte at %08x: %02x, Box86 %02x", first,
                              mine, aot_mem[first]);
            rt::logf("aot check: %s (%08x) differs on its call %u: %s; in eax %08x ecx %08x edx %08x ebx %08x "
                     "esp %08x ebp %08x esi %08x edi %08x",
                     F.f->name, F.f->va, F.checked_all, what, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                     regs[6], regs[7]);
        }
    }
    std::memcpy(regs, b, sizeof b);   // the game's result is kept
    return true;
}

void unhook_at(Cpu &c, uint32_t va, const uint8_t *before, bool &hooked);

// D2_AOT_PROF=1 (the lane, issue #115): each entry from Box86 timed, for
// which functions are worth a trap; the outermost ones' total against the
// window's own time.
const bool g_prof = std::getenv("D2_AOT_PROF") != nullptr;
// The probe (issue #115): a function whose compiled code takes less than
// this per call from Box86 costs more in the trap than it saves, and is left
// to Box86 (unhooked; compiled callers still call it in C). Its entries
// kProbeSkip on are timed, kProbeN of them. 0: every function stays hooked.
// (Box86's own time for it, run nested, can't be the measure: a nested run
// costs 8-10 us on the lane, and more than an empty one's for the function's
// own: 11-29 us against compiled code's 0.6-2.7 for the fight's hottest.)
constexpr uint32_t kProbeSkip = 32, kProbeN = 256;
double g_min_us = 8.0;   // issue #115: with exits, 8 did better than 2 on both lane scenes
uint32_t g_dropped = 0, g_dropped_window = 0;
// The nested runs compiled code starts (aot_guest_call): their time and
// number, this thread's, for the probe.
__thread uint64_t g_nested_ticks = 0;
__thread uint32_t g_nested_n = 0;
uint64_t g_prof_outer = 0, g_prof_window_t0 = 0;
// ... and where Box86 called from (the return address), this window: the
// callers that would save the most crossings compiled.
std::map<uint64_t, uint32_t> g_prof_sites;

uint32_t enter_fn(Fn &F, Cpu &c)
{
    uint32_t regs[8];
    c.regs_gp(regs);
    g_cpu = &c;
    F.entered++;
    ExitJump x;
    bool done = true;
    if (g_mode == AotMode::Check && F.fj)
        done = check_fn(F, c, regs, x);
    else if (g_mode == AotMode::Sig && F.probe < kProbeSkip + kProbeN && g_min_us > 0) {
        if (F.probe++ < kProbeSkip) {
            done = run_fn(F.f, regs, x);
        } else {
            const uint64_t t0 = ticks(), n0 = g_nested_ticks;
            const uint32_t c0 = g_nested_n;
            done = run_fn(F.f, regs, x);
            F.probe_ticks += ticks() - t0;
            F.probe_nested_ticks += g_nested_ticks - n0;
            F.probe_nested += g_nested_n - c0;
            // What it saves is its compiled code's time, less a crossing's
            // worth for each nested run it starts (issue #115: 4f8850, two
            // crossings a call, looked worth its trap with the run counted in).
            const double own_us = (double)(F.probe_ticks - F.probe_nested_ticks) / kProbeN / kTicksPerUs -
                                  (double)F.probe_nested / kProbeN * g_min_us;
            if (F.probe == kProbeSkip + kProbeN && g_prof)
                rt::logf("aot probe: %08x %.2f us in compiled code, %.2f nested runs a call: %s", F.f->va, own_us,
                         (double)F.probe_nested / kProbeN, own_us < g_min_us ? "left to Box86" : "kept");
            if (F.probe == kProbeSkip + kProbeN && own_us < g_min_us) {
                // Box86 runs it from its next call (the page's code marked
                // changed, as the game's own writes are).
                unhook_at(c, F.f->va, F.before, F.hooked);
                F.dropped = true;
                g_dropped++;
                g_dropped_window++;
            }
        }
    } else if (g_mode == AotMode::Sig && g_prof) {
        g_prof_sites[(uint64_t)F.f->va << 32 | aot_ld32(aot_mem, regs[4])]++;
        const uint64_t t0 = ticks();
        g_prof_depth++;
        done = run_fn(F.f, regs, x);
        g_prof_depth--;
        const uint64_t dt = ticks() - t0;
        F.ticks_in += dt;
        if (!g_prof_depth)
            g_prof_outer += dt;
    } else if (g_mode == AotMode::Sig)
        done = run_fn(F.f, regs, x);
    else
        nested_run(c, F.tramp, regs);   // hooked only in Sig and Check; between modes, the original
    if (!done) {
        // Box86 goes on at the exit's target with its registers: esp as the
        // trap's tail leaves it once it has taken off what its ret pops.
        for (int k = 0; k < 8; k++)
            if (k != 4)
                c.set_reg(k, x.r[k]);
        c.set_reg(4, x.r[4] - (4u + F.f->pops));
        rt::bridge().redirect_next(x.target);
        return x.r[0];
    }
    for (int k = 0; k < 8; k++)
        if (k != 4)
            c.set_reg(k, regs[k]);
    return regs[0];
}

// A native's guest_call (issue #115: the floor's interpolated light calls
// 4f8850 about 100k times a fight window) into a compiled function: in C,
// without the nested run. Sig only (Check's goes through the trap and is
// checked); a function the natives changed since it was compiled isn't
// hooked, and stays nested; one the probe left to Box86 still runs in C here
// (the run costs more than any compiled function).
bool direct_fn(Cpu &c, uint32_t va, uint32_t *regs)
{
    if (g_mode != AotMode::Sig)
        return false;
    const auto it = g_fn_at.find(va);
    if (it == g_fn_at.end())
        return false;
    Fn &F = g_fns[it->second];
    if ((!F.hooked && !F.dropped) || F.f->may_hard)   // a hard exit needs a trap to go back to
        return false;
    if ((size_t)(F.f - aot_funcs) < g_stale_hard.size() && g_stale_hard[F.f - aot_funcs])   // so does a native's hand-back
        return false;
    g_cpu = &c;
    F.direct++;
    g_no_exit++;
    F.f->sig(regs);
    g_no_exit--;
    return true;
}


uint32_t enter(Root &R, Cpu &c)
{
    uint32_t regs[8];
    c.regs_gp(regs);
    g_cpu = &c;
    AotMode m = g_mode;
    Variant v = kSig;
    if (m == AotMode::Bench) {
        const int n = R.native ? kHandedBack : kNative;
        v = (Variant)(R.turn++ % (uint32_t)n);
        if (v == kBox86 && (R.turn & 63) == 1 && g_empty) {   // now and then, the cost of a nested run itself
            uint32_t e[8];
            std::memcpy(e, regs, sizeof e);
            const uint64_t t0 = ticks();
            nested_run(c, g_empty, e);
            g_empty_ticks += ticks() - t0;
            g_empty_n++;
        }
    }
    const uint64_t t0 = ticks();
    ExitJump x;
    bool done = true;
    switch (m) {
    case AotMode::Sig: done = run_catching(R.r->sig, regs, x); break;
    case AotMode::Ctx: done = run_catching(R.r->ctx, regs, x); break;
    case AotMode::SigCalls: done = run_catching(R.calls->sig, regs, x); break;
    case AotMode::CtxCalls: done = run_catching(R.calls->ctx, regs, x); break;
    case AotMode::Bench:
        switch (v) {
        case kBox86: nested_run(c, R.tramp, regs); break;
        case kSig: done = run_catching(R.r->sig, regs, x); break;
        case kCtx: done = run_catching(R.r->ctx, regs, x); break;
        case kSigCalls: done = run_catching(R.calls->sig, regs, x); break;
        case kCtxCalls: done = run_catching(R.calls->ctx, regs, x); break;
        default: {
            // The native reads the Cpu; its result is eax, and it may write
            // others (a hand-back runs the game's code on from the entry:
            // not timed as the native's, counted as Box86's).
            const uint32_t eax = R.native(c);
            if (rt::bridge().take_redirect()) {
                // It handed the call back (resume_original; nothing written
                // but below esp): the whole call runs nested from the
                // trampoline instead, and counts as Box86's.
                for (int k = 0; k < 8; k++)
                    c.set_reg(k, regs[k]);
                nested_run(c, R.tramp, regs);
                v = kHandedBack;
            } else {
                c.regs_gp(regs);
                regs[0] = eax;
            }
        }
        }
        break;
    default: nested_run(c, R.tramp, regs); break;
    }
    const uint64_t t1 = ticks();
    R.calls_n[m == AotMode::Bench ? v : kSig]++;
    R.ticks_n[m == AotMode::Bench ? v : kSig] += t1 - t0;
    if (!done) {   // an exit (issue #115): Box86 goes on there, as enter_fn's
        for (int k = 0; k < 8; k++)
            if (k != 4)
                c.set_reg(k, x.r[k]);
        c.set_reg(4, x.r[4] - (4u + R.r->pops));
        rt::bridge().redirect_next(x.target);
        return x.r[0];
    }
    for (int k = 0; k < 8; k++)
        if (k != 4)
            c.set_reg(k, regs[k]);
    return regs[0];
}

// A jump to `trap` at va, what was there kept in before[5].
void hook_at(Cpu &c, uint32_t va, uint32_t trap, uint8_t *before, bool &hooked)
{
    if (hooked)
        return;
    c.read(va, before, 5);
    uint8_t jmp[5] = {0xE9};
    const uint32_t rel = trap - (va + 5);
    std::memcpy(jmp + 1, &rel, 4);
    c.write(va, jmp, 5);
    c.invalidate_code(va, 5);
    hooked = true;
}

void unhook_at(Cpu &c, uint32_t va, const uint8_t *before, bool &hooked)
{
    if (!hooked)
        return;
    c.write(va, before, 5);
    c.invalidate_code(va, 5);
    hooked = false;
}

void hook(Root &R, Cpu &c) { hook_at(c, R.r->va, R.trap, R.before, R.hooked); }
void unhook(Root &R, Cpu &c) { unhook_at(c, R.r->va, R.before, R.hooked); }

}  // namespace

// (A build with CpuAot has no other mode: the app's switch starts off.)
bool aot_resumable(uint32_t va) { return (g_mode == AotMode::Sig || g_cpu_aot) && g_resume_at.count(va); }

bool aot_resume(Cpu &c, uint32_t va, uint32_t esp, uint32_t stack_args)
{
    if (g_mode != AotMode::Sig && !g_cpu_aot)
        return false;
    const auto it = g_resume_at.find(va);
    if (it == g_resume_at.end())
        return false;
    uint32_t r[8];
    c.regs_gp(r);
    r[4] = esp;
    const uint32_t ra = c.read_u32(esp - it->second->sp);
    g_at_resume = va;
    Cpu *const prev = g_cpu;
    g_cpu = &c;
    ExitJump x;
    const bool done = run_catching(it->second->fn, r, x);
    g_cpu = prev;
    g_resumed++;
    g_resumed_by[va]++;
    if (!done) {   // Box86 on at the exit's target
        g_resume_exits++;
        std::memcpy(r, x.r, sizeof r);
    }
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    c.set_reg(4, r[4] - 4u - 4u * stack_args);   // the bridge's epilogue adds that back
    rt::bridge().redirect_next(done ? ra : x.target);
    return true;
}

namespace {
// The trap a thread goes on at after a shim's yield at a callout (g_reent_at).
// Each compiled function runs from its site to its ret (r[4] past it, as
// aot_resume's), then its caller's from the return address, while that is a
// site; Box86 on from the first that isn't, or at an exit's target (another
// yield's: this trap again).
// run_catching's function: the site g_reent_cur names, the registers in place
// (AotRegs is the eight words).
const AotReent *g_reent_cur;
uint32_t reent_run(uint32_t *r)
{
    g_reent_cur->fn(reinterpret_cast<AotRegs *>(r), g_reent_cur->va);
    return r[0];
}

uint32_t reenter(Cpu &c)
{
    uint32_t r[8];
    c.regs_gp(r);
    const auto p = g_reent_pending.find(r[4]);
    if (p == g_reent_pending.end()) {
        rt::logf("aot: a thread at the return sites' trap with esp %08x, which no yield left", r[4]);
        std::abort();
    }
    uint32_t at = p->second;
    g_reent_pending.erase(p);
    Cpu *const prev = g_cpu;
    g_cpu = &c;
    g_reent_runs++;
    uint32_t target = at;
    for (;;) {
        const auto it = g_reent_at.find(at);
        if (it == g_reent_at.end()) {
            target = at;
            g_reent_ended[at]++;
            left(kLeftReentEnd, at);
            break;
        }
        const uint32_t ra = c.read_u32(r[4] - it->second->sp);
        ExitJump x;
        g_reent_fns++;
        g_reent_cur = it->second;
        if (!run_catching(reent_run, r, x)) {
            g_reent_exits++;
            std::memcpy(r, x.r, sizeof r);
            target = x.target;
            break;
        }
        at = ra;
    }
    g_cpu = prev;
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    c.set_reg(4, r[4] - 4u);   // the bridge's epilogue adds that back
    rt::bridge().redirect_next(target);
    return r[0];
}
}  // namespace

bool aot_linked() { return (aot_roots && aot_roots[0].va) || (aot_funcs && aot_funcs[0].va); }
AotMode aot_mode() { return g_mode; }

void aot_set_native(uint32_t va, AotNativeFn fn)
{
    for (int k = 0; k < g_root_count; k++)
        if (g_roots[k].r->va == va)
            g_roots[k].native = fn;
    if (g_native_count < 8)
        g_natives[g_native_count++] = {va, fn};
}

void set_aot_wanted(AotMode mode) { g_wanted = mode; }
void set_aot_equivalent(uint32_t va) { g_equivalent.insert(va); }
void set_aot_inline_equivalent(uint32_t va, const uint8_t *orig, uint32_t n)
{
    g_inline_equivalent.push_back({va, n, orig});
}
void set_aot_min_us(double us) { g_min_us = us; }

uint32_t aot_trap_for(uint32_t va)
{
    if (g_mode != AotMode::Sig && g_mode != AotMode::Check)
        return 0;
    const auto it = g_fn_at.find(va);
    if (it == g_fn_at.end())
        return 0;
    g_handed++;
    return g_fns[it->second].trap;
}

bool aot_direct(Cpu &c, uint32_t va, uint32_t *regs)
{
    // Not direct_fn: the function is the native's own hook, so changed since
    // compiled and never hooked, but its compiled code is what the native
    // hands back to (as aot_trap_for). Only one that can't exit, callouts
    // included: with no trap to go back to, a callout's fallback is a nested
    // run, and one through a pointer may wait on another guest thread there
    // (466870's at [6cc4b0] did, in the fight: 10 s, then for good).
    if (g_mode != AotMode::Sig && !g_cpu_aot)
        return false;
    const auto it = g_fn_at.find(va);
    if (it == g_fn_at.end() || (g_fns[it->second].f->may_exit && !g_cpu_aot))   // CpuAot: nothing exits
        return false;
    g_cpu = &c;
    g_no_exit++;
    g_fns[it->second].f->sig(regs);
    g_no_exit--;
    g_handed_direct++;
    return true;
}
void set_aot_fns(uint32_t first, uint32_t last)
{
    g_fns_first = first;
    g_fns_last = last;
}

namespace {
// The functions' entries (issues #114, #115): one per compiled function
// whose entry can be hooked, each with a trampoline and a trap of its own.
// Hooked in Sig (the compiled code from Box86's calls) and Check
// (set_aot_mode).
void install_fns(Cpu &c)
{
    const AotFunc *funcs = aot_funcs ? aot_funcs : nullptr;
    if (!funcs)
        return;
    size_t n = 0, all = 0;
    for (const AotFunc *f = funcs; f->va; f++, all++)
        n += f->keep ? 1 : 0;
    if (!n)
        return;
    // CpuAot (issue #138) hooks nothing, so it needs no trampoline, no trap
    // and no return site's entry: 6.8 MiB of host heap and 448 KiB of the
    // guest's on the lane. It keeps the rows (aot_direct, the natives' kept
    // registers) and the hand-backs.
    const bool hooks = !g_cpu_aot;
    const uint32_t bytes = (uint32_t)((32 * n + 0xfff) & ~(size_t)0xfff);
    const uint32_t at = hooks ? rt::va_alloc(bytes, 0x0067b850) : 0;
    if (hooks && !at) {
        rt::logf("aot check: no memory for %u trampolines", (unsigned)n);
        return;
    }
    g_fns.reserve(n);
    std::unordered_map<uint32_t, const AotTramp *> relocs;
    uint32_t relocs_n = 0;
    if (aot_tramps)
        for (const AotTramp *m = aot_tramps; m->va; m++)
            relocs[m->va] = m;
    std::string skipped;
    uint32_t moved = 0, index = 0, left_out = 0, in_line = 0;
    // D2_AOT_FNS=<first>-<last> (the lane, issue #115): only the hookable
    // functions with those indexes (from 0, in the table's order), to find
    // by halves one that goes wrong where the check doesn't see it.
    uint32_t first = g_fns_first, last = g_fns_last;
    if (const char *m = std::getenv("D2_AOT_MIN_US"))   // the lane: the probe's threshold (set_aot_min_us)
        g_min_us = std::atof(m);
    if (const char *r = std::getenv("D2_AOT_FNS"))
        std::sscanf(r, "%u-%u", &first, &last);
    for (const AotFunc *f = funcs; f->va; f++) {
        const AotFunc *fj = aotj_funcs && aotj_funcs[f - funcs].va == f->va ? &aotj_funcs[f - funcs] : nullptr;
        if (!f->keep) {
            if (skipped.size() < 160)
                skipped += (skipped.empty() ? "" : ", ") + std::string(f->name);
            continue;
        }
        if (index < first || index > last) {
            index++;
            left_out++;
            continue;
        }
        index++;
        uint8_t have[16];
        c.read(f->va, have, 16);
        if (std::memcmp(have, f->head, f->keep) != 0) {   // a native's hook there already, or another Game.exe
            moved++;
            continue;
        }
        g_fns.emplace_back();
        Fn &F = g_fns.back();
        F.f = f;
        F.fj = fj;
        if (!hooks)
            continue;
        F.tramp = at + 32 * (uint32_t)(g_fns.size() - 1);
        uint8_t t[32];
        uint32_t len = f->keep;
        std::memcpy(t, f->head, f->keep);
        auto mv = relocs.find(f->va);
        if (mv != relocs.end()) {   // a branch in the first bytes, done from the trampoline
            const AotTramp *m = mv->second;
            len = m->len;
            std::memcpy(t, m->code, len);
            for (int k = 0; k < m->nrel; k++) {
                uint32_t to;
                std::memcpy(&to, t + m->rel[k], 4);
                to -= F.tramp + m->rel[k] + 4;
                std::memcpy(t + m->rel[k], &to, 4);
            }
            relocs_n++;
        }
        t[len] = 0xe9;
        const uint32_t rel = (f->va + f->keep) - (F.tramp + len + 5);
        std::memcpy(t + len + 1, &rel, 4);
        c.write(F.tramp, t, len + 5u);
        Fn *Fp = &F;
        const std::string trap_name = std::string("aotfn:") + f->name;
        F.trap = rt::native_entry(trap_name.c_str(), f->pops / 4, [Fp](Cpu &cc) { return enter_fn(*Fp, cc); });
        // A leaf (issue #115) never runs Box86 nested, so Box86 can enter it
        // in line, without leaving its run: a trap of its own, for Sig. The
        // check runs the original nested, through the other.
        if (f->leaf && !std::getenv("D2_AOT_NOINLINE")) {
            F.trap_in_line = rt::native_entry((trap_name + "/in line").c_str(), f->pops / 4,
                                              [Fp](Cpu &cc) { return enter_fn(*Fp, cc); });
            if (F.trap_in_line)
                c.set_inline_shim(F.trap_in_line);
            in_line++;
        }
    }
    if (hooks)
        c.invalidate_code(at, bytes);
    for (uint32_t k = 0; k < g_fns.size(); k++)
        g_fn_at[g_fns[k].f->va] = k;
    // D2_AOT_NODIRECT (the lane): the natives' guest_calls nested, as before.
    if (!std::getenv("D2_AOT_NODIRECT"))
        rt::set_guest_direct(direct_fn);
    g_callouts = !std::getenv("D2_AOT_NOCALLOUT");
    {
        const char *e = std::getenv("D2_AOT_STALE");
        g_stale_calls = e && !std::strcmp(e, "1");
        // D2_AOT_RESUME=0: none; =<va>,<va>...: those alone.
        e = std::getenv("D2_AOT_RESUME");
        if (aot_resumes && !(e && !std::strcmp(e, "0")))
            for (const AotResume *r = aot_resumes; r->va; r++) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "%x", r->va);
                if (!e || !*e || std::strstr(e, hex))
                    g_resume_at[r->va] = r;
            }
    }
    if (!g_resume_at.empty())
        rt::logf("aot: %u of the natives' hand-backs compiled", (unsigned)g_resume_at.size());
    {
        const char *e = std::getenv("D2_AOT_REENT");
        if (hooks && aot_reents && !(e && !std::strcmp(e, "0"))) {
            for (const AotReent *r = aot_reents; r->va; r++)
                g_reent_at[r->va] = r;
            g_reent_trap = rt::native_entry("aot:reenter", 0, [](Cpu &cc) { return reenter(cc); });
            rt::logf("aot: %u return sites' entries compiled", (unsigned)g_reent_at.size());
        }
    }
    rt::logf("aot: %u of %u compiled functions can be hooked (%u entered in line)%s%s%s", (unsigned)g_fns.size(),
             (unsigned)all, (unsigned)in_line, moved ? " (some had other bytes at their entry)" : "", skipped.empty() ? "" : "; not: ",
             skipped.c_str());
    if (relocs_n)
        rt::logf("aot: %u of them with a branch in their first bytes done from the trampoline", (unsigned)relocs_n);
    if (left_out)
        rt::logf("aot: %u left out (D2_AOT_FNS %u-%u)", (unsigned)left_out, (unsigned)first, (unsigned)last);
}
}  // namespace

namespace {
AotMode g_pending = AotMode::Off;   // the lane's D2_AOT, until the natives are in
}  // namespace

int install_aot(Cpu &c)
{
    AotMode mode = g_wanted;
    if (const char *e = std::getenv("D2_AOT"))
        mode = (AotMode)std::atoi(e);
    if (!aot_linked()) {
        if (mode != AotMode::Off)
            rt::logf("aot: this build has no compiled code (build it with D2_AOT=1, docs/aot.md)");
        return 0;
    }
    aot_mem = aot_guest_base();   // guest 0 in host memory, as the natives have it
    aot_fpu = static_cast<AotFpu *>(c.x87_state());   // issue #136: compiled x87 works on it
    if (d2rt::cpu_is_aot(c)) {
        // No Box86 (issue #138): every compiled function reached by its
        // address, from the dispatcher. Nothing is hooked.
        g_cpu_aot = true;
        g_all_at.clear();
        for (uint32_t k = 0; aot_funcs && aot_funcs[k].va; k++)
            g_all_at[aot_funcs[k].va] = k;
        g_fast_at.build(g_all_at);
        d2rt::cpu_aot_set_dispatch(cpuaot_dispatch);
        {   // the image, from its PE header: what a write can change the code of
            const uint32_t base = 0x00400000u;
            if (aot_mem[base] == 'M' && aot_mem[base + 1] == 'Z') {
                const uint32_t pe = base + aot_ld32(aot_mem, base + 0x3c);
                d2rt::cpu_aot_set_code_span(c, base, base + aot_ld32(aot_mem, pe + 0x50));   // SizeOfImage
            }
        }
#ifndef __3DS__
        struct sigaction sa = {};
        sa.sa_sigaction = cpuaot_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
#endif
        rt::logf("aot: CpuAot, %u compiled functions by their address, no Box86", (unsigned)g_all_at.size());
    }
    if (!aot_fpu) {
        rt::logf("aot: the CPU backend doesn't give its x87 state: no compiled code");
        return 0;
    }
    const uint32_t at = rt::va_alloc(0x1000, 0x0067b850);
    if (!at) {
        rt::logf("aot: no memory for the trampolines");
        return 0;
    }
    g_empty = at + 0xff0;
    const uint8_t ret = 0xc3;
    c.write(g_empty, &ret, 1);
    int n = 0;
    for (int k = 0; aot_roots[k].va && g_root_count < 8; k++) {
        const AotRoot *r = &aot_roots[k];
        uint8_t have[16];
        c.read(r->va, have, 16);
        if (std::memcmp(have, r->head, 16) != 0) {
            rt::logf("aot: %s at %08x: the bytes differ from the build's Game.exe, not hooked", r->name, r->va);
            continue;
        }
        // D2_AOT_ONLY=<name,...> (the lane): only those roots. Each hooked
        // root changes the lane's thread switches from its first call on,
        // and with them the world the virtual clock makes.
        if (const char *only = std::getenv("D2_AOT_ONLY")) {
            const size_t len = std::strlen(r->name);
            const char *at = std::strstr(only, r->name);
            if (!at || (at[len] != 0 && at[len] != ',')) {
                rt::logf("aot: %s not hooked (D2_AOT_ONLY)", r->name);
                continue;
            }
        }
        Root &R = g_roots[g_root_count++];
        R.r = r;
        R.calls = aotn_roots ? &aotn_roots[k] : r;
        R.tramp = at + 32 * k;
        uint8_t t[32];
        std::memcpy(t, r->head, r->keep);
        t[r->keep] = 0xe9;
        const uint32_t rel = (r->va + r->keep) - (R.tramp + r->keep + 5);
        std::memcpy(t + r->keep + 1, &rel, 4);
        c.write(R.tramp, t, r->keep + 5u);
        Root *Rp = &R;
        // A name of its own: the bridge keeps one trap per name, and a native
        // on the same entry (PathSearch) would otherwise get this one back and
        // mark it in line (hook_function), with the nested runs then inside
        // the dynarec's own run (issue #113: the console's bench froze).
        const std::string trap_name = std::string("aot:") + r->name;
        R.trap = rt::native_entry(trap_name.c_str(), r->pops / 4, [Rp](Cpu &cc) { return enter(*Rp, cc); });
        for (int j = 0; j < g_native_count; j++)
            if (g_natives[j].va == r->va)
                R.native = g_natives[j].fn;
        n++;
    }
    c.invalidate_code(at, 0x1000);
    const size_t host0 = mallinfo().uordblks;
    install_fns(c);
    rt::logf("aot: the functions' rows took %u KiB of host heap", (unsigned)((mallinfo().uordblks - host0) >> 10));
    // Sig and Check hook every compiled function, which the natives' hooks
    // must go in before (issue #115: on the console they're in before the
    // OPTIONS row turns it on); aot_after_natives() turns them on.
    if (mode == AotMode::Sig || mode == AotMode::Check) {
        g_pending = mode;
        set_aot_mode(c, AotMode::Off);
    } else {
        set_aot_mode(c, mode);
    }
    rt::logf("aot: %d compiled roots%s, mode %d", n, aotj_funcs ? " (with the check build)" : "", (int)mode);
    return n;
}

void aot_after_natives(Cpu &c)
{
    if (g_pending == AotMode::Off)
        return;
    set_aot_mode(c, g_pending);
    g_pending = AotMode::Off;
}

bool aot_compiled(uint32_t va)
{
#ifdef D2RT_CPU_AOT
    // A build with CpuAot runs nothing but compiled code, from the start
    // (before install_aot: d2_native asks while it installs).
    for (uint32_t k = 0; aot_funcs && aot_funcs[k].va; k++)
        if (aot_funcs[k].va == va)
            return true;
#endif
    const AotMode m = g_pending != AotMode::Off ? g_pending : g_mode;
    return (m == AotMode::Sig || m == AotMode::Check) && g_fn_at.count(va);
}

namespace {
// The shim of the native that changed va, at its trap slot (the hook's jmp
// from the entry), else null: a patch in line, whose call exits to Box86.
const d2rt::Shim *stale_shim(uint32_t va)
{
    auto it = g_stale_slot.find(va);
    if (it == g_stale_slot.end()) {
        uint32_t slot = 0;
        if (aot_mem[va] == 0xe9) {
            int32_t rel;
            std::memcpy(&rel, aot_mem + va + 1, 4);
            const uint32_t to = va + 5u + (uint32_t)rel;
            if (g_callouts && rt::bridge().slot_shim(to))
                slot = to;
        }
        it = g_stale_slot.emplace(va, slot).first;
    }
    return it->second ? rt::bridge().slot_shim(it->second) : nullptr;
}

// FNV-1a over a compiled function's code as it is now, as the recompiler's
// code_hash() took it from the Game.exe it compiled.
uint32_t code_hash(Cpu &c, const AotFunc *f)
{
    uint32_t h = 0x811c9dc5u;
    uint8_t buf[256];
    for (uint32_t k = 0; k < f->nranges; k++)
        for (uint32_t va = f->ranges[2 * k], hi = f->ranges[2 * k + 1]; va < hi;) {
            const uint32_t n = std::min<uint32_t>(hi - va, sizeof buf);
            c.read(va, buf, n);
            for (const InlinePatch &p : g_inline_equivalent)   // its original bytes, as compiled
                for (uint32_t j = std::max(p.va, va); j < std::min(p.va + p.n, va + n); j++)
                    buf[j - va] = p.orig[j - p.va];
            for (uint32_t j = 0; j < n; j++)
                h = (h ^ buf[j]) * 0x01000193u;
            va += n;
        }
    return h;
}

// The compiled functions to leave to Box86 (issue #115): those whose code
// changed since it was compiled (a native's patch: its hook or code it
// replaced in line, which the compiled code would skip), and every compiled
// function that reaches one in C. With every function unhooked.
std::vector<uint8_t> stale_funcs(Cpu &c, uint32_t &changed, std::string &names)
{
    size_t n = 0;
    while (aot_funcs[n].va)
        n++;
    if (n > kMaxStale) {
        rt::logf("aot: %u functions compiled, more than aot_stale's %u", (unsigned)n, (unsigned)kMaxStale);
        std::abort();
    }
    // bad 1: not hooked (changed, or reaches one that may exit); 2: hooked,
    // but reaches one compiled code calls the native of, so not in line.
    std::vector<uint8_t> bad(n), block(n);
    changed = 0;
    for (size_t k = 0; k < n; k++) {
        aot_stale[k] = 0;
        if (!aot_funcs[k].nranges || code_hash(c, &aot_funcs[k]) == aot_funcs[k].hash)
            continue;
        bad[k] = 1;
        changed++;
        // The whole list (issue #138): every one named, * where it may exit
        // (its callers stay with Box86 then, D2_AOT_STALE or not).
        names += std::string(names.empty() ? "" : " ") + aot_funcs[k].name + (aot_funcs[k].may_exit ? "*" : "");
        // Else its compiled callers call the native (aot_stale): they stay
        // hooked, but not in line, a nested run can't start there. Where the
        // native hands back or waits, the call exits to Box86 (issue #138):
        // a nested run can't wait on another guest thread (0040a960's
        // WaitForSingleObject hung the fight when it was one).
        // A native that does what its function does, by the check's lane
        // (set_aot_equivalent): compiled callers call the original's C.
        if (g_equivalent.count(aot_funcs[k].va))
            continue;
        aot_stale[k] = 1;
        // A native that hands back, or waits, exits (aot_call_stale, issue
        // #138): what it hands Box86 runs there, not nested.
        block[k] = !g_stale_calls;
    }
    g_stale_hard.assign(n, 0);
    for (size_t k = 0; k < n; k++)
        // ... and where no native's shim is at its entry: a patch in line,
        // whose call exits (aot_call_stale), from a leaf too.
        g_stale_hard[k] = aot_stale[k] && (aot_funcs[k].may_exit || !stale_shim(aot_funcs[k].va));
    for (bool more = changed != 0; more;) {
        more = false;
        for (size_t k = 0; k < n; k++)
            for (uint32_t j = 0; j < aot_funcs[k].ncallees; j++) {
                const uint16_t t = aot_funcs[k].callees[j];
                if (block[t] && !block[k]) {
                    block[k] = 1;
                    bad[k] = 1;
                    more = true;
                }
                if (g_stale_hard[t] && !g_stale_hard[k]) {
                    g_stale_hard[k] = 1;
                    more = true;
                }
                if ((aot_stale[t] || bad[t] == 2) && !bad[k]) {
                    bad[k] = 2;
                    more = true;
                }
            }
    }
    return bad;
}
}  // namespace

void set_aot_mode(Cpu &c, AotMode mode)
{
    if (mode == AotMode::Check && (g_fns.empty() || !g_fns[0].fj))
        mode = AotMode::Sig;
    g_mode = mode;
    if (g_cpu_aot)   // nothing runs the x86: no hooks; the dispatcher finds what natives changed
        return;
    // Sig and Check hook every function they can (the roots among them); the
    // spike's other modes the roots alone.
    const bool fns = (mode == AotMode::Sig || mode == AotMode::Check) && !g_fns.empty();
    for (Fn &F : g_fns)
        if (!fns)
            unhook_at(c, F.f->va, F.before, F.hooked);
    for (int k = 0; k < g_root_count; k++) {
        if (mode == AotMode::Off || fns)
            unhook(g_roots[k], c);
        else
            hook(g_roots[k], c);
    }
    if (!fns)
        return;
    // Each time it's turned on: the natives may have hooked since, and what
    // they changed (and whatever reaches it) stays with Box86.
    for (Fn &F : g_fns)
        unhook_at(c, F.f->va, F.before, F.hooked);
    uint32_t changed = 0;
    std::string names;
    const std::vector<uint8_t> bad = stale_funcs(c, changed, names);
    g_unhooked_at.clear();
    for (size_t k = 0; k < bad.size(); k++)
        if (!aot_funcs[k].keep && !bad[k] && aot_funcs[k].pops != AOT_POPS_LOOSE)   // a loose one is the dispatcher's: Box86's here
            g_unhooked_at[aot_funcs[k].va] = (uint32_t)k;
    g_stale_slot.clear();
    uint32_t hooked = 0, left = 0, reaching = 0;
    g_dropped = 0;
    for (Fn &F : g_fns) {
        F.probe = 0;
        F.probe_ticks = F.probe_nested_ticks = 0;
        F.probe_nested = 0;
        F.dropped = false;
        const uint8_t b = bad[F.f - aot_funcs];
        if (b == 1) {
            left++;
            continue;
        }
        reaching += b == 2;
        hook_at(c, F.f->va, mode == AotMode::Sig && F.trap_in_line && !b ? F.trap_in_line : F.trap, F.before,
                F.hooked);
        hooked++;
    }
    rt::logf("aot: %u functions hooked; %u changed since compiled (natives%s%s): %u left to Box86 with them "
             "(those and what reaches one that may exit); %u hooked that reach one that can't, which compiled code "
             "calls through Box86, so not in line",
             (unsigned)hooked, (unsigned)changed, "", "", (unsigned)left, (unsigned)reaching);
    for (size_t p = 0; p < names.size();) {   // the changed ones, * where they may exit, in lines logf's buffer holds
        size_t q = std::min(names.size(), p + 400);
        if (q < names.size())
            q = names.rfind(' ', q) > p ? names.rfind(' ', q) : q;
        rt::logf("aot: changed since compiled: %s", names.substr(p, q - p).c_str());
        p = q + (q < names.size());
    }
    const std::string unapplied = compiled_sites_not_applied();
    if (!unapplied.empty())
        rt::logf("aot: natives' sites compiled but not applied, their functions changed since compiled: %s",
                 unapplied.c_str());
}

std::vector<std::string> aot_strict_lines()
{
    std::vector<std::string> out;
    if (!g_strict)
        return out;
    // The compiled function an address is in, if any.
    auto owner = [](uint32_t a) -> const AotFunc * {
        for (size_t k = 0; aot_funcs && aot_funcs[k].va; k++)
            for (uint32_t j = 0; j + 1 < 2u * aot_funcs[k].nranges; j += 2)
                if (aot_funcs[k].ranges[j] <= a && a < aot_funcs[k].ranges[j + 1])
                    return &aot_funcs[k];
        return nullptr;
    };
    uint64_t by[kLeftCauses] = {}, all = 0;
    size_t places[kLeftCauses] = {};
    for (const auto &kv : g_left) {
        by[kv.first.first] += kv.second;
        places[kv.first.first]++;
        if (kv.first.first != kLeftTrap)
            all += kv.second;
    }
    char line[256];
    std::snprintf(line, sizeof line,
                  "aot strict: %llu times compiled code left to Box86 (a build without it has nowhere to send them), "
                  "and %llu waits on through the return sites' trap",
                  (unsigned long long)all, (unsigned long long)by[kLeftTrap]);
    out.push_back(line);
    for (int cause = 0; cause < kLeftCauses; cause++) {
        if (!by[cause])
            continue;
        std::vector<std::pair<uint64_t, uint32_t>> rows;
        for (const auto &kv : g_left)
            if (kv.first.first == cause)
                rows.push_back({kv.second, kv.first.second});
        std::sort(rows.rbegin(), rows.rend());
        std::snprintf(line, sizeof line, "aot strict: %llu %s %zu places", (unsigned long long)by[cause],
                      kLeftNames[cause], places[cause]);
        out.push_back(line);
        for (size_t k = 0; k < rows.size() && k < 60; k++) {
            const AotFunc *f = owner(rows[k].second);
            const d2rt::Shim *sh = rt::bridge().slot_shim(rows[k].second);
            const bool stale = f && aot_stale[f - aot_funcs];
            std::snprintf(line, sizeof line, "aot strict:   %08x %llu%s%s%s%s", rows[k].second,
                          (unsigned long long)rows[k].first,
                          sh ? " " : f ? (f->va == rows[k].second ? " compiled" : " in ") : " not compiled",
                          sh ? sh->tag.c_str() : f && f->va != rows[k].second ? f->name : "",
                          f && !f->keep ? ", can't be hooked" : "", stale ? ", changed by a native" : "");
            out.push_back(line);
        }
        if (rows.size() > 60) {
            std::snprintf(line, sizeof line, "aot strict:   ... and %zu more places", rows.size() - 60);
            out.push_back(line);
        }
    }
    return out;
}

std::vector<std::string> aot_lines()
{
    std::vector<std::string> out;
#ifdef AOT_SAMPLE_ON
    aot_sample_lines(out);
#endif
    const double empty_us = g_empty_n ? (double)g_empty_ticks / g_empty_n / kTicksPerUs : 0;
    for (int k = 0; k < g_root_count; k++) {
        Root &R = g_roots[k];
        uint32_t all = 0;
        for (uint32_t x : R.calls_n)
            all += x;
        if (!all)
            continue;
        char line[320];
        int p = std::snprintf(line, sizeof line, "aot: %s %u calls:", R.r->name, all);
        for (int v = 0; v < kVariants; v++)
            if (R.calls_n[v] && p < (int)sizeof line)
                p += std::snprintf(line + p, sizeof line - p, " %s %u %.2f us", kVariantNames[v], R.calls_n[v],
                                   (double)R.ticks_n[v] / R.calls_n[v] / kTicksPerUs);
        if (g_mode == AotMode::Bench && p < (int)sizeof line)
            p += std::snprintf(line + p, sizeof line - p, "; a nested run alone %.2f us (%u)", empty_us, g_empty_n);
        if (R.checked && p < (int)sizeof line)
            p += std::snprintf(line + p, sizeof line - p, "; checked %u, differ %u", R.checked, R.differ);
        out.push_back(line);
        std::memset(R.calls_n, 0, sizeof R.calls_n);
        std::memset(R.ticks_n, 0, sizeof R.ticks_n);
        R.checked = R.differ = 0;
    }
    if (g_mode == AotMode::Check && !g_fns.empty()) {
        uint32_t calls = 0, differ = 0, unchecked = 0, fns = 0, calls_all = 0, differ_all = 0, unchecked_all = 0,
                 fns_all = 0, x87_calls = 0, x87_fns = 0, x87_all = 0;
        for (Fn &F : g_fns) {
            if (F.f->x87) {   // issue #136: what of it is x87 code
                x87_all++;
                x87_calls += F.checked_all;
                x87_fns += (F.checked_all || F.unchecked_all) ? 1 : 0;
            }
            calls += F.checked;
            differ += F.differ;
            unchecked += F.unchecked;
            fns += (F.checked || F.unchecked) ? 1 : 0;
            calls_all += F.checked_all;
            differ_all += F.differ_all;
            unchecked_all += F.unchecked_all;
            fns_all += (F.checked_all || F.unchecked_all) ? 1 : 0;
            F.checked = F.differ = F.unchecked = 0;
        }
        char line[384];
        std::snprintf(line, sizeof line,
                      "aot check: %u calls checked (%u functions), %u differ, %u unchecked (a call to Box86 inside); "
                      "since the start %u calls of %u of %u functions, %u differ, %u unchecked; with x87 code %u "
                      "calls of %u of %u functions",
                      calls, fns, differ, unchecked, calls_all, fns_all, (unsigned)g_fns.size(), differ_all,
                      unchecked_all, x87_calls, x87_fns, x87_all);
        out.push_back(line);
    }
    {
        uint32_t traps = 0;
        std::vector<std::pair<uint32_t, const Fn *>> by;
        for (Fn &F : g_fns) {
            if (F.entered)
                by.push_back({F.entered, &F});
            traps += F.entered;
        }
        if (traps) {
            char line[512];
            int p = std::snprintf(line, sizeof line, "aot: %u calls from Box86 into compiled code:", traps);
            std::sort(by.begin(), by.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            for (size_t k = 0; k < by.size() && k < (g_prof ? 12 : 6) && p < (int)sizeof line; k++) {
                p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second->f->va, by[k].first);
                if (g_prof && p < (int)sizeof line)
                    p += std::snprintf(line + p, sizeof line - p, " (%.2f us)",
                                       (double)by[k].second->ticks_in / by[k].first / kTicksPerUs);
            }
            out.push_back(line);
        }
        if (!g_prof_sites.empty()) {
            std::vector<std::pair<uint32_t, uint64_t>> sites;
            for (const auto &kv : g_prof_sites)
                sites.push_back({kv.second, kv.first});
            std::sort(sites.begin(), sites.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            char line[512];
            int p = std::snprintf(line, sizeof line, "aot: calls from Box86 by return address:");
            for (size_t k = 0; k < sites.size() && k < 10 && p < (int)sizeof line; k++)
                p += std::snprintf(line + p, sizeof line - p, " %08x<-%08x %u", (uint32_t)(sites[k].second >> 32),
                                   (uint32_t)sites[k].second, sites[k].first);
            out.push_back(line);
            g_prof_sites.clear();
        }
        // The window's own time, in every mode (the lane's virtual clock
        // keeps the game's work per window the same: its host time compares
        // the modes); with D2_AOT_PROF, the time inside compiled code.
        const uint64_t now = ticks();
        if (g_prof_window_t0 && !g_fns.empty()) {
            char line[160];
            if (g_prof)
                std::snprintf(line, sizeof line, "aot time: %.0f ms this window, %.0f inside compiled code",
                              (double)(now - g_prof_window_t0) / kTicksPerUs / 1000,
                              (double)g_prof_outer / kTicksPerUs / 1000);
            else
                std::snprintf(line, sizeof line, "aot time: %.0f ms this window",
                              (double)(now - g_prof_window_t0) / kTicksPerUs / 1000);
            out.push_back(line);
        }
        g_prof_window_t0 = now;
        g_prof_outer = 0;
        if (!g_co_by.empty()) {   // D2_AOT_CALLOUTS=1: CpuAot's callouts this window, the most first
            std::vector<std::pair<uint32_t, uint32_t>> by;
            uint32_t all = 0;
            for (const auto &kv : g_co_by)
                by.push_back({kv.second, kv.first}), all += kv.second;
            std::sort(by.begin(), by.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            char line[640];
            int p = std::snprintf(line, sizeof line, "aot: %u callouts to %u places:", all, (unsigned)by.size());
            for (size_t k = 0; k < by.size() && k < 24 && p < (int)sizeof line; k++) {
                const d2rt::Shim *sh = rt::bridge().slot_shim(by[k].second);
                p += std::snprintf(line + p, sizeof line - p, " %s%s%08x %u", sh ? sh->tag.c_str() : "", sh ? "@" : "",
                                   by[k].second, by[k].first);
            }
            out.push_back(line);
            g_co_by.clear();
        }
        std::vector<std::pair<uint32_t, const Fn *>> direct;
        uint32_t directs = 0;
        for (Fn &F : g_fns) {
            if (F.direct)
                direct.push_back({F.direct, &F}), directs += F.direct;
            F.entered = 0, F.ticks_in = 0, F.direct = 0;
        }
        if (directs) {
            std::sort(direct.begin(), direct.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            char line[320];
            int p = std::snprintf(line, sizeof line, "aot: %u calls from natives run in C, not nested:", directs);
            for (size_t k = 0; k < direct.size() && k < 6 && p < (int)sizeof line; k++)
                p += std::snprintf(line + p, sizeof line - p, " %08x %u", direct[k].second->f->va, direct[k].first);
            out.push_back(line);
        }
    }
    if (g_dropped_window) {
        char line[160];
        std::snprintf(line, sizeof line, "aot: %u functions left to Box86 after their first calls (under %.2f us each), "
                      "%u since turned on", g_dropped_window, g_min_us, g_dropped);
        out.push_back(line);
        g_dropped_window = 0;
    }
    if (g_exits) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: %u exits to Box86 (the rest of a call):", g_exits);
        std::vector<std::pair<uint32_t, uint32_t>> to(g_exits_to.begin(), g_exits_to.end());
        std::sort(to.begin(), to.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t k = 0; k < to.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", to[k].first, to[k].second);
        out.push_back(line);
        g_exits = 0;
        g_exits_to.clear();
    }
    if (g_handed || g_handed_direct) {
        char line[128];
        std::snprintf(line, sizeof line, "aot: natives' hand-backs to compiled code: %u through its trap, %u in C",
                      g_handed, g_handed_direct);
        out.push_back(line);
        g_handed = g_handed_direct = 0;
    }
    if (g_callout_shims || g_callout_fns || g_callout_back) {
        char line[256];
        int p = std::snprintf(line, sizeof line,
                              "aot: callouts in C: %u to shims, %u to compiled functions; %u back to Box86 at the return:",
                              g_callout_shims, g_callout_fns, g_callout_back);
        std::vector<std::pair<uint32_t, uint32_t>> to(g_callout_back_by.begin(), g_callout_back_by.end());
        std::sort(to.begin(), to.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t k = 0; k < to.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", to[k].first, to[k].second);
        out.push_back(line);
        g_callout_shims = g_callout_fns = g_callout_back = 0;
        g_callout_back_by.clear();
    }
    if (g_stale_shims || g_stale_guest) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: calls to functions natives changed: %u to their shims in C "
                              "(%u handed back to the game's code, %u waits in place), %u through Box86:",
                              g_stale_shims, g_stale_handed, g_stale_waits, g_stale_guest);
        std::vector<std::pair<uint32_t, uint32_t>> by;
        for (const auto &kv : g_stale_by)
            by.push_back({kv.second, kv.first});
        std::sort(by.rbegin(), by.rend());
        for (size_t k = 0; k < by.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second, by[k].first);
        out.push_back(line);
        g_stale_shims = g_stale_handed = g_stale_waits = g_stale_guest = 0;
        g_stale_by.clear();
    }
    if (g_reent_runs) {
        char line[256];
        int p = std::snprintf(line, sizeof line,
                              "aot: threads on at a return site in compiled code: %u (%u functions run, %u exited); "
                              "Box86 on at:", g_reent_runs, g_reent_fns, g_reent_exits);
        std::vector<std::pair<uint32_t, uint32_t>> by;
        for (const auto &kv : g_reent_ended)
            by.push_back({kv.second, kv.first});
        std::sort(by.rbegin(), by.rend());
        for (size_t k = 0; k < by.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second, by[k].first);
        out.push_back(line);
        g_reent_runs = g_reent_fns = g_reent_exits = 0;
        g_reent_ended.clear();
    }
    if (g_resumed) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: the natives' hand-backs in compiled code: %u (%u exited):",
                              g_resumed, g_resume_exits);
        for (const auto &kv : g_resumed_by)
            if (p < (int)sizeof line)
                p += std::snprintf(line + p, sizeof line - p, " %08x %u", kv.first, kv.second);
        out.push_back(line);
        g_resumed = g_resume_exits = 0;
        g_resumed_by.clear();
    }
    if (g_nested) {
        char line[256];
        int p = std::snprintf(line, sizeof line, "aot: %u nested runs of callees not compiled:", g_nested);
        std::vector<std::pair<uint32_t, uint32_t>> by;
        for (const auto &kv : g_nested_by)
            by.push_back({kv.second, kv.first});
        std::sort(by.rbegin(), by.rend());
        for (size_t k = 0; k < by.size() && k < 6 && p < (int)sizeof line; k++)
            p += std::snprintf(line + p, sizeof line - p, " %08x %u", by[k].second, by[k].first);
        out.push_back(line);
        g_nested = 0;
        g_nested_by.clear();
    }
    g_empty_n = 0;
    g_empty_ticks = 0;
    return out;
}

}  // namespace game

// ---- what the generated C calls (aot_rt.h) ----

// The host test (tests/d2d) runs no game code: it sets this to count a
// nested run as left to the game.
void (*aot_guest_hook)(uint32_t va, AotRegs *r) = nullptr;

extern "C" {

uint32_t aot_fs_base(void) { return rt::current_teb(); }

void aot_guest_call(uint32_t va, AotRegs *r)
{
    game::g_nested++;
    game::g_nested_by[va]++;
    game::left(game::kLeftNested, va);
    if (g_cpu_aot) {
        cpuaot_call(va, r);
        return;
    }
    if (game::g_journaling)
        game::g_impure = true;   // the check (issue #114): Box86's writes aren't journaled
    if (aot_guest_hook) {
        aot_guest_hook(va, r);
        return;
    }
    const uint64_t t0 = game::ticks();
    game::nested_run(*game::g_cpu, va, r->r);
    game::g_nested_ticks += game::ticks() - t0;
    game::g_nested_n++;
}

// ---- CpuAot (issue #138, rt_cpu_aot.h): no Box86 ----------------------------
//
// The code at eip run until control is at `until` (0: until the thread's
// sentinel): a compiled function from its entry, then on at the return
// address it popped; the Bridge's trap (a shim, with its redirects); a
// native's hook at an entry, its shim. Anything else stops the run.
static std::map<uint32_t, uint32_t> g_aot_esp_off;        // callouts that came back with another esp, by callee

static game::AotStatus g_aot_status;
extern "C++" {
game::AotStatus game::aot_status() { return g_aot_status; }
}

static void cpuaot_bind(d2rt::Cpu &c)
{
    game::g_cpu = &c;
    aot_fpu = static_cast<AotFpu *>(d2rt::cpu_aot_fpu(c));
    aot_xmm = reinterpret_cast<AotXmm *>(reinterpret_cast<uint8_t *>(aot_fpu) + sizeof(AotFpu));
}

static bool cpuaot_dispatch(d2rt::Cpu &c, uint32_t eip, uint32_t until)
{
    using namespace game;
    for (;;) {
        if (until && eip == until)
            return true;
        if (d2rt::cpu_aot_is_trap(c, eip)) {
            if (const d2rt::Cpu::IntrinNoGilFn in = g_fast_on ? d2rt::cpu_aot_intrinsic(c, eip) : nullptr) {
                // A clock read or a lock call nobody waits on (the runtime's
                // in-line intrinsics): no Bridge, the call done here.
                const uint32_t esp = c.reg(4);
                d2rt::Cpu::IntrinFrame fr = {aot_ld32(aot_mem, esp), aot_ld32(aot_mem, esp + 4u), 0, 0, nullptr};
                if (in(fr, eip)) {
                    c.set_reg(0, fr.eax);
                    c.set_reg(4, esp + 4u + fr.pop);
                    if (d2rt::cpu_aot_slice_over(c)) {
                        c.set_reg(d2rt::R_EIP, fr.ret);
                        d2rt::cpu_aot_yield(c);
                        cpuaot_bind(c);
                    }
                    eip = fr.ret;
                    continue;
                }
            }
            c.set_reg(d2rt::R_EIP, eip);
            g_at_trap = eip;
            AOT_AT_SAVE;
            AOT_AT_SET(eip);
            if (!d2rt::cpu_aot_trap(c, eip)) {
                if (eip == rt::bridge().sentinel())
                    return false;        // the thread's, or a nested run's, end
                AOT_AT_SET(1);
                d2rt::cpu_aot_yield(c);  // a yield or a wait: here until the scheduler runs the thread again
            } else if (d2rt::cpu_aot_slice_over(c)) {
                AOT_AT_SET(1);
                d2rt::cpu_aot_yield(c);  // preempted: its slice's share of trap calls made
            }
            AOT_AT_BACK;
            cpuaot_bind(c);
            eip = c.reg(d2rt::R_EIP);
            continue;
        }
        if (d2rt::cpu_aot_take_code_changed(c) && aot_funcs && aot_funcs[0].va) {
            uint32_t changed = 0;   // a native hooked: what it changed, for the compiled callers (AOT_STALE)
            std::string names;
            stale_funcs(c, changed, names);
            g_stale_slot.clear();
            // The natives bound to a function's address (rt::entry_natives):
            // each one's trap by the function's row, and the row marked so
            // that compiled callers go to it (aot_call_stale) instead of the
            // compiled original, which is the hand-backs' alone.
            g_native_slot.assign(g_all_at.size(), 0);
            for (const auto &e : rt::entry_natives()) {
                const auto f = g_all_at.find(e.first);
                if (f == g_all_at.end())
                    continue;   // not compiled: the dispatcher asks rt::entry_native
                g_native_slot[f->second] = e.second;
                aot_stale[f->second] = 1;
            }
            // Each one named: the game writes none of its code, so this is
            // the natives' installs (one check after them) and then a native
            // turned on or off, nothing else.
            static uint32_t checks = 0;
            g_aot_status = {(uint32_t)g_all_at.size(), changed, (uint32_t)rt::entry_natives().size()};
            rt::logf("aot: the compiled functions' bytes checked against the image (check %u): %u differ%s%s; %u natives bound "
                     "to their functions' addresses",
                     (unsigned)++checks, (unsigned)changed, changed ? ": " : "", names.substr(0, 400).c_str(),
                     (unsigned)rt::entry_natives().size());
#ifdef D2RT_CPU_AOT
            // The start's check is the Game.exe's (issue #139): with every
            // native's site known to the recompiler, a function that differs
            // then is a Game.exe other than the one compiled, and nothing can
            // run it. (D2_NATIVE_MASK, a test's: a group compiled and not
            // applied differs, and the run goes on to where it is reached.)
            if (checks == 1 && changed && !std::getenv("D2_NATIVE_MASK"))
                d2rt::cpu_aot_stop("this Game.exe isn't the one this build was compiled from; functions that differ, the first at",
                                   (uint32_t)std::strtoul(names.c_str(), nullptr, 16));
#endif
        }
        if (aot_mem[eip] == 0xe9) {   // a native's hook at the entry: its shim's trap
            int32_t rel;
            std::memcpy(&rel, aot_mem + eip + 1, 4);
            const uint32_t to = eip + 5u + (uint32_t)rel;
            if (d2rt::cpu_aot_is_trap(c, to)) {
                eip = to;
                continue;
            }
        }
        const auto it = g_all_at.find(eip);
        if (const uint32_t slot = it != g_all_at.end() ? (it->second < g_native_slot.size() ? g_native_slot[it->second] : 0)
                                                       : rt::entry_native(eip)) {
            eip = slot;   // a native bound to this function: its trap
            continue;
        }
        if (it == g_all_at.end() && (eip == 0x006829c0u || eip == 0x00683840u)) {
            // The C runtime's memcpy and memmove (cdecl: to, from, count; to
            // in eax). The recompiler writes them in line where they are
            // called (its intrinsics) and can't compile them as functions
            // (their jumps through tables indexed from the middle), so one
            // reached through a pointer or by a loose function's jump is
            // done here.
            uint32_t r[8];
            c.regs_gp(r);
            const uint32_t to = aot_ld32(aot_mem, r[4] + 4u);
            aot_memmove(to, aot_ld32(aot_mem, r[4] + 8u), aot_ld32(aot_mem, r[4] + 12u));
            eip = aot_ld32(aot_mem, r[4]);
            c.set_reg(0, to);
            c.set_reg(4, r[4] + 4u);
            continue;
        }
        if (it == g_all_at.end())
            d2rt::cpu_aot_stop("no compiled code here", eip);
        const AotFunc &f = aot_funcs[it->second];
        if (aot_stale[it->second])   // compiled as a group changes it, and the group isn't applied here
            d2rt::cpu_aot_stop("the code here isn't what was compiled (a natives' group compiled, not applied)", eip);
        uint32_t r[8];
        c.regs_gp(r);
        const uint32_t esp0 = r[4], ra = aot_ld32(aot_mem, esp0);
        cpuaot_bind(c);
        g_at_fn = eip;
        AOT_AT_SAVE;
        const uint32_t to = f.sig(r);
        AOT_AT_BACK;
        if (f.pops == AOT_POPS_LOOSE) {
            // A loose function: esp as its ret or its tail jump left it, and
            // on where it says (the return address it popped, the jump's
            // target, which may be a native's site).
            static std::vector<uint8_t> ran;   // each one's first run named: the lane's proof that it reaches one
            if (ran.size() <= it->second)
                ran.resize(g_all_at.size());
            if (!ran[it->second]) {
                ran[it->second] = 1;
                rt::logf("cpu_aot: the loose function %08x ran (entered with esp %08x, return %08x): on at %08x, esp %08x", f.va,
                         esp0, ra, to, r[4]);
            }
            eip = aot_site_target(to);
        } else {
            r[4] = esp0 + 4u + f.pops;
            eip = ra;
        }
        for (int k = 0; k < 8; k++)
            c.set_reg(k, r[k]);
    }
}

// A call from compiled code to x, the return address at [esp]: until control
// is back there, with the registers as the callee left them.
static void cpuaot_call(uint32_t x, AotRegs *t)
{
    d2rt::Cpu &c = *game::g_cpu;
    const uint32_t ra = aot_ld32(aot_mem, t->r[4]);
    // The two callouts there are most of (the lane's fight: 3,100 a frame,
    // half of them the clock and the lock calls, the rest mostly compiled
    // functions called through a pointer), without the dispatcher's loop,
    // its copies of the registers to the CPU and back, or the Bridge.
    if (!g_fast_on) {
    } else if (d2rt::cpu_aot_is_trap(c, x)) {
        if (const d2rt::Cpu::IntrinNoGilFn in = d2rt::cpu_aot_intrinsic(c, x)) {
            const uint32_t esp = t->r[4];
            d2rt::Cpu::IntrinFrame fr = {ra, aot_ld32(aot_mem, esp + 4u), 0, 0, nullptr};
            if (in(fr, x)) {
                t->r[0] = fr.eax;
                t->r[4] = esp + 4u + fr.pop;
                if (d2rt::cpu_aot_slice_over(c)) {   // its slice's share of calls made: the others' turn
                    for (int k = 0; k < 8; k++)
                        c.set_reg(k, t->r[k]);
                    c.set_reg(d2rt::R_EIP, ra);
                    d2rt::cpu_aot_yield(c);
                    cpuaot_bind(c);
                }
                return;
            }
        }
    } else if (!d2rt::cpu_aot_code_changed(c) && aot_mem[x] != 0xe9) {
        const uint32_t row = g_fast_at.find(x);
        if (row != FastAt::kNone && !aot_stale[row] && aot_funcs[row].pops != AOT_POPS_LOOSE) {
            const uint32_t esp0 = t->r[4];
            g_at_fn = x;
            AOT_AT_SAVE;
            aot_funcs[row].sig(t->r);
            AOT_AT_BACK;
            t->r[4] = esp0 + 4u + aot_funcs[row].pops;
            return;
        }
    }
    for (int k = 0; k < 8; k++)
        c.set_reg(k, t->r[k]);
    if (!cpuaot_dispatch(c, x, ra))
        d2rt::cpu_aot_stop("a thread ended inside a call from compiled code to", x);
    c.regs_gp(t->r);
    cpuaot_bind(c);
}

[[noreturn]] static void call_out_back(uint32_t x, uint32_t to, const uint32_t *r, bool clean = false);

// A register the original keeps that its native didn't (named once each):
// compiled callers above take it as kept.
static void stale_kept(uint32_t va, const uint32_t *in, const uint32_t *r)
{
    using namespace game;
    static std::set<uint32_t> named;
    const auto f = g_fn_at.find(va);
    const uint8_t keeps = f == g_fn_at.end() ? 0 : g_fns[f->second].f->preserved;
    for (int k = 0; k < 8; k++)
        if (k != 4 && (keeps >> k & 1) && r[k] != in[k] && named.insert(va << 3 | k).second)
            rt::logf("aot: the native at %08x left %s changed (%08x, was %08x), which the original keeps", va,
                     reg_name(k), r[k], in[k]);
}

void aot_call_stale(uint32_t va, AotRegs *t)
{
    using namespace game;
    g_stale_by[va]++;
    if (g_cpu_aot) {
        cpuaot_call(va, t);
        return;
    }
    const d2rt::Shim *sh = game::stale_shim(va);
    if (!sh && !g_no_exit) {
        // No shim at its entry (a patch in line): Box86 runs it from there and
        // returns through the compiled callers' return addresses (issue #138).
        g_stale_guest++;
        uint32_t *r = t->r;
        g_cause = kLeftChanged;
        aot_exit(va, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    if (!sh) {   // nothing to exit to: Box86's call, nested
        g_stale_guest++;
        uint32_t in[8];
        std::memcpy(in, t->r, sizeof in);
        aot_guest_call(va, t);
        stale_kept(va, in, t->r);
        return;
    }
    if (g_journaling)
        g_impure = true;   // the shim's writes aren't journaled
    // As its trap would: the shim with the registers at the call, then its
    // ret (eax, the pops, on at the return or where it redirects).
    uint32_t *r = t->r;
    Cpu &c = *g_cpu;
    const uint32_t esp0 = r[4], ra = c.read_u32(esp0);
    const uint32_t esp_add = 4u + (sh->stdcall_cleanup ? 4u * sh->argc : 0u);   // sh may dangle after fn
    uint32_t in[8];
    std::memcpy(in, r, sizeof in);
    const TrapRegs saved(c);
    for (int k = 0; k < 8; k++)
        c.set_reg(k, r[k]);
    const uint32_t eax = sh->fn(c);
    uint32_t rd = rt::bridge().take_redirect();
    c.trap_epilogue(eax, esp_add, rd ? rd : ra);
    if (rd == ra)   // its hand-back's compiled code ran to the return (aot_resume)
        rd = 0;
    g_stale_shims++;
    if ((rd || rt::bridge().yield_requested()) && !g_no_exit) {
        // Handed back to the game's code, or a wait (issue #138): as a
        // callout's, an exit, Box86 on at that code or at the return (the
        // trap's tail yields), not a nested run, which can't wait on another
        // guest thread. So a caller that reaches a native's function stays
        // compiled even where the function may exit.
        c.regs_gp(r);
        saved.put(c);
        g_stale_handed += rd != 0;
        call_out_back(va, rd ? rd : ra, r);
    }
    if (rt::bridge().take_yield_request()) {   // a wait: done here, as in a nested run
        rt::finish_nested_block(c);
        g_stale_waits++;
    }
    if (rd) {   // handed back to the game's code: Box86 runs it to the return
        c.write_u32(esp0, rt::bridge().sentinel());
        run_to_sentinel(c, rd, va);
        c.write_u32(esp0, ra);
        g_stale_handed++;
    }
    c.regs_gp(r);
    saved.put(c);
    stale_kept(va, in, r);
}

void aot_exit(uint32_t target, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
              uint32_t esi, uint32_t edi)
{
    target = game::aot_site_target(target);
    if (g_cpu_aot && d2rt::cpu_aot_is_trap(*game::g_cpu, target)) {
        // A shim that doesn't return (ExitProcess, ExitThread: the only exits
        // the recompiler writes to an import): run here, the thread's last.
        // It yields in the dispatcher and the scheduler doesn't run it again.
        d2rt::Cpu &c = *game::g_cpu;
        const uint32_t r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
        for (int k = 0; k < 8; k++)
            c.set_reg(k, r[k]);
        cpuaot_dispatch(c, target, 0);
        d2rt::cpu_aot_stop("a shim that doesn't return came back:", target);
    }
    if (g_cpu_aot) {
        rt::logf("cpu_aot: an exit in the compiled code to %08x, its return address %08x", target,
                 aot_ld32(aot_mem, esp));
        d2rt::cpu_aot_stop("an exit in the compiled code (a call it can't make in C), to", target);
    }
    game::left(game::g_cause, game::g_cause_at ? game::g_cause_at : target);
    game::g_cause = game::kLeftExit;
    game::g_cause_at = 0;
    game::ExitJump *x = game::g_exit_jump;
    if (!x || game::g_no_exit) {   // compiled code entered other than by a trap (AotFunc::may_exit not honoured): a bug of ours
        rt::logf("aot: an exit to %08x with no trap to go back to", target);
        aot_unreachable(target);
    }
    x->target = target;
    {   // D2_AOT_EXIT_LOG=<hex va> (issue #138): the first 40 exits to va, with the return address at esp
        static const uint32_t to =
            std::getenv("D2_AOT_EXIT_LOG") ? (uint32_t)std::strtoul(std::getenv("D2_AOT_EXIT_LOG"), nullptr, 16) : 0;
        static long logged = 0;
        if (to && target == to && logged < 40) {
            logged++;
            const auto it = game::g_fn_at.find(target);
            const game::Fn *F = it == game::g_fn_at.end() ? nullptr : &game::g_fns[it->second];
            rt::logf("aot: exit to %08x, [esp] %08x: %s, hooked %d, dropped %d, stale %d, mode %d, callouts %d, "
                     "shim %d, no_exit %d", target, aot_ld32(aot_mem, esp), F ? "in the table" : "not in the table",
                     F ? F->hooked : -1, F ? F->dropped : -1, F ? aot_stale[F->f - aot_funcs] : -1,
                     (int)game::g_mode, game::g_callouts, rt::bridge().slot_shim(target) != nullptr, game::g_no_exit);
        }
    }
    const uint32_t r[8] = {eax, ecx, edx, ebx, esp, ebp, esi, edi};
    std::memcpy(x->r, r, sizeof r);
    std::longjmp(x->jb, 1);
}

// Box86 goes on at the call's return with what the callee left: esp, or a
// callee-saved register, not what the compiled caller assumed; or a shim that
// asked to yield (the trap's tail that entered compiled code does it) or to
// run guest code first (rd).
//
// clean: only a yield; registers and esp as the compiled caller has them at
// the return. Where the return is a site with an entry (issue #138), the
// thread goes on there in compiled code: the exit is to the return sites'
// trap, the site kept by esp.
[[noreturn]] static void call_out_back(uint32_t x, uint32_t to, const uint32_t *r, bool clean)
{
    using namespace game;
    g_callout_back++;
    g_callout_back_by[x]++;
    g_cause = clean ? kLeftBackWait : kLeftBack;
    g_cause_at = x;
    if (clean && g_reent_trap && g_mode == AotMode::Sig && g_reent_at.count(to)) {
        g_reent_pending[r[4]] = to;
        to = g_reent_trap;
        g_cause = kLeftTrap;
    }
    aot_exit(to, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

void aot_call_out(uint32_t x, AotRegs *t, uint32_t pops)
{
    using namespace game;
    x = aot_site_target(x);   // a native's call at one of its sites (native_sites_table.h)
    uint32_t *r = t->r;
    const uint32_t esp0 = r[4], ra = aot_ld32(aot_mem, esp0), want = esp0 + 4u + pops;
    if (g_cpu_aot) {
        if (g_co_on)
            g_co_by[x]++;
        // No trap to go back to: the callee where it is, whatever it is. One
        // that pops other than the site counted is named once and goes on
        // (the code after takes esp as it comes back).
        cpuaot_call(x, t);
        if (r[4] != want && !g_aot_esp_off[x]++)
            rt::logf("cpu_aot: a callout to %08x (return %08x) came back with esp %08x, the site counted %08x", x, ra,
                     r[4], want);
        return;
    }
    const uint32_t keep[4] = {r[3], r[5], r[6], r[7]};
    if (g_no_exit) {
        // No trap to exit to: a compiled callee in C if it can't exit
        // either, else Box86's call, nested (a shim's wait done in place).
        const auto it = g_callouts ? g_fn_at.find(x) : g_fn_at.end();
        const Fn *F = it == g_fn_at.end() ? nullptr : &g_fns[it->second];
        if (F && (F->hooked || F->dropped) && !F->f->may_hard && g_mode == AotMode::Sig) {
            F->f->sig(r);
            r[4] = esp0 + 4u + F->f->pops;
            g_callout_fns++;
        } else
            aot_guest_call(x, t);
        if (r[4] != want || r[3] != keep[0] || r[5] != keep[1] || r[6] != keep[2] || r[7] != keep[3]) {
            rt::logf("aot: a callout to %08x (return %08x) came back with esp %08x, not %08x, where nothing can "
                     "exit: the compiled caller can't go on", x, ra, r[4], want);
            aot_stop("a callout came back with another esp, to", x);
        }
        return;
    }
    // The check's journaled run too: a compiled callee's journaled code, and
    // a shim's call unchecked (its writes aren't in the journal).
    if ((g_mode == AotMode::Sig || (g_mode == AotMode::Check && g_journaling)) && g_callouts) {
        if (const d2rt::Shim *sh = rt::bridge().slot_shim(x)) {
            if (g_journaling)
                g_impure = true;
            // As the trap would: the CPU's registers as at the call, the
            // shim, then its ret's pops. (The trap's own context restored
            // after: its tail goes on from it.)
            const uint32_t popped = 4u + (sh->stdcall_cleanup ? 4u * sh->argc : 0u);
            // Only the trap's general registers and eip kept: the FPU's
            // state is the shim's to leave (compiled x87 code works on the
            // CPU's own, and a float result in st0 is the caller's), flags don't outlive a
            // call (and reading them makes Box86 compute its deferred ones),
            // and the full context's 768-byte blob each way cost more than
            // the call.
            Cpu &c = *g_cpu;
            uint32_t saved[9];
            c.regs_gp(saved);
            saved[8] = c.reg(d2rt::R_EIP);
            for (int k = 0; k < 8; k++)
                c.set_reg(k, r[k]);
            {   // each shim's first callout, named (the lane)
                static const d2rt::Shim *last;
                static std::set<const d2rt::Shim *> seen;
                if (sh != last && seen.insert(last = sh).second)
                    rt::logf("aot: callout to %s from %08x", sh->tag.c_str(), ra);
            }
            const uint32_t eax = sh->fn(c);
            c.regs_gp(r);
            r[0] = eax;
            r[4] += popped;   // the trap's tail: from esp as the shim left it
            const uint32_t rd = rt::bridge().take_redirect();
            const bool yield = rt::bridge().yield_requested();
            for (int k = 0; k < 8; k++)
                c.set_reg(k, saved[k]);
            c.set_reg(d2rt::R_EIP, saved[8]);
            g_callout_shims++;
            if (rd || yield || r[4] != want) {
                const bool kept = r[3] == keep[0] && r[5] == keep[1] && r[6] == keep[2] && r[7] == keep[3];
                call_out_back(x, rd ? rd : ra, r, !rd && r[4] == want && kept);
            }
        } else {
            const auto it = g_fn_at.find(x);
            if (it == g_fn_at.end()) {
                // One that can't be hooked: its compiled code, as a hooked
                // one's (the check's run, its journaled code), else an exit.
                const auto u = g_unhooked_at.find(x);
                const uint32_t k = u == g_unhooked_at.end() ? 0 : u->second;
                const AotFunc *af = u == g_unhooked_at.end() ? nullptr
                                    : !g_journaling       ? &aot_funcs[k]
                                    : aotj_funcs && aotj_funcs[k].va == x ? &aotj_funcs[k]
                                                                          : nullptr;
                if (!af) {
                    g_cause = kLeftNotCompiled;
                    aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
                }
                af->sig(r);
                r[4] = esp0 + 4u + aot_funcs[k].pops;
                if (g_journaling)
                    g_garbage |= (uint8_t)(0x07u & ~(aot_funcs[k].out | aot_funcs[k].preserved));
                g_callout_fns++;
                if (r[4] != want)
                    call_out_back(x, ra, r);
                if (r[3] != keep[0] || r[5] != keep[1] || r[6] != keep[2] || r[7] != keep[3])
                    call_out_back(x, ra, r);
                return;
            }
            Fn &F = g_fns[it->second];
            const AotFunc *af = g_journaling ? F.fj : F.f;
            if (aot_stale[F.f - aot_funcs]) {   // changed by a native since it was compiled: the native's
                aot_call_stale(x, t);
                g_callout_fns++;
                if (r[4] != want)
                    call_out_back(x, ra, r);
            } else if ((!F.hooked && !F.dropped) || !af) {
                g_cause = kLeftNotHooked;
                aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
            } else {
                af->sig(r);   // an exit inside goes back to our own trap, as from a direct call
                r[4] = esp0 + 4u + F.f->pops;
                if (g_journaling)
                    g_garbage |= (uint8_t)(0x07u & ~(F.f->out | F.f->preserved));
                g_callout_fns++;
                if (r[4] != want)
                    call_out_back(x, ra, r);
            }
        }
        if (r[3] != keep[0] || r[5] != keep[1] || r[6] != keep[2] || r[7] != keep[3])
            call_out_back(x, ra, r);
        return;
    }
    aot_exit(x, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
}

// As the backend's ReadTSC (shim_impl.c).
#ifdef __3DS__
extern uint64_t svcGetSystemTick(void);
#endif
uint64_t aot_rdtsc(void)
{
    if (game::g_journaling)
        game::g_impure = true;   // the check: two reads of the clock never agree
#ifdef __3DS__
    return svcGetSystemTick();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

// As the backend's my_cpuid (shim_impl.c): compiled code and Box86 see one CPU.
void aot_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    const uint32_t leaf = *eax;
    *eax = 0;
    if (leaf == 0) {
        *eax = 1;
        *ebx = 0x756e6547;   // GenuineIntel
        *edx = 0x49656e69;
        *ecx = 0x6c65746e;
    } else if (leaf == 1) {
        *eax = 0x00000681;   // family 6, model 8, stepping 1
        *ebx = 0;
        *ecx = 0;
        *edx = rt::cpuid1_edx();
    } else {
        *ebx = *ecx = *edx = 0;
    }
}

void aot_bad_ret(uint32_t fn, uint32_t esp0, uint32_t esp)
{
    rt::logf("aot: %08x returned with esp %08x, entered with %08x: the static stack doesn't hold", fn, esp, esp0);
    aot_stop("a function returned with another esp than it was entered with:", fn);
}

void aot_unreachable(uint32_t va)
{
    rt::logf("aot: reached %08x, which the recompiler took for unreachable", va);
    aot_stop("code the recompiler took for unreachable was reached at", va);
}

void aot_fault(uint32_t va)
{
    rt::logf("aot: a division by zero at %08x", va);
    aot_stop("a division by zero at", va);
}

void aot_journal(uint32_t va, uint32_t n)
{
    game::g_journal.push_back({va, n, (uint32_t)game::g_old.size()});
    game::g_old.insert(game::g_old.end(), aot_mem + va, aot_mem + va + n);
}

}  // extern "C"
