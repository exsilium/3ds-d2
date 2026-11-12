// See autoplay.h.
#include "game/autoplay.h"

#include "game/d2_units.h"

#include "runtime/rt_host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game {
namespace {

constexpr int kNpcMenuUi = 0x08;        // GameState::ui: an NPC's menu (talk, trade, travel...)
constexpr uint64_t kUnitRetryMs = 1500; // tapunit, tapmenu: tapped again where the unit is now
constexpr uint64_t kHoverMs = 300;      // ...the cursor on the spot that long before the press
// tapunit: the game pixels a tap may land on, above the bottom panel.
constexpr int kEdgeX0 = 24, kEdgeX1 = 615, kEdgeY0 = 40, kEdgeY1 = 380;

}  // namespace

void Autoplay::load(const std::string &script)
{
    events_.clear();
    next_ = 0;
    shift_ms_ = 0;
    wait_at_ = 0;
    waiting_for_.clear();
    retry_tap_ = false;
    retry_unit_ = 0;
    menu_unit_ = 0;
    retry_line_ = 0;
    click_at_ = 0;
    click_until_ = 0;
    skip_videos_ = false;
    skip_release_ms_ = skip_next_ms_ = 0;
    close_panels_ = false;
    close_release_ms_ = close_next_ms_ = 0;
    pad_ = Pad();
    std::string item;
    for (size_t i = 0; i <= script.size(); i++) {
        const char ch = i < script.size() ? script[i] : '\n';
        if (ch == ';' || ch == '\n' || ch == '\r') {
            size_t b = item.find_first_not_of(" \t");
            if (b != std::string::npos && item[b] != '#')
                events_.push_back(item.substr(b));
            item.clear();
        } else {
            item += ch;
        }
    }
}

