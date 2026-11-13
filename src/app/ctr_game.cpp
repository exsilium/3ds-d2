// 3ds-d2 game app: Diablo II 1.14d (Game.exe, windowed GDI mode) on 3ds-d2's
// Win32 runtime and the ARMv6 Box86 backend.
//
//   sdmc:/3ds/3ds-d2/game/   Game.exe and the MPQs (read-only, the user's copy)
//   sdmc:/3ds/3ds-d2/save/   everything the game writes (saves, settings, registry)
//   sdmc:/3ds/3ds-d2/logs/   game.log (runtime diagnostics), a profiler run's traces
//   sdmc:/3ds/3ds-d2/reports/  a crashed or reported session's files (ctr_report.cpp)
//
// Display: the game draws an 800x600 8-bit frame. The top screen shows all of
// it stretched over the full 400x240 (2:1 across, 2.5:1 down, filtered), or a
// 1:1 400x240 window that follows the cursor, for reading text; Select
// switches. The bottom screen (game::SecondScreen) has tabs: the automap,
// touch buttons for the hotkeys, a 1:1 view around the cursor, diagnostics
// (frame rate, memory, the runtime's messages), and an open panel at 1:1 to
// touch directly; it is also the touchpad. When the game focuses a text
// field, an on-screen keyboard takes it over.
// Controls: src/game/controls.h. L+R+Select saves the frame into the
// console's Camera album (ctr_screenshot.cpp).
//
// The frame is drawn by the CPU straight into the RGB565 framebuffer: a
// palette lookup per sample, no texture upload, no GPU state. Cinematics
// (src/game/cinematics.cpp) arrive as YUV and are drawn the same way, scaled
// to fit the screen with their aspect ratio; their sound is ctr_audio.cpp's.
#include "app/ctr_writer.h"
#include "game/autoplay.h"
#include "game/automap.h"
#include "game/cinematics.h"
#include "game/controls.h"
#include "game/d2_memory.h"
#include "game/d2_native.h"
#include "game/d2_state.h"
#include "game/d2_units.h"
#include "game/draw_census.h"
#include "game/dcc_native.h"
#include "game/frame_reads.h"
#include "game/tiles_ahead.h"
#include "game/frame_trace.h"
#include "game/port_options.h"
#include "game/game_exe.h"
#include "game/osk.h"
#include "game/profiler.h"
#include "game/second_screen.h"
#include "game/sprite_trace.h"
#include "game/storm_reads.h"
#include "runtime/ds_emul.h"
#include "runtime/rt_host.h"

#include <3ds.h>
#include <3ds/allocator/mappable.h>

#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Memory: libctru's heap holds only the host side (JIT code, runtime tables,
// file buffers); the guest arena (63 MiB, kCtrGameLayout) is mapped above it
// by the backend with svcControlMemory at a 16 MiB-aligned address (mman_ctr.c),
// which a default all-memory heap would leave no room for. The linear heap
// holds just the framebuffers and the keyboard applet's buffers.
//
// Two memory budgets. The .3dsx (Homebrew Launcher, Azahar) gets about 91 MiB
// of application memory; the CIA flavour (-DD2_CIA) asks for the New 3DS's
// extended mode in its exheader (124 MiB) and spends the difference on the
// guest's VirtualAlloc (the sprite cache) and the host's JIT.
extern "C" {
extern u32 __ctru_heap;
extern uint32_t dyn86_jit_allocfail;   // the backend's count of refused code allocations
#ifdef D2_CIA
// All the memory the 124 MiB mode leaves goes to libctru's heap (size 0 =
// everything after the linear heap): the console's kernel will not map memory
// at an address of our choosing, so the backend carves the guest arena out of
// this heap with a 16 MiB-aligned memalign (mman_ctr.c), and the rest is the
// host's: JIT, jump tables, runtime.
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 2u << 20;

// Translated code lives in a static pool in .bss (mman_ctr.c): the console
// refused to make heap pages executable (every code allocation failed), and
// .bss sits outside its 96 MiB heap window, in budget the heap cannot use.
// The town needed 8 MiB of code in Azahar; issue #23's Act I session had
// 14 MiB of chunks open after 28 minutes and 16 MiB after 2 h 52. A full
// pool refuses new chunks without trying the heap (issue #90), and the JIT
// evicts old code (#23: exact at 6 MiB of the fight's 7.2 on the lane).
// Box86 takes it in 2 MiB chunks, so its size is a multiple of 2 MiB.
alignas(0x1000) uint8_t dyn86_ctr_jitpool[14u << 20];
extern const uint32_t dyn86_ctr_jitpool_size = 14u << 20;
extern uint32_t dyn86_ctr_jitpool_refused;   // mman_ctr.c: chunks refused, the pool full
// The tile prefetch's store (issue #89, rt::set_prefetch_arena), here for
// the same reason: out of the heap, whose last MiB the JIT's records and
// the port's own allocations share. The pool's other MiB goes to the budget
// __system_allocateHeaps hands the linear heap, for jump tables that would
// otherwise take the main heap.
alignas(64) uint8_t g_prefetch_arena[1u << 20];

// libctru's own __system_allocateHeaps panics (svcBreak) when the kernel
// refuses the heap it asks for, and the console refused "all that is left":
// it grants less than its commit budget reports. This one asks for that
// budget, then steps down 4 MiB at a time until the kernel agrees, and keeps
// a record for main() to show. Same placement as libctru (OS_HEAP_AREA_BEGIN).
extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_linear_heap;
struct HeapSetup {
    u32 budget;        // what the resource limit says is left
    u32 region_free;   // what the APPLICATION region says is free
    u32 tries;         // allocation attempts until one succeeded
    Result last_rc;    // the last refusal
    Result linear_rc;
    u32 jmptbl_linear; // linear memory set aside for jump tables
};
HeapSetup g_heap_setup;

// Box86's second-level jump tables (256 KiB each, 12 MiB in play): the first
// ones go to the linear memory set aside above, the rest to the heap.
static u32 g_jmptbl_linear_used;
void *d2rt_jmptbl_alloc(size_t n)
{
    if (g_jmptbl_linear_used + n <= g_heap_setup.jmptbl_linear) {
        if (void *p = linearAlloc(n)) {
            g_jmptbl_linear_used += n;
            return p;
        }
    }
    return malloc(n);
}
// The guest arena, reserved first thing in main (reserve_arena) and lent to
// the backend when it maps it (mman_ctr.c): newlib's memalign needs the
// arena's size + 16 MiB of contiguous heap, which the 95 MiB heap only has
// before Game.exe and the rest of the startup take their share.
// Given back to the heap once the game has ended (give_back_arena), for the
// report's sockets; munmap still keeps the block's address for the app then,
// so that a late unmap does not free it a second time.
static void *g_arena, *g_arena_block;
static size_t g_arena_size;
void *dyn86_ctr_arena_take(size_t len) { return g_arena && len <= g_arena_size ? g_arena : NULL; }
int dyn86_ctr_arena_release(void *p) { return p && p == g_arena_block; }
void d2rt_jmptbl_free(void *p)
{
    const u32 a = (u32)p;
    if (a >= __ctru_linear_heap && a - __ctru_linear_heap < __ctru_linear_heap_size) {
        g_jmptbl_linear_used -= linearGetSize(p);
        linearFree(p);
    } else {
        free(p);
    }
}
void __system_allocateHeaps(void)
{
    HeapSetup &hs = g_heap_setup;
    Handle reslimit = 0;
    s64 max_commit = 0, cur_commit = 0;
    if (R_SUCCEEDED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
        ResourceLimitType type = RESLIMIT_COMMIT;
        svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
        svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
        svcCloseHandle(reslimit);
    }
    hs.budget = (u32)(max_commit - cur_commit) & ~0xFFFu;
    hs.region_free = (u32)osGetMemRegionFree(MEMREGION_APPLICATION) & ~0xFFFu;
    u32 want = (hs.budget < hs.region_free ? hs.budget : hs.region_free);
    want = want > __ctru_linear_heap_size ? (want - __ctru_linear_heap_size) & ~0xFFFFFu : 0;
    // The console's heap window ends at 0x0E000000: it mapped 95 MiB and
    // refused 99. Starting at its limit keeps Azahar (which allows more) on
    // exactly the console's memory layout.
    constexpr u32 kHeapWindowMax = 95u << 20;
    if (want > kHeapWindowMax)
        want = kHeapWindowMax;
    const MemPerm rw = (MemPerm)(MEMPERM_READ | MEMPERM_WRITE);
    for (;;) {
        hs.tries++;
        const Result rc = svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, want, MEMOP_ALLOC, rw);
        if (R_SUCCEEDED(rc))
            break;
        hs.last_rc = rc;
        if (want <= (8u << 20))
            svcBreak(USERBREAK_PANIC);   // not even 8 MiB: nothing can run
        want -= 4u << 20;
    }
    __ctru_heap_size = want;
    // The budget the heap window leaves (~2.9 MiB on the console) can only be
    // linear memory: it goes to the linear heap for Box86's jump tables
    // (d2rt_jmptbl_alloc below), so the guest arena can be that much larger
    // (rt_layout.h). 256 KiB stays unclaimed.
    const u32 avail = hs.budget < hs.region_free ? hs.budget : hs.region_free;
    const u32 spare = avail > want + __ctru_linear_heap_size ? avail - want - __ctru_linear_heap_size : 0;
    hs.jmptbl_linear = spare > (256u << 10) ? (spare - (256u << 10)) & ~0x3FFFFu : 0;
    hs.linear_rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size + hs.jmptbl_linear,
                                    MEMOP_ALLOC_LINEAR, rw);
    if (R_SUCCEEDED(hs.linear_rc)) {
        __ctru_linear_heap_size += hs.jmptbl_linear;
    } else {
        hs.jmptbl_linear = 0;
        hs.linear_rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, rw);
    }
    if (R_FAILED(hs.linear_rc))
        svcBreak(USERBREAK_PANIC);   // no framebuffers: nothing can be shown
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}
#else
u32 __ctru_heap_size = 24u << 20;
u32 __ctru_linear_heap_size = 1u << 20;
#endif

// The host heap's growth (issue #24): newlib's malloc takes more of libctru's
// heap through _sbrk_r (linked with --wrap=_sbrk_r), and returns NULL when
// that is refused. Kept here: the room left above the heap's top, its lowest
// since the last heartbeat, and the refused requests. Called under malloc's
// lock, so nothing here may allocate or log.
extern char *fake_heap_end;
void *__real__sbrk_r(struct _reent *r, ptrdiff_t incr);
uint32_t g_heap_room = 0, g_heap_room_low = UINT32_MAX;
uint32_t g_heap_refused = 0, g_heap_refused_first = 0;   // requests refused, the first one's size
void *__wrap__sbrk_r(struct _reent *r, ptrdiff_t incr)
{
    void *p = __real__sbrk_r(r, incr);
    if (p == (void *)-1) {
        if (!g_heap_refused++)
            g_heap_refused_first = (uint32_t)incr;
        return p;
    }
    g_heap_room = (uint32_t)(fake_heap_end - ((char *)p + incr));
    if (g_heap_room < g_heap_room_low)
        g_heap_room_low = g_heap_room;
    return p;
}

// The JIT's share of the host heap (issue #24, Sentry 3DS-D2-12). Box86's
// records of translated blocks take the heap 64 KiB at a time as new code
// is translated, and are never given back on their own: after 1 h 47 min
// of new areas they had taken all but 48 KiB above the heap's top, and the
// app's next C++ new (the profiler's report) found nothing and the game
// ended in std::terminate. Box86 now asks before each new 64 KiB (for a
// block's record only); refused, it evicts old blocks and reuses their
// records. The reserve is the app's while the game runs: the heartbeat's
// and the profiler's tables, the bottom screen's strings, the trace and
// report buffers (tens of KiB each), with room for the heap's fragments
// (the report's pictures at the game's end come after the guest arena is
// given back). Under box86's allocator lock: may not log or allocate.
constexpr size_t kHostHeapReserve = 1024u << 10;
extern unsigned d2rt_meta_reserve_refusals;   // box86 (custommem.c): new blocks refused so
int d2rt_meta_may_grow(size_t bytes)
{
    const struct mallinfo mi = mallinfo();
    const size_t free = __ctru_heap_size > mi.uordblks ? __ctru_heap_size - mi.uordblks : 0;
    return free >= bytes + kHostHeapReserve;
}
}

namespace d2rt {
namespace audio {
rt::VideoAudio *video_sink();   // ctr_audio.cpp
}  // namespace audio
}  // namespace d2rt

namespace shot {
struct Source {
    const uint8_t *pixels;
    uint32_t width, height;
    const uint32_t *palette;
    bool yuv;
    uint32_t display_height;
    const uint16_t *fb565 = nullptr;
    const uint8_t *rgb = nullptr;
};
// ctr_screenshot.cpp: the frame, and the bottom screen as shown (RGB565, the
// framebuffer's layout), as a pair.
bool save(const Source &src, const uint16_t *bottom, std::string &msg);
}  // namespace shot

namespace report {   // ctr_report.cpp
struct Host {
    PrintConsole *log;
    void (*message)(const char *);
    void (*restore_bottom)();
    const char *app;
    const char *game_files;
};
std::string start_line();
const char *closed_line();
std::string keep_unclosed_session();
std::string keep_session(const Host &host, const char *reason, uint32_t before_ms);
bool enabled();
bool offer_one(const Host &host, const std::string &dir);
bool offer_pending(const Host &host);
}  // namespace report

