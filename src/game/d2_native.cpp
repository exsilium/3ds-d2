// Native replacements for Diablo II 1.14d's hottest drawing routines and
// its per-frame lighting (the light-occlusion grid and its sampling).
//
// The game draws floors and walls in software, almost entirely in a few leaf
// routines that remap pixels through a 256-byte shade table. Translated to
// ARM they cost ~10 instructions per pixel (each x86 memory operand becomes
// address arithmetic + the arena offset + the access); written natively they
// cost ~3. Each replacement reproduces the original's effect byte for byte,
// quirks included, from a specification of the disassembly: the Linux game
// lane checks that every frame of a scripted session hashes the same with
// and without them (M4_FRAMEHASH, M4_NATIVE=0).
//
// Calling convention of all of them: MSVC fastcall (ecx, edx, then stack
// arguments popped by the callee), which rt::hook_function maps directly.
// No caller reads EAX afterwards; callee-saved registers are untouched here.
#include "game/d2_native.h"
#include "game/pklib_explode.h"

#include "runtime/cpu.h"
#include "runtime/rt_host.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace d2rt;

namespace game {
namespace {

// Guest memory is one block: host = guest + base (WinVita's single arena).
uint8_t *g_base = nullptr;
inline uint8_t *H(uint32_t va) { return g_base + va; }
inline int32_t I32(uint32_t va) { int32_t v; std::memcpy(&v, H(va), 4); return v; }
inline uint32_t U32(uint32_t va) { uint32_t v; std::memcpy(&v, H(va), 4); return v; }

// Globals of the tile renderer (see the specification).
constexpr uint32_t kPitch = 0x007d544c;       // destination row pitch
constexpr uint32_t kFbBase = 0x007d5448;      // framebuffer base
constexpr uint32_t kRowOff = 0x007c9828;      // [y*4] = y*pitch, y in [-32, 700)
constexpr uint32_t kClipLeft = 0x007ca318, kClipRight = 0x007d233c;
constexpr uint32_t kClipTop = 0x007d2334, kClipBottom = 0x007c97a0;
constexpr uint32_t kIsoXoff = 0x0072db48, kIsoWidth = 0x0072db84, kIsoSoff = 0x0072dbc0;   // constant tables

// The iso floor block's row shape, read from the game's own constant tables.
int32_t g_xoff[15], g_width[15], g_soff[15];

inline void remap(uint8_t *d, const uint8_t *s, const uint8_t *tbl, int32_t n)
{
    int32_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const uint8_t a = tbl[s[i]], b = tbl[s[i + 1]], c = tbl[s[i + 2]], e = tbl[s[i + 3]];
        d[i] = a;
        d[i + 1] = b;
        d[i + 2] = c;
        d[i + 3] = e;
    }
    for (; i < n; i++)
        d[i] = tbl[s[i]];
}

inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

// 4f69b0 FloorIsoFlat(ecx=src 256-byte iso block, edx=dst, [8]=tbl), ret 4.
void iso_flat(const uint8_t *src, uint8_t *dst, const uint8_t *tbl)
{
    const int32_t pitch = I32(kPitch);
    for (int r = 0; r < 15; r++)
        remap(dst + (intptr_t)r * pitch + g_xoff[r], src + g_soff[r], tbl, g_width[r]);
}

uint32_t floor_iso_flat(Cpu &c)
{
    iso_flat(H(c.reg(R_ECX)), H(c.reg(R_EDX)), H(c.arg(0)));
    return 0;
}

// 4f6f60 RleFlat(ecx=src stream, edx=dst, [8]=tbl, [c]=rows), ret 8.
// Pairs (skip, run); (0,0) ends a row. A run of 0 or over 32 draws nothing
// but still advances (the original's 32-entry jump table).
void rle_flat_draw(const uint8_t *src, uint8_t *rowp, const uint8_t *tbl, uint32_t rows)
{
    uint8_t *d = rowp;
    const int32_t pitch = I32(kPitch);
    while (rows != 0) {
        const uint8_t skip = src[0], run = src[1];
        src += 2;
        if (skip == 0 && run == 0) {
            rows--;
            rowp += pitch;
            d = rowp;
            continue;
        }
        d += skip;
        if (run >= 1 && run <= 32)
            remap(d, src, tbl, run);
        d += run;
        src += run;
    }
}

uint32_t rle_flat(Cpu &c)
{
    rle_flat_draw(H(c.reg(R_ECX)), H(c.reg(R_EDX)), H(c.arg(0)), c.arg(1));
    return 0;
}

// 4f7580 RleFlatClipped(ecx=x0, edx=y0, [8]=src, [c]=dst, [10]=tbl, [14]=rows), ret 0x10.
void rle_flat_clipped_draw(int32_t x0, int32_t y, const uint8_t *src, uint8_t *rowp, const uint8_t *tbl, int32_t rows)
{
    uint8_t *d = rowp;
    if (rows <= 0)
        return;
    const int32_t pitch = I32(kPitch);
    const int32_t clip_l = I32(kClipLeft), clip_r = I32(kClipRight);
    const int32_t clip_t = I32(kClipTop), clip_b = I32(kClipBottom);
    int32_t i = 0;
    do {
        const uint8_t skip = src[0], run = src[1];
        src += 2;
        if (skip == 0 && run == 0) {
            i++;
            y++;
            rowp += pitch;
            d = rowp;
            continue;
        }
        if (y >= clip_b)   // checked on non-end-of-row pairs only, before the skip
            return;
        d += skip;
        if (y >= clip_t) {
            const int32_t x = x0 + (int32_t)(d - rowp);
            const int32_t l = imax(0, clip_l - x);
            const int32_t r = imin(imax(0, clip_r - x), (int32_t)run);
            const int32_t n = r - l;
            if (n >= 1 && n <= 32)   // the limit applies to the clipped count
                remap(d + l, src + l, tbl, n);
        }
        d += run;
        src += run;
    } while (i < rows);
}

uint32_t rle_flat_clipped(Cpu &c)
{
    rle_flat_clipped_draw((int32_t)c.reg(R_ECX), (int32_t)c.reg(R_EDX), H(c.arg(0)), H(c.arg(1)), H(c.arg(2)),
                          (int32_t)c.arg(3));
    return 0;
}

// 4f7ea0 WallBlockCopy(ecx=src stream, edx=x, [8]=y), ret 4: a 32-row RLE
// wall block copied as is (memcpy runs, no 32-pixel limit), clipped unless
// it lies fully inside the window.
uint32_t wall_block_copy(Cpu &c)
{
    const uint8_t *src = H(c.reg(R_ECX));
    const int32_t x = (int32_t)c.reg(R_EDX), y = (int32_t)c.arg(0);
    if (y < I32(0x007d232c) || y >= I32(0x007c978c) || x < I32(0x007c9794) || x >= I32(0x007d2324))
        return 0;   // trivially rejected
    const uint32_t fb = (uint32_t)I32(kFbBase);
    if (!fb)
        return 0;   // the original asserts here (never happens in play)
    const int32_t pitch = I32(kPitch);
    uint8_t *rowp = H(fb + (uint32_t)I32(kRowOff + 4 * y) + (uint32_t)x), *d = rowp;
    if (y >= I32(0x007ca31c) && y < I32(0x007c9790) && x >= I32(0x007d2328) && x < I32(0x007c9784)) {
        int32_t rows = 32;
        while (rows != 0) {
            const uint8_t skip = src[0], run = src[1];
            src += 2;
            if (skip == 0 && run == 0) {
                rows--;
                rowp += pitch;
                d = rowp;
                continue;
            }
            d += skip;
            std::memcpy(d, src, run);
            d += run;
            src += run;
        }
        return 0;
    }
    const int32_t clip_l = I32(kClipLeft), clip_r = I32(kClipRight);
    const int32_t clip_t = I32(kClipTop), clip_b = I32(kClipBottom);
    int32_t i = 0, yy = y;
    do {
        const uint8_t skip = src[0], run = src[1];
        src += 2;
        if (skip == 0 && run == 0) {
            i++;
            yy++;
            rowp += pitch;
            d = rowp;
            continue;
        }
        if (yy >= clip_b)
            return 0;
        d += skip;
        if (yy >= clip_t) {
            const int32_t xx = x + (int32_t)(d - rowp);
            const int32_t l = imax(0, clip_l - xx);
            const int32_t r = imin(imax(0, clip_r - xx), (int32_t)run);
            if (r - l > 0)
                std::memcpy(d + l, src + l, (size_t)(r - l));
        }
        d += run;
        src += run;
    } while (i < 32);
    return 0;
}

// ---- DC6 sprites ------------------------------------------------------------------
//
// Stream of signed bytes: 0x80 ends a row; 0x81..0xff skip (b & 0x7f) pixels;
// 0x00..0x7f are a literal run of that many bytes, which follow. Rows step by
// a signed pitch (DC6 is stored bottom-up). On end of row the row count is
// decremented, then tested, as in the original.
//
// The game depends on what these leave in EAX: 608540 returns it, and so
// does 6014c0 above it (frames diverge when 606d40 returns 0). So each one
// tracks the value the original's loop leaves there, from its disassembly.
// "lo0" is the end: the final 0x80 byte is loaded into AL and masked to 0.

struct OpCopy {
    void operator()(uint8_t *d, const uint8_t *s, int32_t n) const { std::memcpy(d, s, (size_t)n); }
};
struct OpBlend {   // d = T2[d*256 + T1[s]]
    const uint8_t *t1, *t2;
    void operator()(uint8_t *d, const uint8_t *s, int32_t n) const
    {
        for (int32_t i = 0; i < n; i++)
            d[i] = t2[d[i] * 256 + t1[s[i]]];
    }
};

// 606d40(ecx=src, edx=dst, [8]=rows, [c]=pitch), ret 8: copy.
// EAX: pitch; next-row pointer on each row; memcpy's return (the literal's
// destination) on each literal; skips leave it; lo0 at the end.
uint32_t dc6_copy(Cpu &c)
{
    const uint8_t *src = H(c.reg(R_ECX));
    uint32_t dst = c.reg(R_EDX), rows = c.arg(0);
    const uint32_t pitch = c.arg(1);
    uint32_t next = dst + pitch, eax = pitch;
    for (;;) {
        const uint8_t b = *src++;
        if (b == 0x80) {
            if (--rows == 0)
                return eax & ~0xffu;
            dst = next;
            next += pitch;
            eax = next;
        } else if (b & 0x80) {
            dst += b & 0x7f;
        } else {
            std::memcpy(H(dst), src, b);
            eax = dst;
            src += b;
            dst += b;
        }
    }
}

// 606e40(.., [10]=T1, [14]=T2), ret 0x10: d = T2[d*256 + T1[s]].
// EAX: dst + pitch; next-row pointer on each row; a skip or a literal leaves
// a value below 256 (cleared by lo0); lo0 at the end.
uint32_t dc6_blend(Cpu &c)
{
    const uint8_t *src = H(c.reg(R_ECX));
    uint32_t dst = c.reg(R_EDX), rows = c.arg(0);
    const uint32_t pitch = c.arg(1);
    const OpBlend op{H(c.arg(2)), H(c.arg(3))};
    uint32_t next = dst + pitch, eax = next;
    for (;;) {
        const uint8_t b = *src++;
        if (b == 0x80) {
            if (--rows == 0)
                return eax & ~0xffu;
            dst = next;
            next += pitch;
            eax = next;
        } else if (b & 0x80) {
            dst += b & 0x7f;
            eax = 0;
        } else {
            op(H(dst), src, b);
            eax = 0;
            src += b;
            dst += b;
        }
    }
}

// Clipped variants: x is the screen column; a literal of b pixels draws
// [l, r) with l = max(0, clipL - x) and r = min(clipR - x, b) (r is not
// clamped at 0). A skip advances x too; a new row resets it to x0.

// 606da0(ecx=src, edx=dst, [8]=rows, [c]=pitch, [10]=x0, [14]=clipL,
// [18]=clipR), ret 0x14: copy, clipped.
// EAX: pitch; next-row pointer on each row; the skip count on a skip; on a
// literal memcpy's return (dst + l) or, when nothing is drawn, r - l. The
// end byte goes to DL, so EAX is returned whole.
uint32_t dc6_copy_clipped(Cpu &c)
{
    const uint8_t *src = H(c.reg(R_ECX));
    uint32_t dst = c.reg(R_EDX), rows = c.arg(0);
    const uint32_t pitch = c.arg(1);
    const int32_t x0 = (int32_t)c.arg(2), cl = (int32_t)c.arg(3), cr = (int32_t)c.arg(4);
    uint32_t next = dst + pitch, eax = pitch;
    int32_t x = x0;
    for (;;) {
        const uint8_t b = *src++;
        if (b == 0x80) {
            if (--rows == 0)
                return eax;
            dst = next;
            next += pitch;
            eax = next;
            x = x0;
        } else if (b & 0x80) {
            const int32_t n = b & 0x7f;
            dst += n;
            x += n;
            eax = (uint32_t)n;
        } else {
            const int32_t l = imax(0, cl - x), r = imin(cr - x, (int32_t)b), n = r - l;
            if (n > 0) {
                std::memcpy(H(dst + l), src + l, (size_t)n);
                eax = dst + l;
            } else {
                eax = (uint32_t)n;
            }
            src += b;
            dst += b;
            x += b;
        }
    }
}

// 607060(.., [10]=x0, [14]=clipL, [18]=clipR, [1c]=T1, [20]=T2), ret 0x1c.
// EAX: dst + pitch; next-row pointer on each row; a skip leaves its count;
// a literal leaves its start pointer when drawn, else its length b; lo0.
uint32_t dc6_blend_clipped(Cpu &c)
{
    const uint8_t *src = H(c.reg(R_ECX));
    uint32_t dst = c.reg(R_EDX), rows = c.arg(0);
    const uint32_t pitch = c.arg(1);
    const int32_t x0 = (int32_t)c.arg(2), cl = (int32_t)c.arg(3), cr = (int32_t)c.arg(4);
    const OpBlend op{H(c.arg(5)), H(c.arg(6))};
    uint32_t next = dst + pitch, eax = next;
    int32_t x = x0;
    for (;;) {
        const uint8_t b = *src++;
        if (b == 0x80) {
            if (--rows == 0)
                return eax & ~0xffu;
            dst = next;
            next += pitch;
            eax = next;
            x = x0;
        } else if (b & 0x80) {
            const int32_t n = b & 0x7f;
            dst += n;
            x += n;
            eax = 0;
        } else {
            const int32_t l = imax(0, cl - x), r = imin(cr - x, (int32_t)b), n = r - l;
            if (n > 0) {
                op(H(dst + l), src + l, n);
                eax = dst;
            } else {
                eax = 0;
            }
            src += b;
            dst += b;
            x += b;
        }
    }
}

// ---- the light-occlusion grid ---------------------------------------------------
//
// Every frame the game rebuilds a 48x48 grid of light-blocking subtiles
// around the view: 2,304 room lookups and collision queries, each a chain
// of small calls. On the console this was ~21% of a town frame, almost all
// of it call and return overhead in translated code.

constexpr uint32_t kPlayerUnit = 0x007a6a70;   // the player's unit, or 0
constexpr uint32_t kGridX = 0x007b0a54, kGridY = 0x007b0a58;   // grid origin, in subtiles
constexpr uint32_t kLightGrid = 0x007b0e68;    // 48x48 cells of 8 bytes; the first dword is "blocked"

// D2's room: neighbour list at +0x00, its length at +0x24, collision data
// at +0x20, bounds in subtiles at +0x4c (x), +0x50 (y), +0x54 (w), +0x58 (h).
inline bool room_has(uint32_t r, int32_t x, int32_t y)
{
    const int32_t rx = I32(r + 0x4c), ry = I32(r + 0x50);
    return x >= rx && x < rx + I32(r + 0x54) && y >= ry && y < ry + I32(r + 0x58);
}

// 463740(ecx=room, edx=x, [8]=y): the room itself or the neighbour holding
// (x, y), else 0.
uint32_t room_find(uint32_t room, int32_t x, int32_t y)
{
    if (!room)
        return 0;
    if (room_has(room, x, y))
        return room;
    const uint32_t list = U32(room), count = U32(room + 0x24);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t n = U32(list + 4 * i);
        if (n && room_has(n, x, y))
            return n;
    }
    return 0;
}