const Pad &Autoplay::step(uint64_t now_ms, Controls &controls, bool &shot, const GameState &state)
{
    static const struct { const char *name; uint32_t bit; } kNames[] = {
        {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"L", kL}, {"R", kR}, {"ZL", kZL}, {"ZR", kZR},
        {"START", kStart}, {"SELECT", kSelect}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight},
    };
    shot = false;
    // skipvideos: A (a left click) while a cinematic plays skips it; one
    // press per video, the next no sooner than a second after a release.
    if (skip_videos_) {
        if (skip_release_ms_) {
            if (now_ms >= skip_release_ms_) {
                pad_.held &= ~kA;
                skip_release_ms_ = 0;
                skip_next_ms_ = now_ms + 1000;
            }
        } else if (state.video && now_ms >= skip_next_ms_) {
            rt::logf("autoplay: A to skip the video");
            pad_.held |= kA;
            skip_release_ms_ = now_ms + 100;
        }
    }
    // closepanels: START (Escape) while a panel is open in the game (an
    // NPC's menu a walk clicked into, say); one press, the next no sooner
    // than a second after the release, so the panel has closed.
    if (close_panels_) {
        if (close_release_ms_) {
            if (now_ms >= close_release_ms_) {
                pad_.held &= ~kStart;
                close_release_ms_ = 0;
                close_next_ms_ = now_ms + 1000;
            }
        } else if (state.in_game && state.panel_open && now_ms >= close_next_ms_) {
            rt::logf("autoplay: START to close a panel (ui %llx)", (unsigned long long)state.ui);
            pad_.held |= kStart;
            close_release_ms_ = now_ms + 100;
        }
    }
    // The click of a tap (tap, tapuntil, tapunit, tapmenu): A, the left
    // mouse button, once the cursor has been over the spot for a few frames
    // (the game resolves a click against what the cursor hovered in the
    // frame before). A, not a touch: the bottom screen would take a touch.
    if (click_at_ && now_ms >= click_at_) {
        click_at_ = 0;
        pad_.held |= kA;
        click_until_ = now_ms + 100;
    }
    if (click_until_ && now_ms >= click_until_) {
        pad_.held &= ~kA;
        click_until_ = 0;
    }
    if (!waiting_for_.empty()) {
        const bool met = (waiting_for_ == "textfield" && state.edit_focus) ||
                         (waiting_for_ == "ingame" && state.in_game) || (waiting_for_ == "menus" && !state.in_game) ||
                         (waiting_for_ == "npcmenu" && state.in_game && (state.ui >> kNpcMenuUi & 1)) ||
                         (waiting_for_ == "menuclosed" && !(state.in_game && (state.ui >> kNpcMenuUi & 1))) ||
                         (waiting_for_.size() > 2 && waiting_for_.compare(0, 2, "ui") == 0 && state.in_game &&
                          (state.ui >> std::atoi(waiting_for_.c_str() + 2) & 1)) ||
                         (waiting_for_.size() == 4 && waiting_for_.compare(0, 3, "act") == 0 && state.in_game &&
                          player_act() == waiting_for_[3] - '0');
        if (!met) {
            if (retry_tap_ && now_ms >= next_retry_ms_ && !click_until_ && !click_at_ && retry_unit_) {
                // tapunit: where the unit is now; one off the screen is
                // walked towards (the spot at the edge in its direction).
                // tapmenu: the line of its menu, where the unit is now.
                UnitSpot u;
                if (find_unit(retry_unit_, u, retry_type_)) {
                    int x = u.x < kEdgeX0 ? kEdgeX0 : u.x > kEdgeX1 ? kEdgeX1 : u.x;
                    int y = u.y < kEdgeY0 ? kEdgeY0 : u.y > kEdgeY1 ? kEdgeY1 : u.y;
                    if (retry_line_)
                        npc_menu_line(u, retry_line_, x, y);
                    if (retry_line_)
                        rt::logf("autoplay: unit %u at %d,%d (%.1f subtiles), menu line %d at %d,%d", u.cls, u.x, u.y,
                                 u.distance, retry_line_, x, y);
                    else
                        rt::logf("autoplay: unit %u at %d,%d (%.1f subtiles), tapping %d,%d", u.cls, u.x, u.y,
                                 u.distance, x, y);
                    controls.warp_cursor(x, y);
                    click_at_ = now_ms + kHoverMs;
                }
                next_retry_ms_ = now_ms + kUnitRetryMs;
            } else if (retry_tap_ && now_ms >= next_retry_ms_ && !click_until_ && !click_at_) {
                controls.warp_cursor(retry_x_, retry_y_);
                click_at_ = now_ms + kHoverMs;
                next_retry_ms_ = now_ms + 5000;
            }
            if (pad_.touching && now_ms >= touch_until_)
                pad_.touching = false;
            return pad_;
        }
        retry_tap_ = false;
        retry_unit_ = 0;
        retry_line_ = 0;
        click_at_ = 0;
        if (click_until_) {
            pad_.held &= ~kA;
            click_until_ = 0;
        }
        rt::logf("autoplay: %s reached", waiting_for_.c_str());
        waiting_for_.clear();
        shift_ms_ = (int64_t)now_ms - (int64_t)wait_at_;   // later events count from here
    }
    while (next_ < events_.size()) {
        const char *ev = events_[next_].c_str();
        unsigned long long at = 0;
        char verb[16] = {}, arg[64] = {};
        int x = 0, y = 0;
        if (std::sscanf(ev, "%llu %15s", &at, verb) < 2) {   // malformed: skip it
            next_++;
            continue;
        }
        const unsigned long long script_at = at;
        at = (unsigned long long)((int64_t)at + shift_ms_);
        if (at > now_ms)
            break;
        next_++;
        rt::logf("autoplay %s", ev);
        const std::string v = verb;
        if (v == "tap" && std::sscanf(ev, "%*s %*s %d %d", &x, &y) == 2) {
            controls.warp_cursor(x, y);
            click_at_ = now_ms + kHoverMs;
        } else if (v == "skipvideos") {
            skip_videos_ = true;
        } else if (v == "closepanels") {
            close_panels_ = std::strstr(ev, "closepanels off") == nullptr;
        } else if (v == "touch" && std::sscanf(ev, "%*s %*s %d %d", &x, &y) == 2) {
            int ms = 100;
            std::sscanf(ev, "%*s %*s %*d %*d %d", &ms);
            pad_.touching = true;
            pad_.touchpad = false;
            pad_.touch_x = x;
            pad_.touch_y = y;
            touch_until_ = now_ms + (uint64_t)ms;
        } else if (v == "circle" && std::sscanf(ev, "%*s %*s %d %d", &x, &y) == 2) {
            pad_.circle_x = x;
            pad_.circle_y = y;
        } else if ((v == "press" || v == "release") && std::sscanf(ev, "%*s %*s %15s", arg) == 1) {
            for (const auto &n : kNames)
                if (std::strcmp(arg, n.name) == 0)
                    pad_.held = v == "press" ? (pad_.held | n.bit) : (pad_.held & ~n.bit);
        } else if (v == "text") {
            const char *t = std::strstr(ev, "text ");
            for (t = t ? t + 5 : ""; *t; t++)
                rt::input_char((unsigned char)*t);
        } else if (v == "shot") {
            shot = true;
        } else if (v == "profile" && std::sscanf(ev, "%*s %*s %63s", arg) == 1) {
            profile_ = arg;
        } else if (v == "tapmenu" && std::sscanf(ev, "%*s %*s %d", &x) == 1 && x > 0) {
            if (!menu_unit_) {
                rt::logf("autoplay: tapmenu without an NPC (tapunit first)");
                continue;
            }
            retry_unit_ = menu_unit_;
            retry_type_ = 1;
            retry_line_ = x;
            waiting_for_ = "menuclosed";
            wait_at_ = script_at;
            retry_tap_ = true;
            next_retry_ms_ = now_ms;
            break;
        } else if (v == "units") {
            log_units();
        } else if (v == "tapobject" && std::sscanf(ev, "%*s %*s %15s", arg) == 1) {
            char state[16] = "ui20";   // the waypoint's panel
            std::sscanf(ev, "%*s %*s %*s %15s", state);
            retry_unit_ = (uint32_t)std::strtoul(arg, nullptr, 10);
            retry_type_ = 2;
            retry_line_ = 0;
            waiting_for_ = state;
            wait_at_ = script_at;
            retry_tap_ = true;
            next_retry_ms_ = now_ms;
            break;
        } else if (v == "tapunit" && std::sscanf(ev, "%*s %*s %15s", arg) == 1) {
            char state[16] = "npcmenu";
            std::sscanf(ev, "%*s %*s %*s %15s", state);
            retry_type_ = 1;
            retry_unit_ = menu_unit_ = unit_class(arg);
            if (!retry_unit_) {
                rt::logf("autoplay: no unit named %s", arg);
                continue;
            }
            waiting_for_ = state;
            wait_at_ = script_at;
            retry_tap_ = true;
            next_retry_ms_ = now_ms;
            break;
        } else if (v == "tapuntil" && std::sscanf(ev, "%*s %*s %15s %d %d", arg, &x, &y) == 3) {
            waiting_for_ = arg;
            wait_at_ = script_at;
            retry_tap_ = true;
            retry_x_ = x;
            retry_y_ = y;
            next_retry_ms_ = now_ms;   // the first tap right away
            break;
        } else if (v == "wait" && std::sscanf(ev, "%*s %*s %15s", arg) == 1) {
            waiting_for_ = arg;
            wait_at_ = script_at;   // this event's own script time
            break;
        }
    }
    if (pad_.touching && now_ms >= touch_until_)
        pad_.touching = false;
    return pad_;
}

}  // namespace game
