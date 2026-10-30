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
//
// Or a cinematic's frame (src/game/cinematics.cpp): then yuv[0] is set,
// pixels and palette are null, and the picture is planar YUV 4:2:0 (BT.601,
// limited range) of width x height, its chroma planes half size rounded up.
struct Frame {
    const uint8_t *pixels;   // top row
    uint32_t width, height;
    int32_t stride;          // bytes from one row to the next (negative for a bottom-up DIB)
    const uint32_t *palette; // 256 entries
    const uint8_t *yuv[3] = {nullptr, nullptr, nullptr};
    uint32_t yuv_stride[3] = {0, 0, 0};
    // The height the picture is meant to have at its width: twice `height`
    // for a video stored at half height. Gives its aspect ratio.
    uint32_t display_height = 0;
};

// A cinematic's soundtrack, played by the platform beside the game's own
// sound (DirectSound is idle while a video plays): interleaved 16-bit PCM.
// write() never blocks; played() is the playback clock the video follows.
class VideoAudio {
public:
    virtual ~VideoAudio() = default;
    virtual bool open(uint32_t rate, uint32_t channels) = 0;
    // Queues up to `frames` frames (a sample per channel each); returns how
    // many it took (fewer when its queue is full).
    virtual uint32_t write(const int16_t *pcm, uint32_t frames) = 0;
    virtual uint64_t played() = 0;   // frames heard since open()
    virtual void close() = 0;
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
    // Bytes of the heap and VirtualAlloc pool (when the layout makes them
    // one) kept back in one contiguous block, for a request that fits
    // nowhere else once the pool's free space has gone to pieces (0: none).
    uint32_t pool_spare = 0;
    // CPUID leaf-1 EDX reported to the guest; 0 keeps the backend's default
    // (FPU CMOV MMX FXSR SSE SSE2). The 3DS reports kCpuidNoSimd: the game
    // then takes its x87/integer paths instead of SSE code the ARM11 emulates.
    uint32_t cpuid_edx = 0;
    uint32_t max_ms = 0;                           // stop the guest after this much guest time (0: no limit)
    std::function<void(const Frame &)> present;   // each frame the game shows
    std::function<void()> poll_input;              // sample the controls (from the message pump, every >= 16 ms)
    // The platform's own housekeeping, on the thread that runs the guest, at
    // least every 50 ms of real time even while the game pumps no messages
    // (a level load): the console's HOME button and sleep (issue #6). It may
    // block (the app suspended) and may call request_exit.
    std::function<void()> service;
    uint32_t trap_report = 0;                      // at exit, print the N most-called imports (stdout)
    uint32_t heartbeat_ms = 0;                     // log progress counters every N ms of guest time
    std::function<void()> heartbeat;               // after each heartbeat's line: the app's own counters
    // Registry DWORDs seeded under the game's settings key when absent (the
    // game's own Options menu still changes and saves them): the 3DS app picks
    // cheaper video settings than a PC's defaults.
    std::vector<std::pair<std::string, uint32_t>> registry_dwords;
    // Sound: WinVita's DirectSound emulation (ds_emul) mixing at `audio_rate`
    // Hz into the sink its settings pick (WX86_SON: "vita" = the platform's
    // realtime sink, "wav" with WX86_SONDUMP, "null"). Off: DirectSoundCreate
    // reports "no driver" and the game runs silent.
    // Called once after linking, before the game runs: the place to replace
    // guest functions with native code (hook_function below).
    std::function<void(d2rt::Cpu &)> install_native;
    bool audio = false;
    int audio_rate = 22050;
    // Asynchronous file reads (real_clock only): io_start runs `worker` on a
    // new host thread, ideally on another core, and returns false if it
    // can't; the worker blocks in io_wait() until io_wake(). A guest thread
    // whose ReadFile misses the cache then waits for the worker while the
    // other guest threads run, instead of stalling them all on the storage.
    // Unset: reads stay synchronous (the deterministic Linux lanes).
    std::function<bool(void (*worker)())> io_start;
    std::function<void()> io_wait, io_wake;
    // Optional, with io_start: io_done() is called by the worker after each
    // completed read, and io_idle_wait(ms) waits for it at most `ms`. The
    // scheduler, with every guest thread waiting on reads, then wakes as soon
    // as one completes instead of polling every millisecond.
    std::function<void()> io_done;
    std::function<void(uint32_t)> io_idle_wait;
    // Where cinematics play their sound (null: they play silent).
    VideoAudio *video_audio = nullptr;
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
    int frame_thread = -1;       // the guest thread that blitted the last one
    uint32_t alloc_failures = 0; // guest heap or VirtualAlloc requests refused
};
Stats &stats();

