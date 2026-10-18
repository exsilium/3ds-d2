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
// switches. The bottom screen is a diagnostics panel (frame rate, frame time,
// guest and host memory, the latest runtime messages) and the touchpad; when
// the game focuses a text field, an on-screen keyboard replaces the messages.
// Controls: src/game/controls.h.
//
// The frame is drawn by the CPU straight into the RGB565 framebuffer: a
// palette lookup per sample, no texture upload, no GPU state.
#include "game/autoplay.h"
#include "game/controls.h"
#include "game/d2_native.h"
#include "game/d2_state.h"
#include "game/osk.h"
#include "runtime/rt_host.h"

#include <3ds.h>
#include <3ds/allocator/mappable.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
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
};
HeapSetup g_heap_setup;
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
    hs.linear_rc = svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, rw);
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

namespace {

const char kRoot[] = "sdmc:/3ds/3ds-d2";
const char kGameDir[] = "sdmc:/3ds/3ds-d2/game";
const char kRomfsGameDir[] = "romfs:/game";   // the CIA carries the game files in its RomFS
const char kSaveDir[] = "sdmc:/3ds/3ds-d2/save";
const char kLogDir[] = "sdmc:/3ds/3ds-d2/logs";

FILE *g_log = nullptr;
game::Controls g_controls;
bool g_zoom = false;   // top screen: false = whole frame, true = 1:1 around the cursor
uint16_t g_pal565[256];
// The palette with each channel in its own 10-bit field (r << 20 | g << 10 | b),
// so four entries sum in plain adds without carries between channels: the
// 2x2 filter then costs four table loads and three adds per output pixel.
uint32_t g_pal_wide[256];

// ---- bottom screen: diagnostics panel ----------------------------------------------
//
// Two console windows on the bottom framebuffer: statistics (rows 0-10,
// rewritten once a second) and the runtime's messages (rows 12-29, scrolling).

PrintConsole g_stats_con, g_log_con;
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

void show_stats(float fps, float draw_ms, float worst_ms)
{
    consoleSelect(&g_stats_con);
    const rt::MemoryReport m = rt::memory_report();
    const struct mallinfo mi = mallinfo();
    const uint64_t t = rt::now_ms() / 1000;
    const rt::Stats &st = rt::stats();
    // Each line is padded to 39 columns: a 40th character would wrap the
    // 40-column window and scroll the panel.
    char line[12][48];
    std::snprintf(line[0], 48, "3ds-d2  game time %02u:%02u:%02u", (unsigned)(t / 3600), (unsigned)(t / 60 % 60),
                  (unsigned)(t % 60));
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
    if (dyn86_jit_allocfail)   // translated code no longer fits: the game will stall
        std::snprintf(line[8], 48, "\x1b[31mJIT MEMORY FULL\x1b[0m code %u jmptbl %u", m.jit_code_kb, m.jmptbl_kb);
    else
        std::snprintf(line[8], 48, " jit code %5u meta %5u jmptbl %5u", m.jit_code_kb, m.jit_meta_kb, m.jmptbl_kb);
    std::snprintf(line[9], 48, "files %3u  read %5u MiB", st.files_opened, (unsigned)(st.bytes_read >> 20));
    std::snprintf(line[10], 48, "view: %s%s", g_zoom ? "1:1 zoom (Select)" : "whole frame (Select: zoom)",
                  g_autoplay_on ? "  AUTO" : "");
    std::snprintf(line[11], 48, "%s", "----------------------------------------");
    std::printf("\x1b[1;1H");
    for (int i = 0; i < 12; i++)
        std::printf(i == 0 ? "\x1b[33m%-39.39s\x1b[0m\n" : i == 11 ? "%-39.39s" : "%-39.39s\n", line[i]);
    consoleSelect(&g_log_con);
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

void status(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf);   // the log window
    logline(buf);
}

// A fatal message from the CPU backend (it exits right after): show it and
// wait for START, instead of dropping silently back to the HOME menu.
extern "C" void d2_ctr_progress_hook(const char *msg)
{
    if (std::strncmp(msg, "FATAL", 5) != 0)
        return;
    consoleSelect(&g_log_con);
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

inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

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

// ---- the render thread -----------------------------------------------------------
//
// Converting and scaling a frame for the top screen costs ~6 ms on the
// console's game core. On a New 3DS it runs on another core instead: present()
// copies the frame (indices + palette, ~0.5 MB) and returns, and this thread
// draws it while the game works on its next frame. A frame that arrives while
// the previous one is still being drawn is skipped on screen (never waited for).

struct Staged {
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    uint32_t palette[256];
    bool zoom;
    int cursor_x, cursor_y;
};
Staged g_staged;
LightEvent g_render_go;
volatile bool g_render_busy = false, g_render_quit = false;
Thread g_render_thread = nullptr;

void draw_staged()
{
    const Staged &st = g_staged;
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
    uint16_t *top = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    if (st.zoom)
        draw_window(top, f, st.cursor_x, st.cursor_y);
    else
        draw_stretched(top, f);
    gfxFlushBuffers();
    gfxSwapBuffers();
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
    g_controls.set_frame((int)f.width, (int)f.height);
    g_timing.frames++;
    g_timing.total_frames++;

    if (__atomic_load_n(&g_render_busy, __ATOMIC_ACQUIRE)) {
        g_frames_skipped++;   // the screen is still showing/drawing the last one
    } else {
        Staged &st = g_staged;
        st.width = f.width;
        st.height = f.height;
        st.pixels.resize((size_t)f.width * f.height);
        for (uint32_t y = 0; y < f.height; y++)
            std::memcpy(&st.pixels[(size_t)y * f.width], f.pixels + (ptrdiff_t)y * f.stride, f.width);
        std::memcpy(st.palette, f.palette, sizeof st.palette);
        st.zoom = g_zoom;
        st.cursor_x = g_controls.cursor_x();
        st.cursor_y = g_controls.cursor_y();
        if (g_render_thread) {
            __atomic_store_n(&g_render_busy, true, __ATOMIC_RELEASE);
            LightEvent_Signal(&g_render_go);
        } else {
            draw_staged();
            g_timing.draw_us += host_us() - t0;
        }
    }
    update_stats();
}

// ---- on-screen keyboard (bottom screen, over the log window) ----------------------

game::Osk g_osk;
bool g_osk_shown = false;            // what the log window shows now: the keyboard or the log
bool g_wait_release = false;         // after the keyboard closes: ignore buttons until all are up
uint32_t g_last_focus = 0;           // the last focused edit box seen (auto-open on a new one)
constexpr int kOskTopRow = 13;       // the keyboard draws in the log window (rows 13-29)

void draw_osk()
{
    consoleSelect(&g_log_con);
    std::printf("\x1b[2J");
    if (g_osk.is_open()) {
        const std::vector<game::Osk::Row> rows = g_osk.rows();
        for (int r = 0; r < (int)rows.size(); r++) {
            std::printf("\x1b[%d;1H", r + 1);
            bool on = false;
            // 39 columns: a character in the 40th would wrap and scroll the window.
            for (int c = 0; c < 39; c++) {
                if (rows[r].key[c] != on) {
                    on = rows[r].key[c];
                    std::printf(on ? "\x1b[47;30m" : "\x1b[0m");
                }
                std::putchar(rows[r].text[c]);
            }
            std::printf("\x1b[0m");
        }
    }
    g_osk_shown = g_osk.is_open();
    g_osk.clear_dirty();
}

void poll()
{
    if (!aptMainLoop()) {   // HOME menu "close", or the system shutting down
        rt::request_exit(0);
        return;
    }
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
            g_osk.tap(pad.touch_x / 8, pad.touch_y / 8 - kOskTopRow);
        if (!g_osk.is_open())
            g_wait_release = true;   // the button that closed it must not reach the game
        pad = game::Pad();
    } else if (g_wait_release) {
        if (pad.held || pad.touching)
            pad = game::Pad();
        else
            g_wait_release = false;
    }
    g_controls.set_context(gs.in_game, gs.panel_open);
    g_controls.update(pad, rt::now_ms());
    if (g_controls.take_zoom_toggle())
        g_zoom = !g_zoom;
    if (g_controls.take_keyboard_request())   // Select+Start
        g_osk.open();
    if (g_osk.dirty() || g_osk.is_open() != g_osk_shown)
        draw_osk();
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

void wait_for_start()
{
    std::printf("\nPress START to exit.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
}

}  // namespace

int main()
{
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    gfxInit(GSP_RGB565_OES, GSP_RGB565_OES, false);
    consoleInit(GFX_BOTTOM, &g_stats_con);
    consoleInit(GFX_BOTTOM, &g_log_con);
    consoleSetWindow(&g_stats_con, 0, 0, 40, 12);
    consoleSetWindow(&g_log_con, 0, 13, 40, 17);
    consoleSelect(&g_log_con);
    mkdir("sdmc:/3ds", 0777);
    mkdir(kRoot, 0777);
    mkdir(kSaveDir, 0777);
    mkdir(kLogDir, 0777);
    g_log = std::fopen((std::string(kLogDir) + "/game.log").c_str(), "w");
    std::freopen((std::string(kLogDir) + "/stderr.txt").c_str(), "w", stderr);
    status("3ds-d2: Diablo II on the 3DS");
    status("heap %u KiB at %08lx, linear %u KiB, app memory %u KiB", (unsigned)(__ctru_heap_size >> 10),
           (unsigned long)__ctru_heap, (unsigned)(__ctru_linear_heap_size >> 10),
           (unsigned)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10));
#ifdef D2_CIA
    status("heap setup: budget %u KiB, region free %u KiB, %u tries, last refusal %08lx",
           (unsigned)(g_heap_setup.budget >> 10), (unsigned)(g_heap_setup.region_free >> 10),
           (unsigned)g_heap_setup.tries, (unsigned long)g_heap_setup.last_rc);
#endif

    // Game files: the CIA's RomFS if it has them, else the SD card.
    std::string game_dir = kGameDir;
    std::vector<uint8_t> exe;
    if (R_SUCCEEDED(romfsInit()) && read_file(std::string(kRomfsGameDir) + "/Game.exe", exe))
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
    // The arena is carved out of the heap (16 MiB-aligned, so it needs its
    // size + 16 MiB free at once) and the host keeps the rest: the jump
    // tables, the JIT's metadata and the runtime (JIT code is in its pool).
    if (__ctru_heap_size >= (95u << 20)) {
        opt.layout = &rt::kCtrGameLayoutN3;   // the console's full heap
    } else if (__ctru_heap_size >= (87u << 20)) {
        opt.layout = &rt::kCtrGameLayout;
    } else {
        status("Only %u KiB of heap: the game needs at least 87 MiB.", (unsigned)(__ctru_heap_size >> 10));
        wait_for_start();
        gfxExit();
        return 1;
    }
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
    // Sound through ds_emul into the DSP (ctr_audio.cpp), mixed on another core.
    setenv("WX86_SON", "vita", 1);   // ds_emul's name for the console's realtime sink
    opt.audio = true;
    // Native floor, wall and sprite blitters in place of their translations.
    opt.install_native = [](d2rt::Cpu &c) { game::install_d2_native(c); };
    opt.log = [](const char *s) {
        char line[600];
        std::snprintf(line, sizeof line, "[rt] %s", s);
        logline(line);
        // Heartbeats: the panel shows those numbers. While the keyboard is
        // up, messages go to the log file only.
        // The sound engine's "[son]" diagnostics (WinVita writes them in
        // French) go to game.log only.
        if (std::strncmp(s, "t=", 2) != 0 && std::strncmp(s, "[son]", 5) != 0 && !g_osk_shown)
            std::printf("%.*s\n", 39, s);
    };
    opt.present = present;
    opt.poll_input = poll;

    start_render_thread();
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    stop_render_thread();
    if (!r.loaded)
        status("FATAL: %s", r.error.c_str());
    else
        status("Game ended: exited=%d code=%u %s", r.exited, r.exit_code, r.error.c_str());
    if (g_log)
        std::fclose(g_log);
    wait_for_start();
    gfxExit();
    return 0;
}
