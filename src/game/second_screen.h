// The bottom screen during play (second_screen.cpp): a row of tabs over a
// 320x222 area that shows one of
//   MAP    the automap (automap.h), centred on the player and following
//          them smoothly; tapping MAP again switches 1:1 <-> half size, as
//          does the size button under the map; the other button there sets
//          how often the crosses and names of others are refreshed
//   KEYS   touch buttons for the game's hotkeys (skills F1-F8, panels, run,
//          weapon swap, show items, chat, close all)
//   ZOOM   the frame at 1:1 around the cursor, readable text; touch points
//          straight at the game
//   INFO   the diagnostics (frame rate, memory) and the runtime's messages
//   PANEL  offered while a panel is open (inventory, stash, shop, character,
//          skills...), and shown by itself when one opens: the panel's half
//          of the frame at 1:1 (D2's panels are 320 pixels wide at 640x480),
//          touched directly. Tapping PANEL again with panels on both sides
//          moves the cursor to the other side.
// The on-screen keyboard (osk.h) takes the whole screen while it is open.
//
// Touch: on the tab bar it picks a tab. In ZOOM and PANEL it points: the
// cursor goes to the touched spot and follows the finger, a short tap is a
// left click there, a long press (half a second, still) a right click;
// dragging near the area's edge scrolls. On KEYS it presses the buttons. On
// MAP and INFO the area stays the touchpad it always was (controls.h).
//
// Platform-neutral. The game thread runs update() each poll and stage() at
// each present it will draw; draw_bottom() needs only the staged view and
// frame, so it may run on another thread.
#ifndef D2_GAME_SECOND_SCREEN_H
#define D2_GAME_SECOND_SCREEN_H

#include "game/controls.h"
#include "game/d2_state.h"
#include "game/osk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

enum class BottomTab : uint8_t { Map, Keys, Zoom, Info, Panel };

const char *bottom_tab_name(BottomTab t);              // "map", "keys"... (settings files)
bool bottom_tab_from_name(const char *s, BottomTab &t);

// Everything the bottom screen draws, taken on the game thread.
struct BottomView {
    enum Kind : uint8_t { kNone, kMap, kKeys, kViewport, kInfo, kKeyboard };
    struct Line {
        std::string text;
        uint16_t color;   // RGB565
    };
    Kind kind = kNone;
    BottomTab tab = BottomTab::Map;   // the highlighted tab
    bool panel_tab = false;           // PANEL is offered
    // kViewport: the frame from (vx, vy) at 1:1 under the bar; a scroll
    // bar when the frame area it moves over (scroll_h) is taller than it.
    int vx = 0, vy = 0, scroll_h = 0;
    // kMap: 320x222 indices into the frame's palette, 0 = nothing; empty
    // until the first capture.
    std::vector<uint8_t> map;
    int map_scale = 1;
    uint32_t map_ms = 500;            // the units' refresh (the button under the map)
    // kKeys: bit i lit = button i shown pressed (or latched).
    uint32_t keys_lit = 0;
    // kInfo, and the upper part of kKeyboard.
    std::vector<Line> lines;
    // kKeyboard.
    std::string osk_title;
    std::vector<Osk::KeyView> osk_keys;
    std::string note;   // a line over the area (kMap without a map)
};

class SecondScreen {
public:
    static constexpr int kWidth = 320, kHeight = 240, kBarH = 18, kAreaH = kHeight - kBarH;
    static constexpr int kOskY = 100;   // the keyboard's area, as the keyboard was always drawn
    static constexpr int kKeyCount = 20;

    // The player's choices (the platform saves them).
    BottomTab tab() const { return tab_; }
    void set_tab(BottomTab t);
    int map_scale() const { return map_scale_; }
    void set_map_scale(int s) { map_scale_ = s == 2 ? 2 : 1; dirty_ = true; }
    // How often the map's units are refreshed while the player moves (ms):
    // one of kMapMs.
    uint32_t map_ms() const { return map_ms_; }
    void set_map_ms(uint32_t ms);
    static constexpr uint32_t kMapMs[] = {250, 500, 1000, 2000, 5000};
    // MAP's buttons, in the strip under the map: size and refresh.
    static constexpr int kMapBtnY = kHeight - 16, kMapBtnH = 16;
    static constexpr int kScaleBtnX = 264, kScaleBtnW = 56, kMsBtnX = 176, kMsBtnW = 84;
    bool take_settings_changed() { bool r = settings_changed_; settings_changed_ = false; return r; }

