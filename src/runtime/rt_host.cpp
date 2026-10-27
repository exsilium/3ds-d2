// See rt_host.h.
#include "runtime/rt_host.h"
#include "runtime/rt_layout.h"

#include "runtime/bridge.h"
#include "runtime/ds_emul.h"
#include "runtime/cpu.h"
#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/host_clock.h"
#include "runtime/prof.h"
#include "runtime/sched_cooperative.h"
#include "runtime/win32_shims_advapi32.h"
#include "runtime/win32_shims_gdi32.h"
#include "runtime/win32_shims_kernel32.h"
#include "runtime/win32_shims_locale.h"
#include "runtime/win32_shims_memory.h"
#include "runtime/win32_shims_misc.h"
#include "runtime/win32_shims_psapi.h"
#include "runtime/win32_shims_shell32.h"
#include "runtime/win32_shims_sync.h"
#include "runtime/win32_shims_user32.h"
#include "runtime/win32_shims_wait.h"
#include "runtime/win32_shims_window.h"

extern "C" {
#include "my_cpuid.h"
size_t d2rt_box86_dynarec_kb(void);
size_t d2rt_box86_custommalloc_kb(void);
size_t d2rt_box86_jmptbl_kb(void);
int dyn86_intrin_report(char *out, unsigned cap);   // translation-time intrinsics: calls and served
extern int dyn86_intrin_n;
int dyn86_block_of_native(uintptr_t pc, uint32_t *x86_addr, uint32_t *x86_size);   // fault reports
// The console's cache maintenance per translated block (mman_ctr.c). Weak:
// the Linux lanes have no such counters.
extern uint32_t dyn86_ctr_cache_syncs __attribute__((weak));
extern uint64_t dyn86_ctr_cache_sync_ticks __attribute__((weak));
}

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <malloc.h>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace d2rt;

