// KERNEL32 functions WinVita leaves to the port: process, modules, threads,
// TLS, critical sections, time, resources, toolhelp, diagnostics. Files are in
// rt_shims_files.cpp. Registered after WinVita's shims, so these win on any
// shared key.
//
// TLS values live in each thread's TEB (offset 0xE10, 64 slots), exactly where
// Windows keeps them, so FS-relative guest code and the API agree.
#include "runtime/rt_host.h"
#include "runtime/rt_layout.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_region.h"
#include "runtime/guest_scratch.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/host_clock.h"
#include "runtime/pe_image.h"
#include "runtime/win32_shims_wait.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace d2rt;

namespace rt {
namespace {

constexpr uint32_t kCurrentProcess = 0xFFFFFFFFu;
constexpr uint32_t kCurrentThread = 0xFFFFFFFEu;
constexpr uint32_t kInfinite = 0xFFFFFFFFu;
constexpr uint32_t kProcessId = 1;

enum : uint32_t {
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_INVALID_HANDLE = 6,
    ERROR_NOT_ENOUGH_MEMORY = 8,
    ERROR_NO_MORE_FILES = 18,
    ERROR_INVALID_PARAMETER = 87,
    ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_MOD_NOT_FOUND = 126,
    ERROR_PROC_NOT_FOUND = 127,
    ERROR_RESOURCE_TYPE_NOT_FOUND = 1813,
    ERROR_RESOURCE_NAME_NOT_FOUND = 1814,
};

uint32_t g_tls_used = 0;   // bitmap of 32 allocated slots (64 exist; the first 32 are handed out)

std::string lower(std::string s)
{
    for (char &ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// ---- modules ----------------------------------------------------------------------
//
// Three kinds: the executable; PE DLLs loaded from the game directory at run
// time (glide3x.dll, a Battle.net CheckRevision DLL); and system DLLs, which
// exist only as shim tables. A system DLL gets a stable pseudo-handle, and
// GetProcAddress on it returns the shim's trap. Game.exe 1.14d is
// monolithic, so the 1.13 split DLLs left in some installs are refused: they
// would load and hook themselves into the running game.

struct Module {
    std::string name;   // lower-case file name ("glide3x.dll")
    uint32_t base;
    PeImage *image;     // null for system DLLs
};
std::vector<Module> g_modules;
uint32_t g_next_pseudo = 0x7E000001u;

const char *const kSystemDlls[] = {
    "kernel32.dll", "user32.dll",   "gdi32.dll", "advapi32.dll", "wsock32.dll",  "ws2_32.dll", "winmm.dll",
    "dsound.dll",   "ddraw.dll",    "imm32.dll", "version.dll",  "shell32.dll",  "ole32.dll",  "psapi.dll",
    "crypt32.dll",  "wintrust.dll", "ntdll.dll", "comctl32.dll", "msvcrt.dll",   "shlwapi.dll",
    "d2.lng",   // the game's language module: only its handle is needed to start a game
};
const char *const kRefusedDlls[] = {
    "storm.dll",    "fog.dll",     "d2win.dll",   "d2client.dll", "d2common.dll", "d2gfx.dll",       "d2game.dll",
    "d2lang.dll",   "d2sound.dll", "d2net.dll",   "d2mcpclient.dll", "d2cmp.dll", "d2launch.dll",    "d2multi.dll",
    "bnclient.dll", "d2gdi.dll",   "d2ddraw.dll", "d2direct3d.dll",  "d2glide.dll",
};
// Optional debug-helper DLLs the game probes for at startup; their
// absence is expected, so a miss is not logged.
const char *const kQuietProbeDlls[] = {
    "dbghelp.dll",
    "imagehlp.dll",
};

bool is_quiet_probe(const std::string &key)
{
    for (const char *n : kQuietProbeDlls)
        if (key == n)
            return true;
    return false;
}

// "C:\Dir\Foo" -> "foo.dll"
std::string module_key(const std::string &path)
{
    std::string n = path;
    const size_t k = n.find_last_of("\\/");
    if (k != std::string::npos)
        n = n.substr(k + 1);
    n = lower(n);
    if (n.find('.') == std::string::npos)
        n += ".dll";
    return n;
}

Module *module_by_name(const std::string &key)
{
    for (Module &m : g_modules)
        if (m.name == key)
            return &m;
    return nullptr;
}

Module *module_by_base(uint32_t base)
{
    for (Module &m : g_modules)
        if (m.base == base)
            return &m;
    return nullptr;
}

bool is_system(const std::string &key)
{
    for (const char *n : kSystemDlls)
        if (key == n)
            return true;
    return false;
}

uint32_t system_handle(const std::string &key)
{
    if (Module *m = module_by_name(key))
        return m->base;
    g_modules.push_back({key, g_next_pseudo++, nullptr});
    return g_modules.back().base;
}

uint32_t get_module_handle(Cpu &c, const std::string &name)
{
    const std::string key = module_key(name);
    if (key == lower(options().exe_name))
        return exe_base();
    if (Module *m = module_by_name(key))
        return m->base;
    if (is_system(key))
        return system_handle(key);
    set_last_error(c, ERROR_MOD_NOT_FOUND);
    return 0;
}

uint32_t load_library(Cpu &c, const std::string &name)
{
    const std::string key = module_key(name);
    if (key == lower(options().exe_name))
        return exe_base();
    if (Module *m = module_by_name(key))
        return m->base;
    if (is_system(key))
        return system_handle(key);
    for (const char *n : kRefusedDlls)
        if (key == n) {
            set_last_error(c, ERROR_MOD_NOT_FOUND);
            return 0;
        }
    // A PE DLL in the game directory: map it, bind its imports, run DllMain.
    std::vector<uint8_t> bytes;
    if (!read_guest_file(name, bytes) && !read_guest_file(key, bytes)) {
        if (!is_quiet_probe(key))
            logf("LoadLibrary %s: not found", name.c_str());
        set_last_error(c, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    std::string err;
    PeImage *img = bridge().load_library_runtime(key, bytes, err);
    if (!img) {
        logf("LoadLibrary %s: %s", name.c_str(), err.c_str());
        set_last_error(c, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    g_modules.push_back({key, img->load_base(), img});
    logf("LoadLibrary %s at %08x", key.c_str(), img->load_base());
    if (img->entry_rva())
        guest_call(c, img->entry_va(), {img->load_base(), 1 /*DLL_PROCESS_ATTACH*/, 0});
    return img->load_base();
}

uint32_t get_proc_address(Cpu &c, uint32_t module, uint32_t name_va)
{
    const bool by_ordinal = name_va < 0x10000;
    const std::string name = by_ordinal ? std::string() : read_cstr(c, name_va);
    if (module == exe_base() || module == 0) {
        set_last_error(c, ERROR_PROC_NOT_FOUND);
        return 0;
    }
    Module *m = module_by_base(module);
    if (!m) {
        set_last_error(c, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    uint32_t va = 0;
    if (m->image)
        va = by_ordinal ? m->image->export_va_ordinal(name_va) : m->image->export_va(name);
    else if (!by_ordinal)
        va = bridge().shim_trap(m->name, name);
    if (!va) {
        logf("GetProcAddress %s!%s: none", m->name.c_str(), by_ordinal ? "#ordinal" : name.c_str());
        set_last_error(c, ERROR_PROC_NOT_FOUND);
    }
    return va;
}

// ---- resources (a module's .rsrc, walked in guest memory: no copies) ---------------

bool res_root(Cpu &c, uint32_t base, uint32_t &root)
{
    const uint32_t pe = base + c.read_u32(base + 0x3C);
    if (c.read_u32(pe) != 0x00004550u)   // "PE\0\0"
        return false;
    const uint32_t rva = c.read_u32(pe + 24 + 96 + 2 * 8);   // optional header, data directory 2
    root = rva ? base + rva : 0;
    return root != 0;
}

std::string res_name(Cpu &c, uint32_t root, uint32_t off)
{
    uint16_t n = 0;
    c.read(root + off, &n, 2);
    std::string s;
    for (uint16_t i = 0; i < n; i++) {
        uint16_t w = 0;
        c.read(root + off + 2 + 2 * i, &w, 2);
        s += (char)std::toupper(w & 0xFF);
    }
    return s;
}

// Entry of directory `dir` matching an integer id (name empty) or an
// upper-case name; `any` takes the first. Returns its OffsetToData, or 0.
uint32_t res_find(Cpu &c, uint32_t root, uint32_t dir, uint32_t id, const std::string &name, bool any)
{
    uint16_t counts[2];
    c.read(dir + 12, counts, 4);
    const uint32_t total = counts[0] + counts[1];
    for (uint32_t i = 0; i < total; i++) {
        const uint32_t e = dir + 16 + 8 * i;
        const uint32_t ename = c.read_u32(e), edata = c.read_u32(e + 4);
        if (any)
            return edata;
        if (ename & 0x80000000u) {
            if (!name.empty() && res_name(c, root, ename & 0x7FFFFFFFu) == name)
                return edata;
        } else if (name.empty() && ename == id) {
            return edata;
        }
    }
    return 0;
}

// MAKEINTRESOURCE id, "#123", or a name.
void res_key(Cpu &c, uint32_t va, uint32_t &id, std::string &name)
{
    id = 0;
    name.clear();
    if (va < 0x10000) {
        id = va;
        return;
    }
    name = read_cstr(c, va, 256);
    if (!name.empty() && name[0] == '#') {
        id = (uint32_t)std::strtoul(name.c_str() + 1, nullptr, 10);
        name.clear();
        return;
    }
    for (char &ch : name)
        ch = (char)std::toupper((unsigned char)ch);
}

uint32_t resource_module_base(uint32_t module) { return module ? module : exe_base(); }

uint32_t find_resource(Cpu &c, uint32_t module, uint32_t name_va, uint32_t type_va)
{
    const uint32_t base = resource_module_base(module);
    uint32_t root = 0;
    if (base != exe_base() && !(module_by_base(base) && module_by_base(base)->image)) {
        set_last_error(c, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    uint32_t id, e = 0;
    std::string key;
    if (res_root(c, base, root)) {
        res_key(c, type_va, id, key);
        e = res_find(c, root, root, id, key, false);
    }
    if (!(e & 0x80000000u)) {
        set_last_error(c, ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }
    res_key(c, name_va, id, key);
    e = res_find(c, root, root + (e & 0x7FFFFFFFu), id, key, false);
    if (e & 0x80000000u)
        e = res_find(c, root, root + (e & 0x7FFFFFFFu), 0, std::string(), true);   // first language
    else
        e = 0;
    if (!e || (e & 0x80000000u)) {
        set_last_error(c, ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }
    return root + e;   // IMAGE_RESOURCE_DATA_ENTRY {OffsetToData (RVA), Size, CodePage, Reserved}
}

// ---- time --------------------------------------------------------------------------
//
// Wall-clock time is the guest clock plus a start epoch: the real time at
// start in real-clock mode, a fixed date under the virtual clock (so lockstep
// runs see identical dates). UTC is local time.

uint64_t g_epoch_s = 0;

uint64_t wall_filetime() { return (g_epoch_s + 11644473600ull) * 10000000ull + now_ms() * 10000ull; }

struct SysTime {
    uint16_t year, month, dow, day, hour, minute, second, ms;
};

// Days since 1970-01-01 <-> civil date (proleptic Gregorian).
void civil_from_days(int64_t z, int &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = (int)(yoe + era * 400 + (m <= 2));
}

int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

SysTime systime_of(uint64_t ft)
{
    const int64_t unix_ms = (int64_t)(ft / 10000ull) - 11644473600000ll;
    int64_t days = unix_ms / 86400000ll, rem = unix_ms % 86400000ll;
    if (rem < 0)
        rem += 86400000ll, days--;
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    SysTime t;
    t.year = (uint16_t)y;
    t.month = (uint16_t)m;
    t.day = (uint16_t)d;
    t.dow = (uint16_t)(((days % 7) + 11) % 7);   // 1970-01-01 was a Thursday (4)
    t.hour = (uint16_t)(rem / 3600000);
    t.minute = (uint16_t)(rem / 60000 % 60);
    t.second = (uint16_t)(rem / 1000 % 60);
    t.ms = (uint16_t)(rem % 1000);
    return t;
}

uint64_t filetime_of(const SysTime &t)
{
    const int64_t days = days_from_civil(t.year, t.month, t.day);
    const int64_t ms = days * 86400000ll + t.hour * 3600000ll + t.minute * 60000ll + t.second * 1000ll + t.ms;
    return (uint64_t)(ms + 11644473600000ll) * 10000ull;
}

uint64_t read_ft(Cpu &c, uint32_t va) { return c.read_u32(va) | ((uint64_t)c.read_u32(va + 4) << 32); }

void write_ft(Cpu &c, uint32_t va, uint64_t ft)
{
    c.write_u32(va, (uint32_t)ft);
    c.write_u32(va + 4, (uint32_t)(ft >> 32));
}

// ---- toolhelp (one process, its live threads, its modules) --------------------------

struct Snapshot : Waitable {
    std::vector<uint32_t> threads;
    std::vector<std::pair<std::string, uint32_t>> modules;   // name, base
    size_t pcur = 0, tcur = 0, mcur = 0;
    bool try_acquire(GuestThread *) override { return false; }
    bool ready(GuestThread *) override { return false; }
    const char *kind() const override { return "snapshot"; }
};

Snapshot *snapshot_of(uint32_t h)
{
    Waitable *w = wx86_handle_find(h);
    return (w && wx86_is_kind(w, "snapshot")) ? static_cast<Snapshot *>(w) : nullptr;
}

void put_wide(Cpu &c, uint32_t va, const std::string &s, uint32_t cap)
{
    for (uint32_t i = 0; i < cap; i++) {
        const uint16_t w = i < s.size() ? (uint8_t)s[i] : 0;
        c.write(va + 2 * i, &w, 2);
        if (!w)
            break;
    }
}

void zero_guest(Cpu &c, uint32_t va, uint32_t n)
{
    static const uint8_t zero[1088] = {};
    c.write(va, zero, n);
}

uint32_t process_entry(Cpu &c, uint32_t h, uint32_t pe, bool wide, bool first)
{
    Snapshot *s = snapshot_of(h);
    if (!s)
        return set_last_error(c, ERROR_INVALID_HANDLE), 0u;
    if (first)
        s->pcur = 0;
    if (s->pcur++ > 0)
        return set_last_error(c, ERROR_NO_MORE_FILES), 0u;
    const uint32_t size = wide ? 556 : 296;
    zero_guest(c, pe, size);
    c.write_u32(pe, size);
    c.write_u32(pe + 8, kProcessId);
    c.write_u32(pe + 20, (uint32_t)s->threads.size());
    c.write_u32(pe + 28, 8);   // NORMAL_PRIORITY_CLASS base priority
    if (wide)
        put_wide(c, pe + 36, options().exe_name, 260);
    else
        write_cstr(c, pe + 36, 260, options().exe_name);
    return 1;
}

uint32_t thread_entry(Cpu &c, uint32_t h, uint32_t te, bool first)
{
    Snapshot *s = snapshot_of(h);
    if (!s)
        return set_last_error(c, ERROR_INVALID_HANDLE), 0u;
    if (first)
        s->tcur = 0;
    if (s->tcur >= s->threads.size())
        return set_last_error(c, ERROR_NO_MORE_FILES), 0u;
    const uint32_t v[7] = {28, 0, s->threads[s->tcur++], kProcessId, 8, 0, 0};
    c.write(te, v, sizeof v);
    return 1;
}

uint32_t module_entry(Cpu &c, uint32_t h, uint32_t me, bool first)
{
    Snapshot *s = snapshot_of(h);
    if (!s)
        return set_last_error(c, ERROR_INVALID_HANDLE), 0u;
    if (first)
        s->mcur = 0;
    if (s->mcur >= s->modules.size())
        return set_last_error(c, ERROR_NO_MORE_FILES), 0u;
    const auto &m = s->modules[s->mcur++];
    zero_guest(c, me, 1064);
    const uint32_t v[8] = {1064, 1, kProcessId, 1, 1, m.second, 0, m.second};
    c.write(me, v, sizeof v);
    for (const auto &lm : bridge().loaded_modules())
        if (lm.first == m.second)
            c.write_u32(me + 24, lm.second);
    put_wide(c, me + 32, m.first, 256);
    put_wide(c, me + 544, "C:\\" + m.first, 260);
    return 1;
}

uint32_t g_unhandled_filter = 0;

}  // namespace

std::string module_name_of(uint32_t base)
{
    if (base == exe_base())
        return options().exe_name;
    Module *m = module_by_base(base);
    return m && m->image ? m->name : std::string();
}

void sleep_guest(uint32_t ms) { wx86_wait_sleep(ms); }

void install_kernel32(Bridge &br)
{
    auto K = [&](const char *name, uint32_t argc, std::function<uint32_t(Cpu &)> fn) {
        Shim s;
        s.argc = argc;
        s.stdcall_cleanup = true;
        s.tag = std::string("KERNEL32.dll!") + name;
        s.fn = std::move(fn);
        br.register_shim("KERNEL32.dll", name, s);
    };
    g_modules.clear();
    g_next_pseudo = 0x7E000001u;
    g_tls_used = 0;
    g_unhandled_filter = 0;
    g_epoch_s = options().real_clock ? (uint64_t)std::time(nullptr) : 1767225600ull;   // 2026-01-01 00:00 UTC

    // ---- process ----
    K("ExitProcess", 1, [](Cpu &c) {
        request_exit(c.arg(0));
        return 0u;
    });
    K("TerminateProcess", 2, [](Cpu &c) -> uint32_t {
        if (c.arg(0) != kCurrentProcess && c.arg(0) != kProcessId)
            return set_last_error(c, ERROR_INVALID_HANDLE), 0u;
        request_exit(c.arg(1));
        return 1;
    });
    K("FatalAppExitA", 2, [](Cpu &c) {
        logf("FatalAppExit: %s", read_cstr(c, c.arg(1)).c_str());
        request_exit(0xC0000409u);
        return 0u;
    });
    K("GetCurrentProcess", 0, [](Cpu &) { return kCurrentProcess; });
    K("GetCurrentProcessId", 0, [](Cpu &) { return kProcessId; });
    K("GetProcessId", 1, [](Cpu &c) { return c.arg(0) == kCurrentProcess ? kProcessId : 0u; });
    K("GetExitCodeProcess", 2, [](Cpu &c) {
        if (c.arg(1))
            c.write_u32(c.arg(1), 259);   // STILL_ACTIVE
        return 1u;
    });
    K("CreateProcessA", 10, [](Cpu &c) {
        logf("CreateProcessA %s %s: refused", read_cstr(c, c.arg(0)).c_str(), read_cstr(c, c.arg(1)).c_str());
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0u;
    });
    K("ReadProcessMemory", 5, [](Cpu &c) -> uint32_t {
        const uint32_t from = c.arg(1), to = c.arg(2), n = c.arg(3), pread = c.arg(4);
        if (c.arg(0) != kCurrentProcess && c.arg(0) != kProcessId)
            return set_last_error(c, ERROR_INVALID_HANDLE), 0u;
        void *src = n ? c.hostptr(from, n) : nullptr, *dst = n ? c.hostptr(to, n) : nullptr;
        if (n && (!src || !dst))
            return set_last_error(c, 299 /*ERROR_PARTIAL_COPY*/), 0u;
        if (n)
            std::memmove(dst, src, n);
        if (pread)
            c.write_u32(pread, n);
        return 1;
    });
    K("GetVersion", 0, [](Cpu &) { return 0x0A280105u; });   // Windows XP: 5.1, build 2600
    K("GetCommandLineA", 0, [](Cpu &c) {
        static uint32_t s = 0;
        if (!s)
            s = wx86_scratch_put_cstr(c, options().command_line.c_str());
        return s;
    });
    K("GetComputerNameA", 2, [](Cpu &c) -> uint32_t {
        static const char kName[] = "NINTENDO3DS";
        const uint32_t cap = c.arg(1) ? c.read_u32(c.arg(1)) : 0;
        if (cap < sizeof kName) {
            if (c.arg(1))
                c.write_u32(c.arg(1), sizeof kName);
            return set_last_error(c, 111 /*ERROR_BUFFER_OVERFLOW*/), 0u;
        }
        c.write(c.arg(0), kName, sizeof kName);
        c.write_u32(c.arg(1), sizeof kName - 1);
        return 1;
    });

    // ---- modules ----
    K("GetModuleHandleA", 1, [](Cpu &c) { return c.arg(0) ? get_module_handle(c, read_cstr(c, c.arg(0))) : exe_base(); });
    K("GetModuleHandleW", 1, [](Cpu &c) { return c.arg(0) ? get_module_handle(c, read_wstr(c, c.arg(0))) : exe_base(); });
    K("LoadLibraryA", 1, [](Cpu &c) { return load_library(c, read_cstr(c, c.arg(0))); });
    K("LoadLibraryW", 1, [](Cpu &c) { return load_library(c, read_wstr(c, c.arg(0))); });
    K("LoadLibraryExA", 3, [](Cpu &c) { return load_library(c, read_cstr(c, c.arg(0))); });
    K("FreeLibrary", 1, [](Cpu &) { return 1u; });   // modules stay mapped until exit
    K("GetProcAddress", 2, [](Cpu &c) { return get_proc_address(c, c.arg(0), c.arg(1)); });

    // ---- resources ----
    K("FindResourceA", 3, [](Cpu &c) { return find_resource(c, c.arg(0), c.arg(1), c.arg(2)); });
    K("LoadResource", 2, [](Cpu &c) -> uint32_t {
        return c.arg(1) ? resource_module_base(c.arg(0)) + c.read_u32(c.arg(1)) : 0u;
    });
    K("LockResource", 1, [](Cpu &c) { return c.arg(0); });
    K("SizeofResource", 2, [](Cpu &c) { return c.arg(1) ? c.read_u32(c.arg(1) + 4) : 0u; });
    K("FreeResource", 1, [](Cpu &) { return 0u; });

    // ---- threads ----
    K("CreateThread", 6, [](Cpu &c) -> uint32_t {
        ThreadScheduler *s = wx86_sched();
        if (!s)
            return 0;
        const uint32_t start = c.arg(2), param = c.arg(3), flags = c.arg(4), pid = c.arg(5);
        GuestThread *t = s->create_thread(start, param, c.arg(1), (flags & 4u) != 0);   // CREATE_SUSPENDED
        if (!t)
            return 0;
        auto *w = new WxThread();
        w->gt = t;
        const uint32_t h = wx86_handle_add(w);
        if (pid)
            c.write_u32(pid, t->id);
        return h;
    });
    K("OpenThread", 3, [](Cpu &c) -> uint32_t {
        ThreadScheduler *s = wx86_sched();
        GuestThread *t = s ? s->thread_by_id(c.arg(2)) : nullptr;
        if (!t)
            return set_last_error(c, ERROR_INVALID_PARAMETER), 0u;
        auto *w = new WxThread();
        w->gt = t;
        return wx86_handle_add(w);
    });
    K("ExitThread", 1, [](Cpu &c) {
        if (ThreadScheduler *s = wx86_sched())
            s->exit_current(c.arg(0));
        return 0u;
    });
    K("GetCurrentThread", 0, [](Cpu &) { return kCurrentThread; });
    K("GetCurrentThreadId", 0, [](Cpu &) -> uint32_t {
        ThreadScheduler *s = wx86_sched();
        return s && s->current() ? s->current()->id : 1u;
    });
    K("ResumeThread", 1, [](Cpu &c) -> uint32_t {
        Waitable *w = wx86_handle_find(c.arg(0));
        if (w && wx86_is_kind(w, "thread"))
            if (ThreadScheduler *s = wx86_sched()) {
                s->resume(static_cast<WxThread *>(w)->gt);
                return 1;
            }
        return 0xFFFFFFFFu;
    });
    K("GetThreadContext", 2, [](Cpu &c) {   // other threads' registers are not exposed
        set_last_error(c, ERROR_INVALID_HANDLE);
        return 0u;
    });
    K("RtlCaptureContext", 1, [](Cpu &c) {
        const uint32_t ctx = c.arg(0);
        // CONTEXT_i386 | CONTROL | INTEGER: the caller's registers, its return address as Eip.
        c.write_u32(ctx, 0x00010003u);
        const int regs[] = {R_EDI, R_ESI, R_EBX, R_EDX, R_ECX, R_EAX, R_EBP};
        for (int i = 0; i < 7; i++)
            c.write_u32(ctx + 0x9C + 4 * i, c.reg(regs[i]));
        c.write_u32(ctx + 0xB8, c.read_u32(c.reg(R_ESP)));
        c.write_u32(ctx + 0xBC, 0x1B);
        c.write_u32(ctx + 0xC0, 0x202);
        c.write_u32(ctx + 0xC4, c.reg(R_ESP) + 8);
        c.write_u32(ctx + 0xC8, 0x23);
        return 0u;
    });

    // ---- TLS ----
    K("TlsAlloc", 0, [](Cpu &) -> uint32_t {
        for (uint32_t i = 0; i < 32; i++)
            if (!(g_tls_used & (1u << i))) {
                g_tls_used |= 1u << i;
                return i;
            }
        return 0xFFFFFFFFu;   // TLS_OUT_OF_INDEXES
    });
    K("TlsFree", 1, [](Cpu &c) -> uint32_t {
        const uint32_t i = c.arg(0);
        if (i >= 32)
            return 0;
        g_tls_used &= ~(1u << i);
        return 1;
    });
    K("TlsGetValue", 1, [](Cpu &c) -> uint32_t {
        const uint32_t i = c.arg(0);
        if (i >= kTlsSlotCount)
            return 0;
        set_last_error(c, 0);
        return c.read_u32(current_teb() + kTebTlsSlots + 4 * i);
    });
    K("TlsSetValue", 2, [](Cpu &c) -> uint32_t {
        const uint32_t i = c.arg(0);
        if (i >= kTlsSlotCount)
            return 0;
        c.write_u32(current_teb() + kTebTlsSlots + 4 * i, c.arg(1));
        return 1;
    });

    // ---- critical sections: state in WinVita's engine table, the guest
    // struct stays zero (see guest_sync.h) ----
    auto init_cs = [](Cpu &c) {
        zero_guest(c, c.arg(0), 24);
        c.write_u32(c.arg(0) + 4, 0xFFFFFFFFu);   // LockCount -1: free
        wx86_crit_forget(c.arg(0));
        wx86_crit_for(c.arg(0));
        return 1u;
    };
    K("InitializeCriticalSection", 1, init_cs);
    K("InitializeCriticalSectionAndSpinCount", 2, init_cs);
    K("DeleteCriticalSection", 1, [](Cpu &c) {
        wx86_crit_forget(c.arg(0));
        return 0u;
    });
    K("EnterCriticalSection", 1, [](Cpu &c) {
        WxCrit *k = wx86_crit_for(c.arg(0));
        ThreadScheduler *s = wx86_sched();
        if (!k->try_acquire(s ? s->current() : nullptr) && s)
            s->wait_noresult(k, kInfinite);   // blocks; the scheduler acquires it on wake
        return 0u;
    });
    K("TryEnterCriticalSection", 1, [](Cpu &c) -> uint32_t {
        ThreadScheduler *s = wx86_sched();
        return wx86_crit_for(c.arg(0))->try_acquire(s ? s->current() : nullptr) ? 1u : 0u;
    });
    K("LeaveCriticalSection", 1, [](Cpu &c) {
        wx86_crit_leave(c.arg(0), wx86_sched());
        return 0u;
    });
    K("InterlockedCompareExchange", 3, [](Cpu &c) -> uint32_t {
        // Guest threads switch only at traps or slice ends, so a shim is atomic.
        const uint32_t p = c.arg(0), old = c.read_u32(p);
        if (old == c.arg(2))
            c.write_u32(p, c.arg(1));
        return old;
    });

    // ---- time ----
    K("GetTickCount", 0, [](Cpu &) { return (uint32_t)now_ms(); });
    K("Sleep", 1, [](Cpu &c) {
        sleep_guest(c.arg(0));
        return 0u;
    });
    K("QueryPerformanceFrequency", 1, [](Cpu &c) {
        c.write_u32(c.arg(0), 1000000u);
        c.write_u32(c.arg(0) + 4, 0);
        return 1u;
    });
    K("QueryPerformanceCounter", 1, [](Cpu &c) {
        const uint64_t us = now_us();
        c.write_u32(c.arg(0), (uint32_t)us);
        c.write_u32(c.arg(0) + 4, (uint32_t)(us >> 32));
        return 1u;
    });
    auto put_systime = [](Cpu &c) {
        const SysTime t = systime_of(wall_filetime());
        c.write(c.arg(0), &t, sizeof t);
        return 0u;
    };
    K("GetLocalTime", 1, put_systime);
    K("GetSystemTime", 1, put_systime);
    K("GetSystemTimeAsFileTime", 1, [](Cpu &c) {
        write_ft(c, c.arg(0), wall_filetime());
        return 0u;
    });
    K("SystemTimeToFileTime", 2, [](Cpu &c) -> uint32_t {
        SysTime t;
        c.read(c.arg(0), &t, sizeof t);
        if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59)
            return set_last_error(c, ERROR_INVALID_PARAMETER), 0u;
        write_ft(c, c.arg(1), filetime_of(t));
        return 1;
    });
    K("FileTimeToSystemTime", 2, [](Cpu &c) {
        const SysTime t = systime_of(read_ft(c, c.arg(0)));
        c.write(c.arg(1), &t, sizeof t);
        return 1u;
    });
    K("FileTimeToLocalFileTime", 2, [](Cpu &c) {
        write_ft(c, c.arg(1), read_ft(c, c.arg(0)));
        return 1u;
    });
    K("CompareFileTime", 2, [](Cpu &c) -> uint32_t {
        const uint64_t a = read_ft(c, c.arg(0)), b = read_ft(c, c.arg(1));
        return a < b ? 0xFFFFFFFFu : a > b ? 1u : 0u;
    });
    K("GetTimeZoneInformation", 1, [](Cpu &c) {
        zero_guest(c, c.arg(0), 172);
        return 0u;   // TIME_ZONE_ID_UNKNOWN: UTC, no daylight saving
    });

    // ---- toolhelp ----
    K("CreateToolhelp32Snapshot", 2, [](Cpu &) -> uint32_t {
        auto *s = new Snapshot();
        if (ThreadScheduler *sch = wx86_sched())
            s->threads = sch->live_thread_ids();
        s->modules.push_back({options().exe_name, exe_base()});
        for (const Module &m : g_modules)
            if (m.image)
                s->modules.push_back({m.name, m.base});
        return wx86_handle_add(s);
    });
    K("Process32First", 2, [](Cpu &c) { return process_entry(c, c.arg(0), c.arg(1), false, true); });
    K("Process32Next", 2, [](Cpu &c) { return process_entry(c, c.arg(0), c.arg(1), false, false); });
    K("Process32FirstW", 2, [](Cpu &c) { return process_entry(c, c.arg(0), c.arg(1), true, true); });
    K("Process32NextW", 2, [](Cpu &c) { return process_entry(c, c.arg(0), c.arg(1), true, false); });
    K("Thread32First", 2, [](Cpu &c) { return thread_entry(c, c.arg(0), c.arg(1), true); });
    K("Thread32Next", 2, [](Cpu &c) { return thread_entry(c, c.arg(0), c.arg(1), false); });
    K("Module32FirstW", 2, [](Cpu &c) { return module_entry(c, c.arg(0), c.arg(1), true); });
    K("Module32NextW", 2, [](Cpu &c) { return module_entry(c, c.arg(0), c.arg(1), false); });

    // ---- memory odds and ends ----
    K("GlobalLock", 1, [](Cpu &c) { return c.arg(0); });   // GlobalAlloc hands out fixed pointers
    K("GlobalUnlock", 1, [](Cpu &c) {
        set_last_error(c, 0);
        return 0u;
    });
    K("IsBadStringPtrA", 2, [](Cpu &c) { return c.arg(0) ? 0u : 1u; });

    // ---- diagnostics and exceptions ----
    K("OutputDebugStringA", 1, [](Cpu &c) {
        std::string s = read_cstr(c, c.arg(0), 1024);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        logf("debug: %s", s.c_str());
        return 0u;
    });
    K("SetUnhandledExceptionFilter", 1, [](Cpu &c) {
        const uint32_t prev = g_unhandled_filter;
        g_unhandled_filter = c.arg(0);
        return prev;
    });
    K("RaiseException", 4, [](Cpu &c) {
        const uint32_t code = c.arg(0);
        if (code == 0x406D1388u)   // MSVC "set thread name": a debugger notification
            return 0u;
        logf("RaiseException %08x flags %x: stopping", code, c.arg(1));
        request_exit(code);
        return 0u;
    });
    K("FormatMessageA", 7, [](Cpu &c) -> uint32_t {
        const uint32_t flags = c.arg(0), id = c.arg(2), buf = c.arg(4), size = c.arg(5);
        char text[64];
        std::snprintf(text, sizeof text, "Error %u.\r\n", id);
        const uint32_t n = (uint32_t)std::strlen(text);
        if (flags & 0x100) {   // FORMAT_MESSAGE_ALLOCATE_BUFFER
            const uint32_t p = va_region().alloc(n + 1);
            if (!p)
                return set_last_error(c, ERROR_NOT_ENOUGH_MEMORY), 0u;
            c.write(p, text, n + 1);
            c.write_u32(buf, p);
            return n;
        }
        if (size < n + 1)
            return set_last_error(c, ERROR_INSUFFICIENT_BUFFER), 0u;
        c.write(buf, text, n + 1);
        return n;
    });
}

}  // namespace rt
