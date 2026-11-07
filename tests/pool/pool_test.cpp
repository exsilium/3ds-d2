// src/runtime/pool_fit.h against the backend's GuestRegion (issue #70): a
// shared pool with a heap face from below, a VirtualAlloc face from above
// and a spare, taken apart by random allocations and frees. Whenever
// pool_fits_top says a VirtualAlloc block fits, the face must place it
// without the spare; whenever it says no, the face must fail or take the
// spare. Run through `scripts/docker.sh test pool`.
#include "runtime/guest_region.h"
#include "runtime/pool_fit.h"

#include <cstdio>
#include <random>
#include <vector>

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

constexpr uint32_t KiB = 1u << 10, MiB = 1u << 20;
constexpr uint32_t kBase = 0x0c10000, kSize = 32 * MiB, kAlign = 0x10000;

int main()
{
    uint32_t checked = 0, yes = 0, no = 0, spare_no = 0;
    for (uint32_t seed = 1; seed <= 40; seed++) {
        wx86::GuestRegion pool, heap, va, spare;
        pool.init(kBase, kSize, 16, "pool");
        heap.share(pool, 16, false, "heap");
        va.share(pool, kAlign, true, "va");
        CHECK(pool.set_spare(spare, 2 * MiB, kAlign));
        std::mt19937 rng(seed);
        std::vector<uint32_t> heap_live, va_live;
        for (int step = 0; step < 4000; step++) {
            // The game's mix: small heap blocks, VirtualAlloc blocks of
            // 64-512 KiB, and frees of either.
            const uint32_t r = rng() % 100;
            if (r < 30) {
                if (const uint32_t a = heap.alloc(16 + rng() % (8 * KiB)))
                    heap_live.push_back(a);
            } else if (r < 55) {
                if (const uint32_t a = va.alloc((1 + rng() % 8) * 64 * KiB))
                    va_live.push_back(a);
            } else if (r < 75 && !heap_live.empty()) {
                const size_t i = rng() % heap_live.size();
                heap.free(heap_live[i]);
                heap_live.erase(heap_live.begin() + (long)i);
            } else if (!va_live.empty()) {
                const size_t i = rng() % va_live.size();
                va.free(va_live[i]);
                va_live.erase(va_live.begin() + (long)i);
            }
            // A request the frames allocator might make: does the rule say
            // what the face then does?
            const uint32_t want = 64 * KiB + rng() % (448 * KiB);
            const bool fits = rt::pool_fits_top(pool.used_map(), pool.base(), kSize, want, kAlign);
            const uint32_t takes = pool.spare_takes();
            const uint32_t a = va.alloc(want);
            const bool from_spare = pool.spare_takes() != takes;
            checked++;
            if (fits) {
                yes++;
                CHECK(a != 0 && !from_spare);
            } else {
                no++;
                spare_no += from_spare;
                CHECK(a == 0 || from_spare);
            }
            if (a)
                va.free(a);
        }
    }
    // Edge cases: an exact 256 KiB piece holds a 256 KiB block; one byte
    // more does not; nothing fits in a full pool.
    {
        wx86::GuestRegion pool, va;
        pool.init(kBase, 1 * MiB, 16, "pool");
        va.share(pool, kAlign, true, "va");
        const uint32_t a = va.alloc(768 * KiB);   // the top 768 KiB: 256 KiB left at the bottom
        CHECK(a == kBase + 256 * KiB);
        CHECK(rt::pool_fits_top(pool.used_map(), pool.base(), 1 * MiB, 256 * KiB, kAlign));
        CHECK(!rt::pool_fits_top(pool.used_map(), pool.base(), 1 * MiB, 256 * KiB + 1, kAlign));
        CHECK(va.alloc(256 * KiB) == kBase);
        CHECK(!rt::pool_fits_top(pool.used_map(), pool.base(), 1 * MiB, 1, kAlign));
    }
    std::printf("pool: %u requests checked, %u fit, %u did not (%u of those from the spare)\n", checked, yes, no,
                spare_no);
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("pool: all passed\n");
    return 0;
}