// The read cache of game files (rt_shims_files.cpp): cumulative counters,
// reported per interval with the heartbeat.
struct FileStats {
    uint64_t calls = 0;       // ReadFile calls on any file
    uint64_t hits = 0;        // cached blocks reused
    uint64_t fills = 0;       // blocks read from the file system
    uint64_t direct = 0;      // reads of a block or more, straight to the guest
    uint64_t seeks = 0;       // SetFilePointer on cached files (no file system access)
    uint64_t disk_reads = 0, disk_bytes = 0, disk_us = 0;   // file system reads by the cache
    uint64_t cache_bytes = 0; // block memory held now (freed with its handle)
    uint64_t async = 0;       // reads handed to the I/O worker, the guest thread waiting
    // Time the game waited on the file system (issue #31): a guest thread
    // waiting on the I/O worker as a guest wait, the other threads running
    // (from the read's submission to the waiter waking; wait_frame_us those
    // of the thread presenting frames, whose frames stop meanwhile); and
    // reads ReadFile completed before returning, every guest thread stopped
    // (an OVERLAPPED read, a full request pool, or synchronous reads).
    uint64_t waits = 0, wait_us = 0, wait_frame_us = 0;
    uint64_t stall_us = 0;
};
const FileStats &file_stats();
// The asynchronous reads' worker (RunOptions::io_start): started and stopped
// by run_exe.
void start_async_io();
void stop_async_io();

// Time-based profile sampling (PROF_COUNTERS builds; a no-op otherwise): a
// host timer thread calls this at a fixed period, on another core. It ends
// the running guest thread's slice at its next translated block, and the
// scheduler samples where it stopped. Safe from any thread, at any time.
void profile_tick();

// Guest memory use, for on-screen diagnostics (walks the allocators: call it
// about once a second, not per frame).
struct MemoryReport {
    uint32_t heap_used, heap_peak, heap_size;
    uint32_t va_used, va_peak, va_size;
    // The heap and VA ranges are one pool when adjacent (rt_host.cpp): what
    // is left of it, and its largest free block.
    uint32_t free, largest_free;
    // RunOptions::pool_spare: bytes in use in it, and requests it has served.
    uint32_t spare_used, spare_takes;
    // Host memory of the CPU backend (0 when the build does not count it):
    uint32_t jit_code_kb;    // translated ARM code
    uint32_t jit_meta_kb;    // block descriptors and the rest of its own allocator
    uint32_t jmptbl_kb;      // jump tables: 256 KiB per 64 KiB of guest code that ever ran
};
MemoryReport memory_report();

// Reads guest memory from host code (game-state probes). False when the
// range is outside the guest span or the runtime is not running.
bool guest_read(uint32_t va, void *out, uint32_t n);

// A host CPU exception's guest picture, for the console's exception handler:
// the guest block and registers when the fault is in translated code, who
// owns the fault address and each register's target, every guest thread,
// memory use and a call-stack hint. `r` = host r0-r15, `far` = the fault
// address. Writes through `line`; reads guest memory directly, takes no
// lock and allocates once (the module list).
void crash_report(const uint32_t r[16], uint32_t far, void (*line)(const char *));

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
bool exit_requested();
// Blocks the calling guest thread for `ms` (0: yields) from inside a shim,
// running the other guest threads meanwhile, exactly as Sleep does.
void sleep_guest(uint32_t ms);
// The guest's clock: the scheduler's (virtual or real), so GetTickCount, QPC,
// Sleep and timed waits always agree.
uint64_t now_ms();
// Closes the host side of every file the game has open (after the I/O
// worker's queued reads), and opens them again where they were: for the
// platform to let go of its storage while the app is suspended (the HOME
// menu). Call from the game's thread while no guest code runs, in pairs.
void suspend_files();
void resume_files();
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

// Replaces the guest function at `va` with native code: checks that the
// function starts with `expect` (n bytes, so a different executable is left
// alone), registers `fn` as a shim taking `stack_args` dwords (stdcall: the
// shim's return pops them, as the original's `ret`), and writes a jmp to the
// shim's trap over the function's first bytes. The guest stack and registers
// arrive exactly as the original would see them (ecx/edx included); callee-
// saved registers are untouched by a shim. Returns false if the bytes differ.
// `in_line`: the shim is served from inside the translated code, without
// leaving the dynarec loop (WinVita's in-line shim path), which saves most
// of a call's fixed cost. Only for native code that never calls back into
// guest code (guest_call) and never blocks.
bool hook_function(d2rt::Cpu &c, uint32_t va, const uint8_t *expect, uint32_t n, uint32_t stack_args,
                   std::function<uint32_t(d2rt::Cpu &)> fn, const char *name, bool in_line = false);
// Native code the guest can call (or a guest function pointer can hold):
// registers `fn` as a shim taking `stack_args` dwords, popped on return,
// and returns its guest address (0 on failure). hook_function is this plus
// a jmp over a guest function.
uint32_t native_entry(const char *name, uint32_t stack_args, std::function<uint32_t(d2rt::Cpu &)> fn);

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

// For a native loop that owns the message pump while it runs (the
// cinematics player): samples the controls as PeekMessageA does, then
// removes and returns the oldest queued keyboard or mouse message. Other
// messages stay queued for the game.
struct InputMessage { uint32_t message, wparam, lparam; };
bool take_input_message(InputMessage &m);

}  // namespace rt

#endif