// 620bb0([8]=unit): the unit's room. Types 2, 4 and 5 (objects, items,
// tiles) hold it at [[+0x2c]]; the others at [[+0x2c]+0x1c] when they have
// a path. (A null unit is a fatal error in the game; the caller checks.)
uint32_t unit_room(uint32_t unit)
{
    const uint32_t type = U32(unit), path = U32(unit + 0x2c);
    if (type == 2 || type == 4 || type == 5)
        return U32(path);
    return path ? U32(path + 0x1c) : 0;
}

// 64cb30([8]=room, [c]=x, [10]=y, [14]=mask), ret 0x10: the collision word
// at (x, y) masked, or 0x27 (blocked) outside any room or collision data.
uint32_t collision(uint32_t room, int32_t x, int32_t y, uint16_t mask)
{
    const uint32_t r = room_find(room, x, y);
    if (!r)
        return 0x27;
    const uint32_t c = U32(r + 0x20);   // 61a010
    if (!c)
        return 0x27;
    const uint32_t grid = U32(c + 0x20);
    if (!grid)
        return 0x27;
    const uint32_t i = ((uint32_t)y - U32(c + 4)) * U32(c + 8) - U32(c) + (uint32_t)x;
    uint16_t w;
    std::memcpy(&w, H(grid + 2 * i), 2);
    return w & mask;
}

