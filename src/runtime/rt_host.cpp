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
}

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

using namespace d2rt;

namespace rt {
namespace {

RunOptions g_opt;
Bridge *g_br = nullptr;
CooperativeScheduler *g_sched = nullptr;
uint32_t g_exe_base = 0;
bool g_exited = false;
bool g_timed_out = false;
Stats g_stats;
uint64_t g_next_beat = 0;
uint32_t g_exit_code = 0;
wx86::GuestRegion g_heap, g_va;

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

MemoryReport memory_report()
{
    const Layout &L = layout();
    return {g_heap.used_bytes(), g_heap.peak(), L.heap_size, g_va.used_bytes(), g_va.peak(), L.va_size,
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
void check_time_limit(uint64_t ms)
{
    if (g_opt.heartbeat_ms && ms >= g_next_beat) {
        g_next_beat = ms + g_opt.heartbeat_ms;
        logf("t=%llus peeks=%u files=%u read=%lluKiB frames=%u heap=%u/%uKiB va=%u/%uKiB jit=%u meta=%u jmptbl=%uKiB",
             (unsigned long long)(ms / 1000), g_stats.peeks, g_stats.files_opened,
             (unsigned long long)(g_stats.bytes_read >> 10), g_stats.frames, g_heap.used_bytes() >> 10,
             g_heap.peak() >> 10, g_va.used_bytes() >> 10, g_va.peak() >> 10, (unsigned)d2rt_box86_dynarec_kb(),
             (unsigned)d2rt_box86_custommalloc_kb(), (unsigned)d2rt_box86_jmptbl_kb());
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
        if (++n <= 16)
            logf("%s full: want %u KiB, live %u KiB, peak %u KiB, largest free %u KiB", f.name, f.want >> 10,
                 f.cur >> 10, f.peak >> 10, f.largest >> 10);
    });
    g_heap.init(L.heap_base, L.heap_size, 16, "heap");
    g_va.init(L.va_base, L.va_size, 0x10000, "va");
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
    res.unresolved_imports = br->unresolved_count();
    res.loaded = true;
    logf("loaded %s at %08x entry %08x, %u unresolved import(s)", g_opt.exe_name.c_str(), g_exe_base,
         img->entry_va(), res.unresolved_imports);

    CooperativeScheduler sched(cpu, br.get(), L.worker_stacks, L.worker_stack, L.worker_tibs);
    g_sched = &sched;
    if (opt.real_clock && opt.sleep_ms)
        sched.set_real_clock([] { return wx86_now_ms() - g_t0_ms; }, opt.sleep_ms);
    if (!opt.real_clock && opt.virtual_tick_us)
        sched.set_virtual_tick_us(opt.virtual_tick_us);
    sched.set_max_switches(~0ull);   // runs end by exit or max_ms, never by a switch count
    sched.set_time_sink([](uint64_t ms) { check_time_limit(ms); });
    wx86_set_scheduler(&sched);
    sched.set_main(img->entry_va(), L.main_stack + 0x200000 - 0x100, L.main_tib);
    sched.run();
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
