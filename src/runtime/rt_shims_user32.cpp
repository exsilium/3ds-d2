// USER32 window classes, windows and the message queue: the parts WinVita
// leaves to the port. WinVita's own USER32 shims (DefWindowProcA, geometry,
// TranslateMessage...) stay registered; these are added on top.
//
// Known simplifications (M3):
//  - CreateWindowExA sends no WM_NCCREATE/WM_CREATE yet.
//  - GetMessageA does not block on an empty queue: it yields briefly and
//    returns WM_NULL. Loops built on PeekMessageA (as D2's is) are exact.
#include "runtime/rt_host.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/host_clock.h"
#include "runtime/win32_shims_wait.h"

#include "runtime/win32_shims_gdi32.h"
#include "runtime/win32_shims_window.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

using namespace d2rt;

namespace rt {
namespace {

constexpr uint32_t WM_NULL = 0x0000, WM_QUIT = 0x0012;

struct Msg { uint32_t hwnd, message, wparam, lparam; };

std::map<std::string, uint32_t> g_classes;   // lower-case class name -> window procedure
std::map<uint32_t, uint32_t> g_windows;      // hwnd -> window procedure
std::deque<Msg> g_queue;
bool g_quit = false;
uint32_t g_quit_code = 0;
uint32_t g_next_hwnd = 0x00010010;
uint16_t g_next_atom = 0xC000;
uint32_t g_main_hwnd = 0;      // the first top-level window: focus, foreground, activation
bool g_activated = false;
bool g_keys[256];              // virtual-key state, fed by the platform's input layer (set_key)
struct Clip { bool open = false; uint32_t format = 0, handle = 0; } g_clip;

// ---- wsprintfA: d i u x X s c %, flags "-0# ", width, precision, h/l prefixes ----
//
// The game formats UI text with it dozens of times a frame, so it formats into
// one host buffer and copies to the guest once. Windows caps output at 1024
// bytes including the NUL.
uint32_t format_guest(Cpu &c, uint32_t out, uint32_t fmt_va, uint32_t args)
{
    char buf[1025];
    uint32_t n = 0;
    const std::string fmt = read_cstr(c, fmt_va, 1024);
    auto put = [&](char ch) {
        if (n < 1024)
            buf[n++] = ch;
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            put(fmt[i]);
            continue;
        }
        const size_t start = i++;
        bool left = false, zero = false, alt = false, space = false;
        for (; i < fmt.size(); i++) {
            if (fmt[i] == '-') left = true;
            else if (fmt[i] == '0') zero = true;
            else if (fmt[i] == '#') alt = true;
            else if (fmt[i] == ' ') space = true;
            else break;
        }
        int width = 0, prec = -1;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
            width = width * 10 + (fmt[i++] - '0');
        if (i < fmt.size() && fmt[i] == '.') {
            prec = 0;
            for (i++; i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9'; i++)
                prec = prec * 10 + (fmt[i] - '0');
        }
        bool half = false;
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'L')) {
            half = fmt[i] == 'h';
            i++;
        }
        if (i >= fmt.size())
            break;
        char body[64];
        std::string str;
        const char conv = fmt[i];
        const char *text = body;
        int len = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': {
            uint32_t v = c.read_u32(args);
            args += 4;
            if (half)
                v = conv == 'd' || conv == 'i' ? (uint32_t)(int32_t)(int16_t)v : (v & 0xFFFF);
            char spec[16], *p = spec;
            *p++ = '%';
            if (alt) *p++ = '#';
            if (space) *p++ = ' ';
            if (prec >= 0) { *p++ = '.'; *p++ = '*'; }
            *p++ = conv == 'i' ? 'd' : conv;
            *p = 0;
            len = prec >= 0 ? std::snprintf(body, sizeof body, spec, prec, v) : std::snprintf(body, sizeof body, spec, v);
            break;
        }
        case 'c':
            body[0] = (char)c.read_u32(args);
            args += 4;
            len = 1;
            break;
        case 's': case 'S': {
            const uint32_t p = c.read_u32(args);
            args += 4;
            str = p ? (conv == 'S' ? read_wstr(c, p, 1024) : read_cstr(c, p, 1024)) : "(null)";
            if (prec >= 0 && (int)str.size() > prec)
                str.resize((size_t)prec);
            text = str.c_str();
            len = (int)str.size();
            break;
        }
        case '%':
            put('%');
            continue;
        default:   // unknown conversion: output as written
            for (size_t k = start; k <= i; k++)
                put(fmt[k]);
            continue;
        }
        const bool numeric = conv != 's' && conv != 'S' && conv != 'c';
        int pad = width > len ? width - len : 0;
        if (!left && zero && numeric && prec < 0) {
            int k = 0;   // zero padding goes after a sign
            if (len && text[0] == '-') { put('-'); k = 1; }
            while (pad-- > 0) put('0');
            for (; k < len; k++) put(text[k]);
            continue;
        }
        if (!left) while (pad-- > 0) put(' ');
        for (int k = 0; k < len; k++) put(text[k]);
        if (left) while (pad-- > 0) put(' ');
    }
    buf[n] = 0;
    c.write(out, buf, n + 1);
    return n;
}