// 4756d0(): marks the cells whose subtile blocks light (mask 0x22). The cells
// are only ever set here, never cleared. The room found for one cell is the
// starting point for the next; where none holds it, the player's room again.
uint32_t light_grid(Cpu &)
{
    const uint32_t player = U32(kPlayerUnit);
    if (!player)
        return 0;
    uint32_t room = unit_room(player);
    if (!room)
        return 0;
    for (int32_t y = 0; y < 48; y++)
        for (int32_t x = 0; x < 48; x++) {
            const int32_t gx = (int32_t)((uint32_t)I32(kGridX) + (uint32_t)x);
            const int32_t gy = (int32_t)((uint32_t)I32(kGridY) + (uint32_t)y);
            const uint32_t r = room_find(room, gx, gy);
            room = r ? r : unit_room(player);
            if ((uint16_t)collision(room, gx, gy, 0x22))
                std::memcpy(H(kLightGrid + 8 * (uint32_t)(y * 48 + x)), "\1\0\0\0", 4);
        }
    return 0;
}

// 4ddef0(eax=dst, [8]=tx, [c]=ty, [10]=ambient), ret 0xc: the light of an
// 8x8 block of subtiles around tile (tx, ty), into 64 entries of 12 bytes
// (bytes 0-3 of each). Without `ambient`, each subtile samples the light
// grid through 475aa0: the cell's byte 4, and with coloured light
// (dword 712b8c) bytes 5-7 too, else those three stay as they were. On
// the console the 64 calls per block were ~16% of a town frame.
constexpr uint32_t kColoredLight = 0x00712b8c;
constexpr uint32_t kAmbientSource = 0x007a0634;

