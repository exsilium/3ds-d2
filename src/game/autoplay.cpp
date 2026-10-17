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

const Pad &Autoplay::step(uint64_t now_ms, Controls &controls, bool &shot)
{
    static const struct { const char *name; uint32_t bit; } kNames[] = {
        {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"L", kL}, {"R", kR}, {"ZL", kZL}, {"ZR", kZR},
        {"START", kStart}, {"SELECT", kSelect}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight},
    };
    shot = false;
    while (next_ < events_.size()) {
        const char *ev = events_[next_].c_str();
        unsigned long long at = 0;
        char verb[16] = {}, arg[16] = {};
        int x = 0, y = 0;
        if (std::sscanf(ev, "%llu %15s", &at, verb) < 2) {   // malformed: skip it
            next_++;
            continue;
        }
        if (at > now_ms)
            break;
        next_++;
        rt::logf("autoplay %s", ev);
        const std::string v = verb;
        if (v == "tap" && std::sscanf(ev, "%*llu %*s %d %d", &x, &y) == 2) {
            controls.warp_cursor(x, y);
            pad_.touching = true;
            pad_.touch_x = 160;
            pad_.touch_y = 120;
            touch_until_ = now_ms + 100;
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
        }
    }
    if (pad_.touching && now_ms >= touch_until_)
        pad_.touching = false;
    return pad_;
}

}  // namespace game