namespace {

const char kRoot[] = "sdmc:/3ds/3ds-d2";
const char kGameDir[] = "sdmc:/3ds/3ds-d2/game";
const char kRomfsGameDir[] = "romfs:/game";   // the CIA carries the game files in its RomFS
const char kSaveDir[] = "sdmc:/3ds/3ds-d2/save";
const char kLogDir[] = "sdmc:/3ds/3ds-d2/logs";
const char kTracePath[] = "sdmc:/3ds/3ds-d2/logs/sprites.trc";   // the profiler run's sprite trace (issue #18)
const char kFileTracePath[] = "sdmc:/3ds/3ds-d2/logs/files.trc";  // ...and its file-read trace (issue #21)
const char kFrameTracePath[] = "sdmc:/3ds/3ds-d2/logs/frames.trc";  // ...and its frame trace
const char kSettings[] = "sdmc:/3ds/3ds-d2/settings.txt";   // the bottom screen's tab and map size, the profiler checklist

FILE *g_log = nullptr;
game::Controls g_controls;
bool g_zoom = false;   // top screen: false = whole frame, true = 1:1 around the cursor
uint16_t g_pal565[256];
// The palette with each channel in its own 10-bit field (r << 20 | g << 10 | b),
// so four entries sum in plain adds without carries between channels: the
// 2x2 filter then costs four table loads and three adds per output pixel.
uint32_t g_pal_wide[256];

// ---- bottom screen -------------------------------------------------------------------
//
// Until the game shows its first frame: one console, the startup messages,
// on the whole bottom screen from its top (issue #98; it had a statistics
// window above, which nothing wrote to any more). From then on the bottom
// screen is game::SecondScreen's (second_screen.h), drawn in pixels with
// each frame; the messages become its INFO tab. The game's end and a fatal
// message bring the console back (console_again).

PrintConsole g_log_con;
game::SecondScreen g_ss;
game::Osk g_osk;
bool g_ss_active = false;    // the second screen owns the bottom framebuffer
bool g_bottom_double = false; // render thread: the bottom screen is double-buffered (second screen)
const uint16_t *g_bottom_shown = nullptr;   // render thread: then, the buffer it shows (screenshots)
extern "C" const uint8_t default_font_bin[];   // libctru's console font: 8x8, 1 bpp, bit 7 leftmost
int g_render_core = -1;                 // the core drawing frames (-1: the game's thread)
volatile uint32_t g_frames_skipped = 0; // frames not shown: the previous one was still drawing
d2rt::Cpu *g_guest_cpu = nullptr;       // the game's CPU, from install_native on
game::SpriteCacheWindow g_sprite_window{};   // the sprite cache in the last heartbeat's 10 s
// The profiler run (game/profiler.h), switched at the heartbeat.
uint32_t g_prof_on = 0;        // its options running
uint32_t g_prof_refused = 0;   // options that would not start: not again in this run
uint32_t g_prof_used = 0;      // options that ran: the session's report is offered for them
std::string g_bench_path;      // the SD card benchmark's file (the game's d2data.mpq)
game::Autoplay g_autoplay;
bool g_autoplay_on = false;   // sdmc:/3ds/3ds-d2/autoplay.txt, until the first physical input

struct Timing {
    uint64_t window_start_ms = 0;   // host clock
    uint32_t frames = 0;            // presents in this window
    uint64_t draw_us = 0;           // time spent drawing them
    uint64_t last_present_us = 0;
    uint64_t worst_gap_us = 0;      // longest time between two presents
    uint32_t total_frames = 0;
} g_timing;

// The hitch counters (issue #19), per heartbeat window: the intervals
// between presents by length, and the longest, from the timestamp present()
// takes anyway. The heartbeat adds what else stopped frames (sprites taken
// and let go of; the frame thread's time blocked, ready while another guest
// thread ran, and waiting on host work; the heartbeat before, whose length
// the frame after it waited; frames not drawn; the mixer's starved grains),
// logs the window as "prof hitch:" and starts the next. Both on the game's
// thread.
struct Hitches {
    uint32_t frames[4] = {};   // intervals of <=50, <=100, <=250, >250 ms
    uint32_t longest_ms = 0;
};
Hitches g_hitch;   // the window so far
struct HitchWindow {
    Hitches h;
    uint32_t wait_ms = 0, ready_ms = 0, host_ms = 0, heartbeat_ms = 0, skipped = 0, famine = 0;
} g_hitch_last;    // the last whole window: INFO shows it
struct HitchMark {
    rt::FrameThreadTimes frame;
    uint32_t skipped = 0;
    unsigned long long famine = 0;
} g_hitch_mark;

uint64_t host_us() { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }

inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

void show_stats(float fps, float draw_ms, float worst_ms)
{
    const rt::MemoryReport m = rt::memory_report();
    const struct mallinfo mi = mallinfo();
    const uint64_t t = rt::now_ms() / 1000;
    const rt::Stats &st = rt::stats();
    char line[13][48];
    std::snprintf(line[0], 48, "3ds-d2  game time %02u:%02u:%02u%s", (unsigned)(t / 3600), (unsigned)(t / 60 % 60),
                  (unsigned)(t % 60), g_prof_on ? "  PROF" : "");   // a profiler run
    std::snprintf(line[1], 48, "fps %5.1f   worst frame gap %6.1f ms", fps, worst_ms);
    std::snprintf(line[2], 48, "draw %5.2f ms c%d skip %u frames %u", draw_ms, g_render_core < 0 ? 0 : g_render_core,
                  (unsigned)g_frames_skipped, (unsigned)g_timing.total_frames);
    // Heap and VA share one pool: their sizes are what each has taken.
    std::snprintf(line[3], 48, "guest heap %6u pk %6u free %6u", m.heap_used >> 10, m.heap_peak >> 10, m.free >> 10);
    std::snprintf(line[4], 48, "guest va   %6u pk %6u big  %6u", m.va_used >> 10, m.va_peak >> 10,
                  m.largest_free >> 10);
    std::snprintf(line[5], 48, "host heap  %6u/%6u KiB", (unsigned)(mi.uordblks >> 10), (unsigned)(__ctru_heap_size >> 10));
    std::snprintf(line[6], 48, "linear free %5u  app free %5u KiB", (unsigned)(linearSpaceFree() >> 10),
                  (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
    // The sprite cache (issue #18): its size, and the last heartbeat's
    // sprites taken and let go of, per 10 s.
    const game::SpriteCacheStats sc = g_guest_cpu ? game::sprite_cache_stats(*g_guest_cpu) : game::SpriteCacheStats{};
    std::snprintf(line[7], 48, "sprites %5u/%5u in %4u out %4u", sc.used >> 10, sc.limit >> 10,
                  g_sprite_window.inserted, g_sprite_window.evicted);
    const bool jit_full = dyn86_jit_allocfail != 0;   // translated code no longer fits: the game will stall
    if (jit_full)
        std::snprintf(line[8], 48, "JIT MEMORY FULL code %u jmptbl %u", m.jit_code_kb, m.jmptbl_kb);
    else
        std::snprintf(line[8], 48, "jit %5u/%5u meta %5u jmptbl %5u", m.jit_used_kb, m.jit_code_kb, m.jit_meta_kb,
                      m.jmptbl_kb);   // jit: held by blocks / chunks opened (issue #23)
    // The hitch counters (issue #19), the last heartbeat's 10 s.
    const HitchWindow &hw = g_hitch_last;
    std::snprintf(line[9], 48, "files %3u read %4u MiB skip %3u fam %3u", st.files_opened,
                  (unsigned)(st.bytes_read >> 20), (unsigned)hw.skipped, (unsigned)hw.famine);
    std::snprintf(line[10], 48, "hitch >100ms %3u >250ms %3u max %5u ms", (unsigned)(hw.h.frames[2] + hw.h.frames[3]),
                  (unsigned)hw.h.frames[3], (unsigned)hw.h.longest_ms);
    std::snprintf(line[11], 48, "wait %4u ready %4u host %4u hb %4u", (unsigned)hw.wait_ms, (unsigned)hw.ready_ms,
                  (unsigned)hw.host_ms, (unsigned)hw.heartbeat_ms);
    std::snprintf(line[12], 48, "top: %s%s", g_zoom ? "1:1 zoom (Select)" : "whole frame (Select: zoom)",
                  g_autoplay_on ? "  AUTO" : "");
    std::vector<game::BottomView::Line> lines;
    for (int i = 0; i < 13; i++) {
        line[i][40] = 0;   // 40 columns
        const uint16_t c = i == 0 ? rgb565(255, 220, 90) : (i == 8 && jit_full) ? rgb565(255, 60, 60) : rgb565(255, 255, 255);
        lines.push_back({line[i], c});
    }
    lines.push_back({"----------------------------------------", rgb565(130, 120, 100)});
    g_ss.set_stats(std::move(lines));
}

void update_stats()
{
    const uint64_t now = host_us() / 1000;
    if (!g_timing.window_start_ms)
        g_timing.window_start_ms = now;
    const uint64_t span = now - g_timing.window_start_ms;
    if (span < 1000)
        return;
    const float fps = g_timing.frames * 1000.0f / span;
    const float draw = g_timing.frames ? g_timing.draw_us / 1000.0f / g_timing.frames : 0;
    show_stats(fps, draw, g_timing.worst_gap_us / 1000.0f);
    g_timing.window_start_ms = now;
    g_timing.frames = 0;
    g_timing.draw_us = 0;
    g_timing.worst_gap_us = 0;
}

// game.log: its lines go to a ring that the writer thread writes to the SD
// card (ctr_writer.h, issue #71). Written from the game's thread, the
// heartbeat's ~11 KiB held the game ~213 ms every 10 s while profiling, and
// one write took 2.4 s (3DS-D2-13). A line is queued at once; the
// heartbeat's lines wake the writer together at its end (log_flush), other
// lines as they come.
char g_log_ring[64 << 10];

void log_flush() { app::writer_kick(); }

void logline(const char *s)
{
    if (g_log) {
        std::fprintf(g_log, "%s\n", s);
        app::writer_kick();
    }
}

// What the system has left (issue #24: going East crashed the HOME Menu
// once and the kernel once, with nothing from the app): free memory in the
// three regions, the app's linear memory and host heap, the heap's growth,
// the app's kernel objects and the memory blocks of its address space (one
// kernel object per run of pages with the same state and permission).
unsigned memory_blocks()
{
    unsigned n = 0;
    u32 addr = 0;
    while (addr < 0x40000000u && n < 100000) {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || !mi.size)
            break;
        n++;
        addr = mi.base_addr + mi.size;
    }
    return n;
}

void system_line(char *out, size_t size, const char *what)
{
    const struct mallinfo mi = mallinfo();
    s64 n[8] = {};
    Handle rl;
    if (R_SUCCEEDED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) {
        ResourceLimitType names[] = {RESLIMIT_THREAD, RESLIMIT_EVENT,        RESLIMIT_MUTEX,          RESLIMIT_SEMAPHORE,
                                     RESLIMIT_TIMER,  RESLIMIT_SHAREDMEMORY, RESLIMIT_ADDRESSARBITER};
        svcGetResourceLimitCurrentValues(n, rl, names, 7);
        svcCloseHandle(rl);
    }
    const uint32_t low = g_heap_room_low == UINT32_MAX ? g_heap_room : g_heap_room_low;
    std::snprintf(out, size,
                  "[rt] system%s: free KiB app %u sys %u base %u linear %u heap %u, heap room %u low %u, heap refused "
                  "%u (first %u B), threads %d events %d mutexes %d semaphores %d timers %d shmem %d arbiters %d, "
                  "blocks %u",
                  what, (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10),
                  (unsigned)(osGetMemRegionFree(MEMREGION_SYSTEM) >> 10),
                  (unsigned)(osGetMemRegionFree(MEMREGION_BASE) >> 10), (unsigned)(linearSpaceFree() >> 10),
                  (unsigned)((__ctru_heap_size - mi.uordblks) >> 10), (unsigned)(g_heap_room >> 10),
                  (unsigned)(low >> 10), (unsigned)g_heap_refused, (unsigned)g_heap_refused_first, (int)n[0],
                  (int)n[1], (int)n[2], (int)n[3], (int)n[4], (int)n[5], (int)n[6], memory_blocks());
}

// A refused heap growth, logged at once from the service call (every 50 ms),
// not at the next heartbeat: a crash that follows may come sooner.
void report_heap_refusals()
{
    static uint32_t reported = 0;
    const uint32_t n = g_heap_refused;
    if (n == reported)
        return;
    reported = n;
    char line[160];
    std::snprintf(line, sizeof line, "[rt] host heap: growth refused %u times (first %u B), room %u KiB",
                  (unsigned)n, (unsigned)g_heap_refused_first, (unsigned)(g_heap_room >> 10));
    logline(line);
}

// The JIT held at the host heap's reserve (d2rt_meta_may_grow): logged the
// first time, then at most once a minute while it goes on. From then on the
// JIT evicts old blocks to translate new ones (its "JIT eviction" lines in
// stderr.txt).
void report_meta_reserve()
{
    static uint32_t reported = 0;
    static uint64_t next_us = 0;
    const uint32_t n = d2rt_meta_reserve_refusals;
    const uint64_t now = host_us();
    if (n == reported || (reported && now < next_us))
        return;
    reported = n;
    next_us = now + 60000000;
    const struct mallinfo mi = mallinfo();
    char line[160];
    std::snprintf(line, sizeof line,
                  "[rt] host heap: JIT records held at the %u KiB reserve (%u refusals), heap %u KiB free, room %u KiB",
                  (unsigned)(kHostHeapReserve >> 10), (unsigned)n, (unsigned)((__ctru_heap_size - mi.uordblks) >> 10),
                  (unsigned)(g_heap_room >> 10));
    logline(line);
}

// One of the heartbeat's lines: written with the rest at its end.
void logline_held(const char *s)
{
    if (g_log)
        std::fprintf(g_log, "%s\n", s);
}

// ---- CPU exceptions (issue #5) --------------------------------------------------------
//
// A guest memory fault in translated code ends the app with a Luma3DS dump of
// host registers only: the guest thread, block and registers have to be
// worked out from the ELF and Game.exe by hand, and what the faulting
// registers pointed at is lost. This handler writes that guest picture to
// game.log (rt::crash_report), then hands the fault back: it removes itself
// and re-runs the faulting instruction, so Luma3DS still shows its screen
// and saves its usual dump of the same fault.

alignas(8) uint8_t g_exc_stack[0x8000];

// Resumes at frame[13] with r0-r12 = frame[0..12], sp = frame + 14, the
// condition flags of `cpsr` and lr = `lr` (user mode cannot set the rest).
__attribute__((naked, noreturn)) void resume_regs(uint32_t *frame, uint32_t cpsr, uint32_t lr)
{
    asm volatile("msr cpsr_f, r1\n\t"
                 "mov lr, r2\n\t"
                 "mov sp, r0\n\t"
                 "ldmia sp!, {r0-r12, pc}\n\t");
}

void on_cpu_exception(ERRF_ExceptionInfo *e, CpuRegisters *regs)
{
    // Once only: a second exception (in this report, or the re-run fault on a
    // thread that falls back to the main thread's handler) ends the app.
    static volatile int entered = 0;
    if (entered++)
        svcBreak(USERBREAK_PANIC);
    app::writer_crash();   // the queued lines, then this report, from this thread
    static const char *const kType[] = {"prefetch abort", "data abort", "undefined instruction", "VFP exception"};
    char s[200];
    std::snprintf(s, sizeof s, "crash: %s at pc %08lx, fsr %08lx, far %08lx, lr %08lx, sp %08lx, cpsr %08lx",
                  e->type <= ERRF_EXCEPTION_VFP ? kType[e->type] : "exception", (unsigned long)regs->pc,
                  (unsigned long)e->fsr, (unsigned long)e->far, (unsigned long)regs->lr, (unsigned long)regs->sp,
                  (unsigned long)regs->cpsr);
    logline(s);
    std::snprintf(s, sizeof s, "crash: r0-r12 %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx",
                  (unsigned long)regs->r[0], (unsigned long)regs->r[1], (unsigned long)regs->r[2],
                  (unsigned long)regs->r[3], (unsigned long)regs->r[4], (unsigned long)regs->r[5],
                  (unsigned long)regs->r[6], (unsigned long)regs->r[7], (unsigned long)regs->r[8],
                  (unsigned long)regs->r[9], (unsigned long)regs->r[10], (unsigned long)regs->r[11],
                  (unsigned long)regs->r[12]);
    logline(s);
    uint32_t r[16];
    for (int i = 0; i < 13; i++)
        r[i] = regs->r[i];
    r[13] = regs->sp;
    r[14] = regs->lr;
    r[15] = regs->pc;
    rt::crash_report(r, e->far, logline);
    logline("crash: handing the fault to Luma3DS (luma/dumps/arm11)");

    // From here the fault is fatal again: no handler, the same instruction.
    threadOnException(nullptr, nullptr, nullptr);
    if (e->type == ERRF_EXCEPTION_VFP)
        __builtin_trap();
    uint32_t *frame = reinterpret_cast<uint32_t *>(regs->sp - 14 * 4);
    for (int i = 0; i < 13; i++)
        frame[i] = regs->r[i];
    frame[13] = regs->pc | ((regs->cpsr & 0x20) ? 1u : 0u);   // Thumb state: interworking load
    resume_regs(frame, regs->cpsr, regs->lr);
}

// The render thread, once started (start_render_thread).
Thread g_render_thread = nullptr;

// A message for the screen: the startup console, or the INFO tab.
void screen_message(const char *s)
{
    if (g_ss_active)
        g_ss.log(s);
    else {
        std::printf("%.*s\n", 39, s);   // the log window
        // Until the render thread presents frames, on the screen at once:
        // the console's text out of the CPU cache, and the bottom screen
        // presented. The console shows memory as it is; Azahar shows a
        // screen only once it is presented, and kept it black until the
        // first frame.
        if (!g_render_thread) {
            gfxFlushBuffers();
            gfxScreenSwapBuffers(GFX_BOTTOM, false);
        }
    }
}

void status(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    screen_message(buf);
    logline(buf);
}

// A fatal message from the CPU backend (it exits right after): show it and
// The bottom screen back to the text console, over whatever the second
// screen drew (a fatal message, the game's end): single-buffered again, the
// consoles on the buffer shown, cleared.
void console_again()
{
    g_ss_active = false;
    if (g_bottom_double) {
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        g_bottom_double = false;
        g_bottom_shown = nullptr;
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
        g_log_con.frameBuffer = fb;
    }
    // All of it: the consoles' clears left the bottom row of the second
    // screen's last picture (MAP's corner button) showing.
    std::memset(g_log_con.frameBuffer, 0, 320 * 240 * sizeof(u16));
    consoleSelect(&g_log_con);
    consoleClear();
}

// wait for START, instead of dropping silently back to the HOME menu.
extern "C" void d2_ctr_progress_hook(const char *msg)
{
    if (std::strncmp(msg, "FATAL", 5) != 0)
        return;
    console_again();
    std::printf("\x1b[31m%s\x1b[0m\n", msg);
    logline(msg);
    std::printf("Logs: sdmc:/3ds/3ds-d2/logs/. START: exit\n");
    if (report::enabled())
        std::printf("A report is offered at the next start.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gspWaitForVBlank();
    }
}

// ---- top screen ----------------------------------------------------------------------

// The framebuffer is column-major (the LCD is mounted rotated): pixel (x, y)
// of the 400x240 top screen is at [x * 240 + (239 - y)].

// The whole frame over the whole screen. Each output pixel averages the 2x2
// block of source pixels at its position, so thin text strokes survive the
// 2.5:1 vertical step. Rows outer: each pass reads two source rows in order.
void draw_stretched(uint16_t *fb, const rt::Frame &f)
{
    static int sx[400];
    static uint32_t sx_width = 0;
    if (sx_width != f.width) {
        sx_width = f.width;
        for (int x = 0; x < 400; x++) {
            sx[x] = x * (int)f.width / 400;
            if (sx[x] + 1 >= (int)f.width)
                sx[x] = (int)f.width - 2;
        }
    }
    for (int y = 0; y < 240; y++) {
        const int sy = y * (int)f.height / 240;
        const uint8_t *r0 = f.pixels + (ptrdiff_t)sy * f.stride;
        const uint8_t *r1 = sy + 1 < (int)f.height ? r0 + f.stride : r0;
        uint16_t *out = fb + (239 - y);
        for (int x = 0; x < 400; x++, out += 240) {
            const int i = sx[x];
            const uint32_t sum = g_pal_wide[r0[i]] + g_pal_wide[r0[i + 1]] + g_pal_wide[r1[i]] + g_pal_wide[r1[i + 1]];
            // sum = 4r << 20 | 4g << 10 | 4b: the top 5/6/5 bits of each average.
            *out = (uint16_t)(((sum >> 14) & 0xF800) | ((sum >> 9) & 0x07E0) | ((sum >> 5) & 0x001F));
        }
    }
}

// A 400x240 window of the frame at 1:1, centred on the cursor.
void draw_window(uint16_t *fb, const rt::Frame &f, int cx, int cy)
{
    int left = cx - 200, top = cy - 120;
    left = left < 0 ? 0 : left > (int)f.width - 400 ? (int)f.width - 400 : left;
    top = top < 0 ? 0 : top > (int)f.height - 240 ? (int)f.height - 240 : top;
    for (int x = 0; x < 400; x++) {
        uint16_t *col = fb + x * 240 + 239;
        const uint8_t *src = f.pixels + (ptrdiff_t)top * f.stride + left + x;
        for (int y = 0; y < 240; y++, src += f.stride)
            *col-- = g_pal565[*src];
    }
}

// A cinematic's YUV 4:2:0 frame, scaled to fit the screen with its aspect
// ratio (640x292: 400x182; 640x480: 320x240) on black. Each output pixel
// averages a 2x2 block of luma and takes the chroma under it; BT.601
// limited range through tables. The rows outer, as in draw_stretched.
struct YuvTables {
    int32_t y[256], rv[256], gu[256], gv[256], bu[256];
    uint16_t r[1024], g[1024], b[1024];   // clamped 0..255, then placed in RGB565; index + 512
    YuvTables()
    {
        for (int i = 0; i < 256; i++) {
            y[i] = 298 * (i - 16) + 128;   // 1.164 (Y - 16), with the rounding of the >> 8 below
            rv[i] = 409 * (i - 128);
            gu[i] = -100 * (i - 128);
            gv[i] = -208 * (i - 128);
            bu[i] = 516 * (i - 128);
        }
        for (int i = 0; i < 1024; i++) {
            const int v = i - 512 < 0 ? 0 : i - 512 > 255 ? 255 : i - 512;
            r[i] = (uint16_t)((v >> 3) << 11);
            g[i] = (uint16_t)((v >> 2) << 5);
            b[i] = (uint16_t)(v >> 3);
        }
    }
};

void draw_yuv(uint16_t *fb, const uint8_t *const plane[3], const uint32_t stride[3], uint32_t width, uint32_t height,
              uint32_t display_height)
{
    static const YuvTables t;
    static int sx[400];
    std::memset(fb, 0, 400 * 240 * sizeof(uint16_t));
    // Fit width x display_height into 400x240.
    int ow = 400, oh = (int)(display_height * 400 / width);
    if (oh > 240)
        oh = 240, ow = (int)(width * 240 / display_height);
    const int x0 = (400 - ow) / 2, y0 = (240 - oh) / 2;
    for (int x = 0; x < ow; x++) {
        sx[x] = x * (int)width / ow;
        if (sx[x] + 1 >= (int)width)
            sx[x] = (int)width - 2;
    }
    for (int y = 0; y < oh; y++) {
        const int sy = y * (int)height / oh;
        const uint8_t *r0 = plane[0] + (size_t)sy * stride[0];
        const uint8_t *r1 = sy + 1 < (int)height ? r0 + stride[0] : r0;
        const uint8_t *u = plane[1] + (size_t)(sy >> 1) * stride[1], *v = plane[2] + (size_t)(sy >> 1) * stride[2];
        uint16_t *out = fb + (size_t)x0 * 240 + (239 - (y0 + y));
        for (int x = 0; x < ow; x++, out += 240) {
            const int i = sx[x], c = i >> 1;
            const int yy = t.y[(r0[i] + r0[i + 1] + r1[i] + r1[i + 1] + 2) >> 2];
            const int cu = u[c], cv = v[c];
            *out = (uint16_t)(t.r[512 + ((yy + t.rv[cv]) >> 8)] | t.g[512 + ((yy + t.gu[cu] + t.gv[cv]) >> 8)] |
                              t.b[512 + ((yy + t.bu[cu]) >> 8)]);
        }
    }
}

// ---- the render thread -----------------------------------------------------------
//
// Converting and scaling a frame for the top screen costs ~6 ms on the
// console's game core. On a New 3DS it runs on another core instead: present()
// copies the frame (indices + palette, ~0.5 MB) and returns, and this thread
// draws it while the game works on its next frame. A frame that arrives while
// the previous one is still being drawn is skipped on screen (never waited for).

struct Staged {
    std::vector<uint8_t> pixels;   // indices, or a cinematic's Y, U, V planes one after the other
    uint32_t width = 0, height = 0;
    uint32_t palette[256];
    bool zoom;
    int cursor_x, cursor_y;
    bool yuv = false;
    uint32_t display_height = 0;
    bool shot = false;   // also save this frame to the Camera album
    game::BottomView bottom;   // the second screen, drawn after the top when bottom_draw
    bool bottom_draw = false;
};
Staged g_staged;
LightEvent g_render_go;
volatile bool g_render_busy = false, g_render_quit = false;

// The frame's copy on the second core (issue #80): with "draw on core 2",
// present() leaves the frame in the game's buffer and the render thread
// copies it first thing. The game draws into the buffer only through its
// renderer, guarded until the copy is done (game::renderer_guard): a call
// into it waits in copy_wait(). The render thread lifts the guard as soon
// as it has copied. "copycheck=1" in settings.txt (Azahar): at the next
// present, the buffer the copy was made from is compared with the copy, and
// a frame the game changed outside its renderer before the copy is logged.
std::atomic<int> g_copy_state{0};   // 0 none, 1 asked for, 3 being copied, 2 done
const uint8_t *g_copy_src = nullptr;
int32_t g_copy_stride = 0;
LightEvent g_copy_event;
bool g_copy_check = false;
uint64_t g_copy_wait_us = 0;        // since the frame trace's last frame
uint32_t g_copy_changed = 0;        // copycheck: the mismatches logged
std::atomic<uint32_t> g_copy_bad{0};   // copycheck: copies that differ from the frame at present
std::vector<uint8_t> g_copy_ref;    // copycheck: the frame as present saw it

void copy_frame_now()
{
    Staged &st = g_staged;
    for (uint32_t y = 0; y < st.height; y++)
        std::memcpy(&st.pixels[(size_t)y * st.width], g_copy_src + (ptrdiff_t)y * g_copy_stride, st.width);
}

void copy_wait()
{
    if (g_copy_state.load(std::memory_order_acquire) == 0)
        return;
    if (g_copy_state.load(std::memory_order_acquire) != 2) {
        const uint64_t t0 = host_us();
        // Not begun (the render thread still waiting for the screen's
        // refresh, await_shown): the game's thread copies it itself, ~1 ms,
        // instead of waiting up to a refresh.
        int asked = 1;
        if (g_copy_state.compare_exchange_strong(asked, 3, std::memory_order_acq_rel)) {
            copy_frame_now();
            game::renderer_unguard();
            g_copy_state.store(2, std::memory_order_release);
        }
        while (g_copy_state.load(std::memory_order_acquire) != 2)
            LightEvent_WaitTimeout(&g_copy_event, 1000000);
        g_copy_wait_us += host_us() - t0;
    }
    if (g_copy_check) {
        // The render thread's copy against the frame as present saw it.
        const uint32_t bad = g_copy_bad.load(std::memory_order_acquire);
        if (bad != g_copy_changed) {
            g_copy_changed = bad;
            const game::GameState gs = game::read_game_state();
            if (bad <= 40)
                status("copy check: the copy differs from the frame at present (%u times; %s%s)", (unsigned)bad,
                       gs.in_game ? "in game" : "menus", gs.panel_open ? ", a panel" : "");
        }
    }
    g_copy_state.store(0, std::memory_order_release);
}

// Screenshots (L+R+Select): the request rides on the next staged frame, and
// the thread that draws it saves it after drawing, so the picture is the one
// on screen. Encoding and writing take a moment, while the screen holds that
// frame (frames arriving meanwhile are skipped, as for any slow draw). The
// result is shown by the game's thread (poll): the console is not thread-safe.
bool g_shot_request = false;
volatile bool g_shot_done = false;
std::string g_shot_msg;

// The request for the frame being staged. It waits while the last result is
// still unread (the drawing thread would overwrite g_shot_msg).
bool take_shot_request()
{
    return !__atomic_load_n(&g_shot_done, __ATOMIC_ACQUIRE) && std::exchange(g_shot_request, false);
}

void save_staged_shot()
{
    Staged &st = g_staged;
    if (!st.shot)
        return;
    st.shot = false;
    const uint64_t t0 = host_us();
    const shot::Source src{st.pixels.data(), st.width, st.height, st.palette, st.yuv, st.display_height};
    // The bottom screen as shown (issue #61): the buffer last drawn by the
    // second screen, or the text console's own. Read here, on the thread
    // that draws it, before its next draw (into the other buffer).
    const uint16_t *bottom = g_bottom_double ? g_bottom_shown
                                             : (const uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    if (gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES)
        bottom = nullptr;
    std::string msg;
    const bool ok = shot::save(src, bottom, msg);
    char ms[24];
    std::snprintf(ms, sizeof ms, ", %u ms", (unsigned)((host_us() - t0) / 1000));
    g_shot_msg = (ok ? "screenshot: " : "screenshot failed: ") + msg + (ok ? ms : "");
    __atomic_store_n(&g_shot_done, true, __ATOMIC_RELEASE);
}

// ---- the top screen's refresh (port_options.h's kPortScreen50) --------------------
//
// The top LCD refreshes at 59.83 Hz, and a swap shows at the next refresh:
// the game's 25 frames a second each stay on it for two or three refreshes,
// 33 or 50 ms, however evenly they come. At 50 Hz each can stay exactly two.
// The rate is the LCD's pixel clock over its line count (PDC0's VTotal, GSP
// register 0x400424, 0x10400424 physical): 413 (the console's own) gives
// 59.83 Hz, 494 gives 50.04 Hz. Only that register changes, and only from
// 413: anything else read there is left alone. The HOME menu, sleep and the
// game's end get 413 back (apt_hook, main), and a return gets 50 Hz again.
constexpr u32 kPdcTopVTotal = 0x400424;
constexpr u32 kVTotal60 = 413, kVTotal50 = 494;
constexpr uint64_t kRefresh60Us = 16713, kRefresh50Us = 19983;
u32 g_vtotal_read = 0;            // VTotal as first read (0: not yet, ~0: the read failed)
std::atomic<bool> g_screen50{false};   // the screen runs at 50 Hz now
bool g_screen50_want = false;     // the option, as present last applied it
LightLock g_screen_lock;          // the game's thread and APT's

// 1: as asked; 0: left as it was (VTotal is not the console's own); -1: GSP refused.
int screen_hz_set(bool fifty)
{
    LightLock_Lock(&g_screen_lock);
    if (!g_vtotal_read) {
        u32 v = 0;
        g_vtotal_read = R_SUCCEEDED(GSPGPU_ReadHWRegs(kPdcTopVTotal, &v, 4)) ? v : ~0u;
    }
    int r = 0;
    if (g_vtotal_read == kVTotal60 && fifty != g_screen50.load(std::memory_order_relaxed)) {
        u32 v = fifty ? kVTotal50 : kVTotal60;
        r = R_SUCCEEDED(GSPGPU_WriteHWRegs(kPdcTopVTotal, &v, 4)) ? 1 : -1;
        if (r > 0)
            g_screen50.store(fifty, std::memory_order_release);
    } else if (g_vtotal_read == kVTotal60) {
        r = 1;
    }
    LightLock_Unlock(&g_screen_lock);
    return r;
}

// What the render thread saw of the screen since the last heartbeat: each
// frame's time on it in refreshes (1, 2, 3, 4 or more), the frames it held
// back a refresh (ready too early at 50 Hz), and the refreshes' own length.
struct Glass {
    uint32_t shown[4] = {};
    uint32_t held = 0;
    uint32_t edge = 0, edge_held = 0;   // ready in the last 2 ms before a refresh, and of those held
    uint32_t phase[4] = {};   // swaps by where in a refresh they fell: 0-5, 5-10, 10-15, 15-20 ms (frame_lock)
    uint64_t span_us = 0;    // the frames' times on the screen, added up
    uint32_t refreshes = 0;  // and in refreshes
};
Glass g_glass;
LightLock g_glass_lock;
uint64_t g_last_shown_us = 0;   // the render thread's: the refresh that showed its last frame
bool g_swapped = false, g_swap_held = false, g_swap_edge = false;   // the render thread's: a swap not yet seen on the screen
int g_swap_phase = -1;   // the render thread's: the swap's quarter of a refresh (frame_lock), -1 none

// The frame lock (issue #86). The game starts a frame when its clock passes
// the next 40 ms; a refresh pair at 50 Hz is 39.97 ms, so the point of a
// refresh where its frames are swapped drifts through the refresh, and a
// frame swapped just before a refresh's end is shown one refresh early or
// late (the 1-and-3 pairs, in bursts every half minute in 3DS-D2-1A). The
// lock keeps the swaps in the middle of a refresh by moving the game's
// clock ahead a little each frame (rt::advance_guest_clock): never back,
// at most 80 us a frame (0.2%). With no push the swaps drift later by
// about 34 us a frame; the most push moves them earlier by about 46.
//
// locktest=1 in settings.txt (Azahar, which keeps 59.83 Hz) measures the
// swaps against a 50 Hz lattice of the host's clock instead of the screen.
std::atomic<bool> g_lock_on{false};
// In game (the frame thread's, each input poll): only there. The menus and
// cinematics keep the host's pace: a cinematic times its video by the
// guest clock against sound the DSP plays, and 0.13% over three minutes
// would put them a quarter second apart.
std::atomic<bool> g_lock_in_game{false};
bool g_lock_test = false;
int32_t g_lock_err_us = 0;    // the render thread's: where the swaps fall from a refresh's middle, averaged
uint64_t g_lock_last_us = 0;  // the render thread's: the last swap
std::atomic<uint32_t> g_lock_pushes{0};   // frames the clock was moved for

void frame_lock(uint64_t now)
{
    const uint64_t ref = g_lock_test ? 0 : g_last_shown_us;
    if (!g_lock_test && (!ref || !g_screen50.load(std::memory_order_acquire)))
        return;
    const int32_t phase = (int32_t)((now - ref) % kRefresh50Us);
    g_swap_phase = phase * 4 / (int32_t)kRefresh50Us;
    if (!g_lock_on.load(std::memory_order_relaxed) || !g_lock_in_game.load(std::memory_order_relaxed))
        return;
    // Late in the refresh (positive) pushes harder. Averaged over about 16
    // frames: one frame's swap moves with what it drew. Only frames 35-45 ms
    // after the last one count: after a late frame the game catches up, and
    // those come at any point of a refresh (Azahar running behind: the lock
    // drifted round the refresh instead of holding). One far off counts as
    // 3 ms off at the most.
    const uint64_t gap = now - g_lock_last_us;
    g_lock_last_us = now;
    if (gap >= 35000 && gap <= 45000) {
        int32_t err = phase - (int32_t)kRefresh50Us / 2;
        err = err < -3000 ? -3000 : err > 3000 ? 3000 : err;
        g_lock_err_us += (err - g_lock_err_us) / 16;
    }
    int32_t push = 40 + g_lock_err_us / 40;
    push = push < 0 ? 0 : push > 80 ? 80 : push;
    if (push) {
        rt::advance_guest_clock((uint32_t)push);
        g_lock_pushes.fetch_add(1, std::memory_order_relaxed);
    }
}

// The top screen, and the bottom one when it was drawn: once double-buffered,
// swapping the bottom without a draw would show the older of its two buffers.
// At 50 Hz, on the render thread, a frame ready less than a refresh after
// the last was shown waits for one more, so that each frame stays two
// refreshes (the game's frames come 35 and 45 ms apart as often as 40: it
// catches up on a late one). The slack is the render thread's wake after
// the refresh, which g_last_shown_us carries: 2 ms of it let a frame ready
// 18-20 ms after the last through, onto the next refresh, so that the last
// stayed one refresh and a 3 came beside it (3DS-D2-19, issue #84).
constexpr uint64_t kHoldSlackUs = 300;
void swap_screens(bool bottom_drawn)
{
    g_swap_held = g_swap_edge = false;
    g_swap_phase = -1;
    if (g_render_thread)
        frame_lock(host_us());
    if (g_render_thread && g_screen50.load(std::memory_order_acquire) && g_last_shown_us) {
        const uint64_t e = host_us() - g_last_shown_us;
        g_swap_edge = e + 2000 >= kRefresh50Us && e < kRefresh50Us;
        if (e + kHoldSlackUs < kRefresh50Us) {
            gspWaitForVBlank();
            g_swap_held = true;
        }
    }
    gfxScreenSwapBuffers(GFX_TOP, true);
    if (bottom_drawn || !g_bottom_double)
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    g_swapped = g_render_thread != nullptr;
}

// The render thread, its frame handed back (g_render_busy cleared, so
// present never skips a frame for this): it waits for the refresh that shows
// the swap, so that its next frame is drawn in the buffer just taken off the
// screen, and counts how long the frame before stayed on it.
void await_shown()
{
    if (!g_swapped)
        return;
    g_swapped = false;
    const uint64_t refresh = g_screen50.load(std::memory_order_acquire) ? kRefresh50Us : kRefresh60Us;
    const bool held = g_swap_held, edge = g_swap_edge;
    const int phase = g_swap_phase;
    gspWaitForVBlank();
    const uint64_t t = host_us();
    if (g_last_shown_us) {
        const uint64_t span = t - g_last_shown_us;
        const uint32_t n = (uint32_t)((span + refresh / 2) / refresh);
        LightLock_Lock(&g_glass_lock);
        g_glass.shown[n < 1 ? 0 : n > 4 ? 3 : n - 1]++;
        g_glass.held += held;
        g_glass.edge += edge;
        g_glass.edge_held += edge && held;
        if (phase >= 0 && phase < 4)
            g_glass.phase[phase]++;
        if (n >= 1 && n <= 4) {
            g_glass.span_us += span;
            g_glass.refreshes += n;
        }
        LightLock_Unlock(&g_glass_lock);
    }
    g_last_shown_us = t;
}

// The second screen's view, when staged to be drawn: true if it was.
bool draw_staged_bottom()
{
    const Staged &st = g_staged;
    if (!st.bottom_draw || st.bottom.kind == game::BottomView::kNone)
        return false;
    // Double-buffered from the second screen's first draw: drawn in the
    // buffer not shown, so the screen never shows one half redrawn. Every
    // draw covers the whole screen.
    // The second buffer is linear memory; the .3dsx's 1 MiB linear heap may
    // not have it (Azahar: a null buffer). Single-buffered then, as before.
    static bool no_double = false;
    if (!g_bottom_double && !no_double) {
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        g_bottom_double = true;
        if (!gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr)) {
            gfxSetDoubleBuffering(GFX_BOTTOM, false);
            g_bottom_double = false;
            no_double = true;
            logline("bottom screen: no linear memory for a second buffer, single-buffered");
        }
    }
    uint16_t *bottom = (uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    if (!bottom)
        return false;
    // A cinematic's view (kVideo) uses neither the frame nor the palette.
    game::draw_bottom(bottom, st.bottom, st.yuv ? nullptr : st.pixels.data(), (int)st.width, (int)st.height, g_pal565,
                      default_font_bin);
    g_bottom_shown = bottom;   // after the swap
    return true;
}

void draw_staged()
{
    const Staged &st = g_staged;
    uint16_t *top = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    if (st.yuv) {
        const uint32_t cw = (st.width + 1) / 2, ch = (st.height + 1) / 2;
        const uint8_t *planes[3] = {st.pixels.data(), st.pixels.data() + (size_t)st.width * st.height,
                                    st.pixels.data() + (size_t)st.width * st.height + (size_t)cw * ch};
        const uint32_t strides[3] = {st.width, cw, cw};
        draw_yuv(top, planes, strides, st.width, st.height, st.display_height);
        const bool bottom_drawn = draw_staged_bottom();
        gfxFlushBuffers();
        swap_screens(bottom_drawn);
        return;
    }
    for (int i = 0; i < 256; i++) {
        const uint32_t p = st.palette[i], r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        g_pal_wide[i] = (r << 20) | (g << 10) | b;
        g_pal565[i] = rgb565(r, g, b);
    }
    rt::Frame f;
    f.pixels = st.pixels.data();
    f.width = st.width;
    f.height = st.height;
    f.stride = (int32_t)st.width;
    f.palette = st.palette;
    if (st.zoom)
        draw_window(top, f, st.cursor_x, st.cursor_y);
    else
        draw_stretched(top, f);
    const bool bottom_drawn = draw_staged_bottom();
    gfxFlushBuffers();
    swap_screens(bottom_drawn);
}

void render_main(void *)
{
    for (;;) {
        LightEvent_Wait(&g_render_go);
        if (g_render_quit)
            break;
        int asked = 1;
        if (g_copy_state.compare_exchange_strong(asked, 3, std::memory_order_acq_rel)) {
            copy_frame_now();
            game::renderer_unguard();
            if (g_copy_check && g_copy_ref.size() == g_staged.pixels.size() &&
                std::memcmp(g_copy_ref.data(), g_staged.pixels.data(), g_copy_ref.size()))
                g_copy_bad.fetch_add(1, std::memory_order_release);
            g_copy_state.store(2, std::memory_order_release);
            LightEvent_Signal(&g_copy_event);
        }
        while (g_copy_state.load(std::memory_order_acquire) == 3)
            svcSleepThread(100000);   // the game's thread copying it (copy_wait): ~1 ms
        const uint64_t t0 = host_us();
        draw_staged();
        g_timing.draw_us += host_us() - t0;
        save_staged_shot();
        __atomic_store_n(&g_render_busy, false, __ATOMIC_RELEASE);
        await_shown();
    }
}

void start_render_thread()
{
    LightEvent_Init(&g_render_go, RESET_ONESHOT);
    LightLock_Init(&g_glass_lock);
    LightLock_Init(&g_screen_lock);
    LightEvent_Init(&g_copy_event, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Core 2 exists on a New 3DS and is the application's to use; core 1 is
    // the system core, usable after the app gives it a CPU time share.
    for (int core : {2, 1}) {
        if (core == 1 && R_FAILED(APT_SetAppCpuTimeLimit(30)))
            continue;
        g_render_thread = threadCreate(render_main, nullptr, 32 * 1024, prio, core, false);
        if (g_render_thread) {
            g_render_core = core;
            break;
        }
    }
    status("frame drawing: %s", g_render_core >= 0 ? (g_render_core == 2 ? "core 2" : "core 1 (30%)")
                                                   : "game thread (no second core)");
}

// The time-based profile sampler (rt::profile_tick): every 2 ms, from core 2
// at a priority above the drawing and the mixer, so its period holds while
// core 0 runs the game. Only during a profiler run's guest code profile.
Thread g_sampler = nullptr;
volatile bool g_sampler_stop = false;

void sampler_main(void *)
{
    while (!g_sampler_stop) {
        svcSleepThread(2000000);
        rt::profile_tick();
    }
}

void start_sampler()
{
    if (g_sampler)
        return;
    g_sampler_stop = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_sampler = threadCreate(sampler_main, nullptr, 4 * 1024, prio - 3, g_render_core >= 0 ? g_render_core : 2, false);
    status("profile sampler: %s", g_sampler ? "every 2 ms" : "not started (instruction-quantum samples)");
}

void stop_sampler()
{
    if (!g_sampler)
        return;
    g_sampler_stop = true;
    threadJoin(g_sampler, U64_MAX);
    threadFree(g_sampler);
    g_sampler = nullptr;
}

// The floor queue's worker (issue #80, game::set_floor_worker): the floor
// tiles a frame queues, drawn on the second core beside the game's thread,
// below the I/O worker and the drawing (3DS-D2-18: at the I/O worker's own
// priority, a finished read waited out the worker's tiles, as threads of
// one priority do not preempt each other). What it has not drawn by the
// floor's end, the game's thread draws itself. Only on core 2 (a New 3DS):
// core 1's time share (30%) could hold a tile the frame thread then waits
// for, and with no worker the queue only adds copies. Switched on and off on the INFO tab's
// OPTIONS page (port_options.h's kPortCore2Draw, saved as "port=").
bool core2_draw() { return (g_ss.port_options() & game::kPortCore2Draw) != 0; }
bool native_dcc() { return (g_ss.port_options() & game::kPortNativeDcc) != 0; }
LightEvent g_floor_event;
Thread g_floor_thread = nullptr;
void (*g_floor_worker)() = nullptr;

void floor_main(void *) { g_floor_worker(); }

bool start_floor_thread(void (*worker)())
{
    if (g_render_core != 2)
        return false;
    g_floor_worker = worker;
    LightEvent_Init(&g_floor_event, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_floor_thread = threadCreate(floor_main, nullptr, 16 * 1024, prio + 1, g_render_core, false);
    return g_floor_thread != nullptr;
}

void join_floor_thread()
{
    if (!g_floor_thread)
        return;
    game::stop_floor_worker();
    threadJoin(g_floor_thread, U64_MAX);
    threadFree(g_floor_thread);
    g_floor_thread = nullptr;
}

// Asynchronous file reads (RunOptions::io_start): the runtime's I/O worker on
// the second core, above the drawing, so a finished SD read reaches the
// waiting guest thread at once. It sleeps on an event between requests.
LightEvent g_io_event;
LightEvent g_io_done_event;   // a read completed: wakes an idle scheduler (RunOptions::io_done)
Thread g_io_thread = nullptr;
void (*g_io_worker)() = nullptr;

void io_main(void *) { g_io_worker(); }

bool start_io_thread(void (*worker)())
{
    g_io_worker = worker;
    LightEvent_Init(&g_io_event, RESET_ONESHOT);
    LightEvent_Init(&g_io_done_event, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_io_thread = threadCreate(io_main, nullptr, 16 * 1024, prio - 1, g_render_core >= 0 ? g_render_core : 2, false);
    return g_io_thread != nullptr;
}

void join_io_thread()
{
    if (!g_io_thread)
        return;
    threadJoin(g_io_thread, U64_MAX);   // the runtime has told it to stop
    threadFree(g_io_thread);
    g_io_thread = nullptr;
}

// The SD card's writer (ctr_writer.h, issue #71): on the drawing core,
// below the drawing and the reads, so it writes in their gaps. Without a
// second core the streams write at once, from the thread that writes.
void start_writer_thread()
{
    if (g_render_core < 0)
        return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    status("SD card writes: %s", app::writer_start(g_render_core, prio + 1) ? "their own thread" : "at once (no thread)");
}

void stop_render_thread()
{
    if (!g_render_thread)
        return;
    g_render_quit = true;
    LightEvent_Signal(&g_render_go);
    threadJoin(g_render_thread, U64_MAX);
    threadFree(g_render_thread);
    g_render_thread = nullptr;
}

// The first frame, the game's or a video's: the bottom screen is the second
// screen's from now on.
void second_screen_active()
{
    if (g_ss_active)
        return;
    g_ss_active = true;
    status("bottom screen: %s tab (tap the tabs to change)",
           g_ss.tab() == game::BottomTab::Map && g_ss.map_help() ? "help" : game::bottom_tab_name(g_ss.tab()));
}

void present(const rt::Frame &f)
{
    if (g_guest_cpu) {   // issue #89: the tiles the next frames will load, read ahead (the OPTIONS row)
        game::set_tile_prefetch((g_ss.port_options() & game::kPortTilePrefetch) != 0);
        game::tiles_ahead_scan(*g_guest_cpu);
    }   // issue #89: the tiles the next frames will load
    copy_wait();   // the last frame's copy, should the game not have drawn since
    const uint64_t t0 = host_us();
    if (g_timing.last_present_us) {
        const uint64_t gap = t0 - g_timing.last_present_us;
        if (gap > g_timing.worst_gap_us)
            g_timing.worst_gap_us = gap;
        const uint32_t ms = (uint32_t)(gap / 1000);
        g_hitch.frames[ms <= 50 ? 0 : ms <= 100 ? 1 : ms <= 250 ? 2 : 3]++;
        if (ms > g_hitch.longest_ms)
            g_hitch.longest_ms = ms;
    }
    g_timing.last_present_us = t0;
    if (!f.yuv[0]) {   // a cinematic's size is not the game's: the cursor keeps the game's frame
        g_controls.set_frame((int)f.width, (int)f.height);
        g_ss.set_frame((int)f.width, (int)f.height);
        game::automap_frame(f.pixels, f.width, f.height, f.stride);
        game::set_automap_capture(g_ss.wants_automap(), g_ss.map_ms(), g_ss.map_scale());
    }
    const uint64_t t_automap = host_us();
    uint64_t t_copy = t_automap, t_bottom = t_automap;    g_timing.frames++;
    g_timing.total_frames++;
    // The checklist's floor row (issue #80): at a frame's end, for the next floor.
    if (game::set_floor_queue_on(core2_draw()) && game::frame_trace_on())
        game::frame_trace_note(core2_draw() ? "floor queue: on" : "floor queue: off");
    const bool fifty = (g_ss.port_options() & game::kPortScreen50) != 0;
    if (fifty != g_screen50_want) {   // the OPTIONS page's screen row
        g_screen50_want = fifty;
        const int r = screen_hz_set(fifty);
        char line[160];
        std::snprintf(line, sizeof line, "screen: %s (the top LCD's VTotal read %u)%s",
                      g_screen50.load() ? "50 Hz" : "59.83 Hz", (unsigned)g_vtotal_read,
                      r == 0 ? ", not the console's own 413: left alone" : r < 0 ? ", GSP refused the write" : "");
        logline(line);
        if (game::frame_trace_on())
            game::frame_trace_note(g_screen50.load() ? "screen: 50 Hz" : "screen: 60 Hz");
    }
    {   // the OPTIONS page's frame lock (issue #86)
        const bool lock = (g_ss.port_options() & game::kPortFrameLock) != 0;
        if (lock != g_lock_on.load(std::memory_order_relaxed)) {
            g_lock_on.store(lock, std::memory_order_relaxed);
            logline(lock ? (g_lock_test ? "frame lock: on, against the host clock's 50 Hz (locktest)"
                                        : "frame lock: on (at 50 Hz, with core 2 draw)")
                         : "frame lock: off");
            if (game::frame_trace_on())
                game::frame_trace_note(lock ? "frame lock: on" : "frame lock: off");
        }
    }
    {   // the OPTIONS page's native sprite decoder (issue #83)
        const game::DccMode want = native_dcc() ? game::DccMode::Offload : game::DccMode::Off;
        if (game::dcc_mode() != want) {
            game::set_dcc_mode(want);
            if (game::frame_trace_on())
                game::frame_trace_note(native_dcc() ? "native dcc: on" : "native dcc: off");
        }
    }
    uint32_t floor_us, flush_us;
    game::floor_queue_take(floor_us, flush_us);
    if (game::frame_trace_on()) {
        rt::note_present();   // the stall column: how long since this present
        game::FrameSample s;
        s.host_us = t0;
        s.tick_ms = rt::tick_ms();
        rt::frame_thread_us(s.thread_us);
        rt::frame_thread_ready_by(s.ready_by);
        s.wait_us = rt::frame_thread_wait_top(s.wait_site, s.wait_owner, s.wait_from);
        s.stalls = rt::take_stall(s.stall, 8);
        s.floor_us = floor_us;
        s.flush_us = flush_us;
        s.copy_wait_us = (uint32_t)g_copy_wait_us;
        g_copy_wait_us = 0;
        game::sprite_decode_totals(s.decodes, s.decode_us);
        s.skipped = g_frames_skipped;
        game::frame_trace_frame(s);
    }

    const uint64_t t_trace = host_us();   // the frame trace's sample: not present's own work
    t_copy = t_bottom = t_trace;
    if (__atomic_load_n(&g_render_busy, __ATOMIC_ACQUIRE)) {
        g_frames_skipped++;   // the screen is still showing/drawing the last one
    } else if (f.yuv[0]) {
        Staged &st = g_staged;
        const uint32_t cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
        st.yuv = true;
        st.width = f.width;
        st.height = f.height;
        st.display_height = f.display_height;
        st.shot = take_shot_request();
        // The bottom screen: the video's panel (issue #59), also over the
        // startup console for the logos before the game's first frame.
        second_screen_active();
        g_ss.set_video(f.video_name, f.video_ms, f.video_length_ms);
        st.bottom_draw = g_ss.stage(st.bottom, rt::now_ms());
        st.pixels.resize((size_t)f.width * f.height + 2 * (size_t)cw * ch);
        uint8_t *o = st.pixels.data();
        for (int p = 0; p < 3; p++) {
            const uint32_t w = p ? cw : f.width, rows = p ? ch : f.height;
            for (uint32_t y = 0; y < rows; y++, o += w)
                std::memcpy(o, f.yuv[p] + (size_t)y * f.yuv_stride[p], w);
        }
        if (g_render_thread) {
            __atomic_store_n(&g_render_busy, true, __ATOMIC_RELEASE);
            LightEvent_Signal(&g_render_go);
        } else {
            draw_staged();
            g_timing.draw_us += host_us() - t0;
            save_staged_shot();
        }
    } else {
        Staged &st = g_staged;
        st.yuv = false;
        st.width = f.width;
        st.height = f.height;
        st.pixels.resize((size_t)f.width * f.height);
        g_copy_src = f.pixels;
        g_copy_stride = f.stride;
        if (g_copy_check) {   // the frame as present sees it, for the render thread's copy to match
            g_copy_ref.resize(st.pixels.size());
            for (uint32_t y = 0; y < f.height; y++)
                std::memcpy(&g_copy_ref[(size_t)y * f.width], f.pixels + (ptrdiff_t)y * f.stride, f.width);
        }
        if (g_render_thread && g_render_core == 2 && core2_draw() && game::renderer_guard(copy_wait))
            g_copy_state.store(1, std::memory_order_release);   // the render thread copies it
        else
            copy_frame_now();
        std::memcpy(st.palette, f.palette, sizeof st.palette);
        t_copy = host_us();
        st.zoom = g_zoom;
        st.cursor_x = g_controls.cursor_x();
        st.cursor_y = g_controls.cursor_y();
        st.shot = take_shot_request();
        second_screen_active();
        st.bottom_draw = g_ss.stage(st.bottom, rt::now_ms());
        t_bottom = host_us();
        if (st.bottom.kind == game::BottomView::kKeyboard)
            g_osk.clear_dirty();
        if (g_render_thread) {
            __atomic_store_n(&g_render_busy, true, __ATOMIC_RELEASE);
            LightEvent_Signal(&g_render_go);
        } else {
            draw_staged();
            g_timing.draw_us += host_us() - t0;
            save_staged_shot();
        }
    }
    update_stats();
    if (game::draw_phases_on() && !f.yuv[0]) {
        const uint64_t t_end = host_us();
        game::draw_phases_present(t_automap - t0, t_copy - t_trace, t_bottom > t_copy ? t_bottom - t_copy : 0,
                                  t_end - t0 - (t_trace - t_automap));
    }
}

// ---- on-screen keyboard (drawn by the second screen) ------------------------------

bool g_wait_release = false;         // after the keyboard closes: ignore buttons until all are up
uint32_t g_last_focus = 0;           // the last focused edit box seen (auto-open on a new one)

// ---- settings: the bottom screen's tab and map size -------------------------------------

// The other guest threads' slice (rt::RunOptions::background_quantum), in
// instructions; the frame thread keeps the scheduler's 300000. settings.txt's
// "bgquantum=" overrides it for a measurement (0: one slice for all).
constexpr uint32_t kBackgroundQuantum = 40000;
uint32_t g_background_quantum = kBackgroundQuantum;
// ...and its time (rt::RunOptions::background_slice_us): "bgslice=" (0: none).
constexpr uint32_t kBackgroundSliceUs = 2000;
uint32_t g_background_slice_us = kBackgroundSliceUs;
// Storm's direct reads (game::install_storm_direct_reads): "stormdirect=0"
// leaves them queued, as the game does.
bool g_storm_direct = true;
// The read thread's rate limit (game::set_storm_read_rate): "stormrate=" in
// KiB/s, 0: the game's 256 KiB/s.
uint32_t g_storm_rate_kib = 0;
// The frame trace's stall column: a frame this long or longer is sampled
// (rt::stall_probe). "stallms=" (a measurement's).
uint32_t g_stall_ms = 150;
// The dynarec's translation knobs (third_party/winx86 dyn86.c; read when a
// block is translated, so set before the game runs): return-address
// prediction (callret=), no flag archiving at a block that ends in a CALL
// (nopend=1, no x86 callee reads its caller's flags), the budget check at a
// block's tail (budgettail=), how far a block may bridge forward (forward=).
// -1: the backend's default. A measurement's, from settings.txt.
int g_dyn_callret = -1, g_dyn_nopend = -1, g_dyn_budgettail = -1, g_dyn_forward = -1;

void load_settings()
{
    FILE *f = std::fopen(kSettings, "r");
    if (!f) {
        g_ss.set_map_help(true);   // the first start: the controls (HELP), until the corner button is tapped
        g_ss.take_settings_changed();
        return;
    }
    char line[64], value[32];
    while (std::fgets(line, sizeof line, f)) {
        game::BottomTab tab;
        if (std::sscanf(line, "bottom=%31s", value) == 1 && game::bottom_tab_from_name(value, tab))
            g_ss.set_tab(tab);
        else if (std::sscanf(line, "mapscale=%31s", value) == 1)
            g_ss.set_map_scale(std::atoi(value));
        else if (std::sscanf(line, "mapms=%31s", value) == 1)
            g_ss.set_map_ms((uint32_t)std::atoi(value));
        else if (std::sscanf(line, "maphelp=%31s", value) == 1)
            g_ss.set_map_help(std::atoi(value) != 0);
        else if (std::strncmp(line, "profile=", 8) == 0)   // the profiler run's checklist ("" = nothing ticked)
            g_ss.set_profile_choice(game::profile_bits(line + 8));
        else if (std::strncmp(line, "tracewrites=", 12) == 0)   // ...and how its traces are written (issue #66)
            g_ss.set_trace_writes(game::trace_writes_from_name(line + 12));
        else if (std::strncmp(line, "bgquantum=", 10) == 0)   // a measurement's, not written back
            g_background_quantum = (uint32_t)std::strtoul(line + 10, nullptr, 10);
        else if (std::strncmp(line, "bgslice=", 8) == 0)
            g_background_slice_us = (uint32_t)std::strtoul(line + 8, nullptr, 10);
        else if (std::strncmp(line, "callret=", 8) == 0)
            g_dyn_callret = std::atoi(line + 8);
        else if (std::strncmp(line, "nopend=", 7) == 0)
            g_dyn_nopend = std::atoi(line + 7);
        else if (std::strncmp(line, "budgettail=", 11) == 0)
            g_dyn_budgettail = std::atoi(line + 11);
        else if (std::strncmp(line, "forward=", 8) == 0)
            g_dyn_forward = std::atoi(line + 8);
        else if (std::strncmp(line, "stallms=", 8) == 0)
            g_stall_ms = (uint32_t)std::strtoul(line + 8, nullptr, 10);
        else if (std::strncmp(line, "stormrate=", 10) == 0)
            g_storm_rate_kib = (uint32_t)std::strtoul(line + 10, nullptr, 10);
        else if (std::strncmp(line, "stormdirect=", 12) == 0)
            g_storm_direct = std::atoi(line + 12) != 0;
        else if (std::strncmp(line, "port=", 5) == 0)   // the OPTIONS page's ticks (port_options.h)
            g_ss.set_port_options(game::port_bits(line + 5));
        else if (std::strncmp(line, "floorqueue=", 11) == 0)   // its first form (issue #80)
            g_ss.set_port_options(std::atoi(line + 11) ? g_ss.port_options() | game::kPortCore2Draw
                                                       : g_ss.port_options() & ~game::kPortCore2Draw);
        else if (std::strncmp(line, "copycheck=", 10) == 0)   // a measurement's (Azahar), issue #80
            g_copy_check = std::atoi(line + 10) != 0;
        else if (std::strncmp(line, "locktest=", 9) == 0)   // the frame lock against the host clock (Azahar), issue #86
            g_lock_test = std::atoi(line + 9) != 0;
        else if (std::strncmp(line, "drawphases=", 11) == 0)   // the draw natives timed (issue #80)
            game::draw_phases_set(std::atoi(line + 11) != 0);

    }
    std::fclose(f);
    g_ss.take_settings_changed();
}

void save_settings()
{
    if (FILE *f = std::fopen(kSettings, "w")) {
        std::fprintf(f, "bottom=%s\nmapscale=%d\nmapms=%u\nmaphelp=%d\nprofile=%s\ntracewrites=%s\nport=%s\n",
                     game::bottom_tab_name(g_ss.tab()), g_ss.map_scale(), (unsigned)g_ss.map_ms(),
                     g_ss.map_help() ? 1 : 0, game::profile_names(g_ss.profile_choice()).c_str(),
                     game::trace_writes_info(g_ss.trace_writes()).name, game::port_names(g_ss.port_options()).c_str());
        std::fclose(f);
    }
}

// HOME button and sleep (issue #6): libctru acts on them inside aptMainLoop,
// on this thread. RunOptions::service calls this every 50 ms even while the
// game pumps no messages (loading a level), not only from the pump (poll);
// before, a press during a load waited for the load to end. False: the HOME
// menu's "close", or the system shutting down.
u32 g_apt_thread = 0;   // the thread in aptMainLoop (the game's)
bool g_storage_closed = false;
void open_storage();
void trace_writes_service();

// A HOME round trip (issue #27): the press seen here, the suspend hook and
// its parts, the return. Host clock, us; logged as one "home:" line when
// the storage is open again.
struct HomeTrip {
    uint64_t seen = 0, panel = 0, hook = 0, hook_exit = 0, restore = 0;
    uint64_t render_wait = 0, storage = 0, reopen = 0;
    bool panel_up = false;   // the "Suspending" panel is on the bottom screen: drawn once a press
} g_home;

void wait_render_idle()
{
    while (__atomic_load_n(&g_render_busy, __ATOMIC_ACQUIRE))
        svcSleepThread(1000000);
}

// The HOME button was pressed: the jump to the HOME menu takes ~5 s before
// the screens change, with the game's last frame up. The bottom screen says
// why, until the HOME menu has the screens. Drawn into the bottom's other
// buffer, so the screen as it was is one swap away on the way back. Only
// once the second screen is double-buffered (not over the startup console,
// which nothing would redraw). The top screen keeps the game's frame.
void show_suspending()
{
    g_home.panel_up = true;
    if (!g_bottom_double)
        return;
    wait_render_idle();   // no new frame starts meanwhile: only this thread (present) starts them
    gspWaitForVBlank1();  // the last frame's swap has taken: one swap per screen per frame
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    game::draw_suspending(fb, default_font_bin);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank1();  // on the glass before libctru takes the screens
    g_home.panel = host_us();
}

// Back from the HOME menu (or the jump did not happen): the bottom screen
// as it was, in the buffer behind the panel, and the second screen redraws
// it with the next frame anyway (apt_hook). At once: during a load or a
// cinematic no frame draws the bottom screen.
void hide_suspending()
{
    g_home.panel_up = false;
    if (!g_home.panel)
        return;
    wait_render_idle();
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

// For example "home: seen +0 ms, panel +21, hook +40, render wait 0,
// storage 120, restore after 8.2 s, reopen 35 ms". "seen -": the suspend
// came without the press seen here first; "panel -": none drawn.
void log_home_trip()
{
    const HomeTrip &h = g_home;
    if (!h.hook)
        return;   // no suspend: nothing to time
    const uint64_t t0 = h.seen ? h.seen : h.hook;
    char panel[16] = "-";
    if (h.panel)
        std::snprintf(panel, sizeof panel, "+%u", (unsigned)((h.panel - t0) / 1000));
    char line[160];
    std::snprintf(line, sizeof line,
                  "home: seen %s ms, panel %s, hook +%u, render wait %u, storage %u, restore after %.1f s, reopen %u ms",
                  h.seen ? "+0" : "-", panel, (unsigned)((h.hook - t0) / 1000), (unsigned)(h.render_wait / 1000),
                  (unsigned)(h.storage / 1000), h.restore ? (double)(h.restore - h.hook_exit) / 1e6 : 0.0,
                  (unsigned)(h.reopen / 1000));
    logline(line);
}

bool apt_service()
{
    svcGetThreadId(&g_apt_thread, CUR_THREAD_HANDLE);
    trace_writes_service();
    // libctru takes the system's signals (the HOME press among them) on its
    // APT thread, priority 0x31 on core 0, below this thread (0x30), which
    // runs the game there and seldom blocks: a press waited seconds for it
    // before aptShouldJumpToHome() said so (issue #27). Block for a moment so
    // it runs; with no signal pending it stays asleep.
    svcSleepThread(1000);
    if (!g_home.panel_up && aptShouldJumpToHome()) {
        g_home = HomeTrip{};
        g_home.seen = host_us();
        show_suspending();
    }
    if (aptMainLoop()) {
        if (g_home.panel_up || g_home.hook) {
            hide_suspending();
            log_home_trip();
            g_home = HomeTrip{};
        }
        return true;
    }
    if (g_storage_closed)
        open_storage();   // closed from the HOME menu: the game may still write on its way out
    rt::request_exit(0);
    return false;
}

// The app's storage while the HOME menu has the system (issue #6: the HOME
// menu stopped itself when it could not open its own extdata while 3ds-d2
// ran; why is not known, so the app keeps nothing open meanwhile): the
// game's files, the log, the RomFS and the SD card's archive are closed on
// the way out and opened again on the way back.
bool g_romfs = false;          // the CIA's RomFS is mounted
std::string g_log_path;

// An existing file opened to write at its end: without append mode, whose
// every write seeks to the end and asks the FS for the file's size. After a
// HOME suspend game.log reopened with "a" cost 55-90 ms a write instead of
// 10-13 (3DS-D2-14). A file that is gone is created.
FILE *open_at_end(const char *path, bool binary)
{
    FILE *f = std::fopen(path, binary ? "r+b" : "r+");
    if (f && std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        f = nullptr;
    }
    return f ? f : std::fopen(path, binary ? "wb" : "w");
}

FILE *open_log(bool again)
{
    FILE *f = again ? open_at_end(g_log_path.c_str(), false) : std::fopen(g_log_path.c_str(), "w");
    if (!f)
        return nullptr;
    if (FILE *w = app::writer_wrap(f, g_log_ring, sizeof g_log_ring, "game.log"))
        return w;
    std::setvbuf(f, nullptr, _IOLBF, 0);   // no stream for the writer: a line at a time, as it comes
    return f;
}

void close_storage()
{
    // What is queued goes out now, and nothing more while the archive is
    // closed: the traces keep queueing, game.log is closed below.
    app::writer_pause(true);
    rt::suspend_files();
    if (FILE *f = g_log) {
        g_log = nullptr;
        std::fclose(f);
    }
    if (g_romfs)
        romfsExit();
    archiveUnmount("sdmc");
    g_storage_closed = true;
}

void open_storage()
{
    g_storage_closed = false;
    archiveMountSdmc();
    if (g_romfs)
        g_romfs = R_SUCCEEDED(romfsInit());
    if (!g_log_path.empty())
        g_log = open_log(true);
    rt::resume_files();
    app::writer_pause(false);
}

// Leaving for the HOME menu or sleep: libctru then hands the screens over
// (capture, GPU right). A frame the drawing thread is still presenting would
// land in the middle of that; wait for it. No new one starts meanwhile: only
// this thread (present) starts them, and it is the one in aptMainLoop.
// For the HOME menu, then, the storage is closed until the return. Only on
// the game's thread inside aptMainLoop, where no guest code runs (a sleep
// may be announced on libctru's own APT thread while the game runs on).
aptHookCookie g_apt_cookie;
void apt_hook(APT_HookType type, void *)
{
    u32 thread = 0;
    svcGetThreadId(&thread, CUR_THREAD_HANDLE);
    const bool game_thread = thread == g_apt_thread;
    if ((type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) && g_screen50_want)
        screen_hz_set(true);   // back from the HOME menu or sleep: 50 Hz again
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONEXIT)
        screen_hz_set(false);  // the console's own rate for the HOME menu, sleep and the end
    if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP) {
        g_ss.invalidate();   // the HOME menu or sleep drew over the bottom screen
        g_timing.last_present_us = 0;   // the time away is not a frame (worst gap, hitches)
    }
    if (type == APTHOOK_ONRESTORE && game_thread) {
        g_home.restore = host_us();
        if (g_storage_closed) {
            open_storage();
            g_home.reopen = host_us() - g_home.restore;
        }
    }
    if (type != APTHOOK_ONSUSPEND && type != APTHOOK_ONSLEEP)
        return;
    const bool home = type == APTHOOK_ONSUSPEND && game_thread;
    if (home)
        g_home.hook = host_us();
    wait_render_idle();
    if (home) {
        const uint64_t t = host_us();
        g_home.render_wait = t - g_home.hook;
        if (!g_storage_closed)
            close_storage();
        g_home.hook_exit = host_us();
        g_home.storage = g_home.hook_exit - t;
    }
}

void poll()
{
    if (!apt_service())
        return;
    hidScanInput();
    const u32 k = hidKeysHeld();
    game::Pad pad;
    static const struct { u32 key; uint32_t bit; } map[] = {
        {KEY_A, game::kA}, {KEY_B, game::kB}, {KEY_X, game::kX}, {KEY_Y, game::kY},
        {KEY_L, game::kL}, {KEY_R, game::kR}, {KEY_ZL, game::kZL}, {KEY_ZR, game::kZR},
        {KEY_START, game::kStart}, {KEY_SELECT, game::kSelect}, {KEY_DUP, game::kUp},
        {KEY_DDOWN, game::kDown}, {KEY_DLEFT, game::kLeft}, {KEY_DRIGHT, game::kRight},
    };
    for (const auto &m : map)
        if (k & m.key)
            pad.held |= m.bit;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    pad.circle_x = cp.dx;
    pad.circle_y = cp.dy;
    pad.cstick_x = cs.dx;
    pad.cstick_y = cs.dy;
    if (k & KEY_TOUCH) {
        touchPosition t;
        hidTouchRead(&t);
        pad.touching = true;
        pad.touch_x = t.px;
        pad.touch_y = t.py;
    }
    const game::GameState gs = game::read_game_state();
    // Autoplay drives the pad until the player touches anything.
    if (g_autoplay_on) {
        if (pad.held || pad.touching || std::abs(pad.circle_x) > 40 || std::abs(pad.circle_y) > 40 ||
            std::abs(pad.cstick_x) > 40 || std::abs(pad.cstick_y) > 40) {
            g_autoplay_on = false;
            rt::logf("autoplay stopped: physical input");
        } else if (g_autoplay.active()) {
            bool shot = false;
            pad = g_autoplay.step(rt::now_ms(), g_controls, shot, gs);
            g_shot_request |= shot;
            // A profiler run from the script: started or stopped at the next
            // heartbeat, as by its button.
            std::string prof;
            if (g_autoplay.take_profile(prof)) {
                if (prof != "off")
                    g_ss.set_profile_choice(game::profile_bits(prof.c_str()));
                g_ss.set_profiling(prof != "off");
                rt::logf("autoplay: profile %s", prof.c_str());
            }
        }
    }
    // A newly focused text field (the hero name box, a game name) opens the keyboard.
    if (gs.edit_focus && gs.edit_focus != g_last_focus && !g_osk.is_open())
        g_osk.open();
    g_last_focus = gs.edit_focus;

    static uint32_t prev_held = 0;
    static bool prev_touch = false;
    const uint32_t pressed = pad.held & ~prev_held;
    prev_held = pad.held;
    const bool touch_down = pad.touching && !prev_touch;
    prev_touch = pad.touching;
    g_osk.tick(rt::now_ms());
    if (g_osk.is_open()) {
        // The keyboard has the touch screen and its buttons; the game gets
        // an idle pad, which also releases anything that was held.
        g_osk.buttons(pressed);
        if (touch_down)
            g_osk.tap(pad.touch_x, pad.touch_y - game::SecondScreen::kOskY);
        if (!g_osk.is_open())
            g_wait_release = true;   // the button that closed it must not reach the game
        pad = game::Pad();
    } else if (g_wait_release) {
        if (pad.held || pad.touching)
            pad = game::Pad();
        else
            g_wait_release = false;
    }
    g_ss.update(pad, gs, g_controls.cursor_x(), g_controls.cursor_y(), rt::now_ms());
    if (g_ss.take_settings_changed())
        save_settings();
    g_controls.set_context(gs.in_game, gs.panel_open, gs.ui, gs.in_game && game::holding_item());
    g_lock_in_game.store(gs.in_game, std::memory_order_relaxed);
    g_controls.update(pad, rt::now_ms());
    if (g_controls.take_zoom_toggle())
        g_zoom = !g_zoom;
    g_ss.request(g_controls.take_bottom_request());   // Select + D-pad
    if (g_controls.take_keyboard_request())   // R+Select
        g_osk.open();
    if (g_controls.take_screenshot_request())   // L+R+Select
        g_shot_request = true;
    if (__atomic_load_n(&g_shot_done, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_shot_done, false, __ATOMIC_RELAXED);
        status("%s", g_shot_msg.c_str());
    }
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = !out.empty() && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// The SD card's cost model, for sizing the game-file read cache
// (rt_shims_files.cpp): reads of each size at scattered offsets of an MPQ,
// as the game reads them. Time per read = per-call overhead + size /
// bandwidth; two sizes give both. A profiler run's option: about a second
// of the game stopped while it reads.
void sd_benchmark(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    struct stat st;
    if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size < (1 << 20)) {
        status("sd bench: cannot open %s", path.c_str());
        if (fd >= 0)
            ::close(fd);
        return;
    }
    std::vector<uint8_t> buf(64 * 1024);
    uint32_t seed = 0x2545F491;
    for (uint32_t size : {512u, 2048u, 4096u, 8192u, 16384u, 32768u, 65536u}) {
        constexpr int kReads = 24;
        uint64_t ticks = 0;
        for (int i = 0; i < kReads; i++) {
            seed = seed * 1664525u + 1013904223u;
            const off_t at = (off_t)((seed % (uint32_t)(st.st_size - 65536)) & ~511u);
            const uint64_t t0 = svcGetSystemTick();
            ::lseek(fd, at, SEEK_SET);
            if (::read(fd, buf.data(), size) != (ssize_t)size)
                status("sd bench: short read at %ld", (long)at);
            ticks += svcGetSystemTick() - t0;
        }
        const double ms = ticks / (SYSCLOCK_ARM11 / 1000.0) / kReads;
        status("sd bench: %5u B: %6.3f ms/read, %5.0f KiB/s", (unsigned)size, ms, size / 1024.0 / (ms / 1000.0));
    }
    ::close(fd);
}

// game.log's last line: the session ended in order (report.h). A log
// without it is a crash, offered as a report at the next start.
void close_log()
{
    logline(report::closed_line());
    app::writer_stop();   // what is queued, written; the streams write at once from here
    if (g_log)
        std::fclose(g_log);
    g_log = nullptr;
}

// The game files' flavour, for reports: the SD card's, or the CIA's own
// with the cinematics (full) or empty stand-ins for them (slim).
const char *game_files(const std::string &dir)
{
    if (dir != kRomfsGameDir)
        return "sdcard";
    struct stat st;
    return stat((dir + "/d2video.mpq").c_str(), &st) == 0 && st.st_size > (1 << 20) ? "romfs-full" : "romfs-slim";
}

report::Host report_host(const std::string &game_dir)
{
#ifdef D2_CIA
    const char *app = "cia";
#else
    const char *app = "3dsx";
#endif
    return {&g_log_con, [](const char *s) { status("%s", s); }, console_again, app, game_files(game_dir)};
}

void wait_for_start()
{
    std::printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();   // the console's text, out of the CPU cache
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
}

// The file-read trace's file, and its buffer: the game reads in small
// pieces, and the SD card wants few large writes. A second run in the same
// session goes on in the same file.
FILE *g_files_trace = nullptr;
char *g_files_trace_buf = nullptr;
bool g_files_traced = false;

bool file_trace_start()
{
    constexpr size_t kBuffer = 32 * 1024;
    g_files_trace_buf = (char *)std::malloc(kBuffer);
    g_files_trace = g_files_trace_buf ? (g_files_traced ? open_at_end(kFileTracePath, false) : std::fopen(kFileTracePath, "w")) : nullptr;
    if (!g_files_trace) {
        std::free(g_files_trace_buf);
        g_files_trace_buf = nullptr;
        return false;
    }
    // The buffer is the writer's ring (ctr_writer.h), or stdio's without it.
    if (FILE *w = app::writer_wrap(g_files_trace, g_files_trace_buf, kBuffer, "files.trc"))
        g_files_trace = w;
    else
        std::setvbuf(g_files_trace, g_files_trace_buf, _IOFBF, kBuffer);
    g_files_traced = true;
    rt::set_file_trace(g_files_trace);
    return true;
}

void file_trace_stop()
{
    if (!g_files_trace)
        return;
    rt::set_file_trace(nullptr);
    std::fclose(g_files_trace);
    std::free(g_files_trace_buf);
    g_files_trace = nullptr;
    g_files_trace_buf = nullptr;
}

// The frame trace (game/frame_trace.h): a line per frame, from present().
// ~25 lines of ~50 bytes a second: 1.2 KiB/s through the writer.
FILE *g_frame_trace = nullptr;
char *g_frame_trace_buf = nullptr;
bool g_frames_traced = false;   // a second run this session appends

// The frame trace's stall column (rt::stall_probe): a thread on the second
// core looks every 20 ms whether the frame thread has gone 150 ms without a
// present, and if so samples where it is; nothing on the game's core.
Thread g_stall_thread = nullptr;
volatile bool g_stall_stop = false;

void stall_main(void *)
{
    while (!g_stall_stop) {
        svcSleepThread(20000000);
        rt::stall_probe((uint64_t)g_stall_ms * 1000);
    }
}

void start_stall_watch()
{
    if (g_stall_thread)
        return;
    g_stall_stop = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_stall_thread = threadCreate(stall_main, nullptr, 4 * 1024, prio - 3, g_render_core >= 0 ? g_render_core : 2, false);
}

void stop_stall_watch()
{
    if (!g_stall_thread)
        return;
    g_stall_stop = true;
    threadJoin(g_stall_thread, U64_MAX);
    threadFree(g_stall_thread);
    g_stall_thread = nullptr;
}

bool frame_trace_file_start()
{
    constexpr size_t kBuffer = 16 * 1024;
    g_frame_trace_buf = (char *)std::malloc(kBuffer);
    g_frame_trace = g_frame_trace_buf ? (g_frames_traced ? open_at_end(kFrameTracePath, false)
                                                         : std::fopen(kFrameTracePath, "w"))
                                      : nullptr;
    if (!g_frame_trace) {
        std::free(g_frame_trace_buf);
        g_frame_trace_buf = nullptr;
        return false;
    }
    if (FILE *w = app::writer_wrap(g_frame_trace, g_frame_trace_buf, kBuffer, "frames.trc"))
        g_frame_trace = w;
    else
        std::setvbuf(g_frame_trace, g_frame_trace_buf, _IOFBF, kBuffer);
    g_frames_traced = true;
    start_stall_watch();   // the trace's stall column
    game::frame_trace_start(g_frame_trace);
    game::frame_trace_note(core2_draw() ? "floor queue: on" : "floor queue: off");
    game::note_game_state_again();
    return true;
}

void frame_trace_file_stop()
{
    if (!g_frame_trace)
        return;
    game::frame_trace_stop();
    stop_stall_watch();
    std::fclose(g_frame_trace);
    std::free(g_frame_trace_buf);
    g_frame_trace = nullptr;
    g_frame_trace_buf = nullptr;
}

// How game.log and the traces reach the SD card (issue #66: a crash in
// game lost the last minutes of both), as the checklist's last row says:
// the writer's mode (ctr_writer.h), and the traces' own records each at
// once into it for "at once". From the service call (every 50 ms, loads
// included) on the game's thread. A change applies at once and is logged as
//   profiler run: trace writes <buffered|second|now>
// (the first only if not buffered; the line keeps its old name, which the
// reports read).
game::TraceWrites g_trace_writes = game::TraceWrites::Buffered;

void trace_writes_service()
{
    const game::TraceWrites m = g_ss.trace_writes();
    if (m != g_trace_writes) {
        g_trace_writes = m;
        const bool each = m == game::TraceWrites::AtOnce;
        rt::set_file_trace_each(each);
        game::sprite_trace_each(each);
        app::writer_set_mode(each                                      ? app::WriterMode::AtOnce
                             : m == game::TraceWrites::EverySecond ? app::WriterMode::EverySecond
                                                                   : app::WriterMode::Batched);
        char line[64];
        std::snprintf(line, sizeof line, "%strace writes %s", game::kProfilerRunLine, game::trace_writes_info(m).name);
        logline(line);
    }
    // Every second: what the traces' own buffers hold, into the writer's
    // rings (it then writes them out by its own second).
    if (m != game::TraceWrites::EverySecond || g_storage_closed)   // the HOME menu has the SD card
        return;
    static uint64_t written_us = 0;
    const uint64_t now = host_us();
    if (now - written_us < 1000000)
        return;
    written_us = now;
    if (g_files_trace)
        std::fflush(g_files_trace);
    game::sprite_trace_flush();
}

// The profiler run's options (profiler.h) to `want`: those no longer
// wanted stopped, the new ones started. An option that does not start is
// left out of the run, and said so.
void profiler_switch(uint32_t want)
{
    const uint32_t stop = g_prof_on & ~want, start = want & ~g_prof_on;
    if (stop & game::kProfSprites)
        game::sprite_trace_stop();
    if (stop & game::kProfGuest) {
        rt::set_profiling(false);
        stop_sampler();
    }
    if (stop & game::kProfFiles)
        file_trace_stop();
    if (stop & game::kProfFrames)
        frame_trace_file_stop();
    g_prof_on &= ~stop;
    uint32_t refused = 0;
    if ((start & game::kProfSprites) && !game::sprite_trace_start(kTracePath))
        refused |= game::kProfSprites;
    if (start & game::kProfGuest) {
        if (rt::set_profiling(true))
            start_sampler();
        else
            refused |= game::kProfGuest;
    }
    if ((start & game::kProfFiles) && !file_trace_start())
        refused |= game::kProfFiles;
    if ((start & game::kProfFrames) && !frame_trace_file_start())
        refused |= game::kProfFrames;
    if (start & game::kProfSdBench)
        sd_benchmark(g_bench_path);   // once, as the run starts
    if (refused)
        status("profiler run: could not start %s", game::profile_names(refused).c_str());
    g_prof_refused |= refused;
    g_prof_on |= start & ~refused;
    g_prof_used |= start & ~refused;
    char line[96];
    std::snprintf(line, sizeof line, "%s%s%s", game::kProfilerRunLine, g_prof_on ? "start " : "stop",
                  game::profile_names(g_prof_on).c_str());
    logline(line);
}

// The profiler run, at the heartbeat: on and off as INFO's checklist says;
// the sprite trace's beat, with the cache and the pool.
void profiler_heartbeat(const rt::MemoryReport &m, const game::SpriteCacheStats &sc)
{
    if (!g_ss.profiling())
        g_prof_refused = 0;
    const uint32_t want = g_ss.profiling() ? g_ss.profile_choice() & ~g_prof_refused : 0;
    if (want != g_prof_on)
        profiler_switch(want);
    if (g_prof_on & game::kProfSprites)
        game::sprite_trace_beat({sc.limit, sc.used, sc.sprites, m.free, m.spare_used ? 1u : 0u, m.largest_free,
                                 game::sprite_cache_taken(), (uint32_t)game::dcc_cells_decoded(),
                                 g_timing.total_frames});
}

// The hitch counters' window (issue #19), at the heartbeat: logged, kept
// for INFO, and the next one started.
void hitch_heartbeat(const game::SpriteCacheWindow &w)
{
    HitchMark now{rt::frame_thread_times(), g_frames_skipped, d2rt::dsound::famine_count()};
    HitchWindow &hw = g_hitch_last;
    hw.h = g_hitch;
    hw.wait_ms = (uint32_t)(now.frame.blocked_ms - g_hitch_mark.frame.blocked_ms);
    hw.ready_ms = (uint32_t)(now.frame.ready_ms - g_hitch_mark.frame.ready_ms);
    hw.host_ms = (uint32_t)(now.frame.host_ms - g_hitch_mark.frame.host_ms);
    hw.heartbeat_ms = (uint32_t)(rt::last_heartbeat_us() / 1000);
    hw.skipped = now.skipped - g_hitch_mark.skipped;
    hw.famine = (uint32_t)(now.famine - g_hitch_mark.famine);
    g_hitch_mark = now;
    g_hitch = Hitches{};
    char line[256];
    std::snprintf(line, sizeof line,
                  "[rt] prof hitch: frames %u, <=50ms %u, <=100ms %u, <=250ms %u, >250ms %u, longest %u ms, "
                  "sprites in %u out %u, wait %u ms, ready %u ms, host %u ms, heartbeat %u ms, skipped %u, famine %u",
                  (unsigned)(hw.h.frames[0] + hw.h.frames[1] + hw.h.frames[2] + hw.h.frames[3]),
                  (unsigned)hw.h.frames[0], (unsigned)hw.h.frames[1], (unsigned)hw.h.frames[2],
                  (unsigned)hw.h.frames[3], (unsigned)hw.h.longest_ms, w.inserted, w.evicted, (unsigned)hw.wait_ms,
                  (unsigned)hw.ready_ms, (unsigned)hw.host_ms, (unsigned)hw.heartbeat_ms, (unsigned)hw.skipped,
                  (unsigned)hw.famine);
    logline_held(line);
    // The screen (swap_screens): how long the frames stayed on it.
    Glass g;
    LightLock_Lock(&g_glass_lock);
    g = g_glass;
    g_glass = Glass{};
    LightLock_Unlock(&g_glass_lock);
    const uint32_t hz100 = g.span_us ? (uint32_t)(100000000ull * g.refreshes / g.span_us) : 0;
    std::snprintf(line, sizeof line,
                  "[rt] screen: %s, refresh %u.%02u Hz measured; frames on it for 1 refresh %u, 2 %u, 3 %u, "
                  "4+ %u; held back a refresh %u; ready in a refresh's last 2 ms %u (held %u)",
                  g_screen50.load() ? "50 Hz" : "59.83 Hz", (unsigned)(hz100 / 100), (unsigned)(hz100 % 100),
                  (unsigned)g.shown[0], (unsigned)g.shown[1], (unsigned)g.shown[2], (unsigned)g.shown[3],
                  (unsigned)g.held, (unsigned)g.edge, (unsigned)g.edge_held);
    logline_held(line);
    // The frame lock (issue #86): where in a refresh the swaps fell, and how
    // far the game's clock has been moved ahead.
    static uint32_t pushes_mark = 0;
    const uint32_t pushes = g_lock_pushes.load(std::memory_order_relaxed);
    std::snprintf(line, sizeof line,
                  "[rt] frame lock: %s%s; swaps in a refresh's 0-5 ms %u, 5-10 %u, 10-15 %u, 15-20 %u; "
                  "clock moved for %u frames, %u.%03u ms ahead in all",
                  g_lock_on.load() ? "on" : "off", g_lock_test ? " (locktest)" : "", (unsigned)g.phase[0],
                  (unsigned)g.phase[1], (unsigned)g.phase[2], (unsigned)g.phase[3], (unsigned)(pushes - pushes_mark),
                  (unsigned)(rt::guest_clock_ahead_us() / 1000), (unsigned)(rt::guest_clock_ahead_us() % 1000));
    pushes_mark = pushes;
    logline_held(line);
    // The native sprite decoder (issue #83): the window's groups by who decoded them.
    static game::DccStats dcc_mark;
    const game::DccStats d = game::dcc_stats();
    std::snprintf(line, sizeof line,
                  "[rt] dcc: %s; groups native %u (on the frame thread %u, the worker full %u), on the worker %u, "
                  "the game's %u (refused %u); native %u ms",
                  game::dcc_mode() == game::DccMode::Off ? "off" : "on", d.native - dcc_mark.native,
                  d.on_frame - dcc_mark.on_frame, d.worker_busy - dcc_mark.worker_busy,
                  d.offloaded - dcc_mark.offloaded, d.game - dcc_mark.game, d.refused - dcc_mark.refused,
                  (unsigned)((d.native_us - dcc_mark.native_us) / 1000));
    dcc_mark = d;
    logline_held(line);
    // The game thread's reads by file (issue #89).
    for (const std::string &l : game::frame_reads_lines())
        logline_held(l.c_str());
    for (const std::string &l : game::tiles_ahead_lines())
        logline_held(l.c_str());
    // The JIT pool full (issue #90): chunks refused, so the JIT evicts.
    static uint32_t refused_seen = 0;
    if (dyn86_ctr_jitpool_refused != refused_seen) {
        refused_seen = dyn86_ctr_jitpool_refused;
        std::snprintf(line, sizeof line, "[rt] jit pool full: %u chunk requests refused (%u MiB pool)",
                      (unsigned)refused_seen, (unsigned)(dyn86_ctr_jitpool_size >> 20));
        logline_held(line);
    }
}

}  // namespace

// abort() (linked with --wrap=abort, issue #66). newlib's ends the app
// through _exit, and libctru's exit frees the heaps while the app's other
// threads still run on stacks in them: they fault on the unmapped pages, and
// the console went down in the kernel (3DS-D2-11, where the JIT's ARMv6
// bit-field check aborted on the game's arrival in Act II). Logged with its
// caller instead, then the app stops at once: Luma3DS dumps the thread that
// aborted, the heaps still mapped.
extern "C" __attribute__((noreturn)) void __wrap_abort(void)
{
    char s[80];
    std::snprintf(s, sizeof s, "FATAL: abort() called from %08lx", (unsigned long)__builtin_return_address(0));
    app::writer_crash();
    logline(s);
    svcBreak(USERBREAK_PANIC);
    for (;;) {
    }
}

#ifdef D2_CIA
// The guest arena is carved out of the heap, 16 MiB-aligned, and the host
// keeps the rest: the jump tables not in linear memory, the JIT's metadata
// and the runtime (JIT code is in its pool). The largest layout that the
// heap and the linear memory set aside for jump tables allow.
static const rt::Layout *reserve_arena()
{
    const bool full = __ctru_heap_size >= (95u << 20);
    // The larger arena only with 3 MiB of jump tables in linear memory. The
    // console set 1,792 KiB aside, so it has always run ctr-game-n3-heap;
    // the JIT pool's cut to 14 MiB (issue #89) gives it 2,816 KiB, and that
    // MiB is for the host's heap, whose last MiB the JIT's records reached
    // with the tile prefetch (3DS-D2-1C), not for 2 MiB more arena.
    const rt::Layout *const candidates[] = {
        full && g_heap_setup.jmptbl_linear >= (3u << 20) ? &rt::kCtrGameLayoutN3 : nullptr,
        full ? &rt::kCtrGameLayoutN3Heap : nullptr,   // the console's full heap
        __ctru_heap_size >= (87u << 20) ? &rt::kCtrGameLayout : nullptr,
    };
    for (const rt::Layout *L : candidates) {
        if (L && (g_arena = memalign(16u << 20, L->span))) {
            g_arena_block = g_arena;
            g_arena_size = L->span;
            return L;
        }
    }
    return nullptr;
}

// After rt::run_exe returns nothing touches the guest arena again (the I/O
// and render threads are joined, the second screen is off), and it is plain
// read-write heap. Without it the heap is nearly full at the game's end, too
// full for the 1 MiB socket buffer a report needs (issue #30).
static void give_back_arena()
{
    std::free(g_arena);
    g_arena = nullptr;
    g_arena_size = 0;
}
#endif

int main()
{
#ifdef D2_CIA
    const rt::Layout *const arena_layout = reserve_arena();   // before anything else takes heap
#endif
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    consoleInit(GFX_BOTTOM, &g_log_con);   // all 40 x 30 of it: text from the top, scrolling once full
    consoleSelect(&g_log_con);
    g_ss.set_keyboard(&g_osk);
    mkdir("sdmc:/3ds", 0777);
    mkdir(kRoot, 0777);
    mkdir(kSaveDir, 0777);
    mkdir(kLogDir, 0777);
    // The last session's logs, if it never closed, move to a report folder
    // before this session's replace them.
    const std::string kept = report::keep_unclosed_session();
    std::remove(kTracePath);   // the last session's, if it closed (its report has its own copy)
    std::remove(kFileTracePath);
    std::remove(kFrameTracePath);
    g_log_path = std::string(kLogDir) + "/game.log";
    g_log = open_log(false);
    std::freopen((std::string(kLogDir) + "/stderr.txt").c_str(), "w", stderr);
    // The main thread's handler is the default for the others; the guest runs here.
    threadOnException(on_cpu_exception, g_exc_stack + sizeof g_exc_stack, WRITE_DATA_TO_HANDLER_STACK);
    status("3ds-d2: Diablo II on the 3DS");
    logline(report::start_line().c_str());
    if (!kept.empty())
        status("The last session did not end normally. Its logs: %s", kept.c_str() + std::strlen(kRoot) + 1);
    status("heap %u KiB at %08lx, linear %u KiB, app memory %u KiB", (unsigned)(__ctru_heap_size >> 10),
           (unsigned long)__ctru_heap, (unsigned)(__ctru_linear_heap_size >> 10),
           (unsigned)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10));
#ifdef D2_CIA
    status("heap setup: budget %u KiB, region free %u KiB, %u tries, last refusal %08lx, jump tables %u KiB linear",
           (unsigned)(g_heap_setup.budget >> 10), (unsigned)(g_heap_setup.region_free >> 10),
           (unsigned)g_heap_setup.tries, (unsigned long)g_heap_setup.last_rc,
           (unsigned)(g_heap_setup.jmptbl_linear >> 10));
    if (arena_layout)
        status("arena: %s, %u KiB at %08lx", arena_layout->name, (unsigned)(g_arena_size >> 10),
               (unsigned long)(uintptr_t)g_arena);
#endif

    // Game files: the CIA's RomFS if it has them, else the SD card.
    std::string game_dir = kGameDir;
    std::vector<uint8_t> exe;
    g_romfs = R_SUCCEEDED(romfsInit());
    if (g_romfs && read_file(std::string(kRomfsGameDir) + "/Game.exe", exe))
        game_dir = kRomfsGameDir;
    status("app memory %u KiB, game files: %s",
           (unsigned)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10), game_dir.c_str());
    // Reports not answered yet (the last session's crash, or one kept for
    // later), before the game takes the memory.
    if (!report::offer_pending(report_host(game_dir))) {
        close_log();
        gfxExit();
        return 0;
    }
    if (exe.empty() && !read_file(game_dir + "/Game.exe", exe)) {
        status("Game.exe not found in %s.", kGameDir);
        status("Copy Game.exe (1.14d) and the .mpq files there.");
        close_log();
        wait_for_start();
        gfxExit();
        return 1;
    }
    // Only 1.14d's Game.exe runs here (issue #77): any other is refused
    // before it starts, with what to do. The session ends in order
    // (close_log), so no crash report is made of it.
    const game::GameExe exe_check = game::check_game_exe(exe.data(), exe.size());
    if (exe_check.kind != game::GameExe::kOk) {
        if (exe_check.kind == game::GameExe::kNotPe)
            status("Game.exe (%u bytes) is not a Windows program.", (unsigned)exe.size());
        else if (exe_check.kind == game::GameExe::kBeforeV114)
            status("Game.exe (%u bytes, entry %08lx) is from a Diablo II before 1.14.", (unsigned)exe.size(),
                   (unsigned long)exe_check.entry);
        else
            status("Game.exe (%u bytes, entry %08lx) is not Diablo II 1.14d.", (unsigned)exe.size(),
                   (unsigned long)exe_check.entry);
        status("3ds-d2 runs only the 1.14d Game.exe (3.6 MB).");
        status("Patch Diablo II to 1.14d (the installers give 1.14c),");
        status("then copy its Game.exe and patch_d2.mpq to %s.", kGameDir);
        close_log();
        wait_for_start();
        gfxExit();
        return 1;
    }
    status("Game.exe: %u bytes. Starting...", (unsigned)exe.size());
    g_bench_path = game_dir + "/d2data.mpq";

    load_settings();
    {
        std::vector<uint8_t> script;
        if (read_file(std::string(kRoot) + "/autoplay.txt", script)) {
            g_autoplay.load(std::string(script.begin(), script.end()));
            g_autoplay_on = true;
            status("autoplay.txt loaded (any input stops it)");
        }
    }

    rt::RunOptions opt;
#ifdef D2_CIA
    if (!arena_layout) {
        status("Only %u KiB of heap: the game needs at least 87 MiB.", (unsigned)(__ctru_heap_size >> 10));
        close_log();
        wait_for_start();
        gfxExit();
        return 1;
    }
    opt.layout = arena_layout;
#else
    opt.layout = &rt::kCtrGameLayout;
#endif
    opt.data_root = game_dir;
    opt.write_root = kSaveDir;
    opt.exe_name = "Game.exe";
    opt.command_line = "game.exe -w";
    opt.cpuid_edx = rt::kCpuidNoSimd;
    opt.total_phys_mb = 64;   // the game's own 24 MiB sprite-cache floor
    opt.pool_spare = game::kCtrPoolSpare;
    opt.pool_large = game::kCtrPoolLarge;
    opt.background_quantum = g_background_quantum;
    rt::set_dynarec_knobs(g_dyn_callret, g_dyn_nopend, g_dyn_budgettail, g_dyn_forward);
    opt.background_slice_us = g_background_slice_us;
    if (g_background_quantum != kBackgroundQuantum || g_background_slice_us != kBackgroundSliceUs)
        status("background threads' slice: %u instructions, %u us (settings.txt)", (unsigned)g_background_quantum,
               (unsigned)g_background_slice_us);
    // Cheaper video defaults than a PC's (the game's Options menu still
    // changes them): 640x480 in game (36% fewer pixels to draw, and exactly
    // 2:1 down to the 240-line screen), low lighting, no blended shadows, no
    // perspective.
    opt.registry_dwords = {{"Resolution", 0}, {"Light Quality", 0}, {"Blended Shadows", 0}, {"Perspective", 0}};
    opt.real_clock = true;
    opt.sleep_ms = [](uint32_t ms) { svcSleepThread((s64)ms * 1000000); };
    opt.heartbeat_ms = 10000;
    opt.heartbeat = [] {
        // A request served from the spare had no other piece of the pool
        // left: the cache goes down to its minimum, once per new take. What
        // stays in the spare after it does not hold the cache there.
        const uint64_t t0 = rt::now_us();
        const rt::MemoryReport m = rt::memory_report();
        static uint32_t spare_takes_seen = 0;
        const bool spare_taken = m.spare_takes != spare_takes_seen;
        spare_takes_seen = m.spare_takes;
        if (m.spare_used || m.free < game::kCtrPoolRelease)
            game::request_pool_release();
        const game::SpriteCacheWindow w = game::sprite_cache_window(*g_guest_cpu);
        game::govern_sprite_cache(*g_guest_cpu, m.free, spare_taken, w.evicted, game::kCtrPoolReserve,
                                  game::kCtrSpriteCacheMin);
        const uint64_t t1 = rt::now_us();
        g_sprite_window = w;
        hitch_heartbeat(w);   // before the sprite cache line, the window's last (tools/session_report.py)
        const game::SpriteCacheStats sc = game::sprite_cache_stats(*g_guest_cpu);
        char line[208], frames[192];
        static uint32_t storm_mark[2] = {};
        uint32_t storm[2];
        game::storm_direct_counts(storm[0], storm[1]);
        if (storm[0] != storm_mark[0] || storm[1] != storm_mark[1]) {
            std::snprintf(line, sizeof line, "[rt] storm reads: %u read by their caller, %u queued (the read thread reading)",
                          (unsigned)(storm[0] - storm_mark[0]), (unsigned)(storm[1] - storm_mark[1]));
            logline_held(line);
            storm_mark[0] = storm[0];
            storm_mark[1] = storm[1];
        }
        if (game::sprite_frames_line(frames, sizeof frames)) {
            std::snprintf(line, sizeof line, "[rt] %s", frames);
            logline_held(line);
        }
        std::snprintf(line, sizeof line, "[rt] sprite cache %u/%u KiB, %u sprites, in %u, out %u, decoded %u in %u ms",
                      sc.used >> 10, sc.limit >> 10, sc.sprites, w.inserted, w.evicted, w.decodes, w.decode_ms);
        logline_held(line);
        const uint64_t t2 = rt::now_us();
        profiler_heartbeat(m, sc);
        const uint64_t t3 = rt::now_us();
        char sys[320];
        system_line(sys, sizeof sys, "");
        logline_held(sys);
        g_heap_room_low = UINT32_MAX;   // "low": since this heartbeat
        const uint64_t t4 = rt::now_us();
        log_flush();   // the heartbeat's lines, in one write
        // Where the heartbeat's time went (issue #71): with the profiler on,
        // about 240 ms against 30 without, and three of 3-4 s in a 3 h
        // session. Every heartbeat while profiling, else only a long one.
        // Written with the next lines: its own write is in it.
        const uint64_t t5 = rt::now_us();
        const uint64_t prof = rt::last_prof_report_us();
        const uint64_t total = prof + (t5 - t0);
        if (g_prof_on || total >= 100000) {
            auto ms = [](uint64_t us) { return (unsigned)((us + 500) / 1000); };
            std::snprintf(line, sizeof line,
                          "[rt] heartbeat steps: total %u ms; profile lines %u, memory %u, cache lines %u, sprite trace %u, "
                          "system %u, write %u",
                          ms(total), ms(prof), ms(t1 - t0), ms(t2 - t1), ms(t3 - t2), ms(t4 - t3), ms(t5 - t4));
            logline_held(line);
        }
        // The writer's work since the last heartbeat (issue #71): per file,
        // the bytes, the writes and the time in them, and anything dropped.
        // While profiling, or when something was dropped.
        static app::WriterStats last[4] = {};
        app::WriterStats now[4];
        const size_t n = app::writer_stats(now, 4);
        std::string sd;
        bool dropped = false;
        for (size_t i = 0; i < n; i++) {
            app::WriterStats was{};
            for (const app::WriterStats &l : last)
                if (l.name && std::strcmp(l.name, now[i].name) == 0 && l.bytes <= now[i].bytes)
                    was = l;
            char item[128];
            std::snprintf(item, sizeof item, "%s%s %u KiB in %u writes, %u ms (longest %u ms so far), %u B dropped",
                          i ? "; " : "", now[i].name, (unsigned)((now[i].bytes - was.bytes) >> 10),
                          now[i].writes - was.writes, now[i].ms - was.ms, now[i].longest_ms, now[i].lost - was.lost);
            sd += item;
            dropped |= now[i].lost != was.lost;
        }
        for (size_t i = 0; i < 4; i++)
            last[i] = i < n ? now[i] : app::WriterStats{};
        if (n && (g_prof_on || dropped))
            logline_held(("[rt] sd writes: " + sd).c_str());
    };
    // Sound through ds_emul into the DSP (ctr_audio.cpp), mixed on another core.
    setenv("WX86_SON", "vita", 1);   // ds_emul's name for the console's realtime sink
    opt.audio = true;
    // Native floor, wall and sprite blitters in place of their translations,
    // and the native cinematics player.
    // Memory (issue #15): a smaller sprite cache, the game's fatal exit
    // ending the run at once, and the cache's fill after each heartbeat;
    // the pool systems' empty blocks given back when the pool runs short,
    // and their blocks 128 KiB instead of 512 KiB; the sprites' frames in
    // blocks of their own, in finer classes (issue #44).
    opt.install_native = [](d2rt::Cpu &c) {
        g_guest_cpu = &c;
        game::set_sprite_cache_floor(c, game::kCtrSpriteCacheFloor);
        game::install_sprite_cache_counter(c);
        game::install_sprite_decode_timer(c);
        game::install_sprite_trace(c);
        game::install_dcc_native(c, native_dcc() ? game::DccMode::Offload : game::DccMode::Off);
        game::install_storm_read_priority(c);
        if (g_storm_direct)
            game::install_storm_direct_reads(c);
        game::install_frame_reads(c);
        rt::set_prefetch_arena(g_prefetch_arena, sizeof g_prefetch_arena);
        if (g_storm_rate_kib)
            game::set_storm_read_rate(c, g_storm_rate_kib << 10);
        game::install_fatal_exit(c);
        game::install_pool_release(c);
        game::set_pool_block_size(c, game::kCtrPoolBlock);
        game::install_sprite_frames(c);
        game::set_floor_queue(g_render_core == 2 ? 1 : 0);
        game::set_floor_queue_on(core2_draw());
        game::FloorWorker fw;
        fw.start = start_floor_thread;
        fw.wait = [] { LightEvent_Wait(&g_floor_event); };
        fw.wake = [] { LightEvent_Signal(&g_floor_event); };
        game::set_floor_worker(fw);
        game::install_d2_native(c);
        game::install_cinematics(c);
        game::install_automap_capture(c);   // the bottom screen's MAP tab
    };
    opt.video_audio = d2rt::audio::video_sink();
    opt.video_event = [](const char *what) {
        if (std::strcmp(what, "close") == 0)
            g_ss.end_video();   // the tab as it was, with the game's next frame
        char sys[320];
        system_line(sys, sizeof sys, std::strcmp(what, "open") ? " at video close" : " at video open");
        logline(sys);
    };
    opt.log = [](const char *s) {
        char line[600];
        std::snprintf(line, sizeof line, "[rt] %s", s);
        // The heartbeat's own lines wait for its end (log_flush).
        if (std::strncmp(s, "t=", 2) == 0 || std::strncmp(s, "prof", 4) == 0)
            logline_held(line);
        else
            logline(line);
        // Heartbeats: the panel shows those numbers.
        // The sound engine's "[son]" diagnostics (WinVita writes them in
        // French) go to game.log only.
        // So do the per-interval profile lines ("prof...").
        if (std::strncmp(s, "t=", 2) != 0 && std::strncmp(s, "[son]", 5) != 0 && std::strncmp(s, "prof", 4) != 0)
            screen_message(s);
    };
    opt.present = present;
    opt.poll_input = poll;
    opt.service = [] {
        apt_service();
        report_heap_refusals();
        report_meta_reserve();
    };
    opt.io_start = start_io_thread;
    opt.io_wait = [] { LightEvent_Wait(&g_io_event); };
    opt.io_wake = [] { LightEvent_Signal(&g_io_event); };
    opt.io_done = [] { LightEvent_Signal(&g_io_done_event); };
    opt.io_idle_wait = [](uint32_t ms) { LightEvent_WaitTimeout(&g_io_done_event, (s64)ms * 1000000); };

    start_render_thread();
    start_writer_thread();
    game::sprite_trace_set_wrap([](FILE *f, char *buf, size_t n) { return app::writer_wrap(f, buf, n, "sprites.trc"); });
    aptHook(&g_apt_cookie, apt_hook, nullptr);
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    join_io_thread();
    join_floor_thread();
    if (g_prof_on)
        profiler_switch(0);   // a run still on: its traces closed, in time for the report
    aptUnhook(&g_apt_cookie);
    stop_render_thread();
    screen_hz_set(false);   // the console's own rate, whatever the game's end
    console_again();   // the game's end shows on the console, as before the second screen
    if (!r.loaded)
        status("FATAL: %s", r.error.c_str());
    else
        status("Game ended: exited=%d code=%u %s", r.exited, r.exit_code, r.error.c_str());
    if (rt::stats().alloc_failures)
        status("Diablo II ran out of memory (%u requests refused, see game.log).", rt::stats().alloc_failures);
    else if (game::fatal_exit_caller())
        status("Diablo II stopped on an internal error at %08lx.", (unsigned long)game::fatal_exit_caller());
    // A report (issue #26), now that the game's memory is free: for an
    // error, or because the player asked for one on the INFO tab.
    const char *why = !r.loaded                    ? "the game did not start"
                      : rt::stats().alloc_failures ? "ran out of memory"
                      : game::fatal_exit_caller()  ? "stopped on an internal error"
                      : g_ss.report_requested()    ? "asked for by the player"
                      : g_prof_used                ? "a profiler run"
                                                   : nullptr;
    // What follows can take a minute or two (a long run's traces copied on
    // the SD card, then read for the report): said on the bottom screen, so
    // that it doesn't look like a hang (issue #96).
    const bool reporting = why && report::enabled();
    if (reporting) {
        status("Preparing the report. This can take a");
        status("minute or two; please wait.");
    }
    const uint64_t closing_ms = osGetTime();
    close_log();
#ifdef D2_CIA
    give_back_arena();
#endif
    if (reporting) {
        const report::Host host = report_host(game_dir);
        const std::string dir =
            report::keep_session(host, why, (uint32_t)(osGetTime() - closing_ms));
        if (!report::offer_one(host, dir)) {
            gfxExit();   // closing: offered again at the next start
            return 0;
        }
    }
    wait_for_start();
    gfxExit();
    return 0;
}