uint32_t light_block(Cpu &c)
{
    uint32_t dst = c.reg(R_EAX);
    const uint32_t tx = c.arg(0), ty = c.arg(1), ambient = c.arg(2);
    if (ambient) {
        // The original's own calls: 61c0e0 gives the intensity, 61c0b0 writes
        // the colour into the top bytes of the argument slots, which keep
        // whatever they held when it finds no light (a quirk kept as is).
        const uint32_t esp = c.reg(R_ESP);   // [esp] = return address
        const uint8_t level = (uint8_t)rt::guest_call(c, 0x0061c0e0, {U32(kAmbientSource)});
        rt::guest_call(c, 0x0061c0b0, {U32(kAmbientSource), esp + 7, esp + 11, esp + 15});
        const uint8_t e[4] = {level, *H(esp + 7), *H(esp + 11), *H(esp + 15)};
        for (int i = 0; i < 64; i++, dst += 12)
            std::memcpy(H(dst), e, 4);
        return 0;
    }
    const int32_t ox = I32(kGridX), oy = I32(kGridY);
    const bool colored = U32(kColoredLight) != 0;
    const uint32_t x0 = tx * 8 - 8, y0 = ty * 8 - 8;   // subtile coordinates, as the original computes them
    for (uint32_t row = 0; row < 8; row++) {
        int32_t cy = (int32_t)((uint32_t)((int32_t)(y0 + 8 * row) >> 3) - (uint32_t)oy);
        cy = cy < 0 ? 0 : cy > 47 ? 47 : cy;
        for (uint32_t col = 0; col < 8; col++, dst += 12) {
            int32_t cx = (int32_t)((uint32_t)((int32_t)(x0 + 8 * col) >> 3) - (uint32_t)ox);
            cx = cx < 0 ? 0 : cx > 47 ? 47 : cx;
            const uint8_t *cell = H(kLightGrid + 8 * (uint32_t)(cy * 48 + cx));
            uint8_t *d = H(dst);
            d[0] = cell[4];
            if (colored) {
                d[1] = cell[5];
                d[2] = cell[6];
                d[3] = cell[7];
            }
        }
    }
    return 0;
}

// ---- the floor tile ---------------------------------------------------------------
//
// A floor tile is a list of iso blocks. Drawn by 4f8b80 one block at a time
// through a per-block wrapper and a blitter: on the console the native
// blitters' entry and exit, once per block, cost about as much as their
// pixels. Here the whole tile is one native call.

constexpr uint32_t kShades = 0x007d2348;       // 32 shade tables of 256 bytes; light >> 3 picks one
constexpr uint32_t kFlatLight = 0x007d2340;    // nonzero: one light per block from the light buffer
constexpr uint32_t kNoGouraud = 0x0072da50;    // nonzero: never interpolate the light

inline const uint8_t *shade(uint8_t light) { return H(kShades + ((uint32_t)(light >> 3) << 8)); }

// 4f88b0's iso branch: a 15-row iso block clipped to the window, at screen
// (x, y), drawn at dst.
void iso_flat_clipped(const uint8_t *src, int32_t x, int32_t y, uint8_t *dst, const uint8_t *tbl)
{
    const int32_t pitch = I32(kPitch);
    const int32_t r0 = imax(0, I32(kClipTop) - y);
    const int32_t r1 = imin(15, I32(kClipBottom) - y);
    uint8_t *rowp = dst + (intptr_t)pitch * r0;
    for (int32_t r = r0; r < r1; r++, rowp += pitch) {
        const int32_t sx = g_xoff[r] + x;
        const int32_t l = imax(0, I32(kClipLeft) - sx);
        const int32_t n = imin(imax(0, I32(kClipRight) - sx), g_width[r]) - l;
        if (n > 0)
            for (int32_t i = 0; i < n; i++)
                rowp[g_xoff[r] + l + i] = tbl[src[g_soff[r] + l + i]];
    }
}