// First activation of the main window, as Windows does when it is shown.
// The game relies on it: without WM_ACTIVATEAPP it treats itself as
// backgrounded (and mutes its music setting).
void activate(Cpu &c, uint32_t hwnd)
{
    auto it = g_windows.find(hwnd);
    if (g_activated || it == g_windows.end())
        return;
    g_activated = true;
    guest_call(c, it->second, {hwnd, 0x001C /*WM_ACTIVATEAPP*/, 1, 0});
    guest_call(c, it->second, {hwnd, 0x0006 /*WM_ACTIVATE*/, 1 /*WA_ACTIVE*/, 0});
    guest_call(c, it->second, {hwnd, 0x0007 /*WM_SETFOCUS*/, 0, 0});
}

std::string guest_lower(Cpu &c, uint32_t va)
{
    std::string s;
    if (va < 0x10000)   // an atom, not a string
        return "#" + std::to_string(va);
    for (uint32_t i = 0; i < 256; i++) {
        uint8_t b = 0;
        c.read(va + i, &b, 1);
        if (!b)
            break;
        s += (char)std::tolower(b);
    }
    return s;
}

void write_msg(Cpu &c, uint32_t pmsg, const Msg &m)
{
    c.write_u32(pmsg + 0, m.hwnd);
    c.write_u32(pmsg + 4, m.message);
    c.write_u32(pmsg + 8, m.wparam);
    c.write_u32(pmsg + 12, m.lparam);
    c.write_u32(pmsg + 16, (uint32_t)now_ms());   // the game's keyboard handler checks message times
    uint32_t x, y;
    wx86_get_cursor(&x, &y);
    c.write_u32(pmsg + 20, x);   // pt: the cursor when the message is retrieved
    c.write_u32(pmsg + 24, y);
}

// ---- input from the platform (rt_host.h: input_*) -----------------------------------
//
// Input becomes posted messages for the main window, which the game's own
// PeekMessage/DispatchMessage loop delivers. Key state, the cursor and the
// button state change when the event is queued, as on Windows, because the
// game's widgets read GetKeyState(VK_LBUTTON) alongside the messages.

constexpr uint32_t WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104,
                   WM_SYSKEYUP = 0x0105, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
                   WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205;
constexpr uint8_t VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_RETURN = 0x0D, VK_MENU = 0x12;

uint64_t g_last_poll = 0;

uint32_t mouse_flags()
{
    return (g_keys[VK_LBUTTON] ? 0x0001u : 0) | (g_keys[VK_RBUTTON] ? 0x0002u : 0) | (g_keys[0x10] ? 0x0004u : 0) |
           (g_keys[0x11] ? 0x0008u : 0);   // MK_LBUTTON MK_RBUTTON MK_SHIFT MK_CONTROL
}

uint32_t cursor_lparam()
{
    uint32_t x, y;
    wx86_get_cursor(&x, &y);
    return (y << 16) | (x & 0xFFFF);
}

void post(uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (g_main_hwnd)
        g_queue.push_back({g_main_hwnd, message, wparam, lparam});
}

