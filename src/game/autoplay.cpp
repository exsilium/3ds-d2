// See autoplay.h.
#include "game/autoplay.h"

#include "runtime/rt_host.h"

#include <cstdio>
#include <cstring>

namespace game {

void Autoplay::load(const std::string &script)
{
    events_.clear();
    next_ = 0;
    shift_ms_ = 0;
    wait_at_ = 0;
    waiting_for_.clear();
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
    if (!waiting_for_.empty()) {
        const bool met = (waiting_for_ == "textfield" && state.edit_focus) ||
                         (waiting_for_ == "ingame" && state.in_game) || (waiting_for_ == "menus" && !state.in_game);
        if (!met) {
            if (retry_tap_ && now_ms >= next_retry_ms_ && !pad_.touching) {
                controls.warp_cursor(retry_x_, retry_y_);
                pad_.touching = true;
                pad_.touchpad = true;
                pad_.touch_x = 160;
                pad_.touch_y = 120;
                touch_until_ = now_ms + 100;
                next_retry_ms_ = now_ms + 5000;
            }
            if (pad_.touching && now_ms >= touch_until_)
                pad_.touching = false;
            return pad_;
        }
        retry_tap_ = false;
        rt::logf("autoplay: %s reached", waiting_for_.c_str());
        waiting_for_.clear();
        shift_ms_ = (int64_t)now_ms - (int64_t)wait_at_;   // later events count from here
    }
    while (next_ < events_.size()) {
        const char *ev = events_[next_].c_str();
        unsigned long long at = 0;
        char verb[16] = {}, arg[16] = {};
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
        if (v == "tap" && std::sscanf(ev, "%*llu %*s %d %d", &x, &y) == 2) {
            controls.warp_cursor(x, y);
            pad_.touching = true;
            pad_.touchpad = true;
            pad_.touch_x = 160;
            pad_.touch_y = 120;
            touch_until_ = now_ms + 100;
        } else if (v == "touch" && std::sscanf(ev, "%*llu %*s %d %d", &x, &y) == 2) {
            int ms = 100;
            std::sscanf(ev, "%*llu %*s %*d %*d %d", &ms);
            pad_.touching = true;
            pad_.touchpad = false;
            pad_.touch_x = x;
            pad_.touch_y = y;
            touch_until_ = now_ms + (uint64_t)ms;
        } else if (v == "circle" && std::sscanf(ev, "%*llu %*s %d %d", &x, &y) == 2) {
            pad_.circle_x = x;
            pad_.circle_y = y;
        } else if ((v == "press" || v == "release") && std::sscanf(ev, "%*llu %*s %15s", arg) == 1) {
            for (const auto &n : kNames)
                if (std::strcmp(arg, n.name) == 0)
                    pad_.held = v == "press" ? (pad_.held | n.bit) : (pad_.held & ~n.bit);
        } else if (v == "text") {
            const char *t = std::strstr(ev, "text ");
            for (t = t ? t + 5 : ""; *t; t++)
                rt::input_char((unsigned char)*t);
        } else if (v == "shot") {
            shot = true;
        } else if (v == "tapuntil" && std::sscanf(ev, "%*llu %*s %15s %d %d", arg, &x, &y) == 3) {
            waiting_for_ = arg;
            wait_at_ = script_at;
            retry_tap_ = true;
            retry_x_ = x;
            retry_y_ = y;
            next_retry_ms_ = now_ms;   // the first tap right away
            break;
        } else if (v == "wait" && std::sscanf(ev, "%*llu %*s %15s", arg) == 1) {
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
