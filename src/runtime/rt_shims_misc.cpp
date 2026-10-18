// The small DLLs: WINMM, SHELL32, OLE32, CRYPT32/WINTRUST, BINKW32, SMACKW32,
// and the GDI32 calls WinVita leaves out.
//
// Videos: only the game's DirectDraw, Direct3D and Glide renderers call
// binkw32, so in the GDI renderer the runtime runs it is never called; the
// cinematics are played natively instead (src/game/cinematics.cpp). The GDI
// renderer calls smackw32 for .smk resources (its function table +0x40),
// which no traced run reached. Both stay failing, as a safety net.
// Code signing: there is nothing to verify offline, so every check reports
// "not signed" and the game carries on (it only matters to Battle.net).
#include "runtime/rt_host.h"

#include "runtime/bridge.h"
#include "runtime/ds_emul.h"
#include "runtime/cpu.h"
#include "runtime/guest_region.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace d2rt;

namespace rt {
namespace {

// GDI as the game uses it in windowed mode: draw into an 8-bit DIB section,
// select it into a memory DC, set its color table, blit it to the window.
// The blit is where a frame is finished, so it is handed to the platform
// (RunOptions::present) straight from guest memory.
struct Dib {
    uint32_t bits, width, height, bpp;
    bool top_down;
    uint32_t palette[256];
};
std::map<uint32_t, Dib> g_dibs;              // handle -> DIB section
std::map<uint32_t, uint32_t> g_dc_selected;  // memory DC -> selected DIB handle
uint32_t g_next_dib = 0x0D000010u, g_next_dc = 0x87000100u;
constexpr uint32_t kStockBitmap = 0x88000011u;   // what a fresh memory DC holds (WinVita's value)

Dib *dib_in(uint32_t dc)
{
    auto it = g_dc_selected.find(dc);
    if (it == g_dc_selected.end())
        return nullptr;
    auto d = g_dibs.find(it->second);
    return d == g_dibs.end() ? nullptr : &d->second;
}

uint32_t blit(Cpu &c, uint32_t src_dc)
{
    Dib *d = dib_in(src_dc);
    if (!d || d->bpp != 8)
        return 1;
    stats().frames++;
    if (options().audio)
        d2rt::dsound::frame_pump(c);   // drives a sink the game's frames pace (wav/null); no-op for a realtime one
    if (!options().present)
        return 1;
    const int32_t stride = (int32_t)(((d->width * 8 + 31) / 32) * 4);
    const uint8_t *base = (const uint8_t *)c.hostptr(d->bits, (uint32_t)stride * d->height);
    if (!base)
        return 1;
    Frame f;
    f.width = d->width;
    f.height = d->height;
    f.palette = d->palette;
    f.stride = d->top_down ? stride : -stride;
    f.pixels = d->top_down ? base : base + (size_t)stride * (d->height - 1);
    options().present(f);
    return 1;
}

}  // namespace

void install_misc(Bridge &br)
{
    auto reg = [&](const char *dll, const char *name, uint32_t argc, bool stdcall, std::function<uint32_t(Cpu &)> fn) {
        Shim s;
        s.argc = argc;
        s.stdcall_cleanup = stdcall;
        s.tag = std::string(dll) + "!" + name;
        s.fn = std::move(fn);
        br.register_shim(dll, name, s);
    };
    auto ret = [](uint32_t v) { return [v](Cpu &) { return v; }; };
    g_dibs.clear();
    g_dc_selected.clear();

    reg("WINMM.dll", "timeGetTime", 0, true, [](Cpu &) { return (uint32_t)now_ms(); });

    reg("SHELL32.dll", "SHGetFolderPathA", 5, true, [](Cpu &c) {
        write_cstr(c, c.arg(4), 260, "C:\\");   // every special folder is the game's own
        return 0u;                               // S_OK
    });
    reg("OLE32.dll", "CoTaskMemFree", 1, true, ret(0));

    // ---- signatures ----
    reg("WINTRUST.dll", "WinVerifyTrust", 3, true, ret(0x800B0100u));   // TRUST_E_NOSIGNATURE
    reg("CRYPT32.dll", "CryptQueryObject", 11, true, ret(0));
    reg("CRYPT32.dll", "CryptMsgGetParam", 5, true, ret(0));
    reg("CRYPT32.dll", "CryptMsgClose", 1, true, ret(1));
    reg("CRYPT32.dll", "CertFindCertificateInStore", 6, true, ret(0));
    reg("CRYPT32.dll", "CertFreeCertificateContext", 1, true, ret(1));
    reg("CRYPT32.dll", "CertCloseStore", 2, true, ret(1));
    reg("CRYPT32.dll", "CertGetNameStringW", 6, true, [](Cpu &c) {
        if (c.arg(4) && c.arg(5)) {
            const uint16_t nul = 0;
            c.write(c.arg(4), &nul, 2);
        }
        return 1u;   // characters written, the NUL included
    });

    // ---- video: unused (see the top of the file) ----
    const struct { const char *dll, *name; uint32_t argc, value; } videos[] = {
        {"BINKW32.dll", "_BinkOpen@8", 2, 0},          {"BINKW32.dll", "_BinkClose@4", 1, 0},
        {"BINKW32.dll", "_BinkNextFrame@4", 1, 0},     {"BINKW32.dll", "_BinkDoFrame@4", 1, 0},
        {"BINKW32.dll", "_BinkWait@4", 1, 0},          {"BINKW32.dll", "_BinkCopyToBuffer@28", 7, 0},
        {"BINKW32.dll", "_BinkDDSurfaceType@4", 1, 0}, {"BINKW32.dll", "_BinkSetSoundSystem@8", 2, 1},
        {"BINKW32.dll", "_BinkOpenDirectSound@4", 1, 0},
        {"SMACKW32.dll", "_SmackOpen@12", 3, 0},       {"SMACKW32.dll", "_SmackClose@4", 1, 0},
        {"SMACKW32.dll", "_SmackNextFrame@4", 1, 0},   {"SMACKW32.dll", "_SmackDoFrame@4", 1, 0},
        {"SMACKW32.dll", "_SmackWait@4", 1, 0},        {"SMACKW32.dll", "_SmackToBuffer@28", 7, 0},
    };
    for (const auto &v : videos)
        reg(v.dll, v.name, v.argc, true, ret(v.value));

    // ---- GDI32 ----
    // CreateDIBSection: the bits live in the VirtualAlloc region, where the
    // game draws and a renderer can later read them without a copy.
    reg("GDI32.dll", "CreateDIBSection", 6, true, [](Cpu &c) -> uint32_t {
        const uint32_t bmi = c.arg(1), ppv = c.arg(3);
        const uint32_t w = c.read_u32(bmi + 4);
        const int32_t h = (int32_t)c.read_u32(bmi + 8);
        uint16_t bpp = 0;
        c.read(bmi + 14, &bpp, 2);
        const uint32_t rows = (uint32_t)(h < 0 ? -h : h);
        const uint32_t size = ((w * bpp + 31) / 32) * 4 * rows;
        const uint32_t bits = size ? va_region().alloc(size) : 0;
        if (ppv)
            c.write_u32(ppv, bits);
        if (!bits)
            return 0;
        Dib d = {bits, w, rows, bpp, h < 0, {}};
        if (bpp == 8) {   // the color table follows the 40-byte header (RGBQUAD: B G R 0)
            uint32_t used = c.read_u32(bmi + 32);
            if (!used || used > 256)
                used = 256;
            for (uint32_t i = 0; i < used; i++) {
                const uint32_t q = c.read_u32(bmi + 40 + 4 * i);
                d.palette[i] = q & 0x00FFFFFFu;
            }
        }
        const uint32_t handle = g_next_dib++;
        g_dibs[handle] = d;
        stats().dib_sections++;
        logf("CreateDIBSection %ux%d %ubpp at %08x", w, h, bpp, bits);
        return handle;
    });
    reg("GDI32.dll", "CreateCompatibleDC", 1, true, [](Cpu &) {
        const uint32_t dc = g_next_dc++;
        g_dc_selected[dc] = kStockBitmap;
        return dc;
    });
    reg("GDI32.dll", "DeleteDC", 1, true, [](Cpu &c) {
        g_dc_selected.erase(c.arg(0));
        return 1u;
    });
    reg("GDI32.dll", "SelectObject", 2, true, [](Cpu &c) -> uint32_t {
        auto it = g_dc_selected.find(c.arg(0));
        if (it == g_dc_selected.end() || !(g_dibs.count(c.arg(1)) || c.arg(1) == kStockBitmap))
            return kStockBitmap;   // pens, brushes, fonts: no state kept
        const uint32_t prev = it->second;
        it->second = c.arg(1);
        return prev;
    });
    reg("GDI32.dll", "DeleteObject", 1, true, [](Cpu &c) {
        auto it = g_dibs.find(c.arg(0));
        if (it != g_dibs.end()) {
            va_region().free(it->second.bits);
            g_dibs.erase(it);
        }
        return 1u;
    });
    reg("GDI32.dll", "SetDIBColorTable", 4, true, [](Cpu &c) -> uint32_t {
        Dib *d = dib_in(c.arg(0));
        const uint32_t start = c.arg(1), n = c.arg(2), src = c.arg(3);
        if (!d || start >= 256)
            return 0;
        const uint32_t count = start + n > 256 ? 256 - start : n;
        uint32_t q[256];
        c.read(src, q, 4 * count);
        for (uint32_t i = 0; i < count; i++)
            d->palette[start + i] = q[i] & 0x00FFFFFFu;
        return count;
    });
    reg("GDI32.dll", "StretchBlt", 11, true, [](Cpu &c) { return blit(c, c.arg(5)); });
    reg("GDI32.dll", "BitBlt", 9, true, [](Cpu &c) { return blit(c, c.arg(5)); });
}

}  // namespace rt
