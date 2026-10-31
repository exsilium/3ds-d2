// The sprite cache's frames, in blocks of their own instead of the game's
// pool system (issue #44); the allocator alone, apart from the game so the
// host can test it. d2_memory.cpp sends the game's allocation of a decoded
// group's frames (0x60c103) and their frees to it.
//
// The pool system gives a block the next power of two from 16 bytes to
// 256 KiB, and mallocs a larger one from the C heap (0x409770). For the
// frames that rounding was 37% of their bytes in 3DS-D2-B, and the Brute's
// death and first attack groups, 386 and 262 KiB, were the requests of two
// `heap full` exits (docs/memory-budget.md, "What a decoded sprite holds").
// Here a unit is one of four classes to each power of two (9.5% rounding in
// the same replay), with no ceiling: a group over 256 KiB gets a block of
// its own, from the same shared pool as every other block.
//
// Blocks come from the pool in its 64 KiB granules (its VirtualAlloc
// alignment). A class's units come in slabs. A slab of 4 KiB pages, for
// units up to 64 KiB, shares a 64 KiB block with other classes' slabs: for
// the units the Linux lane's fight held at its end (31 classes, most with a
// handful of units), a block per class would hold 41% more than the units,
// pages 2.5%. A larger unit's slab is a block of its own, of 64 to 256 KiB
// (a unit over 256 KiB: its own size in granules). A slab is the smallest
// that wastes at most an eighth of itself after its last unit, else the one
// that wastes the least.
//
// A unit goes to the fullest slab of its class with room, a slab to the
// fullest block with a run of pages for it, so that the others empty; an
// empty slab gives its pages back, an empty block goes back to the pool at
// once. Blocks are kept apart by owner (the game's pool system the frames
// were asked of), so that the frames of a pool the game destroys go with
// it, as they would have.
#ifndef D2_GAME_SPRITE_FRAMES_H
#define D2_GAME_SPRITE_FRAMES_H

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace game {

class FrameHeap {
public:
    static constexpr uint32_t kGranule = 64u << 10;
    static constexpr uint32_t kPage = 4u << 10;
    static constexpr uint32_t kPages = kGranule / kPage;      // in a shared block
    static constexpr uint32_t kMaxSlab = 256u << 10;          // the largest slab of several units
    static constexpr uint32_t kPoolLargest = 256u << 10;      // the pool system's largest class
    static constexpr uint32_t kMaxRequest = 64u << 20;        // larger is a bad size: left to the pool system

    // Where blocks come from: take(bytes) gives a block of that many bytes (a
    // multiple of kGranule) or 0; give(va, bytes) takes it back.
    struct Source {
        std::function<uint32_t(uint32_t bytes)> take;
        std::function<void(uint32_t va, uint32_t bytes)> give;
    };
    explicit FrameHeap(Source s) : src_(std::move(s)) {}
    ~FrameHeap();

    // The class of an n-byte request: four to each power of two, 16 bytes
    // at least (tools/sprite_trace.py's quarter_block).
    static uint32_t class_bytes(uint32_t n);
    // What the pool system would have taken for it (pool_block there).
    static uint32_t pool_bytes(uint32_t n);
    // The slab a class's units come in: pages of a shared block up to
    // kGranule, else a block of its own.
    static uint32_t slab_bytes(uint32_t unit);

    // A unit for n bytes, or 0 when no block could be had (or n is 0 or over
    // kMaxRequest).
    uint32_t alloc(uint32_t n, uint32_t owner);
    // Frees the unit at va; false when va is not the start of a unit.
    bool free(uint32_t va);
    // The class of the unit at va (0: not one).
    uint32_t unit_bytes(uint32_t va) const;
    // Every unit of `owner` freed; returns the bytes their blocks held.
    uint32_t drop_owner(uint32_t owner);

    struct Stats {
        uint32_t held = 0;       // bytes in blocks
        uint32_t in_use = 0;     // of those, in units (their classes)
        uint32_t asked = 0;      // the requests of the units in use
        uint32_t blocks = 0, units = 0;
        // Since the start: blocks taken and given back, units over 256 KiB
        // (the pool system's heap blocks), requests refused for want of a
        // block, and the largest request.
        uint32_t taken = 0, given = 0, big = 0, refused = 0, largest = 0;
    };
    const Stats &stats() const { return st_; }

private:
    // A pool block: one slab, or (shared) the pages of several.
    struct Block {
        uint32_t va, bytes, owner;
        bool shared;
        uint16_t pages;   // shared: the pages in use
    };
    struct Slab {
        uint32_t va, bytes, unit, units, used, owner;
        Block *block;
        std::vector<uint32_t> free_bits;   // bit set: the unit is free
        std::vector<uint32_t> asked;       // each unit's request
    };
    Slab *slab_at(uint32_t va) const;
    Block *take_block(uint32_t bytes, uint32_t owner, bool shared);
    void give_block(Block *b);
    void release(Slab *s);

    Source src_;
    std::map<uint32_t, Slab *> by_va_;                                       // every slab, by address
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Slab *>> open_;      // {owner, unit}: slabs with room
    std::vector<Block *> blocks_;
    Stats st_;
};

}  // namespace game

#endif
