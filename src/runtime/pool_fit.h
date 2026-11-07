// Whether a shared pool has room for a block from its top face, without the
// spare (issue #70). GuestRegion::carve (backend patch 0038) puts such a
// block in the highest free piece that holds it: the size rounded up to the
// face's alignment, at the top of the piece, aligned down. A piece that
// cannot hold it that way makes the face take the spare instead, which is
// kept for the game's own requests.
//
// `used` is the pool's map of used blocks (address -> size, in address
// order: GuestRegion::used_map), which also holds the spare's record; the
// free pieces are the gaps between them in [base, base + size).
#ifndef D2_RUNTIME_POOL_FIT_H
#define D2_RUNTIME_POOL_FIT_H

#include <algorithm>
#include <cstdint>

namespace rt {

template <class Map>
bool pool_fits_top(const Map &used, uint32_t base, uint32_t size, uint32_t bytes, uint32_t align)
{
    const uint32_t sz = (bytes + align - 1) & ~(align - 1);
    if (!sz || sz < bytes)
        return false;
    auto fits = [&](uint32_t from, uint32_t to) { return to - from >= sz && ((to - sz) & ~(align - 1)) >= from; };
    uint32_t at = base;
    for (const auto &u : used) {
        if (u.first > at && fits(at, u.first))
            return true;
        at = std::max(at, u.first + u.second);
    }
    const uint32_t end = base + size;
    return end > at && fits(at, end);
}

}  // namespace rt

#endif
