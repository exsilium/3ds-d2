// 3ds-d2 game app: Diablo II 1.14d (Game.exe, windowed GDI mode) on 3ds-d2's
// Win32 runtime and the ARMv6 Box86 backend.
//
//   sdmc:/3ds/3ds-d2/game/   Game.exe and the MPQs (read-only, the user's copy)
//   sdmc:/3ds/3ds-d2/save/   everything the game writes (saves, settings, registry)
//   sdmc:/3ds/3ds-d2/logs/   game.log (runtime diagnostics)
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
#include "game/autoplay.h"
#include "game/automap.h"
#include "game/cinematics.h"
#include "game/controls.h"
#include "game/d2_memory.h"
#include "game/d2_native.h"
#include "game/d2_state.h"
#include "game/osk.h"
#include "game/second_screen.h"
#include "runtime/rt_host.h"

#include <3ds.h>
#include <3ds/allocator/mappable.h>

#include <cstdarg>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// The town needed 8 MiB of code in Azahar.
alignas(0x1000) uint8_t dyn86_ctr_jitpool[16u << 20];
extern const uint32_t dyn86_ctr_jitpool_size = 16u << 20;

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
static void *g_arena;
static size_t g_arena_size;
void *dyn86_ctr_arena_take(size_t len) { return g_arena && len <= g_arena_size ? g_arena : NULL; }
int dyn86_ctr_arena_release(void *p) { return p && p == g_arena; }
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
};
bool save(const Source &src, std::string &msg);   // ctr_screenshot.cpp
}  // namespace shot

