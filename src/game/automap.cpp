// See automap.h.
//
// The map has two parts, drawn differently:
//
// The tiles (floors, walls, objects, extras) are the bulk of it, about 15 ms
// of the game's 16 ms map draw in Azahar. automap_draw() draws them itself,
// from the game's own map data, at the player's exact position: cheap enough
// for every frame, so the bottom map scrolls smoothly. Each layer is a binary
// tree of cells ordered by y; a cell names a frame of the map's DC6 cell file
// and a position in map pixels (world pixels / 10). The game draws a cell's
// frame with its bottom-left corner at (x + xoffs, y + yoffs) less the view's
// offset, opaque (draw mode 5); drawing the same way matched every pixel of a
// capture of the game's own map in the lane.
//
// The units (the white crosses of NPCs and other players, names, waypoint
// labels) need more of the game than that: they are captured by letting the
// game draw them, alone, into its own frame, now and then (set_automap_
// capture's interval): just before, the part of the frame they will cover
// is copied aside and cleared to index 0; every pixel the game drew is kept
// as a unit pixel; the frame gets its copy back, so the top screen shows no
// map unless the player asked for one (Tab). The game is told to centre that
// drawing on a small region at the frame's top-left corner, the size of the
// bottom map with a margin, so it draws only units near the player (about a
// millisecond). Between captures the unit pixels move with the map. The
// player's own cross is left out of the capture (the game's units pass skips
// the player while capturing) and drawn here every frame, at the centre.
// (The sprite and line drawers do not take their target from the tile
// renderer's frame base, 0x7d5448: pointing that elsewhere left the map in
// the frame, hence the copy aside.)
#include "game/automap.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"
#include "runtime/rt_layout.h"

#include "runtime/guest_region.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace d2rt;