namespace rt {
namespace {

RunOptions g_opt;
Bridge *g_br = nullptr;
CooperativeScheduler *g_sched = nullptr;
std::atomic<Cpu *> g_tick_cpu{nullptr};   // the CPU while the scheduler runs (profile_tick)
uint32_t g_exe_base = 0;
bool g_exited = false;
bool g_timed_out = false;
Stats g_stats;
uint64_t g_next_beat = 0;
uint32_t g_exit_code = 0;
// RunOptions::service: only on the thread that runs the guest (now_ms is also
// read by host threads), and not from inside itself.
thread_local bool t_runs_guest = false;
bool g_in_service = false;
uint64_t g_last_service_ms = 0;   // real clock (wx86_now_ms)
wx86::GuestRegion g_heap, g_va;
// When the layout puts the heap and VirtualAlloc ranges side by side (all of
// them do), both are faces of one pool: the heap grows up from its base, VA
// down from the top. D2's heap peaks while loading (~16 MiB at hero
// selection) and its VirtualAlloc in play (44 MiB in Blood Moor on the
// console), so fixed ranges ran out of VA with half the heap idle.
wx86::GuestRegion g_pool;
uint32_t g_pool_size = 0;   // 0: separate ranges
// RunOptions::pool_spare, kept back from the pool (GuestRegion::set_spare),
// and the takes from it the heartbeat has logged.
wx86::GuestRegion g_spare;
uint32_t g_spare_size = 0, g_spare_logged = 0;

// VirtualAlloc blocks by the guest code that allocated them (the return
// address of its VirtualAlloc call), so an allocation failure can say who
// holds the pool. The shim reports a request before it allocates, so each
// new block takes the owner of the request before it (va_owner_sync).
std::map<uint32_t, uint32_t> g_va_owner;
uint32_t g_va_pending = 0;

void va_owner_sync()
{
    const auto &used = g_va.used_map();
    for (auto it = g_va_owner.begin(); it != g_va_owner.end();)
        it = used.count(it->first) ? std::next(it) : g_va_owner.erase(it);
    for (const auto &u : used)
        if (g_va_owner.emplace(u.first, g_va_pending).second)
            g_va_pending = 0;
}

void va_observer(const WxMemEvent &e)
{
    if (e.kind == WX86_MEM_VA_RESERVE || e.kind == WX86_MEM_VA_COMMIT_FRESH) {
        va_owner_sync();
        g_va_pending = e.cpu->read_u32(e.cpu->reg(R_ESP));
    } else if (e.kind == WX86_MEM_VA_RELEASE) {
        g_va_owner.erase(e.addr);
    }
}

// One line: the largest owners of VirtualAlloc memory.
void log_va_owners()
{
    va_owner_sync();
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> by;   // owner -> blocks, bytes
    for (const auto &o : g_va_owner) {
        auto &b = by[o.second];
        b.first++;
        b.second += g_va.size_of(o.first);
    }
    std::vector<std::pair<uint32_t, uint32_t>> order;   // bytes, owner
    for (const auto &b : by)
        order.emplace_back(b.second.second, b.first);
    std::sort(order.rbegin(), order.rend());
    std::string line = "VirtualAlloc by caller:";
    for (size_t i = 0; i < order.size() && i < 8; i++) {
        char item[48];
        std::snprintf(item, sizeof item, " %08x %u KiB/%u", order[i].second, order[i].first >> 10,
                      by[order[i].second].first);
        line += item;
    }
    logf("%s", line.c_str());
}

// VirtualQuery support for WinVita's memory shims: report the arena regions.
bool vquery(uint32_t addr, uint32_t &base, uint32_t &size, uint32_t &state, uint32_t &type, uint32_t &protect,
            uint32_t &allocbase)
{
    const Layout &L = layout();
    struct R { uint32_t b, s; } regions[] = {{L.heap_base, L.heap_size}, {L.va_base, L.va_size}};
    for (auto &r : regions)
        if (addr >= r.b && addr < r.b + r.s) {
            base = addr & ~0xFFFu, size = 0x1000, state = 0x1000 /*MEM_COMMIT*/, type = 0x20000 /*MEM_PRIVATE*/,
            protect = 0x04 /*PAGE_READWRITE*/, allocbase = r.b;
            return true;
        }
    return false;
}

void init_teb(Cpu &cpu, uint32_t teb, uint32_t stack_top, uint32_t stack_limit)
{
    std::vector<uint8_t> zero(0x1000, 0);
    cpu.write(teb, zero.data(), (uint32_t)zero.size());
    cpu.write_u32(teb + 0x00, 0xFFFFFFFFu);   // ExceptionList: end of SEH chain
    cpu.write_u32(teb + 0x04, stack_top);     // StackBase
    cpu.write_u32(teb + 0x08, stack_limit);   // StackLimit
    cpu.write_u32(teb + kTebSelf, teb);
    cpu.write_u32(teb + kTebPeb, layout().peb);
}

}  // namespace

const RunOptions &options() { return g_opt; }
const Layout &layout() { return *g_opt.layout; }
wx86::GuestRegion &va_region() { return g_va; }
Stats &stats() { return g_stats; }
bool guest_read(uint32_t va, void *out, uint32_t n)
{
    if (!g_br || !g_opt.layout || (uint64_t)va + n > g_opt.layout->span || va < 0x10000)
        return false;
    return g_br->cpu()->read(va, out, n);
}

// ---- host fault report (issue #5) ---------------------------------------------------

namespace {

// What owns guest address `va`: a heap or VirtualAlloc block, a module, or a
// guest thread's stack. Into `buf`; "-" when nothing does.
using Modules = std::vector<std::pair<uint32_t, uint32_t>>;

const char *owner_of(uint32_t va, const Modules &mods, char *buf, size_t cap)
{
    if (const uint32_t b = g_heap.block_of(va)) {
        std::snprintf(buf, cap, "heap block %08x+%x of %x", b, va - b, g_heap.size_of(b));
        return buf;
    }
    if (const uint32_t b = g_va.block_of(va)) {
        std::snprintf(buf, cap, "VirtualAlloc block %08x+%x of %x", b, va - b, g_va.size_of(b));
        return buf;
    }
    for (const auto &m : mods)
        if (va >= m.first && va - m.first < m.second) {
            std::snprintf(buf, cap, "module %08x+%x", m.first, va - m.first);
            return buf;
        }
    if (g_sched)
        for (const auto &t : g_sched->threads())
            if (t->stack_top && va >= t->stack_base && va < t->stack_top) {
                std::snprintf(buf, cap, "stack of thread %u", t->id);
                return buf;
            }
    std::snprintf(buf, cap, "-");
    return buf;
}

}  // namespace

void crash_report(const uint32_t r[16], uint32_t far, void (*line)(const char *))
{
    char s[256], o[96];
    if (!g_br || !g_opt.layout) {
        line("crash: no guest running");
        return;
    }
    const Layout &L = *g_opt.layout;
    Cpu *cpu = g_br->cpu();
    const Modules mods = g_br->loaded_modules();   // once: the only allocation here
    const uintptr_t membase = (uintptr_t)cpu->hostptr(L.heap_base, 1) - L.heap_base;
    // Guest address of a host address, or ~0 outside the arena.
    const auto guest = [&](uintptr_t host) -> uint32_t {
        return host >= membase && host - membase < L.span ? (uint32_t)(host - membase) : 0xFFFFFFFFu;
    };
    const auto mapped = [&](uint32_t va, uint32_t n) { return va >= 0x10000 && (uint64_t)va + n <= L.span; };
    const auto word = [&](uint32_t va) {
        uint32_t v;
        std::memcpy(&v, (const void *)(membase + va), 4);
        return v;
    };

    uint32_t bx = 0, bsize = 0;
    const bool jit = dyn86_block_of_native(r[15], &bx, &bsize) != 0;
    if (jit) {
        // Box86's ARM mapping: r4-r11 = EAX ECX EDX EBX ESP EBP ESI EDI, r14 = the
        // last EIP the block stored (its start, or the target of a call).
        std::snprintf(s, sizeof s, "crash: in translated guest code, block %08x (%u x86 bytes), xEIP %08x", bx, bsize,
                      r[14]);
        line(s);
        std::snprintf(s, sizeof s, "crash: EAX %08x ECX %08x EDX %08x EBX %08x ESP %08x EBP %08x ESI %08x EDI %08x",
                      r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
        line(s);
    } else {
        line("crash: not in translated code (host code: symbolize pc and lr against the ELF)");
    }
    const uint32_t gfar = guest(far);
    if (gfar != 0xFFFFFFFFu) {
        std::snprintf(s, sizeof s, "crash: fault address = guest %08x (%s)", gfar, owner_of(gfar, mods, o, sizeof o));
        line(s);
    }
    if (g_sched)
        if (GuestThread *t = g_sched->current()) {
            std::snprintf(s, sizeof s, "crash: guest thread %u, entry %08x, stack %08x-%08x, TEB %08x", t->id, t->entry,
                          t->stack_base, t->stack_top, t->tib);
            line(s);
        }

    // Each guest register that points into guest memory: who owns it, and the
    // 16 bytes there (a corrupted object shows up next to its neighbours).
    static const char *const kNames[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    for (int i = 0; jit && i < 8; i++) {
        const uint32_t v = r[4 + i];
        if (!mapped(v, 16))
            continue;
        std::snprintf(s, sizeof s, "crash: %s -> %s: %08x %08x %08x %08x", kNames[i], owner_of(v, mods, o, sizeof o),
                      word(v), word(v + 4), word(v + 8), word(v + 12));
        line(s);
    }

    // Memory pressure, and every guest thread with its stack and TEB (a stack
    // or TEB past its region overlaps the heap: see the scheduler's REFUSED).
    std::snprintf(s, sizeof s, "crash: heap %u KiB (peak %u), VirtualAlloc %u KiB (peak %u), pool largest free %u KiB",
                  g_heap.cur() >> 10, g_heap.peak() >> 10, g_va.cur() >> 10, g_va.peak() >> 10,
                  g_va.largest_free() >> 10);
    line(s);
    if (g_sched)
        for (const auto &t : g_sched->threads()) {
            std::snprintf(s, sizeof s, "crash: thread %u entry %08x state %d stack %08x-%08x TEB %08x", t->id, t->entry,
                          (int)t->state, t->stack_base, t->stack_top, t->tib);
            line(s);
        }

    // Guest call-stack hint: words on the guest stack that point into a module.
    if (jit && mapped(r[8], 4)) {
        uint32_t top = r[8] + 0x200;
        if (g_sched)
            if (GuestThread *t = g_sched->current())
                if (t->stack_top && r[8] >= t->stack_base && r[8] < t->stack_top)
                    top = std::min(top, t->stack_top);
        for (uint32_t a = r[8]; a + 4 <= top && mapped(a, 4); a += 4) {
            const uint32_t v = word(a);
            for (const auto &m : mods)
                if (v >= m.first && v - m.first < m.second) {
                    std::snprintf(s, sizeof s, "crash: [esp+%03x] %08x", a - r[8], v);
                    line(s);
                    break;
                }
        }
    }
}

uint32_t native_entry(const char *name, uint32_t stack_args, std::function<uint32_t(Cpu &)> fn)
{
    Shim s;
    s.argc = stack_args;
    s.stdcall_cleanup = true;
    s.tag = std::string("3DSD2NATIVE!") + name;
    s.fn = std::move(fn);
    g_br->register_shim("3DSD2NATIVE", name, s);
    return g_br->shim_trap("3DSD2NATIVE", name);
}

bool hook_function(Cpu &c, uint32_t va, const uint8_t *expect, uint32_t n, uint32_t stack_args,
                   std::function<uint32_t(Cpu &)> fn, const char *name, bool in_line)
{
    if (n < 5)
        return false;
    std::vector<uint8_t> have(n);
    if (!c.read(va, have.data(), n) || std::memcmp(have.data(), expect, n) != 0) {
        logf("native %s: bytes at %08x differ, not hooked", name, va);
        return false;
    }
    const uint32_t trap = native_entry(name, stack_args, std::move(fn));
    if (!trap)
        return false;
    uint8_t jmp[5] = {0xE9};
    const uint32_t rel = trap - (va + 5);
    std::memcpy(jmp + 1, &rel, 4);
    c.write(va, jmp, 5);
    c.invalidate_code(va, 5);
    if (in_line)
        c.set_inline_shim(trap);
    logf("native %s at %08x%s", name, va, in_line ? " (in line)" : "");
    return true;
}

MemoryReport memory_report()
{
    const Layout &L = layout();
    const uint32_t heap = g_heap.used_bytes(), va = g_va.used_bytes();
    // The pool's own count holds the spare whole, and not the blocks in it.
    const uint32_t free = g_pool_size ? g_pool_size - g_pool.used_bytes() : L.heap_size + L.va_size - heap - va;
    return {heap, g_heap.peak(), L.heap_size, va, g_va.peak(), L.va_size, free, g_va.largest_free(),
            g_spare_size ? g_spare.used_bytes() : 0, g_spare_size ? g_pool.spare_takes() : 0,
            (uint32_t)d2rt_box86_dynarec_kb(), (uint32_t)d2rt_box86_custommalloc_kb(), (uint32_t)d2rt_box86_jmptbl_kb()};
}
Bridge &bridge() { return *g_br; }
uint32_t exe_base() { return g_exe_base; }

uint32_t current_teb()
{
    if (ThreadScheduler *s = wx86_sched())
        if (GuestThread *t = s->current())
            if (t->tib)
                return t->tib;
    return layout().main_tib;
}

void set_last_error(Cpu &c, uint32_t err) { c.write_u32(current_teb() + kTebLastError, err); }

uint32_t guest_call(Cpu &c, uint32_t va, std::initializer_list<uint32_t> args)
{
    X86Context saved;
    c.save_context(saved);
    uint32_t esp = (c.reg(R_ESP) - 0x100) & ~15u;   // below the caller's frame
    for (auto it = args.end(); it != args.begin();) {
        --it;
        esp -= 4;
        c.write_u32(esp, *it);
    }
    esp -= 4;
    c.write_u32(esp, g_br->sentinel());
    c.set_reg(R_ESP, esp);
    const char *fault = nullptr;
    const bool ok = c.run(va, &fault);
    const uint32_t ret = c.reg(R_EAX);
    c.load_context(saved);
    if (!ok)
        logf("guest_call %08x faulted: %s at %08x", va, fault ? fault : "?", c.fault_addr());
    return ret;
}

namespace {
// What the last heartbeat interval cost. Always: scheduler idle time and the
// console's cache maintenance. With PROF_COUNTERS (package-cia --prof): the
// split between translated code and shims, the busiest shims, and an EIP
// profile per guest thread (one sample per preempted slice), after which the
// samples are reset so every interval is a profile of its own.
struct ProfMark {
    uint64_t ms, idle_ms, switches, sync_ticks;
    uint32_t syncs, frames;
    FileStats files;
    uint64_t run_ns = 0, trap_ns = 0, trap_calls = 0, waits = 0, waits_now = 0, cs = 0, cs_contended = 0, ticks = 0;
} g_mark{};

#ifdef PROF_COUNTERS
struct ThreadProf { uint32_t tid, entry; uint64_t samples, blocks, run_ns = 0, trap_ns = 0, slices = 0; };

void log_eips(const char *head, const std::vector<std::pair<uint32_t, uint64_t>> &v)
{
    char line[400];
    size_t off = std::snprintf(line, sizeof line, "%s", head);
    for (const auto &e : v) {
        const int w = std::snprintf(line + off, sizeof line - off, " %08x:%llu", (unsigned)e.first,
                                    (unsigned long long)e.second);
        if (w <= 0 || off + w >= sizeof line)
            break;
        off += w;
    }
    logf("%s", line);
}
#endif

void prof_report(uint64_t ms)
{
    const uint32_t syncs = &dyn86_ctr_cache_syncs ? dyn86_ctr_cache_syncs : 0;
    const uint64_t ticks = &dyn86_ctr_cache_sync_ticks ? dyn86_ctr_cache_sync_ticks : 0;
    ProfMark now{ms, g_sched ? g_sched->idle_ms_ : 0, g_sched ? g_sched->switches() : 0, ticks, syncs, g_stats.frames,
                 file_stats()};
    logf("prof: %llu ms, frames %u, idle %llu ms, switches %llu, cache syncs %u (%llu us)",
         (unsigned long long)(now.ms - g_mark.ms), now.frames - g_mark.frames,
         (unsigned long long)(now.idle_ms - g_mark.idle_ms), (unsigned long long)(now.switches - g_mark.switches),
         now.syncs - g_mark.syncs, (unsigned long long)((now.sync_ticks - g_mark.sync_ticks) / 268u));   // 268 MHz tick
    if (dyn86_intrin_n) {
        char line[256];
        if (dyn86_intrin_report(line, sizeof line) > 0)
            logf("prof %s", line);
    }
    const FileStats &f = now.files, &f0 = g_mark.files;
    logf("prof files: %llu reads (%llu async), %llu hits, %llu fills, %llu direct, %llu seeks; disk %llu reads %llu KiB in "
         "%llu ms; cache %llu KiB",
         (unsigned long long)(f.calls - f0.calls), (unsigned long long)(f.async - f0.async),
         (unsigned long long)(f.hits - f0.hits),
         (unsigned long long)(f.fills - f0.fills), (unsigned long long)(f.direct - f0.direct),
         (unsigned long long)(f.seeks - f0.seeks), (unsigned long long)(f.disk_reads - f0.disk_reads),
         (unsigned long long)((f.disk_bytes - f0.disk_bytes) >> 10), (unsigned long long)((f.disk_us - f0.disk_us) / 1000),
         (unsigned long long)(f.cache_bytes >> 10));
#ifdef PROF_COUNTERS
    now.run_ns = prof::run_ns;
    now.trap_ns = prof::trap_ns;
    now.trap_calls = prof::trap_calls;
    now.waits = prof::wait_calls;
    now.waits_now = prof::wait_immediate;
    now.cs = prof::cs_enter;
    now.cs_contended = prof::cs_contended;
    now.ticks = prof::ticks();
    logf("prof: run %llu ms (shims %llu ms in %llu calls), waits %llu (%llu immediate), cs %llu (%llu contended), "
         "samples %llu",
         (unsigned long long)((now.run_ns - g_mark.run_ns) / 1000000), (unsigned long long)((now.trap_ns - g_mark.trap_ns) / 1000000),
         (unsigned long long)(now.trap_calls - g_mark.trap_calls), (unsigned long long)(now.waits - g_mark.waits),
         (unsigned long long)(now.waits_now - g_mark.waits_now), (unsigned long long)(now.cs - g_mark.cs),
         (unsigned long long)(now.cs_contended - g_mark.cs_contended), (unsigned long long)prof::eip_samples());
    if (now.ticks != g_mark.ticks)   // time-based sampling: ticks not sampled landed outside guest slices
        logf("prof: %llu timer ticks, %llu in guest slices", (unsigned long long)(now.ticks - g_mark.ticks),
             (unsigned long long)prof::eip_samples());
    char shims[400];
    if (g_br && g_br->prof_window(shims, sizeof shims, 12))
        logf("prof shims:%s", shims);

    std::vector<ThreadProf> threads;
    prof::threads([](uint32_t tid, uint32_t entry, uint64_t samples, uint64_t blocks, void *u) {
        static_cast<std::vector<ThreadProf> *>(u)->push_back({tid, entry, samples, blocks});
    }, &threads);
    // Time per thread: unlike the samples, it also covers threads that always
    // yield before their quantum expires.
    prof::thread_times([](uint32_t tid, uint64_t run, uint64_t trap, uint64_t slices, void *u) {
        for (ThreadProf &t : *static_cast<std::vector<ThreadProf> *>(u))
            if (t.tid == tid) {
                t.run_ns = run;
                t.trap_ns = trap;
                t.slices = slices;
            }
    }, &threads);
    using Eips = std::vector<std::pair<uint32_t, uint64_t>>;
    const auto collect = [](uint32_t eip, uint64_t hits, void *u) { static_cast<Eips *>(u)->push_back({eip, hits}); };
    for (const ThreadProf &t : threads) {
        if (!t.samples && !t.blocks && !t.slices)
            continue;
        logf("prof thread %u (entry %08x): run %llu ms (shims %llu ms) in %llu slices, %llu samples, %llu blocks",
             t.tid, t.entry, (unsigned long long)(t.run_ns / 1000000), (unsigned long long)(t.trap_ns / 1000000),
             (unsigned long long)t.slices, (unsigned long long)t.samples, (unsigned long long)t.blocks);
        Eips v;
        // Time samples spread over many EIPs: the busy threads get a longer list.
        prof::top_eips_tid(t.tid, t.samples > 1000 ? 120 : 40, collect, &v);
        for (size_t i = 0; i < v.size(); i += 20) {
            char head[32];
            std::snprintf(head, sizeof head, "prof eip t%u", t.tid);
            log_eips(head, Eips(v.begin() + i, v.begin() + std::min(v.size(), i + 20)));
        }
    }
    Eips buckets;   // 64-byte buckets, all threads: hot loops spread over several EIPs
    prof::top_eips(40, collect, &buckets);
    for (size_t i = 0; i < buckets.size(); i += 20)
        log_eips("prof bucket", Eips(buckets.begin() + i, buckets.begin() + std::min(buckets.size(), i + 20)));
    prof::reset_samples();
#endif
    g_mark = now;
}

void check_time_limit(uint64_t ms)
{
    if (g_opt.service && t_runs_guest && !g_in_service) {
        const uint64_t real = wx86_now_ms();
        if (real - g_last_service_ms >= 50) {
            g_in_service = true;
            g_opt.service();
            g_in_service = false;
            g_last_service_ms = wx86_now_ms();
        }
    }
    if (g_opt.heartbeat_ms && ms >= g_next_beat) {
        g_next_beat = ms + g_opt.heartbeat_ms;
        prof_report(ms);
#ifdef __GLIBC__
        const size_t host_used = mallinfo2().uordblks;
#else
        const size_t host_used = mallinfo().uordblks;
#endif
        logf("t=%llus peeks=%u files=%u read=%lluKiB frames=%u heap=%u/%uKiB va=%u/%uKiB free=%uKiB jit=%u meta=%u jmptbl=%uKiB "
             "host=%uKiB",
             (unsigned long long)(ms / 1000), g_stats.peeks, g_stats.files_opened,
             (unsigned long long)(g_stats.bytes_read >> 10), g_stats.frames, g_heap.used_bytes() >> 10,
             g_heap.peak() >> 10, g_va.used_bytes() >> 10, g_va.peak() >> 10,
             (unsigned)((g_pool_size ? g_pool_size - g_pool.cur() : g_va.largest_free()) >> 10),
             (unsigned)d2rt_box86_dynarec_kb(),
             (unsigned)d2rt_box86_custommalloc_kb(), (unsigned)d2rt_box86_jmptbl_kb(), (unsigned)(host_used >> 10));
        if (g_spare_size && g_pool.spare_takes() != g_spare_logged) {
            g_spare_logged = g_pool.spare_takes();
            logf("pool spare: %u requests taken so far, %u of %u KiB in use, pool largest free %u KiB", g_spare_logged,
                 g_spare.cur() >> 10, g_spare_size >> 10, g_pool.largest_free() >> 10);
        }
        if (g_opt.heartbeat)
            g_opt.heartbeat();
    }
    if (g_opt.max_ms && ms >= g_opt.max_ms && !g_timed_out && g_sched) {
        g_timed_out = true;
        logf("time limit %u ms reached", g_opt.max_ms);
        g_sched->request_shutdown();
    }
}
uint64_t g_t0_ms = 0;   // real clock at run start: guest time starts at 0 in both modes
}  // namespace

void tail_call(Cpu &c, uint32_t va, uint32_t shim_argc, std::initializer_list<uint32_t> args)
{
    // At the trap ESP0 points at [ret][shim args]. The bridge will pop
    // 4 + 4*shim_argc bytes; the callee (stdcall) pops 4 + 4*args. Lay out
    // [ret][args] so that both end at ESP0 + 4 + 4*shim_argc.
    const uint32_t esp0 = c.reg(R_ESP);
    const uint32_t ret = c.read_u32(esp0);
    const uint32_t entry = esp0 + 4 * shim_argc - 4 * (uint32_t)args.size();
    c.write_u32(entry, ret);
    uint32_t p = entry + 4;
    for (uint32_t a : args) {
        c.write_u32(p, a);
        p += 4;
    }
    c.set_reg(R_ESP, entry - (4 + 4 * shim_argc));   // the bridge's epilogue adds that back
    g_br->redirect_next(va);
}

void profile_tick()
{
#ifdef PROF_COUNTERS
    if (Cpu *c = g_tick_cpu.load(std::memory_order_acquire)) {
        prof::tick();
        c->nudge_thread_budget(nullptr);   // the cooperative scheduler's one emu: no registry walk
    }
#endif
}

uint64_t now_ms()
{
    uint64_t ms = (g_opt.real_clock || !g_sched) ? wx86_now_ms() - g_t0_ms : g_sched->virt_ms();
    check_time_limit(ms);
    return ms;
}

uint64_t now_us()
{
    if (g_opt.real_clock || !g_sched)
        return wx86_now_us() - g_t0_ms * 1000ull;
    // Virtual time has millisecond steps; a per-call tick keeps QPC strictly
    // increasing between them, as on a real machine.
    static uint64_t last = 0;
    uint64_t v = g_sched->virt_ms() * 1000ull;
    last = v > last ? v : last + 1;
    return last;
}

void request_exit(uint32_t code)
{
    g_exited = true;
    g_exit_code = code;
    if (g_sched)
        g_sched->request_shutdown();
}

bool exit_requested() { return g_exited; }

void logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_opt.log)
        g_opt.log(buf);
}

std::string read_cstr(Cpu &c, uint32_t va, uint32_t max)
{
    std::string s;
    if (!va)
        return s;
    char buf[64];
    while (s.size() < max) {
        const uint32_t n = 64 - (va & 63);   // never cross a 64-byte line: stays inside one page
        c.read(va, buf, n);
        for (uint32_t i = 0; i < n; i++) {
            if (!buf[i] || s.size() >= max)
                return s;
            s += buf[i];
        }
        va += n;
    }
    return s;
}

std::string read_wstr(Cpu &c, uint32_t va, uint32_t max)
{
    std::string s;
    if (!va)
        return s;
    for (uint32_t i = 0; i < max; i++) {
        uint16_t w = 0;
        c.read(va + 2 * i, &w, 2);
        if (!w)
            break;
        if (w < 0x80) {
            s += (char)w;
        } else if (w < 0x800) {
            s += (char)(0xC0 | (w >> 6));
            s += (char)(0x80 | (w & 0x3F));
        } else {
            s += (char)(0xE0 | (w >> 12));
            s += (char)(0x80 | ((w >> 6) & 0x3F));
            s += (char)(0x80 | (w & 0x3F));
        }
    }
    return s;
}

uint32_t write_cstr(Cpu &c, uint32_t va, uint32_t cap, const std::string &s)
{
    const uint32_t n = (uint32_t)s.size();
    if (n + 1 > cap) {
        if (cap && va)
            c.write(va, "", 1);
        return n + 1;
    }
    c.write(va, s.c_str(), n + 1);
    return n;
}

RunResult run_exe(std::vector<uint8_t> exe, const RunOptions &opt)
{
    RunResult res;
    g_opt = opt;
    if (g_opt.write_root.empty())
        g_opt.write_root = g_opt.data_root;
    const Layout &L = *g_opt.layout;
    g_exited = g_timed_out = false;
    g_exit_code = 0;
    g_stats = Stats();
    g_next_beat = g_opt.heartbeat_ms;
    g_t0_ms = wx86_now_ms();
    g_last_service_ms = g_t0_ms;
    struct RunsGuest {
        RunsGuest() { t_runs_guest = true; }
        ~RunsGuest() { t_runs_guest = false; }
    } runs_guest;

    // Single-arena layout (rt_layout.h): the bridge reads its bases from the
    // environment at construction; the backend allocates the arena from WX86_ARENA.
    char hex[16];
    // WX86_ARENA counts 16 MiB of alignment slack on top of the usable span
    // (cpu_box86.cpp subtracts it); on the 3DS mman_ctr.c returns 16 MiB-aligned
    // blocks, so that slack is never actually allocated there.
    std::snprintf(hex, sizeof hex, "%x", L.span + 0x1000000u);
    setenv("WX86_ARENA", hex, 0);
    std::snprintf(hex, sizeof hex, "%x", L.main_stack);
    setenv("WX86_STACKBASE", hex, 1);
    std::snprintf(hex, sizeof hex, "%x", L.trap_base);
    setenv("WX86_TRAPBASE", hex, 1);
    std::snprintf(hex, sizeof hex, "%x", L.dll_base);
    setenv("WX86_MODBASE", hex, 1);
    logf("layout %s: span %08x heap %08x+%x va %08x+%x", L.name, L.span, L.heap_base, L.heap_size, L.va_base,
         L.va_size);

    if (g_opt.cpuid_edx)
        wx86_set_cpuid1_edx(g_opt.cpuid_edx);
    Cpu *cpu = make_cpu_box86();
    static std::unique_ptr<Bridge> br;
    br.reset(new Bridge(cpu));
    g_br = br.get();
    // DLLs load from dll_base up to the next region above it.
    uint32_t module_limit = L.heap_base;
    for (uint32_t b : {L.worker_stacks, L.va_base, L.main_stack, L.trap_base})
        if (b > L.dll_base && b < module_limit)
            module_limit = b;
    br->set_module_limit(module_limit);

    std::string err;
    PeImage *img = br->add_module(g_opt.exe_name, exe, err);
    if (!img) {
        res.error = "load: " + err;
        return res;
    }
    g_exe_base = img->load_base();
    std::vector<uint8_t>().swap(exe);   // the image holds its own copy now

    // Guest memory: heap, VirtualAlloc arena, scratch, PEB and main TEB.
    if (!cpu->map(L.heap_base, L.heap_size, nullptr, P_RW) || !cpu->map(L.va_base, L.va_size, nullptr, P_RW) ||
        !cpu->map(L.scratch_base, L.scratch_size, nullptr, P_RW) || !cpu->map(L.peb, 0x2000, nullptr, P_RW)) {
        res.error = "map guest regions";
        return res;
    }
    // Allocation failures: the first few of each region are logged with its
    // state, so an "Out of memory" from the game can be traced to its cause.
    wx86::region_set_fail_handler([](const wx86::RegionFail &f) {
        static unsigned n = 0;
        g_stats.alloc_failures++;
        if (++n <= 16)
            logf("%s full: want %u KiB, live %u KiB, peak %u KiB, largest free %u KiB", f.name, f.want >> 10,
                 f.cur >> 10, f.peak >> 10, f.largest >> 10);
        if (n <= 2)
            log_va_owners();
        if (n <= 2 && g_spare_size)
            logf("pool spare: %u requests taken, %u of %u KiB in use", g_pool.spare_takes(), g_spare.cur() >> 10,
                 g_spare_size >> 10);
    });
    g_va_owner.clear();
    g_va_pending = 0;
    wx86_mem_set_observer(va_observer);
    if (L.heap_base + L.heap_size == L.va_base) {
        g_pool_size = L.heap_size + L.va_size;
        g_pool.init(L.heap_base, g_pool_size, 16, "pool");
        g_heap.share(g_pool, 16, false, "heap");
        g_va.share(g_pool, 0x10000, true, "va");
        g_spare_size = g_spare_logged = 0;
        if (g_opt.pool_spare) {
            if (g_pool.set_spare(g_spare, g_opt.pool_spare, 0x10000))
                g_spare_size = g_spare.largest_free();   // all of it, still unused
            logf("pool spare: %u KiB%s", g_opt.pool_spare >> 10, g_spare_size ? "" : " does not fit, none");
        }
    } else {
        g_heap.init(L.heap_base, L.heap_size, 16, "heap");
        g_va.init(L.va_base, L.va_size, 0x10000, "va");
    }
    wx86_scratch_init(L.scratch_base, L.scratch_size);
    cpu->write_u32(L.peb + 0x08, g_exe_base);   // PEB.ImageBaseAddress
    init_teb(*cpu, L.main_tib, L.main_stack + 0x200000, L.main_stack);
    cpu->set_fs_base(L.main_tib);

    // WinVita's generic shims first, then 3ds-d2's (the last registration wins).
    win32_shims_kernel32_install(*br);
    Wx86MemoryPlan plan;
    plan.heap = &g_heap;
    plan.va = &g_va;
    plan.vquery = vquery;
    win32_shims_memory_install(*br, plan);
    if (g_opt.total_phys_mb)
        wx86_mem_set_total_phys_mb(g_opt.total_phys_mb);
    win32_shims_sync_install(*br);
    win32_shims_wait_install(*br);
    win32_shims_locale_install(*br);
    win32_shims_misc_install(*br);
    win32_shims_user32_install(*br);
    win32_shims_window_install(*br);
    win32_shims_gdi32_install(*br);
    wx86_set_screen_size(800, 600);   // the console's screen is the game's window
    win32_shims_advapi32_install(*br);
    win32_shims_shell32_install(*br);
    win32_shims_psapi_install(*br);
    install_kernel32(*br);
    install_files(*br);
    install_user32(*br);
    install_advapi32(*br);
    install_misc(*br);
    install_net(*br);   // includes DSOUND "no driver"; ds_emul replaces it when audio is on
    if (g_opt.audio) {
        dsound::set_logger([](const char *line) { logf("%s", line); });
        dsound::HostOps ops;
        ops.va_alloc = [](uint32_t n) -> uint32_t { return g_va.alloc(n); };
        ops.tick_ms = [](Cpu &) -> uint32_t { return (uint32_t)now_ms(); };
        ops.write_root = g_opt.write_root.c_str();
        ops.mix_rate = g_opt.audio_rate;
        dsound::install(*br, *cpu, ops);
    }
    // Unimplemented imports: logged once per name (a game loop would flood the
    // log otherwise), and return 0.
    static std::set<std::string> reported;
    reported.clear();
    br->set_default_shim([](Cpu &, const std::string &tag) -> uint32_t {
        if (reported.insert(tag).second)
            logf("unimplemented import called: %s", tag.c_str());
        return 0;
    });

    if (!br->commit(err) || !br->link(err)) {   // commit maps the images, link patches the IATs
        res.error = "link: " + err;
        return res;
    }
    img->release_image();   // the guest copy is mapped and linked: the host copy is dead weight
    if (g_opt.install_native)
        g_opt.install_native(*cpu);
    // The game's clock reads, served in line from translated code (WinVita's
    // lock-free in-line intrinsic): no trap, no exit from the dynarec loop.
    // On the console a GetTickCount trap cost more than its body: ~10% of a
    // town frame went to them. Same value as the shims (now_ms). Armed
    // before the game runs: a slot already translated can't be rewritten.
    static const char *const kClockImports[][2] = {{"KERNEL32.dll", "GetTickCount"}, {"WINMM.dll", "timeGetTime"}};
    for (const auto &imp : kClockImports) {
        const uint32_t slot = br->shim_trap_existing(imp[0], imp[1]);
        if (slot && cpu->set_intrinsic_nogil(slot, [](Cpu::IntrinFrame &f, uint32_t) {
                f.eax = (uint32_t)now_ms();
                f.pop = 0;
                return true;
            }))
            logf("%s: served in line", imp[1]);
    }
    // The hottest other imports go through the bridge without leaving the
    // dynarec loop (WinVita's in-line shim path): same shim, no trap. Under
    // the cooperative scheduler a shim that must wait yields, and the in-line
    // path turns a yield into a clean stop, as a trap would. Per call this
    // saved about half of what these cost at their call sites.
    static const char *const kInLineImports[][2] = {{"KERNEL32.dll", "EnterCriticalSection"},
                                                    {"KERNEL32.dll", "LeaveCriticalSection"},
                                                    {"KERNEL32.dll", "QueryPerformanceCounter"},
                                                    {"USER32.dll", "wsprintfA"},
                                                    {"GDI32.dll", "StretchBlt"}};
    for (const auto &imp : kInLineImports)
        if (const uint32_t slot = br->shim_trap_existing(imp[0], imp[1])) {
            cpu->set_inline_shim(slot);
            logf("%s: in line", imp[1]);
        }
    res.unresolved_imports = br->unresolved_count();
    res.loaded = true;
    logf("loaded %s at %08x entry %08x, %u unresolved import(s)", g_opt.exe_name.c_str(), g_exe_base,
         img->entry_va(), res.unresolved_imports);

    CooperativeScheduler sched(cpu, br.get(), L.worker_stacks, L.worker_stack, L.worker_tibs);
    // The worker TEBs grow up from worker_tibs: stop them at the next region
    // of the layout (the heap, on the 3DS) instead of letting the 17th
    // thread's TEB land in it. Issue #5.
    {
        uint32_t ceiling = L.span;
        for (const uint32_t b : {L.dll_base, L.heap_base, L.va_base, L.scratch_base, L.peb, L.main_tib,
                                 L.worker_stacks, L.main_stack, L.trap_base})
            if (b > L.worker_tibs && b < ceiling)
                ceiling = b;
        sched.set_tib_ceiling(ceiling);
    }
    g_sched = &sched;
    if (opt.real_clock && opt.sleep_ms)
        sched.set_real_clock([] { return wx86_now_ms() - g_t0_ms; }, opt.sleep_ms);
    if (!opt.real_clock && opt.virtual_tick_us)
        sched.set_virtual_tick_us(opt.virtual_tick_us);
    sched.set_max_switches(~0ull);   // runs end by exit or max_ms, never by a switch count
    sched.set_time_sink([](uint64_t ms) { check_time_limit(ms); });
    wx86_set_scheduler(&sched);
    sched.set_main(img->entry_va(), L.main_stack + 0x200000 - 0x100, L.main_tib);
    g_tick_cpu.store(cpu, std::memory_order_release);
    start_async_io();
    sched.run();
    stop_async_io();
    g_tick_cpu.store(nullptr, std::memory_order_release);
    wx86_set_scheduler(nullptr);
    g_sched = nullptr;
    if (g_opt.audio)
        dsound::shutdown();

    const std::string why = sched.stop_reason() ? sched.stop_reason() : "";
    if (!g_exited && !g_timed_out && why == "all finished") {   // the main thread returned from its entry point
        g_exited = true;
        g_exit_code = cpu->reg(R_EAX);
    }
    if (!g_exited && !g_timed_out)
        res.error = "stopped: " + why;   // a fault, a deadlock
    res.exited = g_exited;
    res.timed_out = g_timed_out;
    res.exit_code = g_exit_code;
    logf("scheduler stopped: %s, exit code %u", why.c_str(), res.exit_code);
    if (g_opt.trap_report)
        br->dump_trap_counts((int)g_opt.trap_report);
    return res;
}

}  // namespace rt
