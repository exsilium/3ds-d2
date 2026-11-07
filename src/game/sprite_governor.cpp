#include "game/sprite_governor.h"

namespace game {

void SpriteGovernor::forget()
{
    for (uint32_t &h : held_)
        h = 0;
    at_ = 0;
    short_beats_ = 0;
}

uint32_t SpriteGovernor::working_set() const
{
    uint32_t most = 0;
    for (uint32_t h : held_)
        if (h > most)
            most = h;
    return most;
}

uint32_t SpriteGovernor::step(const Beat &b, uint32_t reserve, uint32_t min_limit)
{
    if (b.limit != last_) {   // the first beat, or the game re-created the cache
        forget();
        game_limit_ = b.limit;
    }
    const bool full = b.used + kStep > b.limit;
    held_[at_] = b.evicted || full ? 0 : b.used;
    at_ = (at_ + 1) % kHistory;

    const uint32_t top = game_limit_;
    uint32_t want = b.limit;
    if (b.spare_used) {
        short_beats_ = 0;
        want = min_limit < top ? min_limit : top;
    } else if (b.pool_free < reserve) {
        if (++short_beats_ >= kShortBeats) {
            short_beats_ = 0;
            const uint32_t from = b.used < b.limit ? b.used : b.limit;
            const uint32_t shortfall = reserve - b.pool_free;
            want = from > shortfall + min_limit ? (from - shortfall) & ~(kStep - 1) : min_limit;
            uint32_t floor = min_limit;
            if (b.pool_free + kFloorSlack >= reserve && working_set() > floor)
                floor = working_set();
            if (want < floor)
                want = floor;
            if (want > b.limit)   // the floor never raises it
                want = b.limit;
        }
    } else {
        short_beats_ = 0;
        if (b.pool_free >= reserve + 2 * kStep && full && b.limit < top)
            want = top - b.limit > kStep ? b.limit + kStep : top;
    }
    last_ = want;
    return want;
}

}  // namespace game
