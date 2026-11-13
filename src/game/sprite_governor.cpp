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
    if (b.spare_taken) {
        // At once, by what the pool is short of the reserve and the spare's
        // own step, from what the cache holds: the least recently used go,
        // the sprites on screen stay (issue #80).
        short_beats_ = 0;
        const uint32_t from = b.used < b.limit ? b.used : b.limit;
        const uint32_t cut = (b.pool_free < reserve ? reserve - b.pool_free : 0) + kStep;
        want = from > cut + min_limit ? (from - cut) & ~(kStep - 1) : min_limit;
        if (want > top)
            want = top;
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
        if (b.pool_free >= reserve + 2 * kStep && full && b.limit < top) {
            uint32_t up = ((b.pool_free - reserve) / 2) & ~(kStep - 1);
            if (up < kStep)
                up = kStep;
            want = top - b.limit > up ? b.limit + up : top;
        }
    }
    last_ = want;
    return want;
}

}  // namespace game