// 4f8b80(ecx=tile, edx=x, [8]=y, [c]=light buffer), ret 8. Per block (20
// bytes from [tile+0x54], [tile+0x50] of them): offset (short x, short y) at
// +0, flags at +8 (1: drawn, 4: RLE), light cell (+6, +7), pixels at
// [+0x10]. Flat light (the light buffer's cell, or interpolation off, or the
// four neighbours within 10 of each other) goes to the native blitters;
// interpolated light keeps the game's own 4f8850 / 4f89f0.
uint32_t floor_tile(Cpu &c)
{
    const uint32_t tile = c.reg(R_ECX);
    const int32_t x = (int32_t)c.reg(R_EDX), y = (int32_t)c.arg(0);
    const uint32_t lightbuf = c.arg(1);
    const int32_t count = I32(tile + 0x50);
    const uint32_t pitch = U32(kPitch);
    const uint32_t rowbase = pitch * (uint32_t)y + U32(kFbBase) + (uint32_t)x;
    for (int32_t i = 0; i < count; i++) {
        const uint32_t e = U32(tile + 0x54) + 20 * (uint32_t)i;
        int16_t ex, ey;
        std::memcpy(&ex, H(e), 2);
        std::memcpy(&ey, H(e + 2), 2);
        const uint32_t dst = (uint32_t)ey * pitch + (uint32_t)ex + rowbase;
        const uint8_t flags = *H(e + 8);
        if (!(flags & 1))
            continue;
        const int32_t sx = ex + x, sy = y + ey;
        if (sy < I32(0x007d2330) || sy >= I32(0x007d2338) || sx < I32(0x007c9788) || sx >= I32(0x007c9798))
            continue;
        const uint32_t cell = (uint32_t)*H(e + 6) + 8u * *H(e + 7);
        bool flat = true;
        uint8_t light;
        uint32_t corners[4];
        if (I32(kFlatLight)) {
            light = *H(lightbuf + 12 * (cell + 9));
        } else {
            const uint8_t *b = H(lightbuf + 12 * cell);
            const int32_t v6c = b[0x6c], v78 = b[0x78], vcc = b[0xcc], vd8 = b[0xd8];
            light = (uint8_t)v6c;
            const int32_t d = std::abs(v78 - v6c) + std::abs(vcc - v6c) + std::abs(vd8 - v78);
            if (!I32(kNoGouraud) && d >= 10) {
                flat = false;
                corners[0] = (uint32_t)((vcc + b[0xc0] + v6c + b[0x60]) >> 2);
                corners[1] = (uint32_t)((b[0x18] + v78 + b[0x0c] + v6c) >> 2);
                corners[2] = (uint32_t)((b[0xe4] + vcc + b[0x84] + v78) >> 2);
                corners[3] = (uint32_t)((b[0x138] + vd8 + b[0x12c] + vcc) >> 2);
            }
        }
        const bool inside = sy >= I32(0x007c9780) && sy < I32(0x007c979c) && sx >= I32(0x007c97a4) &&
                            sx < I32(0x007d2320);
        const uint32_t src = U32(e + 0x10);
        if (flat) {
            const uint8_t *tbl = shade(light);
            if (inside) {          // 4f8810
                if (flags & 4)
                    rle_flat_draw(H(src), H(dst), tbl, 15);
                else
                    iso_flat(H(src), H(dst), tbl);
            } else {               // 4f88b0
                if (flags & 4)
                    rle_flat_clipped_draw(sx, sy, H(src), H(dst), tbl, 15);
                else
                    iso_flat_clipped(H(src), sx, sy, H(dst), tbl);
            }
        } else {
            // The original passes its four corners by address: here they go
            // below the stack pointer, under guest_call's own frame.
            const uint32_t buf = c.reg(R_ESP) - 0x20;
            std::memcpy(H(buf), corners, sizeof corners);
            c.set_reg(R_ECX, e);
            if (inside) {
                c.set_reg(R_EDX, dst);
                rt::guest_call(c, 0x004f8850, {buf});
            } else {
                c.set_reg(R_EDX, (uint32_t)sx);
                rt::guest_call(c, 0x004f89f0, {(uint32_t)sy, dst, buf});
            }
        }
    }
    return 0;
}

// ---- sound: MPQ ADPCM ---------------------------------------------------------------
//
// The game's sounds are compressed WAVs in the MPQs, decoded by the loader
// thread as they stream: Blizzard's ADPCM (Storm's DecompressADPCM), a tight
// loop the profile showed as most of the loader's own time in town.

constexpr uint32_t kAdpcmSteps = 0x006cfd38;    // 89 dword step sizes
constexpr uint32_t kAdpcmIndexAdj = 0x006cfcb8; // 32 dword step-index changes
uint32_t g_adpcm_step[89];
int32_t g_adpcm_adj[32];

// 41dce0(ecx=dst, eax=src bytes, [8]=dst bytes, [c]=src, [10]=channels),
// ret 0xc: returns the bytes written. Input: a byte at +1 with the shift,
// then a 16-bit first sample per channel, then one byte per sample (the
// channels alternating): bit 7 clear, a delta (bits 0-5 select fractions of
// the step, bit 6 its sign); set, a code: 0 repeats the sample (step index
// down 1), 1 and 2 move the step index by +8 / -8 without a sample.
uint32_t adpcm_decode(Cpu &c)
{
    const uint32_t dst0 = c.reg(R_ECX);
    uint32_t dst = dst0;
    const uint32_t src = c.arg(1), end = src + c.reg(R_EAX), channels = c.arg(2);
    uint32_t avail = c.arg(0);
    int32_t pred[2] = {0, 0}, index[2] = {0x2c, 0x2c};
    const uint32_t shift = *H(src + 1) & 31u;   // x86 masks a 32-bit shift count
    uint32_t p = src + 2;
    const auto put = [&](int32_t v) {
        const int16_t w = (int16_t)v;
        std::memcpy(H(dst), &w, 2);
        dst += 2;
        avail -= 2;
    };
    for (uint32_t i = 0; i < channels; i++) {
        int16_t s;
        std::memcpy(&s, H(p), 2);
        p += 2;
        pred[i & 1] = s;   // (the original's frame holds two)
        if (avail < 2)
            return dst - dst0;
        put(s);
    }
    if (p >= end)
        return dst - dst0;
    uint32_t ch = channels - 1;
    while (p < end) {
        if (channels == 2)
            ch = ch == 0;
        const uint8_t b = *H(p++);
        int32_t &ix = index[ch & 1];
        if (b & 0x80) {
            switch (b & 0x7f) {
            case 0:
                if (ix)
                    ix--;
                if (avail < 2)
                    return dst - dst0;
                put(pred[ch & 1]);
                break;
            case 1:
                ix = ix + 8 > 0x58 ? 0x58 : ix + 8;
                if (channels == 2)
                    ch = ch == 0;   // no sample: the same channel decodes next
                break;
            case 2:
                ix = ix - 8 < 0 ? 0 : ix - 8;
                if (channels == 2)
                    ch = ch == 0;
                break;
            default:
                break;
            }
            continue;
        }
        const uint32_t step = g_adpcm_step[ix];
        uint32_t diff = step >> shift;
        if (b & 0x01) diff += step;
        if (b & 0x02) diff += step >> 1;
        if (b & 0x04) diff += step >> 2;
        if (b & 0x08) diff += step >> 3;
        if (b & 0x10) diff += step >> 4;
        if (b & 0x20) diff += step >> 5;
        int32_t v = pred[ch & 1];
        if (b & 0x40) {
            v = (int32_t)((uint32_t)v - diff);
            if (v <= -32768)
                v = -32768;
        } else {
            v = (int32_t)((uint32_t)v + diff);
            if (v >= 32767)
                v = 32767;
        }
        pred[ch & 1] = v;
        if (avail < 2)
            return dst - dst0;
        put(v);
        ix += g_adpcm_adj[b & 0x1f];
        ix = ix < 0 ? 0 : ix > 0x58 ? 0x58 : ix;
    }
    return dst - dst0;
}