namespace game {
namespace {

constexpr uint32_t kCallSite = 0x00456f93;
// cmp [0x7a27e8], ebx; je +0f; call 0x45ae90; cmp eax, 3; je +05; call 0x45ad60
constexpr uint8_t kCallSiteBytes[] = {0x39, 0x1d, 0xe8, 0x27, 0x7a, 0x00, 0x74, 0x0f, 0xe8, 0xf0, 0x3e, 0x00,
                                      0x00, 0x83, 0xf8, 0x03, 0x74, 0x05, 0xe8, 0xb6, 0x3d, 0x00, 0x00};
constexpr uint32_t kAutomapOn = 0x007a27e8;     // ui[0x0A], the map toggle (Tab)
constexpr uint32_t kAutomapMode = 0x0045ae90;   // 3: no map to draw
constexpr uint32_t kDrawAutomap = 0x0045ad60;   // the whole map, as Tab shows it
constexpr uint32_t kMapSetup = 0x00457520;      // the map's bounds and offsets for this frame
constexpr uint32_t kDrawUnits = 0x0045ac90;     // the units' crosses and names
// 0x45a860(eax = unit): one unit's cross and name. Its first instructions
// (push ebp; mov ebp, esp; sub esp, 0x14), moved into the unit filter.
constexpr uint32_t kDrawUnit = 0x0045a860;
constexpr uint8_t kDrawUnitBytes[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14, 0x56};
constexpr uint32_t kMapAvailable = 0x007a0634;  // 0: no map (the game's own check)
constexpr uint32_t kPlayerUnit = 0x007a6a70;    // the player's unit, or 0

// The map's state (D2Client's automap globals).
constexpr uint32_t kLayer = 0x007a5164;         // the level's map layer
constexpr uint32_t kCellFile = 0x007a5168;      // the map's cells (MaxiMap.dc6)
constexpr uint32_t kExtraCellFiles = 0x007a516c;// three cell files for the extras layer
constexpr uint32_t kOffsetX = 0x007a5198, kOffsetY = 0x007a519c;   // map pixel at the view's corner
constexpr uint32_t kUnitBounds = 0x007a51c8;    // x0, y0, x1, y1: units drawn inside
constexpr uint32_t kPlayerColour = 0x007a51b0;  // the player's cross

// The stub and the unit filter, in the zero slack after .text's last byte
// (0x6cb5a1), in its last mapped page; the flag the filter reads after them.
constexpr uint32_t kStub = 0x006cb5b0, kStubSize = 59;
constexpr uint32_t kFilter = 0x006cb600, kFilterSize = 29;
constexpr uint32_t kCapturing = 0x006cb640;
constexpr uint32_t kSlackEnd = 0x006cb650;

uint8_t *g_base = nullptr;   // host = guest + base (WinVita's single arena)
uint32_t g_span = 0;         // guest addresses below this are readable
inline bool readable(uint32_t va, uint32_t n) { return va >= 0x10000 && (uint64_t)va + n <= g_span; }
inline uint32_t U32(uint32_t va) { uint32_t v; std::memcpy(&v, g_base + va, 4); return v; }
inline int16_t I16(uint32_t va) { int16_t v; std::memcpy(&v, g_base + va, 2); return v; }
inline void W32(uint32_t va, uint32_t v) { std::memcpy(g_base + va, &v, 4); }

// ---- where the player is ----------------------------------------------------------------

// The point the game centres the player's cross on, in map pixels (the
// cells' coordinates): the player's world pixel / 10, plus (8, -8). The
// path's position is in subtiles, 16.16 fixed point; a subtile is 32 x 16
// world pixels on the diagonals.
bool player_point(int32_t &x, int32_t &y)
{
    const uint32_t player = U32(kPlayerUnit);
    if (!player || !readable(player + 0x2c, 4))
        return false;
    const uint32_t path = U32(player + 0x2c);
    if (!readable(path, 8))
        return false;
    const int64_t sx = (int32_t)U32(path), sy = (int32_t)U32(path + 4);
    // world x = (sx - sy) * 16, y = (sx + sy) * 8, both / 65536; then / 10.
    const int64_t wx = (sx - sy) * 16, wy = (sx + sy) * 8;
    x = (int32_t)(wx / (65536 * 10)) + 8;
    y = (int32_t)(wy / (65536 * 10)) - 8;
    return true;
}

// The game's checks before it draws the map: a map, a player, the player in a room.
bool map_ready()
{
    if (!U32(kMapAvailable) || !U32(kLayer) || !U32(kCellFile))
        return false;
    const uint32_t player = U32(kPlayerUnit);
    if (!player || !readable(player + 0x2c, 4))
        return false;
    const uint32_t path = U32(player + 0x2c);
    return readable(path + 0x1c, 4) && U32(path + 0x1c) != 0;
}

// ---- the units, captured ----------------------------------------------------------------

bool g_want = false;
uint32_t g_move_ms = 500;
int g_scale = 1;
// The game's frame, as the last present showed it (host pointer into guest memory).
uint8_t *g_frame = nullptr;
uint32_t g_fw = 0, g_fh = 0;
int32_t g_fstride = 0;
uint32_t g_buf = 0, g_buf_size = 0;   // the frame's part copied aside (guest)
uint32_t g_seq = 0;                   // captures since the frame or the scale changed
bool g_force = false;
uint32_t g_last_x = 0, g_last_y = 0;  // the path's position at the last capture
uint64_t g_last_ms = 0;
constexpr uint64_t kStillMs = 2000;   // standing still: a capture every two seconds

// The capture's region: the bottom map's view in map pixels with a margin,
// centred on the player, at the frame's top-left corner; the game draws a
// name centred over its unit, so the part cleared reaches further right and
// down than the region.
constexpr int kViewW = 320, kViewH = 222;   // the bottom map (second_screen.h)
constexpr int kMargin = 48;
int g_rw = 0, g_rh = 0, g_aw = 0, g_ah = 0;
int32_t g_cap_x = 0, g_cap_y = 0;     // the player's point at the capture
struct UnitPixel {
    int16_t x, y;                     // from g_cap_x/y, map pixels
    uint8_t p;
};
std::vector<UnitPixel> g_units;
uint32_t g_saved[6];                  // the game's offsets and unit bounds

bool ensure_buffer(uint32_t size)
{
    if (g_buf && size <= g_buf_size)
        return true;
    if (g_buf)
        rt::va_region().free(g_buf);
    g_buf = rt::va_region().alloc(size);
    g_buf_size = g_buf ? size : 0;
    rt::logf("automap: capture buffer %u KiB %s", size >> 10, g_buf ? "allocated" : "refused");
    return g_buf != 0;
}

// Captures are rationed by time: one every g_move_ms while the player moves,
// one every kStillMs standing still (attacking in place too: the units move,
// little else), and one at once when the map is shown again, its size
// changes or the player is about to leave the last capture's margin.
bool capture_due()
{
    if (!g_want || !g_frame || !map_ready())
        return false;
    const uint32_t player = U32(kPlayerUnit), path = U32(player + 0x2c);
    const uint32_t x = U32(path), y = U32(path + 4);
    const uint64_t now = rt::now_ms();
    if (!(g_seq == 0 || g_force)) {
        int32_t px, py;
        const bool away = player_point(px, py) &&
                          (std::abs(px - g_cap_x) > kMargin * g_scale / 2 || std::abs(py - g_cap_y) > kMargin * g_scale / 2);
        const bool still = x == g_last_x && y == g_last_y;
        if (!away && now - g_last_ms < (still ? std::max<uint64_t>(kStillMs, g_move_ms) : g_move_ms))
            return false;
    }
    g_rw = std::min<int>((int)g_fw, (kViewW + 2 * kMargin) * g_scale);
    g_rh = std::min<int>((int)g_fh, (kViewH + 2 * kMargin) * g_scale);
    g_aw = std::min<int>((int)g_fw, g_rw + 96);
    g_ah = std::min<int>((int)g_fh, g_rh + 16);
    if (!ensure_buffer((uint32_t)(g_aw * g_ah)))
        return false;
    g_force = false;
    g_last_x = x;
    g_last_y = y;
    g_last_ms = now;
    return true;
}

// The UI pass's call goes to a stub of guest code (below) that runs the
// game's functions as ordinary guest code: the map's first draw in a level
// waits for the game's other threads, which a guest_call from native code
// cannot do (it hung the game on the loading screen). The native parts only
// decide, copy and compare.
//
// Returns bit 0: draw the game's own map (Tab); bit 1: capture the units.
uint32_t pass_decide(Cpu &)
{
    const bool top = U32(kAutomapOn) != 0;
    const bool capture = capture_due();
    return (top ? 1u : 0u) | (capture ? 2u : 0u);
}

// After the game's map setup (kMapSetup), before its units pass.
uint32_t capture_begin(Cpu &)
{
    player_point(g_cap_x, g_cap_y);
    for (int i = 0; i < 2; i++)
        g_saved[i] = U32(kOffsetX + 4 * i);
    for (int i = 0; i < 4; i++)
        g_saved[2 + i] = U32(kUnitBounds + 4 * i);
    W32(kOffsetX, (uint32_t)(g_cap_x - g_rw / 2));
    W32(kOffsetY, (uint32_t)(g_cap_y - g_rh / 2));
    const int32_t bounds[4] = {-8, -8, g_rw + 8, g_rh + 8};
    for (int i = 0; i < 4; i++)
        W32(kUnitBounds + 4 * i, (uint32_t)bounds[i]);
    uint8_t *copy = g_base + g_buf;
    for (int y = 0; y < g_ah; y++) {
        uint8_t *f = g_frame + (ptrdiff_t)y * g_fstride;
        std::memcpy(copy + (size_t)y * g_aw, f, (size_t)g_aw);
        std::memset(f, 0, (size_t)g_aw);
    }
    g_base[kCapturing] = 1;
    return 0;
}

uint32_t capture_end(Cpu &)
{
    g_base[kCapturing] = 0;
    for (int i = 0; i < 2; i++)
        W32(kOffsetX + 4 * i, g_saved[i]);
    for (int i = 0; i < 4; i++)
        W32(kUnitBounds + 4 * i, g_saved[2 + i]);
    g_units.clear();
    const uint8_t *copy = g_base + g_buf;
    for (int y = 0; y < g_ah; y++) {
        uint8_t *f = g_frame + (ptrdiff_t)y * g_fstride;
        for (int x = 0; x < g_aw; x++)
            if (f[x])
                g_units.push_back({(int16_t)(x - g_rw / 2), (int16_t)(y - g_rh / 2), f[x]});
        std::memcpy(f, copy + (size_t)y * g_aw, (size_t)g_aw);
    }
    g_seq++;
    return 0;
}

void put_call(uint8_t *code, uint32_t base, uint32_t at, uint32_t target, uint8_t op = 0xE8)
{
    code[at - base] = op;
    const uint32_t rel = target - (at + 5);
    std::memcpy(code + (at - base) + 1, &rel, 4);
}

// ---- the tiles, drawn here --------------------------------------------------------------

// A cell's frame, decoded: its runs of pixels, top row first (most of a
// frame is empty: a floor cell's 16 x 32 holds a few dozen pixels).
struct Sprite {
    struct Run {
        int16_t row, x, len;
        uint32_t at;                  // into px
    };
    int w = 0, h = 0, xo = 0, yo = 0;
    std::vector<Run> runs;
    std::vector<uint8_t> px;
};
std::unordered_map<uint32_t, Sprite> g_sprites;   // by the frame's guest address
uint32_t g_sprites_file = 0;                      // the cell file they came from

// The map draws the ground as single dots on every other pixel of every
// other row, black between. At 1:1 every step of the map turns each of those
// pixels on or off, which reads as a dark flash over the whole ground. Each
// such dot (nothing on its sides, above or below) also colours the black
// pixels around it, diagonals included: the ground shows solid, as at half
// size, while lines and the names' letters keep their edges.
void fill_dots(Sprite &s)
{
    std::vector<uint8_t> out = s.px;
    auto at = [&](int x, int y) -> uint8_t { return x < 0 || y < 0 || x >= s.w || y >= s.h ? 0 : s.px[(size_t)y * s.w + x]; };
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
            const uint8_t p = at(x, y);
            if (!p || at(x - 1, y) || at(x + 1, y) || at(x, y - 1) || at(x, y + 1))
                continue;
            for (int j = std::max(0, y - 1); j <= std::min(s.h - 1, y + 1); j++)
                for (int i = std::max(0, x - 1); i <= std::min(s.w - 1, x + 1); i++)
                    if (!out[(size_t)j * s.w + i])
                        out[(size_t)j * s.w + i] = p;
        }
    s.px.swap(out);
}

// A GfxCell: flags, width, height, x and y offset, ?, ?, length, then DC6
// run-length data, bottom row first: 0x80 ends a row, 0x80 | n skips n
// pixels, n < 0x80 is followed by n pixels.
const Sprite *sprite(uint32_t cell)
{
    auto it = g_sprites.find(cell);
    if (it != g_sprites.end())
        return &it->second;
    if (g_sprites.size() > 600)
        g_sprites.clear();
    Sprite &s = g_sprites[cell];
    if (!readable(cell, 0x20))
        return &s;
    const uint32_t w = U32(cell + 4), h = U32(cell + 8), len = U32(cell + 0x1c);
    if (w == 0 || h == 0 || w > 512 || h > 512 || len > 512 * 512 || !readable(cell + 0x20, len))
        return &s;
    s.w = (int)w;
    s.h = (int)h;
    s.xo = (int32_t)U32(cell + 0x0c);
    s.yo = (int32_t)U32(cell + 0x10);
    s.px.assign((size_t)w * h, 0);
    const uint8_t *d = g_base + cell + 0x20, *end = d + len;
    int x = 0, row = s.h - 1;
    while (d < end && row >= 0) {
        const uint8_t b = *d++;
        if (b == 0x80) {
            x = 0;
            row--;
        } else if (b & 0x80) {
            x += b & 0x7f;
        } else {
            for (int i = 0; i < b && d < end; i++, x++) {
                const uint8_t p = *d++;
                if (x < s.w)
                    s.px[(size_t)row * s.w + x] = p;
            }
        }
    }
    fill_dots(s);
    // The runs, and only their pixels kept.
    std::vector<uint8_t> px;
    for (int r = 0; r < s.h; r++)
        for (int x = 0; x < s.w;) {
            if (!s.px[(size_t)r * s.w + x]) {
                x++;
                continue;
            }
            const int x0 = x;
            while (x < s.w && s.px[(size_t)r * s.w + x])
                x++;
            s.runs.push_back({(int16_t)r, (int16_t)x0, (int16_t)(x - x0), (uint32_t)px.size()});
            px.insert(px.end(), s.px.begin() + (ptrdiff_t)r * s.w + x0, s.px.begin() + (ptrdiff_t)r * s.w + x);
        }
    s.px.swap(px);
    return &s;
}

// The view: map pixels around the player's point (cx, cy) into w x h at
// 1:1 or half size.
struct View {
    uint8_t *out;
    int w, h, scale;
    int32_t cx, cy;
    int vx(int32_t mx) const { return (scale == 1 ? mx - cx : floordiv(mx - cx)) + w / 2; }
    int vy(int32_t my) const { return (scale == 1 ? my - cy : floordiv(my - cy)) + h / 2; }
    static int floordiv(int32_t v) { return v >> 1; }   // arithmetic shift: floor
    void put(int32_t mx, int32_t my, uint8_t p) const
    {
        const int x = vx(mx), y = vy(my);
        if (x >= 0 && y >= 0 && x < w && y < h)
            out[(size_t)y * w + x] = p;
    }
};

void draw_sprite(const View &v, const Sprite &s, int32_t x0, int32_t bottom)
{
    const int32_t top = bottom - s.h + 1;
    if (v.vx(x0 + s.w - 1) < 0 || v.vx(x0) >= v.w || v.vy(bottom) < 0 || v.vy(top) >= v.h)
        return;
    for (const Sprite::Run &r : s.runs) {
        const int vy = v.vy(top + r.row);
        if (vy < 0 || vy >= v.h)
            continue;
        uint8_t *dst = v.out + (size_t)vy * v.w;
        const uint8_t *src = &s.px[r.at];
        if (v.scale == 1) {
            int vx = v.vx(x0 + r.x), n = r.len;
            if (vx < 0) {
                src -= vx;
                n += vx;
                vx = 0;
            }
            if (vx + n > v.w)
                n = v.w - vx;
            if (n > 0)
                std::memcpy(dst + vx, src, (size_t)n);
        } else {
            for (int c = 0; c < r.len; c++) {
                const int vx = v.vx(x0 + r.x + c);
                if (vx >= 0 && vx < v.w)
                    dst[vx] = src[c];
            }
        }
    }
}

// One layer's tree, in the game's order (by y), only the cells whose frame
// can reach the view: left (+0x0c) holds smaller y, right (+0x10) larger.
void draw_layer(const View &v, uint32_t root, uint32_t file, int32_t ymin, int32_t ymax, int32_t xmin, int32_t xmax)
{
    if (!readable(file, 0x18))
        return;
    const uint32_t cells = U32(file + 0x14) * std::max<uint32_t>(1, U32(file + 0x10));
    std::vector<uint32_t> stack;
    uint32_t node = root;
    int budget = 100000;   // a damaged tree must not hang the game
    while ((node || !stack.empty()) && budget-- > 0) {
        while (node && budget-- > 0) {
            if (!readable(node, 0x14))
                return;
            if (I16(node + 8) >= ymin) {
                stack.push_back(node);
                node = U32(node + 0x0c);
            } else {
                node = U32(node + 0x10);
            }
        }
        if (stack.empty())
            break;
        node = stack.back();
        stack.pop_back();
        const int32_t y = I16(node + 8);
        if (y > ymax)
            break;
        const int32_t x = I16(node + 6);
        const uint32_t no = (uint16_t)U32(node + 4);
        if (x >= xmin && x <= xmax && no < cells && readable(file + 0x18 + 4 * no, 4)) {
            const Sprite *s = sprite(U32(file + 0x18 + 4 * no));
            if (s->w)
                draw_sprite(v, *s, x + s->xo, y + s->yo);
        }
        node = U32(node + 0x10);
    }
}

// The game's cross (0x45a7f0): 12 lines through 13 points, doubled.
void draw_cross(const View &v, uint8_t colour)
{
    static const int8_t kPoints[13][2] = {{0, -1}, {2, -2}, {4, -1}, {2, 0}, {4, 1}, {2, 2}, {0, 1},
                                          {-2, 2}, {-4, 1}, {-2, 0}, {-4, -1}, {-2, -2}, {0, -1}};
    const int x0 = v.w / 2, y0 = v.h / 2;
    for (int i = 0; i < 12; i++) {
        int x = x0 + kPoints[i][0] * 2, y = y0 + kPoints[i][1] * 2;
        const int x1 = x0 + kPoints[i + 1][0] * 2, y1 = y0 + kPoints[i + 1][1] * 2;
        const int dx = std::abs(x1 - x), dy = -std::abs(y1 - y), sx = x < x1 ? 1 : -1, sy = y < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            if (x >= 0 && y >= 0 && x < v.w && y < v.h)
                v.out[(size_t)y * v.w + x] = colour;
            if (x == x1 && y == y1)
                break;
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y += sy;
            }
        }
    }
}