// PC/AT set-1 scan code (lParam bits 16-23 of WM_KEY*).
uint32_t scan_code(uint8_t vk)
{
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (vk >= 'A' && vk <= 'Z')
        return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9')
        return 0x02 + (vk - '1');
    if (vk == '0')
        return 0x0B;
    if (vk >= 0x70 && vk <= 0x79)   // F1-F10
        return 0x3B + (vk - 0x70);
    switch (vk) {
    case 0x08: return 0x0E;   // Backspace
    case 0x09: return 0x0F;   // Tab
    case 0x0D: return 0x1C;   // Enter
    case 0x10: return 0x2A;   // Shift
    case 0x11: return 0x1D;   // Ctrl
    case 0x12: return 0x38;   // Alt
    case 0x1B: return 0x01;   // Escape
    case 0x20: return 0x39;   // Space
    default: return 0;
    }
}

// PeekMessage core: 1 = a message was written, 0 = none.
uint32_t peek(Cpu &c, uint32_t pmsg, bool remove)
{
    stats().peeks++;
    // Input is sampled from the message pump, not per frame: some screens
    // (the pause menu) stop redrawing but still pump messages.
    if (options().poll_input) {
        const uint64_t now = now_ms();
        if (now - g_last_poll >= 16) {
            g_last_poll = now;
            options().poll_input();
        }
    }
    if (!g_queue.empty()) {
        write_msg(c, pmsg, g_queue.front());
        if (remove)
            g_queue.pop_front();
        return 1;
    }
    if (g_quit) {
        write_msg(c, pmsg, {0, WM_QUIT, g_quit_code, 0});
        if (remove)
            g_quit = false;
        return 1;
    }
    return 0;
}

uint32_t register_class(Cpu &c, uint32_t wndproc, uint32_t name_va)
{
    const std::string name = guest_lower(c, name_va);
    g_classes[name] = wndproc;
    return g_next_atom++;
}

}  // namespace

void set_key(uint8_t vk, bool down) { g_keys[vk] = down; }

void input_mouse_move(int x, int y)
{
    wx86_set_cursor(x < 0 ? 0 : (uint32_t)x, y < 0 ? 0 : (uint32_t)y);
    // Consecutive moves collapse into one: the game only needs the latest.
    if (!g_queue.empty() && g_queue.back().message == WM_MOUSEMOVE && g_queue.back().hwnd == g_main_hwnd) {
        g_queue.back().wparam = mouse_flags();
        g_queue.back().lparam = cursor_lparam();
        return;
    }
    post(WM_MOUSEMOVE, mouse_flags(), cursor_lparam());
}

void input_button(bool right, bool down)
{
    const uint8_t vk = right ? VK_RBUTTON : VK_LBUTTON;
    if (g_keys[vk] == down)
        return;
    g_keys[vk] = down;
    const uint32_t msg = right ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
    post(msg, mouse_flags(), cursor_lparam());
}

void input_key(uint8_t vk, bool down)
{
    if (g_keys[vk] == down)
        return;
    g_keys[vk] = down;
    const bool sys = vk == VK_MENU;   // Alt arrives as a system key
    const uint32_t scan = scan_code(vk) << 16;
    if (down) {
        post(sys ? WM_SYSKEYDOWN : WM_KEYDOWN, vk, 1 | scan | (sys ? 0x20000000u : 0));
        if (vk == VK_RETURN || vk == 0x1B || vk == 0x08)   // TranslateMessage's characters for these keys
            post(WM_CHAR, vk, 1 | scan);
    } else {
        post(sys ? WM_SYSKEYUP : WM_KEYUP, vk, 0xC0000001u | scan | (sys ? 0x20000000u : 0));
    }
}

void input_char(uint32_t ch) { post(WM_CHAR, ch, 1); }

bool input_ready() { return g_main_hwnd != 0 && g_activated; }

bool take_input_message(InputMessage &m)
{
    if (options().poll_input) {
        const uint64_t now = now_ms();
        if (now - g_last_poll >= 16) {
            g_last_poll = now;
            options().poll_input();
        }
    }
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        const uint32_t msg = it->message;
        if ((msg >= WM_KEYDOWN && msg <= 0x0109) || (msg >= WM_MOUSEMOVE && msg <= 0x020E)) {   // WM_KEYFIRST..LAST, WM_MOUSEFIRST..LAST
            m = {msg, it->wparam, it->lparam};
            g_queue.erase(it);
            return true;
        }
    }
    return false;
}