// ---- sound: MPQ Huffman --------------------------------------------------------------
//
// The second stage of the compressed WAVs: Storm's adaptive Huffman (FGK)
// decoder, about a third of the loader thread's own time in Blood Moor once
// ADPCM was native. The tree logic follows StormLib's huff.cpp (Ladislav
// Zezula, MIT licence), which reproduces Storm's; the weight tables are the
// game's own (0x6cf3a0, 0x102 bytes apart, identical to StormLib's). The
// output of a decoder is fixed by the stream, so it matches the original's
// byte for byte; only the way the tree is held in memory differs.

constexpr uint32_t kHuffWeights = 0x006cf3a0;

struct HuffItem {
    HuffItem *next, *prev;   // the weight-sorted list: next = lower weight
    uint32_t value, weight;
    HuffItem *parent, *child_lo;
};

class HuffTree {
public:
    // 41f9e0(ecx=dst, edx=&dst bytes, [8]=src), ret 0xc: decodes until the
    // end code or a full buffer and stores the bytes written in *edx.
    uint32_t decompress(uint8_t *out, uint32_t out_len, uint32_t src)
    {
        bits_ = U32(src);   // the game's stream: a first dword, then 16-bit words
        count_ = 32;
        in_ = src + 4;
        if (!out_len)
            return 0;
        const uint32_t type = get8();
        build(type);
        const bool sparse = type == 0;
        uint32_t n = 0;
        for (;;) {
            uint32_t v = decode();
            if (v == 0x101) {
                v = get8();
                insert_new(head_.prev->value, v);
                if (!sparse)
                    inc_weights(by_byte_[v]);
            }
            if (v == 0x100)
                return n;
            out[n++] = (uint8_t)v;
            if (n == out_len)
                return n;
            if (sparse)
                inc_weights(by_byte_[v]);
        }
    }

private:
    // ---- input: bits from the low end, refilled 16 bits at a time
    void need(uint32_t k)
    {
        if (count_ < k) {
            uint16_t w;
            std::memcpy(&w, H(in_), 2);
            bits_ |= (uint32_t)w << count_;
            count_ += 16;
            in_ += 2;
        }
    }
    uint32_t get8()
    {
        need(8);
        const uint32_t v = bits_ & 0xff;
        bits_ >>= 8;
        count_ -= 8;
        return v;
    }
    uint32_t get1()
    {
        need(1);
        const uint32_t v = bits_ & 1;
        bits_ >>= 1;
        count_ -= 1;
        return v;
    }
    uint32_t peek7()
    {
        need(7);
        return bits_ & 0x7f;
    }
    void skip(uint32_t k)
    {
        need(k);
        bits_ >>= k;
        count_ -= k;
    }

