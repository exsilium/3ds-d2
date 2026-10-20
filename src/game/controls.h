// 3ds-d2's control scheme for Diablo II: turns the 3DS's buttons, circle pad,
// C-stick and touch screen into the mouse and keyboard input the game
// expects (through rt::input_*). Platform-neutral: the 3DS app fills a Pad
// from HID each poll; the Linux lane can replay scripted pads.
//
// Default mapping (docs/controls.md):
//   Circle pad   in the game world: walk (the cursor orbits the character and
//                the left button is held); in menus and open panels: move the cursor
//   C-stick      move the cursor (New 3DS)
//   Touch        a touchpad: drag moves the cursor, a short tap is a left click
//                at the cursor
//   A / B        left / right mouse button (held)
//   X            inventory (I)        Y   show items (Alt, held)
//   L            stand still (Shift, held)
//   R            layer: R+A character (C), R+B skill tree (T), R+X automap (Tab),
//                R+Y quests (Q), R+D-pad skill hotkeys F1-F4, R+Start Enter,
//                R+Select on-screen keyboard (opens by itself on a text field)
//   L+R+Select   screenshot into the Camera album
//   D-pad        belt potions 1-4
//   ZL / ZR      weapon swap (W) / automap (Tab)            (New 3DS)
//   Start        Escape (game menu)
//   Select       top screen: whole frame <-> 1:1 zoom around the cursor
#ifndef D2_GAME_CONTROLS_H
#define D2_GAME_CONTROLS_H

#include <cstdint>

namespace game {

enum Button : uint32_t {
    kA = 1u << 0, kB = 1u << 1, kX = 1u << 2, kY = 1u << 3,
    kL = 1u << 4, kR = 1u << 5, kZL = 1u << 6, kZR = 1u << 7,
    kStart = 1u << 8, kSelect = 1u << 9,
    kUp = 1u << 10, kDown = 1u << 11, kLeft = 1u << 12, kRight = 1u << 13,
};

struct Pad {
    uint32_t held = 0;          // Button bits
    int circle_x = 0, circle_y = 0;   // -156..156, y up (libctru's circle pad range)
    int cstick_x = 0, cstick_y = 0;   // -156..156, y up
    bool touching = false;
    int touch_x = 0, touch_y = 0;     // touch-screen pixels (320x240)
    // A scripted tap (autoplay "tap"): the touchpad's, whatever the bottom
    // screen shows.
    bool touchpad = false;
    // From the bottom screen (second_screen.h), which takes the touches it
    // acts on out of `touching`: a spot the cursor goes to, in game pixels,
    // with the mouse buttons held there; and keys its buttons hold down.
    bool pointing = false;
    int point_x = 0, point_y = 0;
    bool point_left = false, point_right = false;
    uint8_t keys[4] = {};             // virtual keys, 0 = none
};

class Controls {
public:
    // Game frame size (cursor bounds, walk radius scale).
    void set_frame(int width, int height);
    // One poll. now_ms: the guest clock (tap timing).
    void update(const Pad &pad, uint64_t now_ms);

    // What the game is showing (from game::GameState): the circle pad walks
    // only in the game world with no panel open.
    void set_context(bool in_game, bool panel_open);
    // Puts the cursor at x,y (scripted tests; a touchpad has no absolute position).
    void warp_cursor(int x, int y);
    int cursor_x() const { return cx_; }
    int cursor_y() const { return cy_; }
    // One-shot requests for the platform layer, cleared when read.
    bool take_zoom_toggle() { bool r = zoom_toggle_; zoom_toggle_ = false; return r; }
    bool take_keyboard_request() { bool r = keyboard_; keyboard_ = false; return r; }
    bool take_screenshot_request() { bool r = screenshot_; screenshot_ = false; return r; }
    // Select + D-pad, for the bottom screen: next or previous tab, MAP, or
    // the map's size. kNone when there is none.
    enum class Bottom : uint8_t { kNone, kNextTab, kPrevTab, kMap, kMapSize };
    Bottom take_bottom_request() { Bottom r = bottom_; bottom_ = Bottom::kNone; return r; }

private:
    void move_cursor(int x, int y);
    void key(uint8_t vk, bool down);
    void stick_cursor(int sx, int sy, float speed_800, float &acc_x, float &acc_y);

    int w_ = 800, h_ = 600;
    int cx_ = 400, cy_ = 300;     // cursor, game pixels
    uint32_t prev_ = 0;           // buttons held at the previous poll
    // Walking with the circle pad.
    bool walking_ = false;
    bool walk_press_pending_ = false;   // cursor moved first; press on the next poll
    bool walk_left_ = false;      // walking holds the left button (A and taps are other sources)
    bool select_used_ = false;    // Select was used with R or the D-pad: no zoom toggle on release
    uint8_t start_vk_ = 0;
    uint8_t bound_[16] = {};      // the key each binding pressed (chosen by the R layer at press time)
    // Touch.
    bool touch_prev_ = false;
    uint64_t touch_start_ms_ = 0;
    int touch_x0_ = 0, touch_y0_ = 0, touch_moved_ = 0, touch_lx_ = 0, touch_ly_ = 0;
    float touch_acc_x_ = 0, touch_acc_y_ = 0;
    uint64_t tap_hold_until_ms_ = 0;
    float cstick_acc_x_ = 0, cstick_acc_y_ = 0, circle_acc_x_ = 0, circle_acc_y_ = 0;
    bool in_game_ = false, panel_open_ = false;
    bool zoom_toggle_ = false, keyboard_ = false, screenshot_ = false;
    Bottom bottom_ = Bottom::kNone;
    uint8_t keys_[256] = {};      // which virtual keys this layer holds down
    uint8_t pad_keys_[4] = {};    // Pad::keys at the previous poll
};

}  // namespace game

#endif