void install_user32(Bridge &br)
{
    auto U = [&](const char *name, uint32_t argc, std::function<uint32_t(Cpu &)> fn) {
        Shim s;
        s.argc = argc;
        s.stdcall_cleanup = true;
        s.tag = std::string("USER32.dll!") + name;
        s.fn = std::move(fn);
        br.register_shim("USER32.dll", name, s);
    };

    g_classes.clear();
    g_windows.clear();
    g_queue.clear();
    g_quit = g_activated = false;
    g_main_hwnd = 0;
    std::memset(g_keys, 0, sizeof g_keys);
    g_clip = Clip();
    g_last_poll = 0;

    U("RegisterClassA", 1, [](Cpu &c) { return register_class(c, c.read_u32(c.arg(0) + 4), c.read_u32(c.arg(0) + 36)); });
    U("RegisterClassExA", 1, [](Cpu &c) { return register_class(c, c.read_u32(c.arg(0) + 8), c.read_u32(c.arg(0) + 40)); });
    U("CreateWindowExA", 12, [](Cpu &c) -> uint32_t {
        auto it = g_classes.find(guest_lower(c, c.arg(1)));
        if (it == g_classes.end()) {
            set_last_error(c, 1407);   // ERROR_CANNOT_FIND_WND_CLASS
            return 0;
        }
        const uint32_t hwnd = g_next_hwnd;
        g_next_hwnd += 0x10;
        g_windows[hwnd] = it->second;
        if (!g_main_hwnd && !c.arg(8))   // hWndParent 0: top-level
            g_main_hwnd = hwnd;
        return hwnd;
    });
    U("SendMessageA", 4, [](Cpu &c) -> uint32_t {
        auto it = g_windows.find(c.arg(0));
        if (it == g_windows.end())
            return 0;
        tail_call(c, it->second, 4, {c.arg(0), c.arg(1), c.arg(2), c.arg(3)});
        return 0;
    });
    U("PostMessageA", 4, [](Cpu &c) {
        g_queue.push_back({c.arg(0), c.arg(1), c.arg(2), c.arg(3)});
        return 1u;
    });
    U("PostThreadMessageA", 4, [](Cpu &c) {
        g_queue.push_back({0, c.arg(1), c.arg(2), c.arg(3)});
        return 1u;
    });
    U("PostQuitMessage", 1, [](Cpu &c) {
        g_quit = true;
        g_quit_code = c.arg(0);
        return 0u;
    });
    U("PeekMessageA", 5, [](Cpu &c) { return peek(c, c.arg(0), (c.arg(4) & 1u) != 0); });   // PM_REMOVE
    U("GetMessageA", 4, [](Cpu &c) -> uint32_t {
        if (peek(c, c.arg(0), true))
            return c.read_u32(c.arg(0) + 4) == WM_QUIT ? 0u : 1u;
        wx86_wait_sleep(1);
        write_msg(c, c.arg(0), {0, WM_NULL, 0, 0});
        return 1;
    });
    U("DispatchMessageA", 1, [](Cpu &c) -> uint32_t {
        const uint32_t p = c.arg(0);
        const uint32_t hwnd = c.read_u32(p), message = c.read_u32(p + 4);
        auto it = g_windows.find(hwnd);
        static const bool msglog = std::getenv("RT_MSGLOG") != nullptr;
        if (msglog)
            logf("dispatch hwnd=%08x msg=%04x w=%08x l=%08x%s", hwnd, message, c.read_u32(p + 8), c.read_u32(p + 12),
                 it == g_windows.end() ? " (no window)" : "");
        if (it == g_windows.end() || message == WM_NULL)
            return 0;
        tail_call(c, it->second, 1, {hwnd, message, c.read_u32(p + 8), c.read_u32(p + 12)});
        return 0;   // the window procedure's result replaces this
    });

    // ---- windows: one visible, focused, never minimised top-level window ----
    U("ShowWindow", 2, [](Cpu &c) {
        if (c.arg(1) != 0)   // not SW_HIDE
            activate(c, c.arg(0));
        return 0u;           // "was hidden"
    });
    U("MoveWindow", 6, [](Cpu &) { return 1u; });
    U("UnregisterClassA", 2, [](Cpu &) { return 1u; });
    U("IsWindowVisible", 1, [](Cpu &c) { return g_windows.count(c.arg(0)) ? 1u : 0u; });
    U("IsIconic", 1, [](Cpu &) { return 0u; });
    U("SetFocus", 1, [](Cpu &) { return g_main_hwnd; });
    U("GetFocus", 0, [](Cpu &) { return g_main_hwnd; });
    U("GetActiveWindow", 0, [](Cpu &) { return g_main_hwnd; });
    U("GetForegroundWindow", 0, [](Cpu &) { return g_main_hwnd; });
    U("GetWindowPlacement", 2, [](Cpu &c) {
        const uint32_t wp[11] = {44, 0, 1 /*SW_SHOWNORMAL*/, 0, 0, 0, 0, 0, 0, wx86_screen_w(), wx86_screen_h()};
        c.write(c.arg(1), wp, sizeof wp);
        return 1u;
    });
    U("MonitorFromWindow", 2, [](Cpu &) { return 0x0001F001u; });
    U("GetMonitorInfoA", 2, [](Cpu &c) {
        const uint32_t w = wx86_screen_w(), h = wx86_screen_h();
        const uint32_t mi[9] = {c.read_u32(c.arg(1)), 0, 0, w, h, 0, 0, w, h};
        c.write(c.arg(1), mi, sizeof mi);
        c.write_u32(c.arg(1) + 36, 1);   // MONITORINFOF_PRIMARY
        return 1u;
    });
    U("DrawTextA", 5, [](Cpu &c) -> uint32_t {   // no text rendering through GDI: measure only
        const int32_t n = (int32_t)c.arg(2);
        const uint32_t len = n < 0 ? (uint32_t)read_cstr(c, c.arg(1)).size() : (uint32_t)n;
        if ((c.arg(4) & 0x400) && c.arg(3)) {   // DT_CALCRECT
            c.write_u32(c.arg(3) + 8, c.read_u32(c.arg(3)) + 8 * len);
            c.write_u32(c.arg(3) + 12, c.read_u32(c.arg(3) + 4) + 16);
        }
        return 16;
    });
    U("MessageBoxA", 4, [](Cpu &c) {
        static std::set<std::string> seen;   // a message box the game repeats is logged once
        const std::string text = "[" + read_cstr(c, c.arg(2)) + "] " + read_cstr(c, c.arg(1));
        if (seen.insert(text).second)
            logf("MessageBox %s", text.c_str());
        return 1u;   // IDOK
    });

    // ---- keyboard ----
    auto key_state = [](Cpu &c) -> uint32_t { return g_keys[c.arg(0) & 0xFF] ? 0xFFFF8000u : 0u; };
    U("GetKeyState", 1, key_state);
    U("GetAsyncKeyState", 1, key_state);
    U("GetKeyboardLayout", 1, [](Cpu &) { return 0x04090409u; });   // en-US

    // ---- text ----
    {
        Shim s;   // cdecl, variadic: the caller pops
        s.argc = 2;
        s.stdcall_cleanup = false;
        s.tag = "USER32.dll!wsprintfA";
        s.fn = [](Cpu &c) { return format_guest(c, c.arg(0), c.arg(1), c.reg(R_ESP) + 12); };
        br.register_shim("USER32.dll", "wsprintfA", s);
    }
    U("wvsprintfA", 3, [](Cpu &c) { return format_guest(c, c.arg(0), c.arg(1), c.arg(2)); });

    // ---- clipboard: one in-process slot ----
    U("OpenClipboard", 1, [](Cpu &) {
        g_clip.open = true;
        return 1u;
    });
    U("CloseClipboard", 0, [](Cpu &) {
        g_clip.open = false;
        return 1u;
    });
    U("EmptyClipboard", 0, [](Cpu &) {
        g_clip.format = g_clip.handle = 0;
        return 1u;
    });
    U("SetClipboardData", 2, [](Cpu &c) {
        g_clip.format = c.arg(0);
        g_clip.handle = c.arg(1);
        return c.arg(1);
    });
    U("GetClipboardData", 1, [](Cpu &c) { return g_clip.handle && g_clip.format == c.arg(0) ? g_clip.handle : 0u; });
    U("IsClipboardFormatAvailable", 1, [](Cpu &c) { return g_clip.handle && g_clip.format == c.arg(0) ? 1u : 0u; });
}

}  // namespace rt