    // ---- the weight-sorted list
    static void unlink(HuffItem *i)
    {
        if (i->next) {
            i->prev->next = i->next;
            i->next->prev = i->prev;
            i->next = i->prev = nullptr;
        }
    }
    static void link_after(HuffItem *at, HuffItem *i)   // StormLib's LinkTwoItems
    {
        i->next = at->next;
        i->prev = at->next->prev;
        at->next->prev = i;
        at->next = i;
    }
    HuffItem *higher_or_equal(HuffItem *i, uint32_t weight)
    {
        if (i)
            for (; i != &head_; i = i->prev)
                if (i->weight >= weight)
                    return i;
        return &head_;
    }
    HuffItem *create(uint32_t value, uint32_t weight, bool before)
    {
        if (used_ >= kItems)
            return nullptr;
        HuffItem *i = &items_[used_++];
        i->next = i->prev = nullptr;
        link_after(before ? head_.prev : &head_, i);
        i->value = value;
        i->weight = weight;
        i->parent = i->child_lo = nullptr;
        return i;
    }
    uint32_t fixup(HuffItem *i, uint32_t max_weight)
    {
        if (i->weight < max_weight) {
            HuffItem *h = higher_or_equal(head_.prev, i->weight);
            unlink(i);
            link_after(h, i);
            return max_weight;
        }
        return i->weight;
    }
    void build(uint32_t type)
    {
        head_.next = head_.prev = &head_;
        used_ = 0;
        min_valid_ = 1;
        std::memset(by_byte_, 0, sizeof by_byte_);
        std::memset(links_, 0, sizeof links_);
        const uint8_t *w = H(kHuffWeights + 0x102 * type);
        uint32_t max_weight = 0;
        for (uint32_t i = 0; i < 0x100; i++)
            if (w[i]) {
                by_byte_[i] = create(i, w[i], false);
                max_weight = fixup(by_byte_[i], max_weight);
            }
        by_byte_[0x100] = create(0x100, 1, true);
        by_byte_[0x101] = create(0x101, 1, true);
        for (HuffItem *lo = head_.prev; lo != &head_;) {
            HuffItem *hi = lo->prev;
            if (hi == &head_)
                break;
            HuffItem *p = create(0, hi->weight + lo->weight, false);
            if (!p)
                break;
            lo->parent = hi->parent = p;
            p->child_lo = lo;
            max_weight = fixup(p, max_weight);
            lo = hi->prev;
        }
        min_valid_ = 1;
    }
    void inc_weights(HuffItem *i)
    {
        for (; i; i = i->parent) {
            i->weight++;
            HuffItem *higher = higher_or_equal(i->prev, i->weight);
            HuffItem *hi = higher->next;
            if (hi != i) {
                unlink(hi);
                link_after(i, hi);
                unlink(i);
                link_after(higher, i);
                HuffItem *lo = hi->parent->child_lo;
                if (i->parent->child_lo == i)
                    i->parent->child_lo = hi;
                if (lo == hi)
                    hi->parent->child_lo = i;
                HuffItem *p = i->parent;
                i->parent = hi->parent;
                hi->parent = p;
                min_valid_++;
            }
        }
    }
    void insert_new(uint32_t v1, uint32_t v2)
    {
        HuffItem *last = head_.prev;
        HuffItem *hi = create(v1, last->weight, true);
        if (!hi)
            return;
        hi->parent = last;
        by_byte_[v1] = hi;
        HuffItem *lo = create(v2, 0, true);
        if (!lo)
            return;
        lo->parent = last;
        last->child_lo = lo;
        by_byte_[v2] = lo;
        inc_weights(lo);
    }

    // ---- decoding, with the 7-bit quick links (an optimization: they
    // never change what is decoded)
    struct Link {
        uint32_t valid, bits;
        HuffItem *item;
        uint32_t value;
    };
    uint32_t decode()
    {
        const uint32_t ix = peek7();
        HuffItem *item;
        HuffItem *at7 = nullptr;
        uint32_t depth = 0;
        if (links_[ix].valid > min_valid_) {
            if (links_[ix].bits <= 7) {
                skip(links_[ix].bits);
                return links_[ix].value;
            }
            item = links_[ix].item;
            skip(7);
        } else {
            item = head_.next;
        }
        while (item->child_lo) {
            item = get1() ? item->child_lo->prev : item->child_lo;
            if (++depth == 7)
                at7 = item;
        }
        if (links_[ix].valid < min_valid_) {
            if (depth > 7) {
                links_[ix] = {min_valid_, depth, at7, 0};
            } else {
                for (uint32_t k = depth ? ix & (0xffffffffu >> (32 - depth)) : 0; k < 128; k += 1u << depth)
                    links_[k] = {min_valid_, depth, nullptr, item->value};
            }
        }
        return item->value;
    }

    static constexpr uint32_t kItems = 515;
    HuffItem items_[kItems];
    uint32_t used_ = 0;
    HuffItem head_{};
    HuffItem *by_byte_[0x102];
    Link links_[128];
    uint32_t min_valid_ = 1;
    uint32_t bits_ = 0, count_ = 0, in_ = 0;
};

HuffTree g_huff;   // guest threads all run on the host's one game thread

// 41e000(ecx=dst, edx=&dst bytes, [8]=src, [c]=src bytes), ret 0xc: Storm's
// PKWARE "explode" wrapper. The original allocates a work buffer, runs the
// library with callbacks that clip at both buffers' ends, frees it, and
// stores the bytes written in *edx (pklib_explode.cpp).
uint32_t pk_decode(Cpu &c)
{
    const uint32_t plen = c.reg(R_EDX), src = c.arg(0), src_len = c.arg(1);
    const uint32_t n = pk_explode(H(src), src_len, H(c.reg(R_ECX)), U32(plen));
    std::memcpy(H(plen), &n, 4);
    return 0;
}

uint32_t huffman_decode(Cpu &c)
{
    const uint32_t plen = c.reg(R_EDX);
    const uint32_t len = U32(plen);
    const uint32_t n = g_huff.decompress(H(c.reg(R_ECX)), len, c.arg(0));
    std::memcpy(H(plen), &n, 4);
    return 0;
}

// ---- string tables -------------------------------------------------------------------
//
// D2Lang's lookup of a string by its key (0x524d30 tries the patch, expansion
// and base tables in turn): a hash, then a probe through the table's nodes
// comparing keys byte by byte. About 16% of the main thread while an area
// loads.

// 5249e0(key, size): the ELF hash of the key (signed chars), modulo size.
uint32_t lang_hash(uint32_t key, uint32_t size)
{
    uint32_t h = 0;
    for (const uint8_t *s = H(key); *s; s++) {
        h = (h << 4) + (uint32_t)(int32_t)(int8_t)*s;
        if (const uint32_t hi = h & 0xf0000000u)
            h = ((hi >> 24) ^ h) & 0x0fffffffu;
    }
    return h % size;
}

// 524c60(edi=table, [8]=string pointers, [c]=key, [10]=&string), ret 0xc:
// the string's index (ax), 0xffff if the key isn't there. Nodes of 17 bytes
// after the header's index array: a used flag (1), the index (+1), the key's
// offset from the table (+7). At most [table+0xd] probes, from the hash on.
// (A found key whose string pointer is null is a fatal error in the game;
// here it returns as found, with that null.)
uint32_t lang_lookup(Cpu &c)
{
    const uint32_t table = c.reg(R_EDI), strings = c.arg(0), key = c.arg(1), out = c.arg(2);
    uint16_t keys;
    std::memcpy(&keys, H(table + 2), 2);
    const uint32_t nodes = table + 2u * keys + 0x15;
    const uint32_t size = U32(table + 4), probes = U32(table + 0xd);
    uint32_t i = lang_hash(key, size);
    for (uint32_t n = 0; n < probes;) {
        const uint32_t node = nodes + 17 * i;
        n++;
        if (*H(node) == 1 && std::strcmp(reinterpret_cast<const char *>(H(U32(node + 7) + table)),
                                         reinterpret_cast<const char *>(H(key))) == 0) {
            const uint32_t str = U32(strings + 4 * i);
            std::memcpy(H(out), &str, 4);
            uint16_t index;
            std::memcpy(&index, H(node + 1), 2);
            return (i & 0xffff0000u) | index;
        }
        i = (uint32_t)((int32_t)(i + 1) % (int32_t)size);
    }
    return (i & 0xffff0000u) | 0xffffu;
}

}  // namespace

