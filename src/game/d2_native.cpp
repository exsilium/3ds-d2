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

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/rt_host.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace d2rt;

// WinVita's translation-time memcpy / memset service (dyn86_memintrin.h).
extern "C" {
void dyn86_mi_arm(int mode, uintptr_t cpy_va, uintptr_t set_va);
void dyn86_mi_set_span(uint32_t span);
}
// And its translation-time intrinsics: a direct native call where a block
// starts at a registered address, no trap, no bridge.
#include "dyn86_intrin.h"

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

// A host-side index of one string table: for each used node, its key's FNV
// hash, in an open-addressed array of node numbers (2 bytes a slot). Built
// on a table's first lookup; rebuilt if the table's address or shape
// changes (the game loads its string tables once).
struct LangIndex {
    uint32_t table = 0, size = 0, probes = 0, keys = 0, nodes = 0;
    std::vector<uint16_t> slots;   // node numbers; 0xffff = empty
    uint32_t mask = 0;
};
LangIndex g_lang_index[4];

inline uint32_t fnv(const uint8_t *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ *s) * 16777619u;
    return h;
}

LangIndex &lang_index(uint32_t table, uint32_t size, uint32_t probes, uint32_t keys, uint32_t nodes)
{
    LangIndex *slot = &g_lang_index[0];
    for (LangIndex &x : g_lang_index) {
        if (x.table == table && x.size == size && x.probes == probes && x.keys == keys && x.nodes == nodes)
            return x;
        if (!x.table || x.table == table)
            slot = &x;
    }
    LangIndex &x = *slot;
    x.table = table;
    x.size = size;
    x.probes = probes;
    x.keys = keys;
    x.nodes = nodes;
    uint32_t cap = 1;
    while (cap < 2 * size)
        cap <<= 1;
    x.mask = cap - 1;
    x.slots.assign(cap, 0xffff);
    for (uint32_t j = 0; j < size && j < 0xffff; j++) {
        const uint32_t node = nodes + 17 * j;
        if (*H(node) != 1)
            continue;
        for (uint32_t k = fnv(H(U32(node + 7) + table)) & x.mask;; k = (k + 1) & x.mask)
            if (x.slots[k] == 0xffff) {
                x.slots[k] = (uint16_t)j;
                break;
            }
    }
    return x;
}

