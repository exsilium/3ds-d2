// 3ds-d2 Win32 runtime host: boots a PE32 executable on WinVita's Box86
// backend, bridge and cooperative scheduler, with WinVita's generic shims plus
// 3ds-d2's own (rt_shims_*.cpp). Platform-neutral: the Linux lanes and the 3DS
// apps all call rt::run_exe().
#ifndef D2_RUNTIME_RT_HOST_H
#define D2_RUNTIME_RT_HOST_H

#include "runtime/rt_layout.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace d2rt { class Bridge; class Cpu; }
namespace wx86 { class GuestRegion; }

namespace rt {

// A finished game frame (the source of a blit to the window): an 8-bit
// indexed image in guest memory plus its palette (0x00RRGGBB). Valid only
// during the present callback.
struct Frame {
    const uint8_t *pixels;   // top row
    uint32_t width, height;
    int32_t stride;          // bytes from one row to the next (negative for a bottom-up DIB)
    const uint32_t *palette; // 256 entries
};

struct RunOptions {
    const Layout *layout = &kTestLayout;
    // Guest files: the program's own directory is C:\ (GetModuleFileNameA
    // reports C:\<exe_name>). Reads look in write_root first, then data_root;
    // creation and writes go to write_root. Names match case-insensitively.
    std::string data_root;
    std::string write_root;                        // empty: same as data_root
    std::string exe_name = "game.exe";
    std::string command_line = "game.exe";
    std::function<void(const char *, size_t)> out; // guest stdout/stderr (WriteFile on a std handle)
    std::function<void(const char *)> log;         // runtime diagnostics
    // Guest time. false: the scheduler's deterministic virtual clock (time warps
    // to the next wait deadline; tests). true: real time, idling with sleep_ms
    // (a console: otherwise the game runs fast-forward).
    bool real_clock = false;
    std::function<void(uint32_t)> sleep_ms;
    // Virtual clock only: microseconds of guest time per scheduler switch, so a
    // guest polling the clock in a yield loop sees time pass (0: time moves
    // only while every thread waits). Deterministic either way.
    uint32_t virtual_tick_us = 0;
    // Physical memory GlobalMemoryStatus reports (0: WinVita's 256 MiB). Diablo
    // II sizes its sprite cache from it; 64 gives the game's own 24 MiB floor.
    uint32_t total_phys_mb = 0;
    // CPUID leaf-1 EDX reported to the guest; 0 keeps the backend's default
    // (FPU CMOV MMX FXSR SSE SSE2). The 3DS reports kCpuidNoSimd: the game
    // then takes its x87/integer paths instead of SSE code the ARM11 emulates.
    uint32_t cpuid_edx = 0;
    uint32_t max_ms = 0;
    std::function<void(const Frame &)> present;   // each frame the game shows
    std::function<void()> poll_input;              // sample the controls (from the message pump, every >= 16 ms)
    uint32_t trap_report = 0;
    uint32_t heartbeat_ms = 0;                     // log progress counters every N ms of guest time                      // at exit, print the N most-called imports (stdout)                           // stop the guest after this much guest time (0: no limit)
};

constexpr uint32_t kCpuidNoSimd = 0x00008001;   // FPU | CMOV

struct RunResult {
    bool loaded = false;
    std::string error;
    uint32_t exit_code = 0;
    bool exited = false;          // ExitProcess was called (or the main thread returned)
    bool timed_out = false;       // max_ms reached
    uint32_t unresolved_imports = 0;
};

// Progress counters, for the heartbeat log and the test lanes.
struct Stats {
    uint32_t peeks = 0;          // PeekMessageA/GetMessageA calls: the message loop is running
    uint32_t files_opened = 0;
    uint64_t bytes_read = 0;
    uint32_t dib_sections = 0;
    uint32_t frames = 0;         // blits of a DIB section to the window
};
Stats &stats();

// Guest memory use, for on-screen diagnostics (walks the allocators: call it
// about once a second, not per frame).
struct MemoryReport {
    uint32_t heap_used, heap_peak, heap_size;
    uint32_t va_used, va_peak, va_size;
    // Host memory of the CPU backend (0 when the build does not count it):
    uint32_t jit_code_kb;    // translated ARM code
    uint32_t jit_meta_kb;    // block descriptors and the rest of its own allocator
    uint32_t jmptbl_kb;      // jump tables: 256 KiB per 64 KiB of guest code that ever ran
};
MemoryReport memory_report();

// Reads guest memory from host code (game-state probes). False when the
// range is outside the guest span or the runtime is not running.
bool guest_read(uint32_t va, void *out, uint32_t n);

// `exe` is taken by value (move it in): its bytes are freed as soon as the
// image is loaded, and the loader's own host copy once it is linked.
RunResult run_exe(std::vector<uint8_t> exe, const RunOptions &opt);

// ---- services for the shim units ----------------------------------------------

const RunOptions &options();
const Layout &layout();
d2rt::Bridge &bridge();
uint32_t exe_base();
// TEB of the guest thread currently running (the main TEB before the scheduler starts).
uint32_t current_teb();
void set_last_error(d2rt::Cpu &c, uint32_t err);
// Calls guest code from inside a shim (window procedures, callbacks): builds the
// frame below the caller's ESP, runs to the bridge sentinel, restores registers.
uint32_t guest_call(d2rt::Cpu &c, uint32_t va, std::initializer_list<uint32_t> args);
// Ends the current stdcall shim (registered with `shim_argc` arguments) by
// jumping to guest function `va` with `args`, as if the guest had called it
// in the shim's place: its return value and stack cleanup become the shim's.
// Unlike guest_call, the callee runs as ordinary guest code, so it may block
// or yield (a window procedure entering a critical section another game
// thread holds). Use it for DispatchMessageA and SendMessageA.
void tail_call(d2rt::Cpu &c, uint32_t va, uint32_t shim_argc, std::initializer_list<uint32_t> args);
void request_exit(uint32_t code);
// The guest's clock: the scheduler's (virtual or real), so GetTickCount, QPC,
// Sleep and timed waits always agree.
uint64_t now_ms();
uint64_t now_us();
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Guest strings.
std::string read_cstr(d2rt::Cpu &c, uint32_t va, uint32_t max = 4096);
std::string read_wstr(d2rt::Cpu &c, uint32_t va, uint32_t max = 4096);   // UTF-16 -> UTF-8 (BMP)
// Copies s (with NUL) into a guest buffer of `cap` bytes; returns the length
// without the NUL, or the required size including it when it does not fit.
uint32_t write_cstr(d2rt::Cpu &c, uint32_t va, uint32_t cap, const std::string &s);

// The VirtualAlloc region (file mapping views come from it too).
wx86::GuestRegion &va_region();

// Guest paths (rt_shims_files.cpp). A guest path ("C:\\Data\\x.mpq",
// "data\\x.mpq", "/x") becomes a host path under write_root or data_root.
// read: the first root holding it (case-insensitive). write: under write_root,
// creating missing parent directories and copying a data_root file up first.
bool host_path(const std::string &guest, bool write, std::string &host);
bool read_guest_file(const std::string &guest, std::vector<uint8_t> &bytes);
std::string guest_full_path(const std::string &guest);   // "C:\\..." form

// Name of a loaded module ("game.exe", "glide3x.dll"); empty if unknown (rt_shims_kernel32.cpp).
std::string module_name_of(uint32_t base);

void install_kernel32(d2rt::Bridge &br);
void install_advapi32(d2rt::Bridge &br);
void install_misc(d2rt::Bridge &br);
void install_net(d2rt::Bridge &br);
void install_files(d2rt::Bridge &br);
void install_user32(d2rt::Bridge &br);

// Input (rt_shims_user32.cpp), called from RunOptions::poll_input. Each call
// queues the window messages Windows would send to the game's main window
// and updates GetKeyState/GetCursorPos at once. Coordinates are game pixels.
void set_key(uint8_t vk, bool down);              // key state only, no message
void input_mouse_move(int x, int y);
void input_button(bool right, bool down);         // at the cursor
void input_key(uint8_t vk, bool down);            // WM_KEYDOWN/UP (Alt: WM_SYSKEY*); Enter, Esc, Backspace add WM_CHAR
void input_char(uint32_t ch);                     // WM_CHAR (text entry)
bool input_ready();                               // the main window exists and is active

}  // namespace rt

#endif
