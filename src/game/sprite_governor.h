// The sprite cache governor's policy (issue #18), apart from the game so the
// host can test it: from one heartbeat's figures, the cache limit to set.
//
// The pool systems keep what the cache evicts (only empty blocks go back to
// the guest pool), so lowering the limit does not give the pool its free
// space back: it stops the cache growing, and the evicted units serve the
// game's other data. A limit cut by twice the shortfall on each sample, and
// restored as soon as the pool was above the reserve, bounced between 4.9
// and 13 MiB 875 times in a 3 h 51 min session, and 43% of its play windows
// re-loaded sprites the cache had just evicted. Hence:
//
// - lower only after two consecutive heartbeats short of the reserve, by the
//   shortfall, from what the cache holds (not from its limit);
// - raise in 256 KiB steps while the pool is a margin above the reserve and
//   the cache is at its limit (raising a limit it does not reach would let it
//   grow by megabytes at the next area before the pool shows it);
// - not below the working set: the most the cache held over the last 10
//   minutes in a heartbeat where it had room and evicted nothing, unless the
//   pool is more than kFloorSlack short of the reserve (a cache at its limit
//   says nothing about what it needs: counting it held the limit where it
//   was for hours, in the 4 h 25 min session of 2026-10-29). Half the
//   reserve was the bound before: the floor then held the pool at 2.7 to 4
//   MiB free, near the 2 MiB spare, for minutes on end (2026-10-29, 30 and
//   20 min sessions), while a sprite trace of the second showed no sprite
//   the cache let go of ever drawn again: holding it bought nothing. That
//   trace's keys changed with each decode, so it could not show one (issue
//   #31): in the next, 10 of 46 MiB were evictions decoded again. The
//   bound wants judging again on a version 3 trace;
// - a request served from the pool's spare means no other piece of the pool
//   was left: the minimum at once.
//
// The first limit seen is the game's size for the cache; a limit other than
// the one step() last returned means the game re-created the cache (between
// the menus and a game), and what was learnt of the old one goes.
#ifndef D2_GAME_SPRITE_GOVERNOR_H
#define D2_GAME_SPRITE_GOVERNOR_H

#include <cstdint>

namespace game {

class SpriteGovernor {
public:
    static constexpr uint32_t kStep = 256u << 10;
    static constexpr uint32_t kHistory = 60;   // heartbeats of 10 s: the working set's window
    static constexpr uint32_t kShortBeats = 2;
    static constexpr uint32_t kFloorSlack = 1u << 20;   // the floor holds only this close to the reserve

    struct Beat {
        uint32_t limit;        // the limit in force
        uint32_t used;         // bytes of sprites it holds
        uint32_t evicted;      // sprites it let go of since the last beat
        uint32_t pool_free;    // the guest pool's free bytes
        bool spare_used;       // a request was served from the pool's spare
    };

    // The limit to set (== beat.limit: no change).
    uint32_t step(const Beat &b, uint32_t reserve, uint32_t min_limit);

    uint32_t working_set() const;
    uint32_t game_limit() const { return game_limit_; }

private:
    void forget();

    uint32_t held_[kHistory] = {};   // bytes held in each beat with room and no evictions (else 0)
    uint32_t at_ = 0;
    uint32_t short_beats_ = 0;
    uint32_t game_limit_ = 0;   // the size the game gave the cache
    uint32_t last_ = 0;         // the limit step() last returned
};

}  // namespace game

#endif
