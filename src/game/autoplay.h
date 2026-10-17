// Scripted pad input, for unattended runs (the Linux game lane's M4_SCRIPT,
// the 3DS app's sdmc:/3ds/3ds-d2/autoplay.txt). Events are separated by ';'
// or newlines; each starts with a guest time in milliseconds:
//   <ms> tap <x> <y>       put the cursor on game pixel x,y and tap the touchpad (100 ms)
//   <ms> press <button>    hold a button: A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT
//   <ms> release <button>
//   <ms> circle <x> <y>    circle pad position (-156..156, y up)
//   <ms> text <string>     type characters (WM_CHAR), as the on-screen keyboard does
//   <ms> shot              ask the host for a screenshot (if it can take one)
//   # ...                  a comment line
#ifndef D2_GAME_AUTOPLAY_H
#define D2_GAME_AUTOPLAY_H

#include "game/controls.h"

#include <cstdint>
#include <string>
#include <vector>

namespace game {

class Autoplay {
public:
    void load(const std::string &script);
    bool active() const { return next_ < events_.size() || pad_.held || pad_.touching; }
    // Applies the events due at now_ms to the scripted pad and returns it.
    // shot is set when a "shot" event fired.
    const Pad &step(uint64_t now_ms, Controls &controls, bool &shot);

private:
    std::vector<std::string> events_;
    size_t next_ = 0;
    Pad pad_;
    uint64_t touch_until_ = 0;
};

}  // namespace game

#endif