void put_stub(Cpu &c, uint32_t at, const uint8_t *code, uint32_t n)
{
    c.write(at, code, n);
    c.invalidate_code(at, n);
}

}  // namespace

bool install_automap_capture(Cpu &c)
{
    uint8_t *p = (uint8_t *)c.hostptr(0x00400000, 1);
    if (!p)
        return false;
    g_base = p - 0x00400000;
    g_span = rt::layout().span;
    uint8_t have[sizeof kCallSiteBytes];
    if (!c.read(kCallSite, have, sizeof have) || std::memcmp(have, kCallSiteBytes, sizeof have) != 0) {
        rt::logf("automap: bytes at %08x differ, not installed", kCallSite);
        return false;
    }
    uint8_t unit[sizeof kDrawUnitBytes];
    if (!c.read(kDrawUnit, unit, sizeof unit) || std::memcmp(unit, kDrawUnitBytes, sizeof unit) != 0) {
        rt::logf("automap: bytes at %08x differ, not installed", kDrawUnit);
        return false;
    }
    uint8_t slack[kSlackEnd - kStub];
    static const uint8_t kZero[kSlackEnd - kStub] = {};
    if (!c.read(kStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0) {
        rt::logf("automap: no room for the stub at %08x, not installed", kStub);
        return false;
    }
    const uint32_t decide = rt::native_entry("AutomapDecide", 0, pass_decide);
    const uint32_t begin = rt::native_entry("AutomapBegin", 0, capture_begin);
    const uint32_t end = rt::native_entry("AutomapEnd", 0, capture_end);
    if (!decide || !begin || !end)
        return false;
    // S+0  call decide          eax: bit 0 the game's map, bit 1 the units
    // S+5  test eax, eax; jz S+58
    // S+9  push eax
    // S+10 call 0x45ae90; cmp eax, 3; je S+57
    // S+20 test byte [esp], 2; jz S+46
    // S+26 call 0x457520; call begin; call 0x45ac90; call end
    // S+46 test byte [esp], 1; jz S+57
    // S+52 call 0x45ad60
    // S+57 pop eax
    // S+58 ret
    uint8_t stub[kStubSize] = {};
    put_call(stub, kStub, kStub + 0, decide);
    stub[5] = 0x85, stub[6] = 0xC0, stub[7] = 0x74, stub[8] = 58 - 9;
    stub[9] = 0x50;
    put_call(stub, kStub, kStub + 10, kAutomapMode);
    stub[15] = 0x83, stub[16] = 0xF8, stub[17] = 0x03, stub[18] = 0x74, stub[19] = 57 - 20;
    stub[20] = 0xF6, stub[21] = 0x04, stub[22] = 0x24, stub[23] = 0x02, stub[24] = 0x74, stub[25] = 46 - 26;
    put_call(stub, kStub, kStub + 26, kMapSetup);
    put_call(stub, kStub, kStub + 31, begin);
    put_call(stub, kStub, kStub + 36, kDrawUnits);
    put_call(stub, kStub, kStub + 41, end);
    stub[46] = 0xF6, stub[47] = 0x04, stub[48] = 0x24, stub[49] = 0x01, stub[50] = 0x74, stub[51] = 57 - 52;
    put_call(stub, kStub, kStub + 52, kDrawAutomap);
    stub[57] = 0x58;
    stub[58] = 0xC3;
    put_stub(c, kStub, stub, sizeof stub);

    // The unit filter, in front of 0x45a860: while capturing, the player's
    // own unit is skipped (its cross is drawn by automap_draw).
    // F+0  cmp byte [kCapturing], 0; je F+18
    // F+9  cmp eax, [0x7a6a70]; jne F+18
    // F+17 ret
    // F+18 push ebp; mov ebp, esp; sub esp, 0x14; jmp 0x45a866
    uint8_t filter[kFilterSize] = {0x80, 0x3D};
    std::memcpy(filter + 2, &kCapturing, 4);
    filter[6] = 0x00, filter[7] = 0x74, filter[8] = 18 - 9;
    filter[9] = 0x3B, filter[10] = 0x05;
    std::memcpy(filter + 11, &kPlayerUnit, 4);
    filter[15] = 0x75, filter[16] = 18 - 17;
    filter[17] = 0xC3;
    std::memcpy(filter + 18, kDrawUnitBytes, 6);
    put_call(filter, kFilter, kFilter + 24, kDrawUnit + 6, 0xE9);
    put_stub(c, kFilter, filter, sizeof filter);
    uint8_t jump[6];
    put_call(jump, kDrawUnit, kDrawUnit, kFilter, 0xE9);
    jump[5] = 0x90;
    put_stub(c, kDrawUnit, jump, sizeof jump);

    uint8_t site[sizeof kCallSiteBytes];
    std::memset(site, 0x90, sizeof site);   // nops up to the next instruction (0x456faa)
    put_call(site, kCallSite, kCallSite, kStub);
    put_stub(c, kCallSite, site, sizeof site);
    rt::logf("automap: UI pass call site at %08x, stub at %08x, unit filter at %08x", kCallSite, kStub, kFilter);
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

void set_automap_capture(bool on, uint32_t move_ms, int scale)
{
    if (on && !g_want)
        g_force = true;   // shown again: the last capture may be old
    if (scale != g_scale)
        g_force = true;   // the region's size changes with it
    g_want = on;
    g_move_ms = move_ms;
    g_scale = scale == 2 ? 2 : 1;
}

uint64_t automap_state(int scale)
{
    int32_t x = 0, y = 0;
    if (!g_base || !map_ready() || !player_point(x, y))
        return 0;
    if (scale == 2) {
        x >>= 1;
        y >>= 1;
    }
    return ((uint64_t)(uint16_t)x << 48) ^ ((uint64_t)(uint16_t)y << 32) ^ ((uint64_t)g_seq << 8) ^ U32(kLayer) ^
           (uint64_t)scale;
}

bool automap_draw(uint8_t *out, int w, int h, int scale)
{
    if (!g_base || !map_ready())
        return false;
    View v{out, w, h, scale == 2 ? 2 : 1, 0, 0};
    if (!player_point(v.cx, v.cy))
        return false;
    const uint32_t layer = U32(kLayer), file = U32(kCellFile);
    if (!readable(layer, 0x18) || !readable(file, 0x18))
        return false;
    if (file != g_sprites_file) {
        g_sprites.clear();
        g_sprites_file = file;
    }
    // The map pixels the view shows, and a cell's reach beyond them: a
    // frame's bottom row is at the cell's y (taller frames reach up from it).
    const int32_t hw = w / 2 * v.scale, hh = h / 2 * v.scale;
    const int32_t xmin = v.cx - hw - 512, xmax = v.cx + hw + 16;
    const int32_t ymin = v.cy - hh - 16, ymax = v.cy + hh + 512;
    for (int k = 0; k < 4; k++) {
        uint32_t f = file;
        if (k == 3) {
            // The extras' cell file by the layer's kind (0x459908).
            const uint32_t kind = U32(layer + 4);
            if (kind >= 1 && kind <= 3)
                f = U32(kExtraCellFiles + 4 * (kind - 1));
        }
        if (const uint32_t root = U32(layer + 8 + 4 * k))
            draw_layer(v, root, f, ymin, ymax, xmin, xmax);
    }
    // The units, from the last capture, where the map has moved them.
    if (g_seq)
        for (const UnitPixel &u : g_units)
            v.put(g_cap_x + u.x, g_cap_y + u.y, u.p);
    draw_cross(v, g_base[kPlayerColour]);
    return true;
}

}  // namespace game
