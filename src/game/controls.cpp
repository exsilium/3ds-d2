// See controls.h.
#include "game/controls.h"

#include "runtime/rt_host.h"

#include <cmath>

namespace game {
namespace {

constexpr uint8_t VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_MENU = 0x12, VK_ESCAPE = 0x1B,
                  VK_F1 = 0x70;
constexpr uint8_t kMouseLeft = 0xFE, kMouseRight = 0xFF;   // bindings that are mouse buttons, not keys

constexpr float kStickMax = 156.0f;
constexpr float kWalkEngage = 0.20f, kWalkRelease = 0.15f;   // hysteresis, fraction of full tilt
constexpr int kTapMs = 250, kTapSlop = 6;                     // a tap: short and nearly still (touch pixels)
constexpr int kTapHoldMs = 120;                               // how long a tap holds the button
constexpr float kTouchGain = 3.0f;                            // touchpad: frame pixels per touch pixel (800 wide)
constexpr float kCstickSpeed = 18.0f;                         // pixels per poll at full tilt (800 wide)
constexpr float kCircleCursorSpeed = 12.0f;                   // circle pad as a cursor (menus)

struct Binding {
    Button button;
    uint8_t normal, with_r;   // virtual key (or kMouse*); 0 = nothing
};

// Buttons that act on press and release. Select and Start are handled apart.
const Binding kBindings[] = {
    {kA, kMouseLeft, 'C'},
    {kB, kMouseRight, 'T'},
    {kX, 'I', VK_TAB},
    {kY, VK_MENU, 'Q'},
    {kL, VK_SHIFT, VK_SHIFT},
    {kUp, '1', VK_F1},
    {kRight, '2', VK_F1 + 1},
    {kDown, '3', VK_F1 + 2},
    {kLeft, '4', VK_F1 + 3},
    {kZL, 'W', 'W'},
    {kZR, VK_TAB, VK_TAB},
};

}  // namespace

void Controls::set_frame(int width, int height)
{
    if (width > 0 && height > 0) {
        w_ = width;
        h_ = height;
    }
}

void Controls::warp_cursor(int x, int y) { move_cursor(x, y); }

void Controls::set_context(bool in_game, bool panel_open)
{
    in_game_ = in_game;
    panel_open_ = panel_open;
}

// Relative cursor from a stick: quadratic response, so small tilts aim finely.
void Controls::stick_cursor(int sx, int sy, float speed_800, float &acc_x, float &acc_y)
{
    const float vx = sx / kStickMax, vy = sy / kStickMax;
    const float speed = speed_800 * w_ / 800.0f;
    if (std::fabs(vx) > 0.15f)
        acc_x += vx * std::fabs(vx) * speed;
    if (std::fabs(vy) > 0.15f)
        acc_y -= vy * std::fabs(vy) * speed;   // stick up = screen up
    const int dx = (int)acc_x, dy = (int)acc_y;
    if (dx || dy) {
        acc_x -= dx;
        acc_y -= dy;
        move_cursor(cx_ + dx, cy_ + dy);
    }
}

void Controls::move_cursor(int x, int y)
{
    x = x < 0 ? 0 : x >= w_ ? w_ - 1 : x;
    y = y < 0 ? 0 : y >= h_ ? h_ - 1 : y;
    if (x == cx_ && y == cy_)
        return;
    cx_ = x;
    cy_ = y;
    rt::input_mouse_move(x, y);
}

void Controls::key(uint8_t vk, bool down)
{
    if (!vk || (keys_[vk] != 0) == down)
        return;
    keys_[vk] = down;
    rt::input_key(vk, down);
}

void Controls::update(const Pad &pad, uint64_t now_ms)
{
    const uint32_t held = pad.held, pressed = held & ~prev_, released = prev_ & ~held;
    const bool r_layer = (held & kR) != 0;

    // ---- Select / Start: zoom, keyboard, Escape, Enter ----
    if (pressed & kSelect) {
        select_used_ = r_layer;
        if (r_layer && (held & kL))
            screenshot_ = true;   // L+R+Select: screenshot to the Camera album
        else if (r_layer)
            keyboard_ = true;   // R+Select: on-screen keyboard
    }
    if (pressed & kStart) {
        start_vk_ = r_layer ? VK_RETURN : VK_ESCAPE;
        key(start_vk_, true);
    }
    if (released & kStart) {
        key(start_vk_, false);
        start_vk_ = 0;
    }
    if ((released & kSelect) && !select_used_)
        zoom_toggle_ = true;

    // ---- bound buttons: the layer chosen at press time is the one released ----
    bool a_left = false, b_right = false;
    for (size_t i = 0; i < sizeof kBindings / sizeof kBindings[0]; i++) {
        const Binding &b = kBindings[i];
        if (pressed & b.button) {
            bound_[i] = r_layer ? b.with_r : b.normal;
            if (bound_[i] != kMouseLeft && bound_[i] != kMouseRight)
                key(bound_[i], true);
        }
        if ((held & b.button) && bound_[i] == kMouseLeft)
            a_left = true;
        if ((held & b.button) && bound_[i] == kMouseRight)
            b_right = true;
        if (released & b.button) {
            if (bound_[i] != kMouseLeft && bound_[i] != kMouseRight)
                key(bound_[i], false);
            bound_[i] = 0;
        }
    }

    // ---- C-stick: relative cursor, quadratic response ----
    stick_cursor(pad.cstick_x, pad.cstick_y, kCstickSpeed, cstick_acc_x_, cstick_acc_y_);

    // ---- touch: a touchpad. Dragging moves the cursor (relative, scaled so
    // one drag across the pad crosses the frame); a short tap clicks where the
    // cursor already is. ----
    // A tap's button stays down for kTapHoldMs: down and up inside one game
    // frame (40 ms) read as no click at all.
    bool tap_left = now_ms < tap_hold_until_ms_;
    if (pad.touching) {
        if (!touch_prev_) {
            touch_start_ms_ = now_ms;
            touch_x0_ = touch_lx_ = pad.touch_x;
            touch_y0_ = touch_ly_ = pad.touch_y;
            touch_moved_ = 0;
        }
        const int d = std::abs(pad.touch_x - touch_x0_) + std::abs(pad.touch_y - touch_y0_);
        touch_moved_ = d > touch_moved_ ? d : touch_moved_;
        touch_acc_x_ += (pad.touch_x - touch_lx_) * kTouchGain * w_ / 800.0f;
        touch_acc_y_ += (pad.touch_y - touch_ly_) * kTouchGain * h_ / 600.0f;
        touch_lx_ = pad.touch_x;
        touch_ly_ = pad.touch_y;
        const int dx = (int)touch_acc_x_, dy = (int)touch_acc_y_;
        if (dx || dy) {
            touch_acc_x_ -= dx;
            touch_acc_y_ -= dy;
            move_cursor(cx_ + dx, cy_ + dy);
        }
    } else if (touch_prev_ && now_ms - touch_start_ms_ < (uint64_t)kTapMs && touch_moved_ <= kTapSlop) {
        tap_left = true;
        tap_hold_until_ms_ = now_ms + kTapHoldMs;
    }
    if (!pad.touching)
        touch_acc_x_ = touch_acc_y_ = 0;
    touch_prev_ = pad.touching;

    // ---- circle pad: walk in the game world; elsewhere (menus, open
    // panels) it moves the cursor like the C-stick ----
    const bool walk_mode = in_game_ && !panel_open_;
    if (!walk_mode) {
        if (walking_) {   // the context changed under a held stick: stop walking
            walking_ = walk_press_pending_ = walk_left_ = false;
        }
        stick_cursor(pad.circle_x, pad.circle_y, kCircleCursorSpeed, circle_acc_x_, circle_acc_y_);
    } else {
        const float vx = pad.circle_x / kStickMax, vy = pad.circle_y / kStickMax;
        float m = std::sqrt(vx * vx + vy * vy);
        if (!walking_ && m > kWalkEngage) {
            walking_ = true;
            walk_press_pending_ = true;
        } else if (walking_ && m < kWalkRelease) {
            walking_ = false;
            walk_press_pending_ = false;
            walk_left_ = false;
        }
        if (walking_) {
            // The click point orbits the character (drawn a little above the
            // frame's centre); a fuller tilt reaches further.
            if (m > 1.0f)
                m = 1.0f;
            const float scale = h_ / 600.0f;
            const float radius = (50.0f + 60.0f * m) * scale;
            const float nx = vx / (m > 0 ? std::sqrt(vx * vx + vy * vy) : 1.0f);
            const float ny = vy / (m > 0 ? std::sqrt(vx * vx + vy * vy) : 1.0f);
            move_cursor(w_ / 2 + (int)(nx * radius), (int)(h_ * 0.47f) - (int)(ny * radius));
            // Move first, press on the next poll: the game resolves a click
            // against what the cursor hovered in the previous frame.
            if (walk_press_pending_)
                walk_press_pending_ = false;
            else
                walk_left_ = true;
        }
    }

    // ---- the mouse buttons, from all their sources ----
    rt::input_button(false, a_left || walk_left_ || tap_left);
    rt::input_button(true, b_right);
    prev_ = held;
}

}  // namespace game