    // Each poll, before Controls::update, with the touch already given to
    // the keyboard if it is open: takes the touches it acts on out of `pad`
    // (touching cleared) and puts what they do in (pointing, keys).
    void update(Pad &pad, const GameState &gs, int cursor_x, int cursor_y, uint64_t now_ms);
    // The frame size, from each game frame (not cinematics).
    void set_frame(int width, int height);
    // Draw everything again at the next stage (the screen was taken away:
    // HOME menu, sleep).
    void invalidate() { dirty_ = true; }
    // Whether the automap capture is wanted (MAP shown in the game world).
    bool wants_automap() const;

    // The keyboard (osk.h), while it is open, replaces everything.
    void set_keyboard(const Osk *osk) { osk_ = osk; }
    // INFO: the diagnostics lines (once a second) and the runtime's messages.
    void set_stats(std::vector<BottomView::Line> lines);
    void log(const char *s);

    // At a present that will be drawn: fills `view` and says whether the
    // screen needs drawing (something changed, or the frame did and the view
    // shows it).
    bool stage(BottomView &view, uint64_t now_ms);

private:
    BottomView::Kind shown_kind() const;
    BottomTab shown_tab() const;
    void tab_tapped(int x, Pad &pad);
    void panel_geometry();
    void follow_cursor();

    BottomTab tab_ = BottomTab::Map;
    int map_scale_ = 1;
    uint32_t map_ms_ = 500;
    bool settings_changed_ = false;
    const Osk *osk_ = nullptr;

    // The game.
    GameState gs_;
    int fw_ = 640, fh_ = 480, cx_ = 320, cy_ = 240;
    // Panels: which halves of the frame hold one (bit 0 left, 1 right, 2 centre).
    uint32_t panel_sides_ = 0;
    uint64_t panel_ui_ = 0;           // the UI entries open at the last change
    bool panel_dismissed_ = false;    // another tab chosen while panels are open
    int side_ = 0;                    // the half shown: 0 left, 1 right, 2 centre

    // The viewport (ZOOM and PANEL).
    int vx_ = 0, vy_ = 0, scroll_h_ = 0;

    // Touch.
    enum class Grip : uint8_t { None, Bar, Pad, Key, Point, MapButton };
    Grip grip_ = Grip::None;
    int key_ = -1;                    // KEYS: the button held
    int tx0_ = 0, ty0_ = 0, moved_ = 0;
    uint64_t t0_ = 0, moved_at_ = 0;
    bool long_fired_ = false, scrolled_ = false;
    int px_ = 0, py_ = 0;             // the point (game pixels)
    uint64_t left_from_ = 0, left_until_ = 0, right_until_ = 0;
    bool prev_touch_ = false;
    bool alt_latched_ = false;        // KEYS: show items (Alt) latched on

    // INFO.
    std::vector<BottomView::Line> stats_;
    static constexpr int kLogLines = 32;
    char log_[kLogLines][41] = {};
    uint32_t log_head_ = 0;           // lines written
    uint64_t log_drawn_ms_ = 0;

    // What was last staged.
    bool dirty_ = true;
    BottomView::Kind last_kind_ = BottomView::kNone;
    BottomTab last_tab_ = BottomTab::Map;
    bool last_panel_tab_ = false;
    uint32_t last_keys_ = 0, last_log_head_ = 0;
    uint64_t last_map_state_ = 0;
    int last_vx_ = -1, last_vy_ = -1, last_scale_ = 0;
    uint32_t presents_ = 0;
};

// Draws `v` into a 320x240 RGB565 framebuffer in the 3DS's layout (column-
// major: pixel (x, y) at fb[x * 240 + 239 - y]). `frame` (fw x fh indices)
// and `pal565` are the game frame for kViewport and the palette for kMap;
// `font` is 8x8, 1 bpp, bit 7 leftmost (libctru's console font).
void draw_bottom(uint16_t *fb, const BottomView &v, const uint8_t *frame, int fw, int fh, const uint16_t *pal565,
                 const uint8_t *font);

}  // namespace game

#endif
