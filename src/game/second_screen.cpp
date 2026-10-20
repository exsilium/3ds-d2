// See second_screen.h.
#include "game/second_screen.h"

#include "game/automap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game {
namespace {

constexpr int kW = SecondScreen::kWidth, kBarH = SecondScreen::kBarH, kAreaH = SecondScreen::kAreaH;

constexpr uint8_t VK_TAB = 0x09, VK_RETURN = 0x0D, VK_MENU = 0x12, VK_SPACE = 0x20, VK_F1 = 0x70;

// Touch: a tap is short and nearly still; a long press is still for longer.
constexpr int kTapMs = 300, kTapSlop = 6, kLongMs = 500;
constexpr int kClickHoldMs = 120;   // a click holds the button this long (one 40 ms game frame is not enough)
constexpr int kSettleMs = 60;       // the cursor rests this long before a click: the game resolves a
                                    // click against what the cursor hovered in the previous frame
constexpr int kEdge = 16, kEdgeStep = 6;   // dragging this close to the area's edge scrolls this much per poll

// KEYS: four columns of five rows.
struct KeyButton {
    const char *label, *hint;
    uint8_t vk;
    bool latch;   // a tap turns it on, the next one off (Alt: items stay shown)
};
const KeyButton kKeys[SecondScreen::kKeyCount] = {
    {"Skill 1", "F1", VK_F1, false},          {"Skill 2", "F2", VK_F1 + 1, false},
    {"Skill 3", "F3", VK_F1 + 2, false},      {"Skill 4", "F4", VK_F1 + 3, false},
    {"Skill 5", "F5", VK_F1 + 4, false},      {"Skill 6", "F6", VK_F1 + 5, false},
    {"Skill 7", "F7", VK_F1 + 6, false},      {"Skill 8", "F8", VK_F1 + 7, false},
    {"Character", "C", 'C', false},           {"Inventory", "I", 'I', false},
    {"Skills", "T", 'T', false},              {"Quests", "Q", 'Q', false},
    {"Party", "P", 'P', false},               {"Merc", "O", 'O', false},
    {"Map", "Tab", VK_TAB, false},            {"Run/Walk", "R", 'R', false},
    {"Swap", "W", 'W', false},                {"Items", "Alt", VK_MENU, true},
    {"Chat", "Enter", VK_RETURN, false},      {"Close all", "Space", VK_SPACE, false},
};
constexpr int kKeyCols = 4, kKeyW = kW / kKeyCols, kKeyH = 44;

// Panels: the UI table entries (d2_state.h) by the half of the frame they
// cover at 640x480. Entries not listed draw nothing over the world.
// Inventory 0x01, quest log 0x0F and game menu 0x09 were seen on the lane;
// the others are the community's names for the 1.13/1.14 UI table.
constexpr uint64_t bit(int i) { return 1ull << i; }
constexpr uint64_t kLeftPanels = bit(0x02)     // character sheet
                               | bit(0x0C)     // shop (with the inventory on the right)
                               | bit(0x0E)     // socketing (anvil)
                               | bit(0x0F)     // quest log
                               | bit(0x14)     // waypoints
                               | bit(0x16)     // party
                               | bit(0x17)     // trade
                               | bit(0x19)     // stash
                               | bit(0x1A)     // Horadric Cube
                               | bit(0x24);    // mercenary
constexpr uint64_t kRightPanels = bit(0x01)    // inventory
                                | bit(0x04);   // skill tree
// The game menu (Escape, 0x09) is left out: it fills the top screen,
// readable as it is.
constexpr uint64_t kCentrePanels = bit(0x08);  // NPC dialogue menu

inline uint16_t rgb(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

const char *bottom_tab_name(BottomTab t)
{
    switch (t) {
    case BottomTab::Map: return "map";
    case BottomTab::Keys: return "keys";
    case BottomTab::Zoom: return "zoom";
    case BottomTab::Info: return "info";
    case BottomTab::Panel: return "panel";
    }
    return "map";
}

bool bottom_tab_from_name(const char *s, BottomTab &t)
{
    for (BottomTab c : {BottomTab::Map, BottomTab::Keys, BottomTab::Zoom, BottomTab::Info})
        if (std::strcmp(s, bottom_tab_name(c)) == 0) {
            t = c;
            return true;
        }
    return false;
}

void SecondScreen::set_tab(BottomTab t)
{
    if (t == BottomTab::Panel)
        return;   // not a choice to keep: it comes with the panels
    if (t != tab_)
        settings_changed_ = true;
    tab_ = t;
    dirty_ = true;
}

void SecondScreen::set_frame(int width, int height)
{
    if (width > 0 && height > 0) {
        fw_ = width;
        fh_ = height;
    }
}

BottomTab SecondScreen::shown_tab() const
{
    if (panel_sides_ && !panel_dismissed_)
        return BottomTab::Panel;
    return tab_;
}

BottomView::Kind SecondScreen::shown_kind() const
{
    if (osk_ && osk_->is_open())
        return BottomView::kKeyboard;
    switch (shown_tab()) {
    case BottomTab::Map: return BottomView::kMap;
    case BottomTab::Keys: return BottomView::kKeys;
    case BottomTab::Zoom:
    case BottomTab::Panel: return BottomView::kViewport;
    case BottomTab::Info: return BottomView::kInfo;
    }
    return BottomView::kInfo;
}

// The map draws the ground as single dots on every other pixel of every
// other row, black between. At 1:1 every step of the map turns each of those
// pixels on or off, which reads as a dark flash over the whole ground. Each
// such dot (nothing on its sides, above or below) also colours the black
// pixels around it, diagonals included: the ground shows solid, as at half
// size, while lines and the names' letters keep their edges. `out` is the
// kW x kAreaH crop at (left, top), already filled from the map.
static void map_fill_dots(const AutomapImage &am, int left, int top, uint8_t *out)
{
    constexpr int kW = SecondScreen::kWidth, kH = SecondScreen::kAreaH;
    const ptrdiff_t st = am.stride;
    const int x0 = std::max(left - 1, 1), x1 = std::min(left + kW + 1, (int)am.width - 1);
    const int y0 = std::max(top - 1, 1), y1 = std::min(top + kH + 1, (int)am.height - 1);
    for (int y = y0; y < y1; y++) {
        const uint8_t *r = am.pixels + (ptrdiff_t)y * st;
        for (int x = x0; x < x1; x++) {
            const uint8_t p = r[x];
            if (!p || r[x - 1] || r[x + 1] || r[x - st] || r[x + st])
                continue;
            for (int oy = y - top - 1; oy <= y - top + 1; oy++) {
                if (oy < 0 || oy >= kH)
                    continue;
                uint8_t *o = out + (size_t)oy * kW;
                for (int ox = x - left - 1; ox <= x - left + 1; ox++)
                    if (ox >= 0 && ox < kW && !o[ox])
                        o[ox] = p;
            }
        }
    }
}

bool SecondScreen::wants_automap() const { return gs_.in_game && shown_kind() == BottomView::kMap; }

// Which halves hold a panel, and which one to show.
void SecondScreen::panel_geometry()
{
    const uint64_t ui = gs_.in_game ? gs_.ui : 0;
    uint32_t sides = 0;
    if (ui & kLeftPanels)
        sides |= 1;
    if (ui & kRightPanels)
        sides |= 2;
    if (!sides && (ui & kCentrePanels))
        sides |= 4;
    const uint64_t panels = ui & (kLeftPanels | kRightPanels | kCentrePanels);
    if (panels != panel_ui_) {
        // A panel opened or closed: show the panels again (unless all closed).
        if ((panels & ~panel_ui_) != 0)
            panel_dismissed_ = false;
        panel_ui_ = panels;
        dirty_ = true;
    }
    if (sides != panel_sides_) {
        panel_sides_ = sides;
        dirty_ = true;
        automap_refresh();   // the map moves over when a side panel opens
    }
    if (sides & 4)
        side_ = 2;
    else if (sides == 1)
        side_ = 0;
    else if (sides == 2)
        side_ = 1;
    else if (sides == 3 && grip_ != Grip::Point)
        side_ = cx_ < fw_ / 2 ? 0 : 1;   // both: the cursor's half
}

// The viewport, while nothing holds it: ZOOM centres the cursor; PANEL keeps
// its half and scrolls only to keep the cursor in view.
void SecondScreen::follow_cursor()
{
    if (shown_tab() == BottomTab::Panel) {
        const int panel_h = fh_ - 48;   // the panels end at the control panel
        scroll_h_ = panel_h;
        vx_ = side_ == 0 ? 0 : side_ == 1 ? fw_ - kW : (fw_ - kW) / 2;
        if (grip_ != Grip::Point) {
            if (cy_ < vy_ + 24)
                vy_ = cy_ - 24;
            else if (cy_ > vy_ + kAreaH - 24)
                vy_ = cy_ - kAreaH + 24;
        }
        vy_ = clampi(vy_, 0, panel_h > kAreaH ? panel_h - kAreaH : 0);
    } else {
        scroll_h_ = 0;
        if (grip_ != Grip::Point) {
            vx_ = cx_ - kW / 2;
            vy_ = cy_ - kAreaH / 2;
        }
        vx_ = clampi(vx_, 0, fw_ > kW ? fw_ - kW : 0);
        vy_ = clampi(vy_, 0, fh_ > kAreaH ? fh_ - kAreaH : 0);
    }
}

void SecondScreen::tab_tapped(int x, Pad &pad)
{
    const int n = panel_sides_ ? 5 : 4;
    const int i = clampi(x * n / kW, 0, n - 1);
    const BottomTab hit = (BottomTab)i;
    if (hit == shown_tab()) {
        // The shown tab again: its second function.
        if (hit == BottomTab::Map) {
            set_map_scale(map_scale_ == 1 ? 2 : 1);
            settings_changed_ = true;
        } else if (hit == BottomTab::Panel && panel_sides_ == 3) {
            // Panels on both sides: the cursor crosses to the same spot on the other.
            pad.pointing = true;
            pad.point_x = clampi(side_ == 0 ? cx_ + fw_ / 2 : cx_ - fw_ / 2, 0, fw_ - 1);
            pad.point_y = cy_;
            side_ ^= 1;
        }
        dirty_ = true;
        return;
    }
    if (hit == BottomTab::Panel) {
        panel_dismissed_ = false;
    } else {
        panel_dismissed_ = panel_sides_ != 0;
        set_tab(hit);
    }
    dirty_ = true;
}

void SecondScreen::update(Pad &pad, const GameState &gs, int cursor_x, int cursor_y, uint64_t now_ms)
{
    gs_ = gs;
    cx_ = cursor_x;
    cy_ = cursor_y;
    panel_geometry();
    const BottomView::Kind kind = shown_kind();
    const bool down = pad.touching && !prev_touch_, up = !pad.touching && prev_touch_;
    prev_touch_ = pad.touching;

    if (kind == BottomView::kKeyboard || pad.touchpad) {
        // The keyboard has the touch screen (the platform gives it the
        // touch); a scripted tap is the touchpad's.
        if (grip_ == Grip::Key || grip_ == Grip::Point)
            grip_ = Grip::None;
    } else {
        if (down) {
            tx0_ = pad.touch_x;
            ty0_ = pad.touch_y;
            t0_ = moved_at_ = now_ms;
            moved_ = 0;
            long_fired_ = scrolled_ = false;
            if (pad.touch_y < kBarH) {
                grip_ = Grip::Bar;
                tab_tapped(pad.touch_x, pad);
            } else if (kind == BottomView::kKeys) {
                const int col = clampi(pad.touch_x / kKeyW, 0, kKeyCols - 1);
                const int row = clampi((pad.touch_y - kBarH) / kKeyH, 0, kKeyCount / kKeyCols - 1);
                grip_ = Grip::Key;
                key_ = row * kKeyCols + col;
                if (kKeys[key_].latch)
                    alt_latched_ = !alt_latched_;
                dirty_ = true;
            } else if (kind == BottomView::kViewport && gs_.in_game) {
                grip_ = Grip::Point;
            } else {
                grip_ = Grip::Pad;
            }
        }
        if (pad.touching && grip_ != Grip::None) {
            const int d = std::abs(pad.touch_x - tx0_) + std::abs(pad.touch_y - ty0_);
            if (d > moved_)
                moved_ = d;
        }
        if (grip_ == Grip::Point && pad.touching) {
            // Resting at the edge scrolls (the spot under the finger moves on),
            // once the touch is too long for a tap: a quick tap there clicks.
            const int ay = pad.touch_y - kBarH, vx0 = vx_, vy0 = vy_;
            const bool hold = now_ms - t0_ >= (uint64_t)kTapMs;
            if (hold && ay < kEdge)
                vy_ -= kEdgeStep;
            else if (hold && ay >= kAreaH - kEdge)
                vy_ += kEdgeStep;
            if (hold && shown_tab() == BottomTab::Zoom) {
                if (pad.touch_x < kEdge)
                    vx_ -= kEdgeStep;
                else if (pad.touch_x >= kW - kEdge)
                    vx_ += kEdgeStep;
            }
            follow_cursor();   // clamps
            scrolled_ |= vx_ != vx0 || vy_ != vy0;   // holding at the edge to scroll is not a long press
            const int x = clampi(vx_ + pad.touch_x, 0, fw_ - 1), y = clampi(vy_ + ay, 0, fh_ - 1);
            if (x != px_ || y != py_)
                moved_at_ = now_ms;
            px_ = x;
            py_ = y;
            if (!long_fired_ && !scrolled_ && moved_ <= kTapSlop && now_ms - t0_ >= (uint64_t)kLongMs) {
                long_fired_ = true;   // a long press: right click
                right_until_ = now_ms + kClickHoldMs;
            }
        }
        if (up && grip_ == Grip::Point && !long_fired_ && !scrolled_ && moved_ <= kTapSlop && now_ms - t0_ < (uint64_t)kTapMs) {
            left_from_ = moved_at_ + kSettleMs > now_ms ? moved_at_ + kSettleMs : now_ms;
            left_until_ = left_from_ + kClickHoldMs;
        }
        if (up && grip_ == Grip::Key)
            dirty_ = true;
        if (up)
            grip_ = Grip::None;
    }

    // What the touch does instead of the touchpad.
    const bool clicking = now_ms < left_until_ || now_ms < right_until_;
    if (grip_ == Grip::Point || clicking) {
        pad.pointing = true;
        pad.point_x = px_;
        pad.point_y = py_;
        pad.point_left = now_ms >= left_from_ && now_ms < left_until_;
        pad.point_right = now_ms < right_until_;
    }
    int nk = 0;
    if (grip_ == Grip::Key && pad.touching && !kKeys[key_].latch)
        pad.keys[nk++] = kKeys[key_].vk;
    if (alt_latched_ && gs_.in_game && kind != BottomView::kKeyboard)
        pad.keys[nk++] = VK_MENU;
    if (grip_ == Grip::Bar || grip_ == Grip::Key || grip_ == Grip::Point)
        pad.touching = false;
    if (grip_ != Grip::Point)
        follow_cursor();
}

void SecondScreen::set_stats(std::vector<BottomView::Line> lines)
{
    stats_ = std::move(lines);
    if (shown_kind() == BottomView::kInfo || shown_kind() == BottomView::kKeyboard)
        dirty_ = true;
}

void SecondScreen::log(const char *s)
{
    // A ring of 40-column lines; longer messages are cut, as the console did.
    char *line = log_[log_head_ % kLogLines];
    std::snprintf(line, 41, "%s", s);
    log_head_++;
}

bool SecondScreen::stage(BottomView &v, uint64_t now_ms)
{
    presents_++;
    const BottomView::Kind kind = shown_kind();
    const BottomTab tab = shown_tab();
    bool redraw = dirty_ || kind != last_kind_ || tab != last_tab_ || (panel_sides_ != 0) != last_panel_tab_;
    v.kind = kind;
    v.tab = tab;
    v.panel_tab = panel_sides_ != 0;
    v.note.clear();
    v.lines.clear();
    v.osk_keys.clear();
    switch (kind) {
    case BottomView::kMap: {
        v.map_scale = map_scale_;
        AutomapImage am;
        if (!gs_.in_game) {
            v.map.clear();
            v.note = "The map shows here in the game.";
        } else if (automap_image(am) && (am.seq != last_map_seq_ || redraw || map_scale_ != last_scale_)) {
            // The player is drawn at (w/2 - 16, h/2 - 8) of the frame, a
            // quarter of the width over when a panel covers one half.
            int mx = (int)am.width / 2 - 16, my = (int)am.height / 2 - 8;
            if ((panel_sides_ & 3) == 1)
                mx += (int)am.width / 4;
            else if ((panel_sides_ & 3) == 2)
                mx -= (int)am.width / 4;
            v.map.assign((size_t)kW * kAreaH, 0);
            const int s = map_scale_;
            const int left = mx - kW * s / 2, top = my - kAreaH * s / 2;
            const int w = (int)am.width, h = (int)am.height;
            for (int y = 0; y < kAreaH; y++) {
                uint8_t *out = &v.map[(size_t)y * kW];
                const int sy = top + y * s;
                const uint8_t *r0 = sy >= 0 && sy < h ? am.pixels + (ptrdiff_t)sy * am.stride : nullptr;
                if (s == 1) {
                    const int xs = std::max(0, -left), xe = std::min(kW, w - left);
                    if (r0 && xs < xe)
                        std::memcpy(out + xs, r0 + left + xs, (size_t)(xe - xs));
                    continue;
                }
                // Half size: the first non-empty pixel of each 2x2 block, so
                // one-pixel lines survive. (Blocks over the image's edge stay
                // empty.)
                const uint8_t *r1 = sy + 1 >= 0 && sy + 1 < h ? am.pixels + (ptrdiff_t)(sy + 1) * am.stride : nullptr;
                const int xs = std::max(0, (-left + 1) / 2), xe = std::min(kW, (w - left) / 2);
                for (int x = xs; x < xe; x++) {
                    const int sx = left + 2 * x;
                    uint8_t p = r0 ? (r0[sx] ? r0[sx] : r0[sx + 1]) : 0;
                    if (!p && r1)
                        p = r1[sx] ? r1[sx] : r1[sx + 1];
                    out[x] = p;
                }
            }
            if (s == 1)
                map_fill_dots(am, left, top, v.map.data());
            last_map_seq_ = am.seq;
            last_scale_ = map_scale_;
            redraw = true;
        } else if (v.map.empty()) {
            v.note = "Drawing the map...";
        }
        break;
    }
    case BottomView::kKeys: {
        uint32_t lit = 0;
        if (grip_ == Grip::Key && key_ >= 0)
            lit |= 1u << key_;
        for (int i = 0; i < kKeyCount; i++)
            if (kKeys[i].latch && alt_latched_)
                lit |= 1u << i;
        redraw |= lit != last_keys_;
        v.keys_lit = last_keys_ = lit;
        break;
    }
    case BottomView::kViewport:
        v.vx = vx_;
        v.vy = vy_;
        v.scroll_h = scroll_h_;
        // The frame changes all the time: every other present is enough.
        redraw |= vx_ != last_vx_ || vy_ != last_vy_ || (presents_ & 1) == 0;
        last_vx_ = vx_;
        last_vy_ = vy_;
        break;
    case BottomView::kInfo:
    case BottomView::kKeyboard: {
        v.lines = stats_;
        if (kind == BottomView::kInfo) {
            // The newest messages under the statistics, as many as fit.
            const int rows = kAreaH / 8 - (int)stats_.size();
            const uint32_t n = log_head_ < (uint32_t)kLogLines ? log_head_ : (uint32_t)kLogLines;
            const uint32_t show = rows > 0 ? ((uint32_t)rows < n ? (uint32_t)rows : n) : 0;
            for (uint32_t i = log_head_ - show; i < log_head_; i++)
                v.lines.push_back({log_[i % kLogLines], rgb(200, 200, 200)});
            // New messages: at most four redraws a second.
            if (log_head_ != last_log_head_ && now_ms - log_drawn_ms_ >= 250) {
                redraw = true;
                last_log_head_ = log_head_;
                log_drawn_ms_ = now_ms;
            }
        } else {
            v.osk_title = osk_->title();
            v.osk_keys = osk_->keys();
            redraw |= osk_->dirty();
        }
        break;
    }
    case BottomView::kNone:
        break;
    }
    last_kind_ = kind;
    last_tab_ = tab;
    last_panel_tab_ = panel_sides_ != 0;
    dirty_ = false;
    return redraw;
}

// ---- drawing ----------------------------------------------------------------------

namespace {

struct Canvas {
    uint16_t *fb;
    const uint8_t *font;
    void px(int x, int y, uint16_t c) const
    {
        if (x >= 0 && x < kW && y >= 0 && y < SecondScreen::kHeight)
            fb[x * 240 + (239 - y)] = c;
    }
    void fill(int x, int y, int w, int h, uint16_t c) const
    {
        const int x0 = clampi(x, 0, kW), x1 = clampi(x + w, 0, kW);
        const int y0 = clampi(y, 0, SecondScreen::kHeight), y1 = clampi(y + h, 0, SecondScreen::kHeight);
        for (int i = x0; i < x1; i++) {
            uint16_t *col = fb + i * 240 + (239 - y0);
            for (int j = y0; j < y1; j++)
                *col-- = c;
        }
    }
    void text(int x, int y, const char *s, uint16_t c) const
    {
        for (; *s; s++, x += 8) {
            const uint8_t *g = font + (uint8_t)*s * 8;
            for (int r = 0; r < 8; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (0x80 >> b))
                        px(x + b, y + r, c);
        }
    }
    void centred(int x, int y, int w, const char *s, uint16_t c) const
    {
        text(x + (w - 8 * (int)std::strlen(s)) / 2, y, s, c);
    }
};

const uint16_t kBg = 0, kBarBg = rgb(40, 36, 30), kBarText = rgb(170, 160, 140), kGold = rgb(199, 179, 119);
const uint16_t kKeyBg = rgb(58, 52, 44), kKeyLit = rgb(199, 179, 119), kHint = rgb(130, 120, 100);

void draw_bar(const Canvas &cv, const BottomView &v)
{
    static const char *const kNames[] = {"MAP", "KEYS", "ZOOM", "INFO", "PANEL"};
    const int n = v.panel_tab ? 5 : 4, w = kW / n;
    cv.fill(0, 0, kW, kBarH, kBarBg);
    for (int i = 0; i < n; i++) {
        const bool on = (int)v.tab == i;
        const int x = i * w;
        if (on)
            cv.fill(x + 1, 1, w - 2, kBarH - 3, kGold);
        cv.centred(x, 5, w, kNames[i], on ? kBg : kBarText);
        if (i)
            cv.fill(x, 3, 1, kBarH - 7, kHint);
    }
    cv.fill(0, kBarH - 1, kW, 1, kHint);
}

}  // namespace

void draw_bottom(uint16_t *fb, const BottomView &v, const uint8_t *frame, int fw, int fh, const uint16_t *pal565,
                 const uint8_t *font)
{
    const Canvas cv{fb, font};
    if (v.kind == BottomView::kNone)
        return;
    if (v.kind == BottomView::kKeyboard) {
        cv.fill(0, 0, kW, SecondScreen::kHeight, kBg);
        for (size_t i = 0; i < v.lines.size() && i < 12; i++)
            cv.text(0, (int)i * 8, v.lines[i].text.c_str(), v.lines[i].color);
        const int y0 = SecondScreen::kOskY;
        const uint16_t key = rgb(200, 200, 200), down = rgb(80, 80, 80);
        cv.text(0, y0 + 2, v.osk_title.c_str(), rgb(255, 255, 255));
        for (const Osk::KeyView &k : v.osk_keys) {
            cv.fill(k.x, y0 + k.y, k.w, k.h, k.pressed ? down : key);
            cv.text(k.x + (k.w - 8 * (int)k.label.size()) / 2, y0 + k.y + (k.h - 8) / 2, k.label.c_str(),
                    k.pressed ? rgb(255, 255, 255) : 0);
        }
        return;
    }
    draw_bar(cv, v);
    switch (v.kind) {
    case BottomView::kMap:
        if (v.map.empty()) {
            cv.fill(0, kBarH, kW, kAreaH, kBg);
            cv.centred(0, kBarH + kAreaH / 2 - 4, kW, v.note.c_str(), kBarText);
            break;
        }
        for (int x = 0; x < kW; x++) {
            uint16_t *col = fb + x * 240 + (239 - kBarH);
            const uint8_t *src = v.map.data() + x;
            for (int y = 0; y < kAreaH; y++, src += kW)
                *col-- = *src ? pal565[*src] : kBg;
        }
        cv.text(kW - 8 * 5 - 2, SecondScreen::kHeight - 10, v.map_scale == 1 ? "x1" : "x1/2", kHint);
        break;
    case BottomView::kKeys:
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        for (int i = 0; i < SecondScreen::kKeyCount; i++) {
            const int x = (i % kKeyCols) * kKeyW, y = kBarH + 1 + (i / kKeyCols) * kKeyH;
            const bool lit = (v.keys_lit >> i) & 1;
            cv.fill(x + 2, y + 2, kKeyW - 4, kKeyH - 4, lit ? kKeyLit : kKeyBg);
            cv.centred(x, y + 11, kKeyW, kKeys[i].label, lit ? kBg : rgb(230, 220, 200));
            cv.centred(x, y + 25, kKeyW, kKeys[i].hint, lit ? kBg : kHint);
        }
        break;
    case BottomView::kViewport: {
        if (!frame || fw < kW || fh < kAreaH) {
            cv.fill(0, kBarH, kW, kAreaH, kBg);
            break;
        }
        const int vx = clampi(v.vx, 0, fw - kW), vy = clampi(v.vy, 0, fh - kAreaH);
        for (int x = 0; x < kW; x++) {
            uint16_t *col = fb + x * 240 + (239 - kBarH);
            const uint8_t *src = frame + (size_t)vy * fw + vx + x;
            for (int y = 0; y < kAreaH; y++, src += fw)
                *col-- = pal565[*src];
        }
        if (v.scroll_h > kAreaH) {
            // Where the view is in the panel: a thin bar on the right.
            const int h = kAreaH * kAreaH / v.scroll_h, y = kBarH + vy * kAreaH / v.scroll_h;
            cv.fill(kW - 3, kBarH, 3, kAreaH, kBarBg);
            cv.fill(kW - 3, y, 3, h, kGold);
        }
        break;
    }
    case BottomView::kInfo:
        cv.fill(0, kBarH, kW, kAreaH, kBg);
        for (size_t i = 0; i < v.lines.size() && (int)i < kAreaH / 8; i++)
            cv.text(0, kBarH + 1 + (int)i * 8, v.lines[i].text.c_str(), v.lines[i].color);
        break;
    default:
        break;
    }
}

}  // namespace game