int install_d2_native(Cpu &c)
{
    // Arena base: any mapped guest address gives it.
    uint8_t *p = (uint8_t *)c.hostptr(0x00400000, 1);
    if (!p)
        return 0;
    g_base = p - 0x00400000;
    for (int r = 0; r < 15; r++) {
        g_xoff[r] = I32(kIsoXoff + 4 * r);
        g_width[r] = I32(kIsoWidth + 4 * r);
        g_soff[r] = I32(kIsoSoff + 4 * r);
    }
    // First bytes of each original (push ebp; mov ebp,esp; ...): only this
    // build of Game.exe gets hooked.
    static const uint8_t k4f69b0[] = {0x55, 0x8b, 0xec, 0x51, 0x53, 0x56, 0x8b, 0x75, 0x08};
    static const uint8_t k4f6f60[] = {0x55, 0x8b, 0xec, 0x51, 0x83, 0x7d, 0x0c, 0x00};
    static const uint8_t k4f7580[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x10, 0x83, 0x7d, 0x14, 0x00};
    // D2_NATIVE_MASK (tests): bit k enables the k-th hook below; default all.
    const char *m = std::getenv("D2_NATIVE_MASK");
    const unsigned mask = m ? (unsigned)std::strtoul(m, nullptr, 0) : ~0u;
    unsigned bit = 1;
    int n = 0;
    // The last argument, true: served in line (rt::hook_function). Not for
    // LightBlock and FloorTile, which call back into the game's own code.
#define HOOK(...) (n += (mask & bit) ? rt::hook_function(__VA_ARGS__) : 0, bit <<= 1)
    HOOK(c, 0x004f69b0, k4f69b0, sizeof k4f69b0, 1, floor_iso_flat, "FloorIsoFlat", true);
    HOOK(c, 0x004f6f60, k4f6f60, sizeof k4f6f60, 2, rle_flat, "RleFlat", true);
    HOOK(c, 0x004f7580, k4f7580, sizeof k4f7580, 4, rle_flat_clipped, "RleFlatClipped", true);
    static const uint8_t k4f7ea0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c, 0x8b, 0x45, 0x08};
    static const uint8_t k606d40[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45, 0x0c, 0x53, 0x56, 0x57};
    static const uint8_t k606da0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x8b, 0x45, 0x0c};
    static const uint8_t k606e40[] = {0x55, 0x8b, 0xec, 0x51, 0x8b, 0x45, 0x0c, 0x53, 0x56};
    static const uint8_t k607060[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x8b, 0x45, 0x10};
    HOOK(c, 0x004f7ea0, k4f7ea0, sizeof k4f7ea0, 1, wall_block_copy, "WallBlockCopy", true);
    HOOK(c, 0x00606d40, k606d40, sizeof k606d40, 2, dc6_copy, "Dc6Copy", true);
    HOOK(c, 0x00606da0, k606da0, sizeof k606da0, 5, dc6_copy_clipped, "Dc6CopyClipped", true);
    HOOK(c, 0x00606e40, k606e40, sizeof k606e40, 4, dc6_blend, "Dc6Blend", true);
    HOOK(c, 0x00607060, k607060, sizeof k607060, 7, dc6_blend_clipped, "Dc6BlendClipped", true);
    static const uint8_t k4756d0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x53, 0xe8};
    HOOK(c, 0x004756d0, k4756d0, sizeof k4756d0, 0, light_grid, "LightGrid", true);
    static const uint8_t k4ddef0[] = {0x55, 0x8b, 0xec, 0x51, 0x83, 0x7d, 0x10, 0x00, 0x53};
    HOOK(c, 0x004ddef0, k4ddef0, sizeof k4ddef0, 3, light_block, "LightBlock");
    static const uint8_t k4f8b80[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x48, 0x53, 0x8b, 0x1d};
    HOOK(c, 0x004f8b80, k4f8b80, sizeof k4f8b80, 2, floor_tile, "FloorTile");
    for (int i = 0; i < 89; i++)
        g_adpcm_step[i] = U32(kAdpcmSteps + 4 * i);
    for (int i = 0; i < 32; i++)
        g_adpcm_adj[i] = I32(kAdpcmIndexAdj + 4 * i);
    static const uint8_t k41dce0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x1c, 0x53, 0x56, 0x8b, 0x75, 0x0c};
    HOOK(c, 0x0041dce0, k41dce0, sizeof k41dce0, 3, adpcm_decode, "AdpcmDecode", true);
    static const uint8_t k41f9e0[] = {0x55, 0x8b, 0xec, 0xb8, 0x80, 0x3a, 0x00, 0x00, 0xe8};
    HOOK(c, 0x0041f9e0, k41f9e0, sizeof k41f9e0, 3, huffman_decode, "HuffmanDecode", true);
    static const uint8_t k41e000[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x18, 0x53, 0x56, 0x57, 0x6a, 0x00};
    HOOK(c, 0x0041e000, k41e000, sizeof k41e000, 3, pk_decode, "PkExplode", true);
    static const uint8_t k524c60[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x08, 0x0f, 0xb7, 0x47, 0x02};
    HOOK(c, 0x00524c60, k524c60, sizeof k524c60, 3, lang_lookup, "LangLookup", true);
#undef HOOK
    return n;
}

}  // namespace game