namespace {

const char kRoot[] = "sdmc:/3ds/3ds-d2";
const char kGameDir[] = "sdmc:/3ds/3ds-d2/game";
const char kRomfsGameDir[] = "romfs:/game";   // the CIA carries the game files in its RomFS
const char kSaveDir[] = "sdmc:/3ds/3ds-d2/save";
const char kLogDir[] = "sdmc:/3ds/3ds-d2/logs";
const char kSettings[] = "sdmc:/3ds/3ds-d2/settings.txt";   // the bottom screen's tab and map size

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
// Until the game shows its first frame: two console windows, statistics
// (rows 0-10) and the startup messages (rows 12-29). From then on the bottom
// screen is game::SecondScreen's (second_screen.h), drawn in pixels with
// each frame; the statistics and messages become its INFO tab.

PrintConsole g_stats_con, g_log_con;
game::SecondScreen g_ss;
game::Osk g_osk;
bool g_ss_active = false;    // the second screen owns the bottom framebuffer
bool g_bottom_double = false; // render thread: the bottom screen is double-buffered (second screen)
extern "C" const uint8_t default_font_bin[];   // libctru's console font: 8x8, 1 bpp, bit 7 leftmost
int g_render_core = -1;                 // the core drawing frames (-1: the game's thread)
volatile uint32_t g_frames_skipped = 0; // frames not shown: the previous one was still drawing
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

uint64_t host_us() { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }

inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

void show_stats(float fps, float draw_ms, float worst_ms)
{
    const rt::MemoryReport m = rt::memory_report();
    const struct mallinfo mi = mallinfo();
    const uint64_t t = rt::now_ms() / 1000;
    const rt::Stats &st = rt::stats();
    char line[11][48];
#ifdef PROF_COUNTERS
    const char *build = "  PROF";   // the profiling build (package-cia --prof)
#else
    const char *build = "";
#endif
    std::snprintf(line[0], 48, "3ds-d2  game time %02u:%02u:%02u%s", (unsigned)(t / 3600), (unsigned)(t / 60 % 60),
                  (unsigned)(t % 60), build);
    std::snprintf(line[1], 48, "fps %5.1f   worst frame gap %6.1f ms", fps, worst_ms);
    std::snprintf(line[2], 48, "draw %5.2f ms c%d skip %u frames %u", draw_ms, g_render_core < 0 ? 0 : g_render_core,
                  (unsigned)g_frames_skipped, (unsigned)g_timing.total_frames);
    // Heap and VA share one pool: their sizes are what each has taken.
    std::snprintf(line[3], 48, "guest heap %6u pk %6u free %6u", m.heap_used >> 10, m.heap_peak >> 10, m.free >> 10);
    std::snprintf(line[4], 48, "guest va   %6u pk %6u big  %6u", m.va_used >> 10, m.va_peak >> 10,
                  m.largest_free >> 10);
    std::snprintf(line[5], 48, "host heap  %6u/%6u KiB", (unsigned)(mi.uordblks >> 10), (unsigned)(__ctru_heap_size >> 10));
    std::snprintf(line[6], 48, "linear free %5u KiB", (unsigned)(linearSpaceFree() >> 10));
    std::snprintf(line[7], 48, "app free    %5u KiB", (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10));
    const bool jit_full = dyn86_jit_allocfail != 0;   // translated code no longer fits: the game will stall
    if (jit_full)
        std::snprintf(line[8], 48, "JIT MEMORY FULL code %u jmptbl %u", m.jit_code_kb, m.jmptbl_kb);
    else
        std::snprintf(line[8], 48, " jit code %5u meta %5u jmptbl %5u", m.jit_code_kb, m.jit_meta_kb, m.jmptbl_kb);
    std::snprintf(line[9], 48, "files %3u  read %5u MiB", st.files_opened, (unsigned)(st.bytes_read >> 20));
    std::snprintf(line[10], 48, "top: %s%s", g_zoom ? "1:1 zoom (Select)" : "whole frame (Select: zoom)",
                  g_autoplay_on ? "  AUTO" : "");
    std::vector<game::BottomView::Line> lines;
    for (int i = 0; i < 11; i++) {
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

void logline(const char *s)
{
    if (g_log) {
        std::fprintf(g_log, "%s\n", s);
        std::fflush(g_log);
    }
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

// A message for the screen: the startup console, or the INFO tab.
void screen_message(const char *s)
{
    if (g_ss_active)
        g_ss.log(s);
    else
        std::printf("%.*s\n", 39, s);   // the log window
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
        u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
        g_stats_con.frameBuffer = g_log_con.frameBuffer = fb;
    }
    // All of it: the consoles' clears left the bottom row of the second
    // screen's last picture (MAP's corner button) showing.
    std::memset(g_log_con.frameBuffer, 0, 320 * 240 * sizeof(u16));
    consoleSelect(&g_stats_con);
    consoleClear();
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
Thread g_render_thread = nullptr;

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
    std::string msg;
    const bool ok = shot::save(src, msg);
    char ms[24];
    std::snprintf(ms, sizeof ms, ", %u ms", (unsigned)((host_us() - t0) / 1000));
    g_shot_msg = (ok ? "screenshot: " : "screenshot failed: ") + msg + (ok ? ms : "");
    __atomic_store_n(&g_shot_done, true, __ATOMIC_RELEASE);
}

// The top screen, and the bottom one when it was drawn: once double-buffered,
// swapping the bottom without a draw would show the older of its two buffers.
void swap_screens(bool bottom_drawn)
{
    gfxScreenSwapBuffers(GFX_TOP, true);
    if (bottom_drawn || !g_bottom_double)
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
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
        gfxFlushBuffers();
        swap_screens(false);
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
    const bool bottom_drawn = st.bottom_draw && st.bottom.kind != game::BottomView::kNone;
    if (bottom_drawn) {
        // Double-buffered from the second screen's first draw: drawn in the
        // buffer not shown, so the screen never shows one half redrawn.
        // Every draw covers the whole screen.
        if (!g_bottom_double) {
            gfxSetDoubleBuffering(GFX_BOTTOM, true);
            g_bottom_double = true;
        }
        uint16_t *bottom = (uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
        game::draw_bottom(bottom, st.bottom, st.pixels.data(), (int)st.width, (int)st.height, g_pal565,
                          default_font_bin);
    }
    gfxFlushBuffers();
    swap_screens(bottom_drawn);
}

void render_main(void *)
{
    for (;;) {
        LightEvent_Wait(&g_render_go);
        if (g_render_quit)
            break;
        const uint64_t t0 = host_us();
        draw_staged();
        g_timing.draw_us += host_us() - t0;
        save_staged_shot();
        __atomic_store_n(&g_render_busy, false, __ATOMIC_RELEASE);
    }
}

void start_render_thread()
{
    LightEvent_Init(&g_render_go, RESET_ONESHOT);
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

#ifdef PROF_COUNTERS
// The time-based profile sampler (rt::profile_tick): every 2 ms, from core 2
// at a priority above the drawing and the mixer, so its period holds while
// core 0 runs the game.
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
#endif

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

void present(const rt::Frame &f)
{
    const uint64_t t0 = host_us();
    if (g_timing.last_present_us && t0 - g_timing.last_present_us > g_timing.worst_gap_us)
        g_timing.worst_gap_us = t0 - g_timing.last_present_us;
    g_timing.last_present_us = t0;
    if (!f.yuv[0]) {   // a cinematic's size is not the game's: the cursor keeps the game's frame
        g_controls.set_frame((int)f.width, (int)f.height);
        g_ss.set_frame((int)f.width, (int)f.height);
        game::automap_frame(f.pixels, f.width, f.height, f.stride);
        game::set_automap_capture(g_ss.wants_automap(), g_ss.map_ms(), g_ss.map_scale());
    }
    g_timing.frames++;
    g_timing.total_frames++;

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
        for (uint32_t y = 0; y < f.height; y++)
            std::memcpy(&st.pixels[(size_t)y * f.width], f.pixels + (ptrdiff_t)y * f.stride, f.width);
        std::memcpy(st.palette, f.palette, sizeof st.palette);
        st.zoom = g_zoom;
        st.cursor_x = g_controls.cursor_x();
        st.cursor_y = g_controls.cursor_y();
        st.shot = take_shot_request();
        if (!g_ss_active) {
            g_ss_active = true;   // the game's first frame: the bottom screen is the second screen's from now on
            status("bottom screen: %s tab (tap the tabs to change)",
               g_ss.tab() == game::BottomTab::Map && g_ss.map_help() ? "help" : game::bottom_tab_name(g_ss.tab()));
        }
        st.bottom_draw = g_ss.stage(st.bottom, rt::now_ms());
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
}

// ---- on-screen keyboard (drawn by the second screen) ------------------------------

bool g_wait_release = false;         // after the keyboard closes: ignore buttons until all are up
uint32_t g_last_focus = 0;           // the last focused edit box seen (auto-open on a new one)

// ---- settings: the bottom screen's tab and map size -------------------------------------

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
    }
    std::fclose(f);
    g_ss.take_settings_changed();
}

void save_settings()
{
    if (FILE *f = std::fopen(kSettings, "w")) {
        std::fprintf(f, "bottom=%s\nmapscale=%d\nmapms=%u\nmaphelp=%d\n", game::bottom_tab_name(g_ss.tab()),
                     g_ss.map_scale(), (unsigned)g_ss.map_ms(), g_ss.map_help() ? 1 : 0);
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

bool apt_service()
{
    svcGetThreadId(&g_apt_thread, CUR_THREAD_HANDLE);
    if (aptMainLoop())
        return true;
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

void close_storage()
{
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
        g_log = std::fopen(g_log_path.c_str(), "a");
    rt::resume_files();
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
    if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
        g_ss.invalidate();   // the HOME menu or sleep drew over the bottom screen
    if (type == APTHOOK_ONRESTORE && g_storage_closed && game_thread)
        open_storage();
    if (type != APTHOOK_ONSUSPEND && type != APTHOOK_ONSLEEP)
        return;
    while (__atomic_load_n(&g_render_busy, __ATOMIC_ACQUIRE))
        svcSleepThread(1000000);
    if (type == APTHOOK_ONSUSPEND && game_thread && !g_storage_closed)
        close_storage();
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
    g_controls.set_context(gs.in_game, gs.panel_open);
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

#ifdef PROF_COUNTERS
// The SD card's cost model, for sizing the game-file read cache
// (rt_shims_files.cpp): reads of each size at scattered offsets of an MPQ,
// as the game reads them. Time per read = per-call overhead + size /
// bandwidth; two sizes give both.
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
#endif

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

}  // namespace

static d2rt::Cpu *g_guest_cpu = nullptr;   // the game's CPU, from install_native on

#ifdef D2_CIA
// The guest arena is carved out of the heap, 16 MiB-aligned, and the host
// keeps the rest: the jump tables not in linear memory, the JIT's metadata
// and the runtime (JIT code is in its pool). The largest layout that the
// heap and the linear memory set aside for jump tables allow.
static const rt::Layout *reserve_arena()
{
    const bool full = __ctru_heap_size >= (95u << 20);
    const rt::Layout *const candidates[] = {
        full && g_heap_setup.jmptbl_linear >= (2u << 20) ? &rt::kCtrGameLayoutN3 : nullptr,
        full ? &rt::kCtrGameLayoutN3Heap : nullptr,   // the console's full heap
        __ctru_heap_size >= (87u << 20) ? &rt::kCtrGameLayout : nullptr,
    };
    for (const rt::Layout *L : candidates) {
        if (L && (g_arena = memalign(16u << 20, L->span))) {
            g_arena_size = L->span;
            return L;
        }
    }
    return nullptr;
}
#endif

int main()
{
#ifdef D2_CIA
    const rt::Layout *const arena_layout = reserve_arena();   // before anything else takes heap
#endif
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    consoleInit(GFX_BOTTOM, &g_stats_con);
    consoleInit(GFX_BOTTOM, &g_log_con);
    consoleSetWindow(&g_stats_con, 0, 0, 40, 12);
    consoleSetWindow(&g_log_con, 0, 13, 40, 17);
    consoleSelect(&g_log_con);
    g_ss.set_keyboard(&g_osk);
    mkdir("sdmc:/3ds", 0777);
    mkdir(kRoot, 0777);
    mkdir(kSaveDir, 0777);
    mkdir(kLogDir, 0777);
    g_log_path = std::string(kLogDir) + "/game.log";
    g_log = std::fopen(g_log_path.c_str(), "w");
    std::freopen((std::string(kLogDir) + "/stderr.txt").c_str(), "w", stderr);
    // The main thread's handler is the default for the others; the guest runs here.
    threadOnException(on_cpu_exception, g_exc_stack + sizeof g_exc_stack, WRITE_DATA_TO_HANDLER_STACK);
    status("3ds-d2: Diablo II on the 3DS");
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
    if (exe.empty() && !read_file(game_dir + "/Game.exe", exe)) {
        status("Game.exe not found in %s.", kGameDir);
        status("Copy Game.exe (1.14d) and the .mpq files there.");
        wait_for_start();
        gfxExit();
        return 1;
    }
    status("Game.exe: %u bytes. Starting...", (unsigned)exe.size());
#ifdef PROF_COUNTERS
    sd_benchmark(game_dir + "/d2data.mpq");
#endif

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
    // Cheaper video defaults than a PC's (the game's Options menu still
    // changes them): 640x480 in game (36% fewer pixels to draw, and exactly
    // 2:1 down to the 240-line screen), low lighting, no blended shadows, no
    // perspective.
    opt.registry_dwords = {{"Resolution", 0}, {"Light Quality", 0}, {"Blended Shadows", 0}, {"Perspective", 0}};
    opt.real_clock = true;
    opt.sleep_ms = [](uint32_t ms) { svcSleepThread((s64)ms * 1000000); };
    opt.heartbeat_ms = 10000;
    opt.heartbeat = [] {
        game::govern_sprite_cache(*g_guest_cpu, rt::memory_report().free, game::kCtrPoolReserve,
                                  game::kCtrSpriteCacheMin);
        const game::SpriteCacheStats sc = game::sprite_cache_stats(*g_guest_cpu);
        char line[96];
        std::snprintf(line, sizeof line, "[rt] sprite cache %u/%u KiB, %u sprites", sc.used >> 10, sc.limit >> 10,
                      sc.sprites);
        logline(line);
    };
    // Sound through ds_emul into the DSP (ctr_audio.cpp), mixed on another core.
    setenv("WX86_SON", "vita", 1);   // ds_emul's name for the console's realtime sink
    opt.audio = true;
    // Native floor, wall and sprite blitters in place of their translations,
    // and the native cinematics player.
    // Memory (issue #15): a smaller sprite cache, the game's fatal exit
    // ending the run at once, and the cache's fill after each heartbeat.
    opt.install_native = [](d2rt::Cpu &c) {
        g_guest_cpu = &c;
        game::set_sprite_cache_floor(c, game::kCtrSpriteCacheFloor);
        game::install_fatal_exit(c);
        game::install_d2_native(c);
        game::install_cinematics(c);
        game::install_automap_capture(c);   // the bottom screen's MAP tab
    };
    opt.video_audio = d2rt::audio::video_sink();
    opt.log = [](const char *s) {
        char line[600];
        std::snprintf(line, sizeof line, "[rt] %s", s);
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
    opt.service = [] { apt_service(); };
    opt.io_start = start_io_thread;
    opt.io_wait = [] { LightEvent_Wait(&g_io_event); };
    opt.io_wake = [] { LightEvent_Signal(&g_io_event); };
    opt.io_done = [] { LightEvent_Signal(&g_io_done_event); };
    opt.io_idle_wait = [](uint32_t ms) { LightEvent_WaitTimeout(&g_io_done_event, (s64)ms * 1000000); };

    start_render_thread();
    aptHook(&g_apt_cookie, apt_hook, nullptr);
#ifdef PROF_COUNTERS
    start_sampler();
#endif
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    join_io_thread();
#ifdef PROF_COUNTERS
    stop_sampler();
#endif
    aptUnhook(&g_apt_cookie);
    stop_render_thread();
    console_again();   // the game's end shows on the console, as before the second screen
    if (!r.loaded)
        status("FATAL: %s", r.error.c_str());
    else
        status("Game ended: exited=%d code=%u %s", r.exited, r.exit_code, r.error.c_str());
    if (rt::stats().alloc_failures)
        status("Diablo II ran out of memory (%u requests refused, see game.log).", rt::stats().alloc_failures);
    else if (game::fatal_exit_caller())
        status("Diablo II stopped on an internal error at %08lx.", (unsigned long)game::fatal_exit_caller());
    if (g_log)
        std::fclose(g_log);
    wait_for_start();
    gfxExit();
    return 0;
}
