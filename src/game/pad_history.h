// The pad between two message pumps (issue #118). The game reads input from
// its message pump, about once a frame (40-80 ms in play, 200 ms at 5 fps),
// so a press and release that both fell between two pumps used to be lost.
// A sampler reads the pad every few milliseconds and pushes each reading
// here. The pump takes what to apply:
//   - nothing changed twice since the last take (no button or the touch
//     screen went down and up, or up and down): the latest reading alone,
//     as when the pad was read at the pump itself;
//   - something did: every reading where a button or the touch screen
//     changed, in order, each with its own time, then the latest, so the
//     short press reaches the game as a press and a release.
// Header only, no platform: the app's sampler thread and the host test
// (tests/pad) use it alike. push() and take() may run on different threads.
#ifndef D2_GAME_PAD_HISTORY_H
#define D2_GAME_PAD_HISTORY_H

#include "game/controls.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace game {

struct PadSample {
    Pad pad;
    uint64_t ms = 0;
};

class PadHistory {
public:
    static constexpr size_t kMax = 64;   // changes kept between two takes; the oldest go first

    void push(const Pad &pad, uint64_t ms)
    {
        lock();
        if (!have_ || pad.held != latest_.pad.held || pad.touching != latest_.pad.touching) {
            if (changes_.size() == kMax) {
                changes_.erase(changes_.begin());
                dropped_++;
            }
            changes_.push_back({pad, ms});
        }
        latest_ = {pad, ms};
        have_ = true;
        unlock();
    }

    // What to apply, in order (empty before the first push). Returns how
    // many changes were replayed: 0 when the latest reading stands alone.
    size_t take(std::vector<PadSample> &out)
    {
        out.clear();
        lock();
        size_t replayed = 0;
        if (have_) {
            if (changed_twice()) {
                out = changes_;
                replayed = changes_.size();
            }
            out.push_back(latest_);
            taken_ = latest_.pad;
            taken_any_ = true;
            changes_.clear();
        }
        unlock();
        return replayed;
    }

    // The changes since the last take forgotten (back from the HOME menu or
    // sleep: what was pressed there wasn't the game's); the latest reading
    // stays, and the next take applies it alone.
    void reset()
    {
        lock();
        changes_.clear();
        unlock();
    }

    uint32_t dropped() const { return dropped_; }

private:
    // Whether a button or the touch screen changed more than once from the
    // state the last take left, through the changes since.
    bool changed_twice() const
    {
        if (changes_.size() < 2)
            return false;
        uint32_t held = taken_any_ ? taken_.held : changes_.front().pad.held;
        bool touch = taken_any_ ? taken_.touching : changes_.front().pad.touching;
        uint32_t flipped = 0;
        bool touch_flipped = false;
        for (const PadSample &s : changes_) {
            const uint32_t d = s.pad.held ^ held;
            if (d & flipped)
                return true;
            flipped |= d;
            if (s.pad.touching != touch) {
                if (touch_flipped)
                    return true;
                touch_flipped = true;
            }
            held = s.pad.held;
            touch = s.pad.touching;
        }
        return false;
    }

    void lock()
    {
        while (busy_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() { busy_.clear(std::memory_order_release); }

    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::vector<PadSample> changes_;
    PadSample latest_;
    Pad taken_;
    bool have_ = false, taken_any_ = false;
    uint32_t dropped_ = 0;
};

}  // namespace game

#endif
