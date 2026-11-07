// src/game/sprite_frames.cpp, the allocator of the sprite cache's frames
// (issue #44): its classes and blocks, the fullest block first, empty blocks
// given back, a pool's frames going with it. Run through `scripts/docker.sh
// test frames`.
#include "game/sprite_frames.h"

#include <cstdio>
#include <map>
#include <vector>

using game::FrameHeap;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

constexpr uint32_t KiB = 1u << 10;

// A pool of granules from 0x10000000, first fit, with a ceiling.
struct Pool {
    std::map<uint32_t, uint32_t> live;   // va -> bytes
    uint32_t next = 0x10000000, limit = 64u << 20, used = 0;
    FrameHeap::Source source()
    {
        return {[this](uint32_t bytes) -> uint32_t {
                    if (bytes % FrameHeap::kGranule || used + bytes > limit)
                        return 0;
                    const uint32_t va = next;
                    next += bytes;
                    live[va] = bytes;
                    used += bytes;
                    return va;
                },
                [this](uint32_t va, uint32_t) {
                    used -= live[va];
                    live.erase(va);
                }};
    }
};

int main()
{
    {   // The classes: four to each power of two, as tools/sprite_trace.py's quarter_block.
        CHECK(FrameHeap::class_bytes(1) == 16 && FrameHeap::class_bytes(16) == 16);
        CHECK(FrameHeap::class_bytes(17) == 32);   // (16, 32]: steps of 16 at least
        CHECK(FrameHeap::class_bytes(129) == 160 && FrameHeap::class_bytes(160) == 160);
        CHECK(FrameHeap::class_bytes(161) == 192 && FrameHeap::class_bytes(256) == 256);
        // The Brute's death and first attack groups (docs/memory-budget.md): no longer the heap's.
        CHECK(FrameHeap::class_bytes(396203) == 448 * KiB);
        CHECK(FrameHeap::class_bytes(268398) == 320 * KiB);
        for (uint32_t n = 17; n < (4u << 20); n = n * 9 / 8 + 1) {
            const uint32_t c = FrameHeap::class_bytes(n);
            CHECK(c >= n && (uint64_t)c * 4 <= (uint64_t)n * 5 + 64);   // under 25% (9.5% on average)
        }
        CHECK(FrameHeap::pool_bytes(129) == 256 && FrameHeap::pool_bytes(396203) == 396207);
    }
    {   // The slabs: pages up to 64 KiB, whole granules above, at most an eighth wasted where they allow it.
        CHECK(FrameHeap::slab_bytes(16) == 4 * KiB);
        CHECK(FrameHeap::slab_bytes(10 * KiB) == 20 * KiB);    // 2 units, none over
        CHECK(FrameHeap::slab_bytes(40 * KiB) == 40 * KiB);
        CHECK(FrameHeap::slab_bytes(56 * KiB) == 56 * KiB);
        CHECK(FrameHeap::slab_bytes(80 * KiB) == 256 * KiB);   // 3 units, 16 KiB over
        CHECK(FrameHeap::slab_bytes(160 * KiB) == 192 * KiB);  // the least waste there is
        CHECK(FrameHeap::slab_bytes(448 * KiB) == 448 * KiB);  // a unit of its own
        for (uint32_t n = 16; n < (4u << 20); n = n * 9 / 8 + 1) {
            const uint32_t u = FrameHeap::class_bytes(n), b = FrameHeap::slab_bytes(u);
            CHECK(b >= u && b % (u <= FrameHeap::kGranule ? FrameHeap::kPage : FrameHeap::kGranule) == 0);
        }
    }
    {   // Slabs share a block; the fullest slab takes the next unit.
        Pool p;
        FrameHeap h(p.source());
        const uint32_t unit = 10 * KiB, base = 0x10000000;   // 2 to a 20 KiB slab, 3 slabs to a block
        std::vector<uint32_t> a;
        for (int i = 0; i < 4; i++)
            a.push_back(h.alloc(unit - 100, 7));
        CHECK(a[0] == base && a[1] == base + unit && a[2] == base + 20 * KiB && a[3] == base + 30 * KiB);
        CHECK(h.alloc(4000, 7) == base + 40 * KiB);   // another class, the next pages of the same block
        CHECK(h.stats().blocks == 1 && h.stats().held == 64 * KiB && h.stats().in_use == 4 * unit + 4096);
        CHECK(h.stats().asked == 4 * (unit - 100) + 4000 && h.stats().units == 5);
        CHECK(h.unit_bytes(a[1]) == unit && h.unit_bytes(a[1] + 1) == 0 && h.unit_bytes(0x0f000000) == 0);
        CHECK(h.free(a[1]));
        CHECK(!h.free(a[1]) && !h.free(a[0] + 4));   // twice, and not a unit's start
        CHECK(h.free(a[3]));
        // Both slabs have room, 1 of 2 each: the first; then the other, full, gets none.
        CHECK(h.alloc(unit, 7) == a[0] + unit);
        CHECK(h.alloc(unit, 7) == a[3]);
        // Another owner, the same class: a block of its own.
        CHECK(h.alloc(unit, 9) == base + 64 * KiB);
        CHECK(h.stats().blocks == 2);
        // An empty slab gives its pages back, for a slab of another class.
        CHECK(h.free(a[2]) && h.free(a[3]));
        CHECK(h.alloc(16 * KiB, 7) == base + 20 * KiB);   // a 16 KiB slab in the 20 KiB it left
        // An empty block goes back to the pool at once.
        CHECK(h.free(base + 64 * KiB));
        CHECK(h.stats().blocks == 1 && h.stats().given == 1 && p.live.count(base + 64 * KiB) == 0);
    }
    {   // A group over 256 KiB: a block of its own, counted.
        Pool p;
        FrameHeap h(p.source());
        const uint32_t va = h.alloc(396203, 7);
        CHECK(va && h.unit_bytes(va) == 448 * KiB && p.live[va] == 448 * KiB);
        CHECK(h.stats().big == 1 && h.stats().largest == 396203);
        CHECK(h.free(va) && p.live.empty() && h.stats().held == 0 && h.stats().in_use == 0);
    }
    {   // No block to be had: refused, counted, nothing held.
        Pool p;
        p.limit = 64 * KiB;
        FrameHeap h(p.source());
        CHECK(h.alloc(100 * KiB, 7) == 0);
        CHECK(h.stats().refused == 1 && h.stats().blocks == 0);
        CHECK(h.alloc(1000, 7) != 0);   // a smaller class still fits
    }
    {   // A pool system destroyed: its blocks go, the others' stay.
        Pool p;
        FrameHeap h(p.source());
        for (int i = 0; i < 50; i++) {
            h.alloc(1000 + 300 * i, 7);
            h.alloc(2000 + 500 * i, 9);
        }
        const uint32_t before = h.stats().held;
        const uint32_t gone = h.drop_owner(7);
        CHECK(gone > 0 && h.stats().held == before - gone);
        for (const auto &b : p.live)
            CHECK(h.unit_bytes(b.first) != 0);   // all 9's
        h.drop_owner(9);
        CHECK(p.live.empty() && h.stats().held == 0 && h.stats().in_use == 0 && h.stats().asked == 0 &&
              h.stats().units == 0);
    }
    {   // Churn: every unit freed gives every block back.
        Pool p;
        FrameHeap h(p.source());
        std::vector<uint32_t> live;
        uint32_t seed = 1;
        for (int i = 0; i < 20000; i++) {
            seed = seed * 1103515245u + 12345u;
            if (live.size() < 300 && (seed >> 16) % 3) {
                const uint32_t n = 64 + (seed >> 8) % (300 * KiB);
                const uint32_t va = h.alloc(n, 7);
                CHECK(va != 0);
                live.push_back(va);
            } else if (!live.empty()) {
                const size_t k = (seed >> 4) % live.size();
                CHECK(h.free(live[k]));
                live[k] = live.back();
                live.pop_back();
            }
        }
        CHECK(h.stats().units == live.size() && h.stats().in_use <= h.stats().held);
        for (uint32_t va : live)
            CHECK(h.free(va));
        CHECK(p.live.empty() && h.stats().held == 0 && h.stats().taken == h.stats().given);
    }
    if (g_fail) {
        std::printf("%d failed\n", g_fail);
        return 1;
    }
    std::printf("frames: all passed\n");
    return 0;
}
