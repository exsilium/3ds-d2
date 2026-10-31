// See sprite_frames.h.
#include "game/sprite_frames.h"

#include <algorithm>

namespace game {

namespace {

// The smallest multiple of `step` up to `top` holding a unit that wastes at
// most an eighth of itself after its last unit, else the one wasting the
// least for its size.
uint32_t fit(uint32_t unit, uint32_t step, uint32_t top)
{
    uint32_t best = 0, best_waste = 0;
    for (uint32_t b = step; b <= top; b += step) {
        if (b < unit)
            continue;
        const uint32_t waste = b % unit;
        if (waste * 8 <= b)
            return b;
        if (!best || (uint64_t)waste * best < (uint64_t)best_waste * b)
            best = b, best_waste = waste;
    }
    return best;
}

// The first run of n free pages in a shared block's mask, or -1.
int free_run(uint16_t used, uint32_t n)
{
    for (uint32_t i = 0; i + n <= FrameHeap::kPages; i++) {
        const uint32_t run = ((1u << n) - 1) << i;
        if (!(used & run))
            return (int)i;
    }
    return -1;
}

}  // namespace

uint32_t FrameHeap::class_bytes(uint32_t n)
{
    if (n <= 16)
        return 16;
    // n in (2^(k-1), 2^k]: steps of 2^(k-3), 16 at least.
    const uint32_t k = 32 - (uint32_t)__builtin_clz(n - 1);
    const uint32_t step = std::max<uint32_t>(1u << (k - 3), 16);
    return (n + step - 1) / step * step;
}

uint32_t FrameHeap::pool_bytes(uint32_t n)
{
    if (n > kPoolLargest)
        return n + 4;   // malloc'd, with its size in front (0x409770)
    return n <= 16 ? 16 : 1u << (32 - __builtin_clz(n - 1));
}

uint32_t FrameHeap::slab_bytes(uint32_t unit)
{
    if (unit <= kGranule)
        return fit(unit, kPage, kGranule);
    if (unit > kMaxSlab)
        return (unit + kGranule - 1) / kGranule * kGranule;
    return fit(unit, kGranule, kMaxSlab);
}

FrameHeap::~FrameHeap()
{
    for (auto &e : by_va_)
        delete e.second;
    for (Block *b : blocks_)
        delete b;
}

FrameHeap::Slab *FrameHeap::slab_at(uint32_t va) const
{
    auto it = by_va_.upper_bound(va);
    if (it == by_va_.begin())
        return nullptr;
    Slab *s = std::prev(it)->second;
    return va - s->va < s->bytes ? s : nullptr;
}

FrameHeap::Block *FrameHeap::take_block(uint32_t bytes, uint32_t owner, bool shared)
{
    const uint32_t va = src_.take(bytes);
    if (!va)
        return nullptr;
    Block *b = new Block{va, bytes, owner, shared, 0};
    blocks_.push_back(b);
    st_.held += bytes;
    st_.blocks++;
    st_.taken++;
    return b;
}

void FrameHeap::give_block(Block *b)
{
    blocks_.erase(std::find(blocks_.begin(), blocks_.end(), b));
    st_.held -= b->bytes;
    st_.blocks--;
    st_.given++;
    src_.give(b->va, b->bytes);
    delete b;
}

uint32_t FrameHeap::alloc(uint32_t n, uint32_t owner)
{
    if (!n || n > kMaxRequest)
        return 0;
    const uint32_t unit = class_bytes(n);
    std::vector<Slab *> &open = open_[{owner, unit}];
    Slab *s = nullptr;
    for (Slab *o : open)   // the fullest
        if (!s || o->used > s->used)
            s = o;
    if (!s) {
        const uint32_t bytes = slab_bytes(unit);
        Block *b = nullptr;
        uint32_t va = 0;
        if (bytes <= kGranule) {
            // Pages of the fullest shared block with a run for them.
            const uint32_t pages = bytes / kPage;
            int at = -1;
            for (Block *o : blocks_) {
                if (!o->shared || o->owner != owner ||
                    (b && __builtin_popcount(o->pages) <= __builtin_popcount(b->pages)))
                    continue;
                const int i = free_run(o->pages, pages);
                if (i >= 0)
                    b = o, at = i;
            }
            if (!b) {
                b = take_block(kGranule, owner, true);
                at = 0;
            }
            if (b) {
                b->pages |= (uint16_t)(((1u << pages) - 1) << at);
                va = b->va + (uint32_t)at * kPage;
            }
        } else if ((b = take_block(bytes, owner, false)) != nullptr) {
            va = b->va;
        }
        if (!b) {
            st_.refused++;
            return 0;
        }
        s = new Slab{va, bytes, unit, bytes / unit, 0, owner, b, {}, {}};
        s->free_bits.assign((s->units + 31) / 32, 0);
        for (uint32_t i = 0; i < s->units; i++)
            s->free_bits[i / 32] |= 1u << (i % 32);
        s->asked.assign(s->units, 0);
        by_va_[va] = s;
        open.push_back(s);
    }
    uint32_t i = 0;
    for (uint32_t w = 0; w < s->free_bits.size(); w++)
        if (s->free_bits[w]) {
            i = w * 32 + (uint32_t)__builtin_ctz(s->free_bits[w]);
            break;
        }
    s->free_bits[i / 32] &= ~(1u << (i % 32));
    s->asked[i] = n;
    if (++s->used == s->units)
        open.erase(std::find(open.begin(), open.end(), s));
    st_.in_use += unit;
    st_.asked += n;
    st_.units++;
    if (n > kPoolLargest)
        st_.big++;
    st_.largest = std::max(st_.largest, n);
    return s->va + i * unit;
}

void FrameHeap::release(Slab *s)
{
    auto &open = open_[{s->owner, s->unit}];
    auto it = std::find(open.begin(), open.end(), s);
    if (it != open.end())
        open.erase(it);
    by_va_.erase(s->va);
    Block *b = s->block;
    if (b->shared) {
        const uint32_t pages = s->bytes / kPage, at = (s->va - b->va) / kPage;
        b->pages &= (uint16_t) ~(((1u << pages) - 1) << at);
    }
    delete s;
    if (!b->shared || !b->pages)
        give_block(b);
}

bool FrameHeap::free(uint32_t va)
{
    Slab *s = slab_at(va);
    if (!s || (va - s->va) % s->unit)
        return false;
    const uint32_t i = (va - s->va) / s->unit;
    if (i >= s->units || (s->free_bits[i / 32] >> (i % 32) & 1))
        return false;
    s->free_bits[i / 32] |= 1u << (i % 32);
    st_.in_use -= s->unit;
    st_.asked -= s->asked[i];
    st_.units--;
    if (s->used-- == s->units)
        open_[{s->owner, s->unit}].push_back(s);
    if (!s->used)
        release(s);
    return true;
}

uint32_t FrameHeap::unit_bytes(uint32_t va) const
{
    const Slab *s = slab_at(va);
    return s && (va - s->va) % s->unit == 0 && (va - s->va) / s->unit < s->units ? s->unit : 0;
}

uint32_t FrameHeap::drop_owner(uint32_t owner)
{
    std::vector<Slab *> mine;
    for (const auto &e : by_va_)
        if (e.second->owner == owner)
            mine.push_back(e.second);
    const uint32_t before = st_.held;
    for (Slab *s : mine) {
        for (uint32_t i = 0; i < s->units; i++)
            if (!(s->free_bits[i / 32] >> (i % 32) & 1)) {
                st_.in_use -= s->unit;
                st_.asked -= s->asked[i];
                st_.units--;
            }
        release(s);
    }
    return before - st_.held;
}

}  // namespace game
