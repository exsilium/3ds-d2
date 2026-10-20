// Scripted pad input, for unattended runs (the Linux game lane's M4_SCRIPT,
// the 3DS app's sdmc:/3ds/3ds-d2/autoplay.txt). Events are separated by ';'
// or newlines; each starts with a guest time in milliseconds:
//   <ms> tap <x> <y>       put the cursor on game pixel x,y and tap the touchpad (100 ms)
//   <ms> touch <x> <y> [ms] touch the bottom screen at x,y (its pixels) for ms (100)
//   <ms> press <button>    hold a button: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT
//   <ms> release <button>
//   <ms> circle <x> <y>    circle pad position (-156..156, y up)
//   <ms> text <string>     type characters (WM_CHAR), as the on-screen keyboard does
//   <ms> shot              ask the host for a screenshot (if it can take one)
//   <ms> wait <state>      from <ms> on, hold the script until the game shows
//                          textfield | ingame | menus; the later events' times
//                          then count from that moment, so one script works at
//                          any speed (Linux lane, Azahar, the console)
//   <ms> tapuntil <state> <x> <y>  tap x,y, then again every 5 s, until <state>
//                          (a click that is ignored while a screen still loads)
//   # ...                  a comment line
#ifndef D2_GAME_AUTOPLAY_H
#define D2_GAME_AUTOPLAY_H

#include "game/controls.h"
#include "game/d2_state.h"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

class Autoplay {
public:
    void load(const std::string &script);
    bool active() const { return next_ < events_.size() || !waiting_for_.empty() || pad_.held || pad_.touching; }
    // Applies the events due at now_ms to the scripted pad and returns it.
    // shot is set when a "shot" event fired.
    const Pad &step(uint64_t now_ms, Controls &controls, bool &shot, const GameState &state);

private:
    std::vector<std::string> events_;
    size_t next_ = 0;
    Pad pad_;
    uint64_t touch_until_ = 0;
    int64_t shift_ms_ = 0;           // added to event times: the time spent waiting
    uint64_t wait_at_ = 0;           // script time of the wait in progress
    std::string waiting_for_;        // a wait in progress
    bool retry_tap_ = false;         // tapuntil: tap again while waiting
    int retry_x_ = 0, retry_y_ = 0;
    uint64_t next_retry_ms_ = 0;
};

}  // namespace game

#endif