// 524c60(edi=table, [8]=string pointers, [c]=key, [10]=&string), ret 0xc:
// the string's index (ax), 0xffff if the key isn't there. Nodes of 17 bytes
// after the header's index array: a used flag (1), the index (+1), the key's
// offset from the table (+7). The original walks at most [table+0xd] nodes
// from the key's hash bucket, comparing keys, and takes the first match. Its
// probe limit is close to the table's size, so a missing key (most keys miss
// the patch and expansion tables) walked nearly every node. The answer is the
// same from the index: of the nodes holding the key, the one fewest steps
// from its bucket, if within the limit.
// (A found key whose string pointer is null is a fatal error in the game;
// here it returns as found, with that null.)
uint32_t lang_lookup(Cpu &c)
{
    const uint32_t table = c.reg(R_EDI), strings = c.arg(0), key = c.arg(1), out = c.arg(2);
    uint16_t keys;
    std::memcpy(&keys, H(table + 2), 2);
    const uint32_t nodes = table + 2u * keys + 0x15;
    const uint32_t size = U32(table + 4), probes = U32(table + 0xd);
    const uint32_t start = lang_hash(key, size);
    if (size > 0xffff) {   // beyond 16-bit node numbers: the original walk
        uint32_t i = start;
        for (uint32_t n = 0; n < probes; n++) {
            const uint32_t node = nodes + 17 * i;
            if (*H(node) == 1 && !std::strcmp(reinterpret_cast<const char *>(H(U32(node + 7) + table)),
                                               reinterpret_cast<const char *>(H(key)))) {
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
    if (!probes)
        return 0xffff;
    const LangIndex &x = lang_index(table, size, probes, keys, nodes);
    const uint8_t *k = H(key);
    uint32_t best = 0xffffffffu, best_dist = 0xffffffffu;
    for (uint32_t s = fnv(k) & x.mask; x.slots[s] != 0xffff; s = (s + 1) & x.mask) {
        const uint32_t j = x.slots[s];
        const uint32_t node = nodes + 17 * j;
        if (std::strcmp(reinterpret_cast<const char *>(H(U32(node + 7) + table)), reinterpret_cast<const char *>(k)))
            continue;
        const uint32_t dist = (j + size - start) % size;
        if (dist < best_dist) {
            best_dist = dist;
            best = j;
        }
    }
    if (best_dist >= probes)
        return 0xffff;
    const uint32_t str = U32(strings + 4 * best);
    std::memcpy(H(out), &str, 4);
    uint16_t index;
    std::memcpy(&index, H(nodes + 17 * best + 1), 2);
    return index;
}

// ---- DCC animations ------------------------------------------------------------------
//
// D2CMP's DCC decoder turns the game's animation files into frames as
// monsters and players are loaded. Its cell decoder was ~11% of the main
// thread while an area loads: per pixel, a bit-reader call and a lookup.

constexpr uint32_t kDccMasks = 0x006e6030;   // [n] = (1 << n) - 1, n = 0..32
uint32_t g_dcc_mask[33];

// 60b200(edi=bits, edx=stream): the next `bits` bits, from the low end of
// 32-bit words. Stream: +4 the current word, +0xc the bit position in it.
uint32_t dcc_bits(uint32_t stream, uint32_t n)
{
    if (!n)
        return 0;
    uint32_t pos = U32(stream + 0xc), word = U32(stream + 4);
    const int32_t avail = 32 - (int32_t)pos;
    uint32_t v;
    if ((int32_t)n <= avail) {
        v = (U32(word) >> pos) & g_dcc_mask[n];
        pos += n;
        if (pos == 32) {
            word += 4;
            pos = 0;
        }
    } else {
        v = (U32(word) >> pos) & g_dcc_mask[avail];
        word += 4;
        pos = n - (uint32_t)avail;
        v |= (U32(word) & g_dcc_mask[pos]) << avail;
    }
    std::memcpy(H(stream + 4), &word, 4);
    std::memcpy(H(stream + 0xc), &pos, 4);
    return v;
}

// 60b7b0(ecx=&entry, edx=equal-cell stream or 0, esi=cell, [8]=pixel
// stream, [c]=previous cell), ret 8: one cell. Cell: +0 its 4-colour entry,
// +4 pixels, +8 stride, +0xc width, +0x10 height. With an equal-cell
// stream and a previous cell, a set bit copies the previous cell (through a
// 5-byte-a-row buffer, as the original) when the sizes match, else clears
// the cell. Otherwise the entry's distinct leading colours give 0, 1 or 2
// bits a pixel: 0 fills the cell with the first colour, else each pixel's
// code picks one of *entry's four; then *entry moves on by 4.
void dcc_cell_do(uint32_t pentry, uint32_t eq, uint32_t cell, uint32_t pix, uint32_t prev)
{
    const uint32_t width = U32(cell + 0xc), stride = U32(cell + 8);
    const int32_t height = I32(cell + 0x10);
    if (eq && U32(prev) && dcc_bits(eq, 1)) {
        if (width == U32(prev + 0xc) && (uint32_t)height == U32(prev + 0x10)) {
            const uint32_t src = U32(prev + 4), dst = U32(cell + 4);
            if (src == dst)
                return;
            // Room for any height and width: rows overlap when a cell is
            // wider than 5, as they do in the original's 0x1c-byte buffer.
            if (height > 64 || width > 64)
                return;
            uint8_t tmp[5 * 64 + 64];
            const uint32_t src_stride = U32(prev + 8);
            for (int32_t r = 0; r < height; r++)
                std::memcpy(tmp + 5 * r, H(src + src_stride * (uint32_t)r), width);
            for (int32_t r = 0; r < height; r++)
                std::memcpy(H(dst + stride * (uint32_t)r), tmp + 5 * r, width);
        } else {
            const uint32_t dst = U32(cell + 4);
            for (int32_t r = 0; r < height; r++)
                std::memset(H(dst + stride * (uint32_t)r), 0, width);
        }
        return;
    }
    const uint8_t *e = H(U32(cell));
    uint32_t bits = 0;
    for (uint32_t i = 1; e[i] != e[i - 1];) {
        bits++;
        i = 1u << bits;
        if (i + 1 > 4)
            break;
    }
    uint32_t dst = U32(cell + 4);
    const uint8_t *colors = H(U32(pentry));
    if (!bits) {
        for (int32_t r = 0; r < height; r++, dst += stride)
            std::memset(H(dst), colors[0], width);
    } else {
        for (int32_t r = 0; r < height; r++, dst += stride)
            for (int32_t x = 0; x < (int32_t)width; x++)
                *H(dst + (uint32_t)x) = colors[dcc_bits(pix, bits)];
    }
    const uint32_t next = U32(pentry) + 4;
    std::memcpy(H(pentry), &next, 4);
}

// Called by the translated code itself (dyn86_intrin_add below): a cell is
// a few dozen pixels and an area load decodes hundreds of thousands, so the
// trap and the bridge's in-line path both cost about as much as the work.
Cpu *g_cpu = nullptr;
dyn86_intrin_t *g_dcc_intrin = nullptr;   // its table entry: the helper keeps the counts

int dcc_cell_intrin(void *, uint32_t esp)
{
    g_dcc_intrin->calls++;
    g_dcc_intrin->served++;
    dcc_cell_do(g_cpu->reg(R_ECX), g_cpu->reg(R_EDX), g_cpu->reg(R_ESI), U32(esp + 4), U32(esp + 8));
    return 1;
}

// 60b2b0(ecx=iterator, eax=frame set): the next DCC cell, in 4x4 steps
// over each frame's box, frames with an empty box skipped; 0 at the end.
// The iterator is the caller's 0x3c-byte frame block, so it stays in guest
// memory, field for field (+0x20 the cell number, +0x34 / +0x38 its size).
inline void put32(uint32_t va, uint32_t v) { std::memcpy(H(va), &v, 4); }

uint32_t dcc_next(uint32_t it, uint32_t set)
{
    const auto f = [it](uint32_t o) { return I32(it + o); };
    const auto put = [it](uint32_t o, int32_t v) { put32(it + o, (uint32_t)v); };
    int32_t w = f(0x34);
    put(0x24, f(0x24) + w);
    int32_t x = f(0x24), width = f(4);
    if (x < width) {
        put(0x30, f(0x30) + w);
        put(0x20, f(0x20) + 1);
        put(0x34, x + 5 < width ? 4 : width - x);
        return 1;
    }
    put(0x28, f(0x28) + f(0x38));
    int32_t y = f(0x28);
    const int32_t height = f(8);
    if (y < height) {
        const int32_t nx = -(f(0xc) % 4);
        put(0x24, 0);
        put(0x34, nx + 5 < width ? nx + 4 : width);
        put(0x2c, f(0x2c) + f(0x18) * f(0x38));
        put(0x30, f(0x2c));
        put(0x1c, f(0x1c) + I32(set + 0x28));
        put(0x20, f(0x1c));
        y = f(0x28);
        put(0x38, y + 5 < height ? 4 : height - y);
        return 1;
    }
    put(0, f(0) + 1);
    int32_t n = f(0);
    uint32_t frame = (uint32_t)n * 0x2c + U32(set + 4);
    for (; n < I32(set + 0x1c); n++, put(0, n), frame += 0x2c)
        if (U32(frame + 4) && U32(frame + 8)) {
            put(4, I32(frame + 4));
            put(8, I32(frame + 8));
            put(0xc, I32(frame + 0xc) - I32(set + 8));
            const int32_t top = I32(frame + 0x28) ? 0 : I32(frame + 8);
            const int32_t by = I32(frame + 0x10) - I32(set + 0xc) - top;
            put(0x10, by);
            put(0x14, I32(frame + 0x20));
            put(0x18, I32(frame + 0x24));
            const int32_t bx = f(0xc);
            const int32_t cell = bx / 4 + (by / 4) * I32(set + 0x28);
            put(0x1c, cell);
            put(0x20, cell);
            put(0x24, 0);
            put(0x28, 0);
            put(0x2c, 0);
            put(0x30, 0);
            const int32_t ax = -(bx % 4), ay = -(by % 4);
            put(0x34, ax + 5 < f(4) ? ax + 4 : f(4));
            put(0x38, ay + 5 < f(8) ? ay + 4 : f(8));
            return 1;
        }
    return 0;
}

uint32_t g_dcc_bit[4];   // 0x6ce268: 1 << n

// 60b9a0([8]=frame set, [c]=cell slots (20 bytes, +0 the cell's last
// entry), [10]=equal-cell stream or 0, [14]=pixel-mask stream, [18]=
// encoding-type stream or 0, [1c]=raw-pixel stream, [20]=pixel-code stream,
// [24]=&{entries, bytes}, [28]=&code bits used), ret 0x24: DCC's pixel
// buffer, one 4-colour entry per changed cell. Per cell: an equal-cell bit
// keeps the last entry; else a 4-bit mask (0xf for a cell's first entry)
// says which colours are new, each 8 raw bits or the last colour plus 4-bit
// steps (more while a step is 0xf); a colour equal to the last ends the
// entry, the decoded ones moved up over the gaps and the rest cleared; the
// colours not in the mask come from the cell's previous entry.
uint32_t dcc_pixel_buffer(Cpu &c)
{
    const uint32_t set = c.arg(0), slots = c.arg(1), eq = c.arg(2), mask_s = c.arg(3), enc = c.arg(4);
    const uint32_t raw = c.arg(5), code = c.arg(6), pbe = c.arg(7), used = c.arg(8);
    const uint32_t start = ((U32(code + 4) - U32(code)) >> 2 << 5) + U32(code + 0xc);
    uint32_t cur = U32(pbe);
    if (U32(set + 0x1c)) {
        uint32_t it = c.reg(R_ESP) - 0x100;   // the original's 0x3c-byte iterator, below the stack
        std::memset(H(it), 0, 0x3c);
        put32(it, 0xffffffffu);
        for (uint32_t more = dcc_next(it, set); more; more = dcc_next(it, set)) {
            const uint32_t slot = slots + 20 * U32(it + 0x20);
            const uint32_t prev = U32(slot);
            uint32_t m = 0xf;
            if (prev) {
                if (eq && dcc_bits(eq, 1))
                    continue;   // equal cell: the last entry stands
                m = dcc_bits(mask_s, 4);
            }
            const bool is_raw = m && enc && dcc_bits(enc, 1);
            const uint32_t bits = m | 0xfffffff0u;
            const auto has = [bits](int32_t i) { return (bits & g_dcc_bit[i & 3]) != 0 || i >= 4; };
            uint8_t *e = H(cur);
            const uint8_t *pe = H(prev);   // with prev 0 every colour is in the mask
            uint8_t last = 0;
            int32_t i = 4;
            bool ended = false;
            while (i != 0) {
                i--;
                if (!(bits & g_dcc_bit[i])) {
                    e[i] = pe[i];
                    continue;
                }
                uint8_t col;
                if (is_raw) {
                    col = (uint8_t)dcc_bits(raw, 8);
                } else {
                    col = last;
                    uint32_t step;
                    do {
                        step = dcc_bits(code, 4);
                        col = (uint8_t)(col + step);
                    } while (step == 0xf);
                }
                e[i] = col;
                if (col == last) {
                    ended = true;
                    break;
                }
                last = col;
            }
            if (ended) {
                int32_t src = i, dst = -1;
                for (;;) {
                    do
                        src++;
                    while (!has(src));
                    if (src >= 4)
                        break;
                    do
                        dst++;
                    while (!has(dst));
                    e[dst] = e[src];
                }
                for (;;) {
                    do
                        dst++;
                    while (!has(dst));
                    if (dst >= 4)
                        break;
                    e[dst] = 0;
                }
                while (i != 0) {
                    i--;
                    if (!(bits & g_dcc_bit[i]))
                        e[i] = pe[i];
                }
            }
            put32(slot, cur);
            cur += 4;
        }
    }
    put32(used, ((U32(code + 4) - U32(code)) >> 2 << 5) - start + U32(code + 0xc));
    put32(pbe + 4, cur - U32(pbe));
    return 0;
}

// ---- Sprite cache ---------------------------------------------------------------------
//
// D2CMP's sprite lookup (0x6001f0) runs once per sprite drawn, so its per-hit
// helpers grow with the units on screen. In fights two of them were ~10% of
// the frame, most of it in the imports they call (issue #37).

// Native code runs between two guest thread switches (one cooperative
// scheduler, no guest thread in parallel), so a section that is free, or
// already held by the current thread, may be skipped: taking and releasing
// it around the work changes nothing. Held by another thread (preempted
// inside it), the original must run, and wait.
bool section_free(uint32_t cs)
{
    WxCrit *k = wx86_crit_lookup(cs);
    if (!k)
        return true;
    ThreadScheduler *s = wx86_sched();
    const GuestThread *t = s ? s->current() : nullptr;
    const uint32_t owner = __atomic_load_n(&k->owner, __ATOMIC_SEQ_CST) & ~WX86_CRIT_WAITERS;
    return owner == 0 || owner == (t ? t->id : 1u);
}

// Resumes the original at `va` once the shim returns: the bridge then pops
// the return address and `stack_args` dwords from the ESP set here.
void resume_original(Cpu &c, uint32_t va, uint32_t esp, uint32_t stack_args)
{
    c.set_reg(R_ESP, esp - 4 - 4 * stack_args);
    rt::bridge().redirect_next(va);
}

// 5ff1b0(ecx=cache, eax=entry): unlinks an entry from the list its flags
// name (+0xc bit 0: the cache's +0x1c / +0x20 list, else +0x14 / +0x18,
// head / tail); entries link by +0x14 next and +0x18 previous. A pinned head
// (+0xc bit 1) stays. Null links are written through as the original does.
void cache_unlink(uint32_t cache, uint32_t e)
{
    const uint32_t head_at = cache + ((U32(e + 0xc) & 1) ? 0x1c : 0x14), tail_at = head_at + 4;
    const uint32_t head = U32(head_at);
    if (!head)
        return;
    if (e == head) {
        if (U32(head + 0xc) & 2)
            return;
        const uint32_t next = U32(head + 0x14);
        put32(head_at, next);
        if (next)
            put32(next + 0x18, 0);
        else
            put32(tail_at, 0);
        put32(head + 0x14, 0);
        put32(head + 0x18, 0);
        return;
    }
    if (e == U32(tail_at)) {
        const uint32_t prev = U32(e + 0x18);
        put32(tail_at, prev);
        put32(prev + 0x14, 0);
        put32(e + 0x14, 0);
        put32(e + 0x18, 0);
        return;
    }
    const uint32_t prev = U32(e + 0x18);
    if (!prev && !U32(e + 0x14))
        return;   // in no list
    put32(prev + 0x14, U32(e + 0x14));
    put32(U32(e + 0x14) + 0x18, U32(e + 0x18));
    put32(e + 0x14, 0);
    put32(e + 0x18, 0);
}

// 5ff5b0(ecx=cache, edx=entry), a cache hit: if the cache is on (+0xc),
// counts the hit (+0x4c), and under its lock (+0x30) moves the entry to the
// recently used end of the main list, flag bit 0 cleared, stamped with
// GetTickCount (+0x10). The stamp is now_ms, the value the GetTickCount
// intrinsic gives: evictions go by it. Its only caller reads no register
// after it.
uint32_t cache_touch(Cpu &c)
{
    const uint32_t cache = c.reg(R_ECX), e = c.reg(R_EDX);
    if (!U32(cache + 0xc))
        return c.reg(R_EAX);
    if (!e || !section_free(cache + 0x30)) {
        // The original from 5ff5bc (the jmp covers 5ff5b3): push esi, push
        // edi, esi = cache, edi = entry. A null entry is its fatal error.
        const uint32_t esp = c.reg(R_ESP);
        put32(esp - 4, c.reg(R_ESI));
        put32(esp - 8, c.reg(R_EDI));
        c.set_reg(R_ESI, cache);
        c.set_reg(R_EDI, e);
        resume_original(c, 0x005ff5bc, esp - 8, 0);
        return c.reg(R_EAX);
    }
    put32(cache + 0x4c, U32(cache + 0x4c) + 1);
    cache_unlink(cache, e);
    put32(e + 0xc, U32(e + 0xc) & ~1u);
    const uint32_t tail = U32(cache + 0x18);
    if (!tail) {
        put32(cache + 0x14, e);
    } else {
        put32(tail + 0x14, e);
        put32(e + 0x18, U32(cache + 0x18));
    }
    put32(cache + 0x18, e);
    put32(e + 0x10, (uint32_t)rt::now_ms());
    return 0;   // LeaveCriticalSection's
}

constexpr uint32_t kUseDcc = 0x00741efc;          // nonzero: animations from .dcc files
constexpr uint32_t kLocaleChanged = 0x0096dfe4;   // the C runtime's __locale_changed
constexpr uint32_t kDc6Name = 0x006e363c;         // the one name always drawn from .dc6
constexpr uint32_t kDirChars = 0x006dcc34, kDirMonsters = 0x006d6cac, kDirObjects = 0x006e3628;

// What 5fe610's monster class tables (ids 0xf2..0x16f and 0x220..0x2c5) do
// to the .dcc choice; read from its jump tables at install.
enum : uint8_t { kKeepDcc, kDc6, kDc6IfNoMode };
uint8_t g_mon_lo[0x7e], g_mon_hi[0xa6];

bool read_class_table(uint32_t index, uint32_t targets, uint8_t *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t k = *H(index + i);
        if (k > 2)
            return false;
        switch (U32(targets + 4 * k)) {
        case 0x005fe6ee: out[i] = kKeepDcc; break;
        case 0x005fe6e5: out[i] = kDc6; break;
        case 0x005fe6df: out[i] = kDc6IfNoMode; break;
        default: return false;
        }
    }
    return true;
}

// The C runtime's _strnicmp(a, b, 0x7fffffff) == 0 in the C locale.
bool ascii_ieq(const uint8_t *a, const uint8_t *b)
{
    for (;; a++, b++) {
        const uint8_t x = *a >= 'A' && *a <= 'Z' ? *a + 0x20 : *a;
        const uint8_t y = *b >= 'A' && *b <= 'Z' ? *b + 0x20 : *b;
        if (x != y)
            return false;
        if (!x)
            return true;
    }
}

// 5fe610(ebx=out, esi=unit's sprite info, [8]=name), ret 4: the sprite's
// file, "<dir>\<token>\<mode>\<name>.dcc" or ".dc6", 1; or an empty string,
// 0. Info: +8 the unit type (0 player, 1 monster, 2 object; 3, 4 and 6 have
// other patterns, left to the original), +0xc the class, +0x10 the mode,
// +0x18 / +0x1c three-character tokens (a space ends one), +0x44 bit 0 set
// for a .dcc file and cleared for the .dc6 name. Formatted as the wsprintfA
// shim formats it: at most 1024 characters, each string read up to 1024.
uint32_t sprite_path(Cpu &c)
{
    const uint32_t out = c.reg(R_EBX), info = c.reg(R_ESI), name = c.arg(0);
    if (!*H(name)) {
        *H(out) = 0;
        c.set_reg(R_ECX, name);
        return 0;
    }
    const uint32_t type = U32(info + 8);
    uint32_t dcc = U32(kUseDcc);
    if (type == 3 || type == 4 || type == 6 || (dcc && U32(kLocaleChanged))) {
        // The original from 5fe616 (the jmp covers 5fe613): push ebp, mov
        // ebp,esp, mov ecx,[ebp+8].
        const uint32_t esp = c.reg(R_ESP);
        put32(esp - 4, c.reg(R_EBP));
        c.set_reg(R_EBP, esp - 4);
        c.set_reg(R_ECX, name);
        resume_original(c, 0x005fe616, esp - 4, 1);
        return c.reg(R_EAX);
    }
    if (dcc && ascii_ieq(H(name), H(kDc6Name))) {
        dcc = 0;
        put32(info + 0x44, U32(info + 0x44) & ~1u);
    }
    uint32_t dir;
    const int32_t cls = I32(info + 0xc);
    if (type == 0) {
        dir = kDirChars;
    } else if (type == 1) {
        dir = kDirMonsters;
        uint8_t act = kKeepDcc;
        if (cls == 0x209)
            act = kDc6;
        else if (cls > 0x209 && (uint32_t)(cls - 0x220) <= 0xa5)
            act = g_mon_hi[cls - 0x220];
        else if (cls < 0x209 && (uint32_t)(cls - 0xf2) <= 0x7d)
            act = g_mon_lo[cls - 0xf2];
        if (act == kDc6 || (act == kDc6IfNoMode && !U32(info + 0x10)))
            dcc = 0;
    } else if (type == 2) {
        dir = kDirObjects;
        if (cls == 0x156 || cls == 0x233)
            dcc = 0;
    } else {
        *H(out) = 0;
        return 0;
    }
    char token[4] = {}, mode[4] = {};
    for (int i = 0; i < 3; i++) {
        const uint8_t t = *H(info + 0x18 + i), m = *H(info + 0x1c + i);
        token[i] = t == ' ' ? 0 : (char)t;
        mode[i] = m == ' ' ? 0 : (char)m;
    }
    char buf[1025];
    uint32_t n = 0;
    const auto put = [&](const char *s) {
        for (uint32_t i = 0; i < 1024 && s[i]; i++)
            if (n < 1024)
                buf[n++] = s[i];
    };
    put(reinterpret_cast<const char *>(H(dir)));
    put("\\");
    put(token);
    put("\\");
    put(mode);
    put("\\");
    put(reinterpret_cast<const char *>(H(name)));
    put(dcc ? ".dcc" : ".dc6");
    buf[n] = 0;
    std::memcpy(H(out), buf, n + 1);
    if (dcc)
        put32(info + 0x44, U32(info + 0x44) | 1);
    return 1;
}

// ---- Locked lookups -------------------------------------------------------------------
//
// A table at [0x883d38] of entries found by id, each with its own lock, and
// a current entry at [0x882d34] taken by id from [0x882d38]. A lookup
// returns the entry locked; 52df40 unlocks the current one. In fights the
// three cost ~3.2 ms of an 81 ms frame, almost all in their four or five
// lock calls (issue #38).
constexpr uint32_t kLockTable = 0x00883d38;     // the table: +0x1c buckets, +0x24 mask, +0x50 its lock
constexpr uint32_t kCurLock = 0x00882d18;       // guards the current id
constexpr uint32_t kCurEntry = 0x00882d34, kCurId = 0x00882d38;

// 52e650(ecx=table, [8]=id): buckets of 12 bytes, {+0 the offset of the
// link in an entry, +8 the first entry}; an entry's next is at link + 4 and
// a list ends at a value <= 0 (signed). The id is the entry's first dword.
uint32_t table_find(uint32_t table, uint32_t id)
{
    const uint32_t bucket = U32(table + 0x1c) + 12 * (id & U32(table + 0x24));
    for (int32_t e = I32(bucket + 8); e > 0; e = I32(U32(bucket) + e + 4))
        if (U32(e) == id)
            return e;
    return 0;
}

// Takes a section for the current thread, as EnterCriticalSection does when
// it doesn't wait; false (and nothing taken) when another thread holds it.
bool take_section(uint32_t cs)
{
    WxCrit *k = wx86_crit_for(cs);
    ThreadScheduler *s = wx86_sched();
    GuestThread *t = s ? s->current() : nullptr;
    return k->ready(t) && k->try_acquire(t);
}

// 52e860's work without its waits: the entry for `id` in `e`, locked, or 0.
// False, with nothing taken, when the original must run: the table's lock
// or the entry's held by another thread, or an entry with no lock (the
// original's fatal error). The table's lock (+0x50) is skipped when free or
// the current thread's, as section_free says.
bool lookup_locked(uint32_t id, uint32_t &e)
{
    const uint32_t table = U32(kLockTable);
    e = 0;
    if (!table)
        return true;
    if (!section_free(table + 0x50))
        return false;
    e = table_find(table, id);
    if (!e)
        return true;
    const uint32_t cs = U32(e + 0x18);
    return cs && take_section(cs);
}

// 52e860(ecx=id): the entry, locked, or 0.
uint32_t locked_lookup(Cpu &c)
{
    uint32_t e;
    if (lookup_locked(c.reg(R_ECX), e))
        return e;
    // The original from 52e868 (the jmp covers 52e864): push ebx, push esi,
    // esi = the table.
    const uint32_t esp = c.reg(R_ESP);
    put32(esp - 4, c.reg(R_EBX));
    put32(esp - 8, c.reg(R_ESI));
    c.set_reg(R_ESI, U32(kLockTable));
    resume_original(c, 0x0052e868, esp - 8, 0);
    return c.reg(R_EAX);
}

// 52f6e0(): the current entry, looked up (52e860) by the id at [0x882d38],
// read under its own lock (0 and -1: none), stored at [0x882d34] and
// returned locked.
uint32_t current_lookup(Cpu &c)
{
    const uint32_t esp = c.reg(R_ESP);
    if (!section_free(kCurLock)) {
        // The original from 52f6e6 (the jmp covers 52f6e4): push esi, push
        // the lock for its EnterCriticalSection.
        put32(esp - 4, c.reg(R_ESI));
        put32(esp - 8, kCurLock);
        resume_original(c, 0x0052f6e6, esp - 8, 0);
        return c.reg(R_EAX);
    }
    uint32_t id = U32(kCurId), e;
    if (id == 0xffffffffu)
        id = 0;
    if (!lookup_locked(id, e)) {
        // The original from 52f709, past its lock: push esi, esi = the id,
        // then the call to 52e860 (which waits) and the store.
        put32(esp - 4, c.reg(R_ESI));
        c.set_reg(R_ESI, id);
        resume_original(c, 0x0052f709, esp - 4, 0);
        return c.reg(R_EAX);
    }
    put32(kCurEntry, e);
    return e;
}

// 52df40(): unlocks the current entry (LeaveCriticalSection on its +0x18).
uint32_t current_unlock(Cpu &c)
{
    const uint32_t e = U32(kCurEntry), cs = e ? U32(e + 0x18) : 0;
    if (!cs) {
        // The original's fatal error, from 52df45 (the jmp covers its first
        // instruction, eax = the current entry).
        resume_original(c, 0x0052df45, c.reg(R_ESP), 0);
        return e;
    }
    wx86_crit_leave(cs, wx86_sched());
    return 0;   // LeaveCriticalSection's
}

// D2_NATIVE_CHECK=1 (Linux lane): each call to one of the natives below that
// the native code takes also runs the original, from a trampoline in .text's
// zero slack, on the same input, and logs where the results differ. The
// native result is the one kept. Not in line (guest_call).
constexpr uint32_t kCheckTouch = 0x006cb7e0, kCheckPath = 0x006cb7f0, kCheckLookup = 0x006cb800,
                   kCheckCurrent = 0x006cb810, kCheckEnd = 0x006cb820;
uint32_t g_check_calls[4], g_check_bad[4];

void check_count(int k, bool bad, const char *name)
{
    g_check_calls[k]++;
    g_check_bad[k] += bad;
    const uint32_t n = g_check_calls[k];
    if ((n & (n - 1)) == 0 || (n & 0xffff) == 0)
        rt::logf("native check: %s %u calls, %u differ", name, g_check_calls[k], g_check_bad[k]);
}

// The places 5ff5b0 can write: the cache, the entry, its neighbours, the tail.
uint32_t cache_touch_checked(Cpu &c)
{
    const uint32_t cache = c.reg(R_ECX), e = c.reg(R_EDX);
    if (!U32(cache + 0xc) || !e || !section_free(cache + 0x30))
        return cache_touch(c);
    const uint32_t at[5] = {cache, e, U32(e + 0x14), U32(e + 0x18), U32(cache + 0x18)};
    const uint32_t len[5] = {0x50, 0x1c, 0x1c, 0x1c, 0x1c};
    uint8_t before[5][0x50], orig[5][0x50];
    for (int i = 0; i < 5; i++)
        if (at[i])
            std::memcpy(before[i], H(at[i]), len[i]);
    rt::guest_call(c, kCheckTouch, {});
    for (int i = 0; i < 5; i++)
        if (at[i])
            std::memcpy(orig[i], H(at[i]), len[i]);
    for (int i = 4; i >= 0; i--)
        if (at[i])
            std::memcpy(H(at[i]), before[i], len[i]);
    const uint32_t r = cache_touch(c);
    bool bad = false;
    for (int i = 0; i < 5; i++)
        if (at[i] && std::memcmp(orig[i], H(at[i]), len[i])) {
            if (g_check_bad[0] < 20)
                rt::logf("native check: SpriteCacheTouch(%08x, %08x) differs at %08x", cache, e, at[i]);
            bad = true;
        }
    check_count(0, bad, "SpriteCacheTouch");
    return r;
}

uint32_t sprite_path_checked(Cpu &c)
{
    const uint32_t out = c.reg(R_EBX), info = c.reg(R_ESI), name = c.arg(0), type = U32(info + 8);
    if (!*H(name) || type == 3 || type == 4 || type == 6 || (U32(kUseDcc) && U32(kLocaleChanged)))
        return sprite_path(c);
    const uint32_t flags = U32(info + 0x44);
    const uint32_t r0 = rt::guest_call(c, kCheckPath, {name});
    char s0[1025];
    for (uint32_t i = 0; i < sizeof s0 && (s0[i] = (char)*H(out + i)) != 0; i++) {
    }
    const uint32_t f0 = U32(info + 0x44);
    put32(info + 0x44, flags);
    const uint32_t r = sprite_path(c);
    const bool bad = r != r0 || U32(info + 0x44) != f0 || std::strcmp(s0, reinterpret_cast<const char *>(H(out)));
    if (bad && g_check_bad[1] < 20)
        rt::logf("native check: SpritePath(type %u, class %d, \"%s\") gave \"%s\" %u %08x, the original \"%s\" %u %08x",
                 type, I32(info + 0xc), reinterpret_cast<const char *>(H(name)), reinterpret_cast<const char *>(H(out)), r,
                 U32(info + 0x44), s0, r0, f0);
    check_count(1, bad, "SpritePath");
    return r;
}

// An entry's lock as the lookups leave it: its owner and recursion count.
struct EntryLock {
    uint32_t owner = 0;
    int count = 0;
    bool operator!=(const EntryLock &o) const { return owner != o.owner || count != o.count; }
};

EntryLock entry_lock(uint32_t e)
{
    const WxCrit *k = e && U32(e + 0x18) ? wx86_crit_lookup(U32(e + 0x18)) : nullptr;
    return k ? EntryLock{k->owner, k->count} : EntryLock{};
}

// The original's entry, then its lock given back (the original took it as
// the native will), so that the native runs on the same state.
uint32_t undo_lookup(uint32_t e)
{
    if (e && U32(e + 0x18))
        wx86_crit_leave(U32(e + 0x18), wx86_sched());
    return e;
}

// lookup_locked would serve this id (it takes nothing to find out).
bool lookup_native(uint32_t id)
{
    const uint32_t table = U32(kLockTable);
    if (!table)
        return true;
    if (!section_free(table + 0x50))
        return false;
    const uint32_t e = table_find(table, id);
    return !e || (U32(e + 0x18) && section_free(U32(e + 0x18)));
}

// `stored`: the native left at [0x882d34] what the original did (always, for
// 52e860, which stores nothing).
void check_lookup(int k, const char *name, uint32_t id, uint32_t e0, const EntryLock &l0, uint32_t e, bool stored)
{
    const EntryLock l = entry_lock(e);
    const bool bad = e != e0 || l != l0 || !stored;
    if (bad && g_check_bad[k] < 20)
        rt::logf("native check: %s(%08x) gave %08x (lock %x/%d), the original %08x (lock %x/%d)%s", name, id, e,
                 l.owner, l.count, e0, l0.owner, l0.count, stored ? "" : ", stored apart");
    check_count(k, bad, name);
}

uint32_t locked_lookup_checked(Cpu &c)
{
    const uint32_t id = c.reg(R_ECX);
    if (!lookup_native(id))
        return locked_lookup(c);
    const uint32_t e0 = rt::guest_call(c, kCheckLookup, {});
    const EntryLock l0 = entry_lock(e0);
    undo_lookup(e0);
    const uint32_t r = locked_lookup(c);
    check_lookup(2, "LockedLookup", id, e0, l0, r, true);
    return r;
}

uint32_t current_lookup_checked(Cpu &c)
{
    const uint32_t id = U32(kCurId) == 0xffffffffu ? 0 : U32(kCurId);
    if (!section_free(kCurLock) || !lookup_native(id))
        return current_lookup(c);
    const uint32_t before = U32(kCurEntry);
    const uint32_t e0 = rt::guest_call(c, kCheckCurrent, {}), stored0 = U32(kCurEntry);
    const EntryLock l0 = entry_lock(e0);
    undo_lookup(e0);
    put32(kCurEntry, before);
    const uint32_t r = current_lookup(c);
    check_lookup(3, "CurrentLookup", id, e0, l0, r, stored0 == e0 && U32(kCurEntry) == r);
    return r;
}

// The trampolines: each original's first instructions (the ones its hook's
// jmp covers), then a jmp to the rest.
bool install_check(Cpu &c)
{
    uint8_t slack[kCheckEnd - kCheckTouch];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kCheckTouch, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack))
        return false;
    uint8_t touch[12] = {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x0c, 0x00, 0xe9};
    const uint32_t rel_touch = 0x005ff5b7 - (kCheckTouch + 12);
    std::memcpy(touch + 8, &rel_touch, 4);
    uint8_t path[11] = {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x08, 0xe9};
    const uint32_t rel_path = 0x005fe616 - (kCheckPath + 11);
    std::memcpy(path + 7, &rel_path, 4);
    uint8_t lookup[13] = {0x53, 0x56, 0x8b, 0x35, 0x38, 0x3d, 0x88, 0x00, 0xe9};
    const uint32_t rel_lookup = 0x0052e868 - (kCheckLookup + 13);
    std::memcpy(lookup + 9, &rel_lookup, 4);
    uint8_t current[11] = {0x56, 0x68, 0x18, 0x2d, 0x88, 0x00, 0xe9};
    const uint32_t rel_current = 0x0052f6e6 - (kCheckCurrent + 11);
    std::memcpy(current + 7, &rel_current, 4);
    c.write(kCheckTouch, touch, sizeof touch);
    c.write(kCheckPath, path, sizeof path);
    c.write(kCheckLookup, lookup, sizeof lookup);
    c.write(kCheckCurrent, current, sizeof current);
    c.invalidate_code(kCheckTouch, sizeof slack);
    return true;
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
    for (int i = 0; i <= 32; i++)
        g_dcc_mask[i] = U32(kDccMasks + 4 * i);
    // DccCell: a translation-time intrinsic, not a hook. ecx, edx and esi
    // are its register arguments (stored for it), ret 8; it changes no guest
    // register the caller reads. Its only entry is the call at 0x60cbbe, and
    // nothing jumps into its body (checked by disassembly), so every entry
    // starts a block at its first byte, as the mechanism requires.
    static const uint8_t k60b7b0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x30, 0xa1, 0xe0, 0x9d, 0x74, 0x00};
    uint8_t cell_have[sizeof k60b7b0];
    g_cpu = &c;
    for (int i = 0; i < 4; i++)
        g_dcc_bit[i] = U32(0x006ce268 + 4 * i);
    if ((mask & bit) && c.read(0x0060b7b0, cell_have, sizeof cell_have) && !std::memcmp(cell_have, k60b7b0, sizeof cell_have) &&
        dyn86_intrin_add(0x0060b7b0, dcc_cell_intrin, 8, DYN86_IR_CX | DYN86_IR_DX | DYN86_IR_SI, 0, "DccCell")) {
        g_dcc_intrin = &dyn86_intrin_tbl[dyn86_intrin_n - 1];
        dyn86_intrin_on = 1;
        rt::logf("native DccCell at 0060b7b0 (translation-time intrinsic)");
        n++;
    }
    bit <<= 1;
    static const uint8_t k60b9a0[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x68, 0x8b, 0x45, 0x24};
    HOOK(c, 0x0060b9a0, k60b9a0, sizeof k60b9a0, 9, dcc_pixel_buffer, "DccPixelBuffer", true);
    // The C runtime's memcpy (0x6829c0, memmove semantics) and memset
    // (0x681ef0), ~4% of the main thread while an area loads: served by the
    // dynarec at their call (WinVita mode 3: copies under 64 bytes in line,
    // larger ones by a native helper), no trap. Every entry is a call to the
    // first byte, and nothing jumps into either body (checked by
    // disassembly). Armed before the game runs: the translator reads it.
    static const uint8_t k6829c0[] = {0x55, 0x8b, 0xec, 0x57, 0x56, 0x8b, 0x75, 0x0c};
    static const uint8_t k681ef0[] = {0x8b, 0x54, 0x24, 0x0c, 0x8b, 0x4c, 0x24, 0x04};
    uint8_t have[8], have2[8];
    if ((mask & bit) && c.read(0x006829c0, have, 8) && !std::memcmp(have, k6829c0, 8) &&
        c.read(0x00681ef0, have2, 8) && !std::memcmp(have2, k681ef0, 8)) {
        dyn86_mi_set_span(rt::layout().span);
        dyn86_mi_arm(3, 0x006829c0, 0x00681ef0);
        rt::logf("memcpy/memset: served by the dynarec (in line under 64 bytes)");
        n++;
    }
    bit <<= 1;
    const bool check = std::getenv("D2_NATIVE_CHECK") && install_check(c);
    static const uint8_t k5ff5b0[] = {0x56, 0x8b, 0xf1, 0x83, 0x7e, 0x0c, 0x00, 0x57, 0x8b, 0xfa};
    HOOK(c, 0x005ff5b0, k5ff5b0, sizeof k5ff5b0, 0, check ? cache_touch_checked : cache_touch, "SpriteCacheTouch",
         !check);
    // SpritePath: only with its class tables as expected.
    static const uint8_t k5fe610[] = {0x55, 0x8b, 0xec, 0x8b, 0x4d, 0x08, 0x83, 0xec, 0x08, 0x80, 0x39, 0x00};
    if (read_class_table(0x005fe85c, 0x005fe850, g_mon_lo, sizeof g_mon_lo) &&
        read_class_table(0x005fe8e8, 0x005fe8dc, g_mon_hi, sizeof g_mon_hi))
        HOOK(c, 0x005fe610, k5fe610, sizeof k5fe610, 1, check ? sprite_path_checked : sprite_path, "SpritePath",
             !check);
    else
        bit <<= 1;
    // The locked lookups: their only entries are calls to the first byte
    // (checked by disassembly).
    static const uint8_t k52e860[] = {0x53, 0x56, 0x8b, 0x35, 0x38, 0x3d, 0x88, 0x00, 0x85, 0xf6, 0x8b, 0xd9};
    HOOK(c, 0x0052e860, k52e860, sizeof k52e860, 0, check ? locked_lookup_checked : locked_lookup, "LockedLookup",
         !check);
    static const uint8_t k52f6e0[] = {0x56, 0x68, 0x18, 0x2d, 0x88, 0x00, 0x33, 0xf6};
    HOOK(c, 0x0052f6e0, k52f6e0, sizeof k52f6e0, 0, check ? current_lookup_checked : current_lookup,
         "CurrentLookup", !check);
    static const uint8_t k52df40[] = {0xa1, 0x34, 0x2d, 0x88, 0x00, 0x85, 0xc0, 0x75, 0x1f};
    HOOK(c, 0x0052df40, k52df40, sizeof k52df40, 0, current_unlock, "CurrentUnlock", true);
#undef HOOK
    return n;
}

uint64_t dcc_cells_decoded() { return g_dcc_intrin ? g_dcc_intrin->calls : 0; }

}  // namespace game
