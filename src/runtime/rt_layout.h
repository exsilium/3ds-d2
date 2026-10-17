// Guest address-space layouts for 3ds-d2's Win32 runtime (WinVita bridge and
// schedulers on the Box86 backend). Each layout keeps everything below `span`,
// so the guest runs from one host block in WinVita's single-arena model
// (host = guest + membase), which the 3DS requires.
#ifndef D2_RUNTIME_RT_LAYOUT_H
#define D2_RUNTIME_RT_LAYOUT_H

#include <cstdint>

namespace rt {

struct Layout {
    const char *name;
    uint32_t dll_base;        // DLLs loaded after the executable (its preferred base is kept)
    uint32_t heap_base, heap_size;        // guest heap (HeapAlloc & co.)
    uint32_t va_base, va_size;            // VirtualAlloc arena
    uint32_t scratch_base, scratch_size;  // engine scratch (returned strings, structs)
    uint32_t peb;             // process environment block (one page)
    uint32_t main_tib;        // main thread's TEB (one page)
    uint32_t worker_stacks;   // worker thread stacks grow upward from here
    uint32_t worker_stack;    // default size of each
    uint32_t worker_tibs;     // worker TEBs, one page each
    uint32_t main_stack;      // bridge main stack base (2 MiB)
    uint32_t trap_base;       // bridge native-thunk window (1 MiB)
    uint32_t span;            // the arena: everything above lies below this
};

// Small fixtures (tests/pe): 32 MiB.
constexpr Layout kTestLayout = {
    "test",
    0x00600000,
    0x00800000, 0x00800000,
    0x01000000, 0x00800000,
    0x01800000, 0x00080000,
    0x01880000, 0x01881000,
    0x01900000, 0x00040000, 0x01b00000,
    0x01c00000, 0x01f00000,
    0x02000000,
};

// Diablo II 1.14d: Game.exe keeps its preferred base (0x00400000, ~5.7 MiB).
// 128 MiB of address space. Only touched pages need to be resident: the
// arena is reserved, and the 3DS build must commit it lazily before this
// layout can run there (see docs/memory-budget.md).
constexpr Layout kGameLayout = {
    "game",
    0x00c00000,
    0x01000000, 0x02000000,   // 32 MiB heap
    0x03000000, 0x04000000,   // 64 MiB VirtualAlloc
    0x07000000, 0x00080000,
    0x07080000, 0x07081000,
    0x07100000, 0x00040000, 0x07500000,
    0x07600000, 0x07f00000,
    0x08000000,
};

// Diablo II on the 3DS: 63 MiB, mapped by the backend at a 16 MiB-aligned
// address above libctru's heap (mman_ctr.c). Azahar gives a .3dsx about 91 MiB
// of application memory; the game app keeps 24 MiB of heap for the host (jump
// tables 12 MiB and JIT code ~8 MiB in town) and 1 MiB of linear memory. Everything small (scratch, PEB/TEBs,
// the main stack, the trap window) sits in the 4 MiB below the image, which a
// single arena backs anyway. Sized from the peaks measured on this layout
// with 64 MiB reported RAM (docs/memory-budget.md).
constexpr Layout kCtrGameLayout = {
    "ctr-game",
    0x00a00000,               // DLLs (1 MiB; none expected for 1.14d), up to the worker stacks
    0x00e00000, 0x01100000,   // 17 MiB heap (peak 16.2 MiB, at hero selection)
    0x01f00000, 0x02000000,   // 32 MiB VirtualAlloc (peak 27.5 MiB in town)
    0x00010000, 0x00070000,   // scratch
    0x00080000, 0x00081000,   // PEB, main TEB
    // Worker stacks grow upward from 0x00b00000 (256 KiB + a 64 KiB gap each,
    // ~9 threads); their TEBs follow at 0x00df0000, which the scheduler
    // treats as the stacks' ceiling and warns about if reached.
    0x00b00000, 0x00040000, 0x00df0000,
    0x00100000, 0x00300000,   // main stack (2 MiB), trap window (1 MiB)
    0x03f00000,
};

// New 3DS hardware, CIA in extended memory mode: the kernel maps at most
// ~95 MiB of heap (its heap window ends at 0x0E000000, docs/memory-budget.md),
// and the arena is carved out of that heap. 75 MiB: VirtualAlloc 44 MiB (the
// sprite cache grows towards its 24 MiB floor; sound buffers come from here
// too), heap 17 MiB (peak 16.2). The host keeps ~20 MiB: it used 14.4 in
// town on the console (jump tables 12), with JIT code in its own .bss pool.
constexpr Layout kCtrGameLayoutN3 = {
    "ctr-game-n3",
    0x00a00000,
    0x00e00000, 0x01100000,   // 17 MiB heap
    0x01f00000, 0x02c00000,   // 44 MiB VirtualAlloc
    0x00010000, 0x00070000,
    0x00080000, 0x00081000,
    0x00b00000, 0x00040000, 0x00df0000,
    0x00100000, 0x00300000,
    0x04b00000,
};

// TEB offsets used by the runtime.
constexpr uint32_t kTebSelf      = 0x18;
constexpr uint32_t kTebPeb       = 0x30;
constexpr uint32_t kTebLastError = 0x34;
constexpr uint32_t kTebTlsSlots  = 0xE10;       // 64 TlsGetValue slots
constexpr uint32_t kTlsSlotCount = 64;

}  // namespace rt

#endif
