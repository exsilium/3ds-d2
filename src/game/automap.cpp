// See automap.h.
//
// The off-screen map, on black: just before the map is drawn, the frame (the
// world and the UI drawn so far) is copied aside and cleared to index 0; the
// game then draws the map into its frame as usual, onto nothing, and every
// pixel that is not 0 is the map's. The frame gets its copy back, so the top
// screen shows no map unless the player asked for one (Tab). One guest
// buffer holds the copy, then the map.
// (The sprite and line drawers do not take their target from the tile
// renderer's frame base, 0x7d5448: pointing that elsewhere left the map in
// the frame. Clearing needs nothing from the renderer.)
//
// On black rather than over the world: the map is drawn blended with what is
// under it, so over the world its colours changed with the world and pixels
// matching it were lost, a shimmer that moved as the player did.
#include "game/automap.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include "runtime/guest_region.h"

#include <algorithm>
#include <cstring>

using namespace d2rt;

namespace game {
namespace {

constexpr uint32_t kCallSite = 0x00456f93;
// cmp [0x7a27e8], ebx; je +0f; call 0x45ae90; cmp eax, 3; je +05; call 0x45ad60
constexpr uint8_t kCallSiteBytes[] = {0x39, 0x1d, 0xe8, 0x27, 0x7a, 0x00, 0x74, 0x0f, 0xe8, 0xf0, 0x3e, 0x00,
                                      0x00, 0x83, 0xf8, 0x03, 0x74, 0x05, 0xe8, 0xb6, 0x3d, 0x00, 0x00};
constexpr uint32_t kAutomapOn = 0x007a27e8;
constexpr uint32_t kAutomapMode = 0x0045ae90;   // 3: no map to draw
constexpr uint32_t kDrawAutomap = 0x0045ad60;

uint8_t *g_base = nullptr;   // host = guest + base (WinVita's single arena)
inline uint32_t U32(uint32_t va) { uint32_t v; std::memcpy(&v, g_base + va, 4); return v; }

bool g_want = false;
uint32_t g_move_ms = 500;
// The game's frame, as the last present showed it (host pointer into guest memory).
uint8_t *g_frame = nullptr;
uint32_t g_fw = 0, g_fh = 0;
int32_t g_fstride = 0;
uint32_t g_buf = 0, g_buf_size = 0;   // the copy, then the map (guest), g_fw x g_fh
uint32_t g_seq = 0;
bool g_force = false;                 // capture at the next pass
// Where the player stood at the last capture, and when (ms).
uint32_t g_last_x = 0, g_last_y = 0;
uint64_t g_last_ms = 0;
constexpr uint32_t kPlayerUnit = 0x007a6a70;   // the player's unit, or 0
constexpr uint64_t kStillMs = 2000;            // standing still: a capture every two seconds

bool ensure_buffer()
{
    if (!g_frame || !g_fw || !g_fh)
        return false;
    const uint32_t size = g_fw * g_fh;
    if (g_buf && size <= g_buf_size)
        return true;
    if (g_buf)
        rt::va_region().free(g_buf);
    g_buf = rt::va_region().alloc(size);
    g_buf_size = g_buf ? size : 0;
    rt::logf("automap: off-screen buffer %u KiB %s", size >> 10, g_buf ? "allocated" : "refused");
    return g_buf != 0;
}

// The UI pass's call goes to a stub of guest code (below) that runs the
// game's functions as ordinary guest code: the map's first draw in a level
// waits for the game's other threads, which a guest_call from native code
// cannot do (it hung the game on the loading screen). The native parts only
// decide, copy and compare.
//
// Returns bit 0: draw the game's own map (Tab); bit 1: capture.
//
// The game's map is slow to draw (about 16 ms a draw in Azahar, a frame's
// worth of slack), so captures are rationed by time: while the player moves,
// one every g_move_ms; standing still (attacking in place too) the map
// changes little, only the markers of others moving, so one every
// kStillMs.
bool capture_due()
{
    if (!g_want)
        return false;
    const uint32_t player = U32(kPlayerUnit), path = player ? U32(player + 0x2c) : 0;
    // The path's position: subtiles, 16.16 fixed point.
    const uint32_t x = path ? U32(path) : 0, y = path ? U32(path + 4) : 0;
    const uint64_t now = rt::now_ms();
    if (!(g_seq == 0 || g_force)) {
        const bool still = x == g_last_x && y == g_last_y;
        if (now - g_last_ms < (still ? std::max<uint64_t>(kStillMs, g_move_ms) : g_move_ms))
            return false;
    }
    if (!ensure_buffer())
        return false;
    g_force = false;
    g_last_x = x;
    g_last_y = y;
    g_last_ms = now;
    return true;
}

uint32_t pass_decide(Cpu &)
{
    const bool top = U32(kAutomapOn) != 0;
    const bool capture = capture_due();
    return (top ? 1u : 0u) | (capture ? 2u : 0u);
}

uint32_t capture_begin(Cpu &)
{
    uint8_t *copy = g_base + g_buf;
    for (uint32_t y = 0; y < g_fh; y++) {
        uint8_t *f = g_frame + (ptrdiff_t)y * g_fstride;
        std::memcpy(copy + (size_t)y * g_fw, f, g_fw);
        std::memset(f, 0, g_fw);
    }
    return 0;
}

uint32_t capture_end(Cpu &)
{
    uint8_t *copy = g_base + g_buf;
    // Row by row, frame and copy trade places in whole-row copies (a byte
    // loop took about twice as long in Azahar).
    static uint8_t row[2048];
    for (uint32_t y = 0; y < g_fh; y++) {
        uint8_t *f = g_frame + (ptrdiff_t)y * g_fstride, *c = copy + (size_t)y * g_fw;
        for (uint32_t x = 0; x < g_fw; x += sizeof row) {
            const uint32_t n = g_fw - x < sizeof row ? g_fw - x : (uint32_t)sizeof row;
            std::memcpy(row, f + x, n);
            std::memcpy(f + x, c + x, n);
            std::memcpy(c + x, row, n);
        }
    }
    g_seq++;
    return 0;
}

// The stub, in the zero slack after .text's last byte (its page is mapped).
constexpr uint32_t kStub = 0x006cb5b0;
constexpr uint32_t kStubSize = 54;

void put_call(uint8_t *code, uint32_t at, uint32_t target)
{
    code[at - kStub] = 0xE8;
    const uint32_t rel = target - (at + 5);
    std::memcpy(code + (at - kStub) + 1, &rel, 4);
}

}  // namespace

bool install_automap_capture(Cpu &c)
{
    uint8_t *p = (uint8_t *)c.hostptr(0x00400000, 1);
    if (!p)
        return false;
    g_base = p - 0x00400000;
    uint8_t have[sizeof kCallSiteBytes];
    if (!c.read(kCallSite, have, sizeof have) || std::memcmp(have, kCallSiteBytes, sizeof have) != 0) {
        rt::logf("automap: bytes at %08x differ, not installed", kCallSite);
        return false;
    }
    uint8_t slack[kStubSize];
    static const uint8_t kZero[kStubSize] = {};
    if (!c.read(kStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0) {
        rt::logf("automap: no room for the stub at %08x, not installed", kStub);
        return false;
    }
    const uint32_t decide = rt::native_entry("AutomapDecide", 0, pass_decide);
    const uint32_t begin = rt::native_entry("AutomapBegin", 0, capture_begin);
    const uint32_t end = rt::native_entry("AutomapEnd", 0, capture_end);
    if (!decide || !begin || !end)
        return false;
    // S+0  call decide          eax: bit 0 the game's map, bit 1 capture
    // S+5  test eax, eax; jz S+53
    // S+9  push eax
    // S+10 call 0x45ae90; cmp eax, 3; je S+52
    // S+20 test byte [esp], 2; jz S+41
    // S+26 call begin; call 0x45ad60; call end
    // S+41 test byte [esp], 1; jz S+52
    // S+47 call 0x45ad60
    // S+52 pop eax
    // S+53 ret
    uint8_t stub[kStubSize] = {};
    put_call(stub, kStub + 0, decide);
    stub[5] = 0x85, stub[6] = 0xC0, stub[7] = 0x74, stub[8] = 53 - 9;
    stub[9] = 0x50;
    put_call(stub, kStub + 10, kAutomapMode);
    stub[15] = 0x83, stub[16] = 0xF8, stub[17] = 0x03, stub[18] = 0x74, stub[19] = 52 - 20;
    stub[20] = 0xF6, stub[21] = 0x04, stub[22] = 0x24, stub[23] = 0x02, stub[24] = 0x74, stub[25] = 41 - 26;
    put_call(stub, kStub + 26, begin);
    put_call(stub, kStub + 31, kDrawAutomap);
    put_call(stub, kStub + 36, end);
    stub[41] = 0xF6, stub[42] = 0x04, stub[43] = 0x24, stub[44] = 0x01, stub[45] = 0x74, stub[46] = 52 - 47;
    put_call(stub, kStub + 47, kDrawAutomap);
    stub[52] = 0x58;
    stub[53] = 0xC3;
    c.write(kStub, stub, sizeof stub);
    c.invalidate_code(kStub, sizeof stub);

    uint8_t site[sizeof kCallSiteBytes];
    std::memset(site, 0x90, sizeof site);   // nops up to the next instruction (0x456faa)
    site[0] = 0xE8;
    const uint32_t rel = kStub - (kCallSite + 5);
    std::memcpy(site + 1, &rel, 4);
    c.write(kCallSite, site, sizeof site);
    c.invalidate_code(kCallSite, sizeof site);
    rt::logf("automap: UI pass call site at %08x, stub at %08x", kCallSite, kStub);
    return true;
}

void automap_frame(const uint8_t *pixels, uint32_t width, uint32_t height, int32_t stride)
{
    if (!g_base || (g_frame == pixels && g_fw == width && g_fh == height && g_fstride == stride))
        return;
    g_frame = const_cast<uint8_t *>(pixels);
    g_fw = width;
    g_fh = height;
    g_fstride = stride;
    g_seq = 0;   // a capture of another size is no longer the map
}

void set_automap_capture(bool on, uint32_t move_ms)
{
    if (on && !g_want)
        g_force = true;   // shown again: the last capture may be old
    g_want = on;
    g_move_ms = move_ms;
}

void automap_refresh() { g_force = true; }

bool automap_image(AutomapImage &out)
{
    if (!g_seq || !g_buf)
        return false;
    out.pixels = g_base + g_buf;
    out.stride = (int32_t)g_fw;
    out.width = g_fw;
    out.height = g_fh;
    out.seq = g_seq;
    return true;
}

}  // namespace game
