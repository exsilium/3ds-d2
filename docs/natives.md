# Natives: game routines in native code

A native replaces one of Game.exe's routines, or one hot loop inside one, with C++ that runs directly on the 3DS's ARM11. Box86 translates the rest of the game.

There are 49 native sites. They are about 0.6% of Game.exe's code but take an estimated 35–45% of the game thread's time. Most live in [src/game/d2_native.cpp](../src/game/d2_native.cpp); the sprite cache and memory natives are in [src/game/d2_memory.cpp](../src/game/d2_memory.cpp), and the DCC decoder in [src/game/dcc_native.cpp](../src/game/dcc_native.cpp).

This page covers how natives are installed, how they are checked against the game's own code, how to measure them, and when one is worth writing.

## When a native pays

- **Every native call has a fixed cost:** leaving translated code and coming back.
  - The first natives replaced routines that each took 2–14% of the game thread: sprite blitters, the light stamps, decoders.
  - #92 found the remaining profile flat. What's left is thousands of small functions, each a few percent at most, and for a 5–10 instruction function the call costs more than the game's own code.
- **So the next natives should replace whole call trees**, such as a monster's AI tick, a path search or a strike (#110), not single small functions.
- **Find the cost first, then write the native.** See [Measuring](#measuring).

## The kinds of native

### 1. A whole function: `rt::hook_function`

```cpp
static const uint8_t k625480[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x08, 0x85, 0xc0, 0x75, 0x04};   // the original's first bytes
HOOK(c, 0x00625480, k625480, sizeof k625480, 3, remap_check ? stat_get_checked : stat_get_native, "StatGet", true);
```

`rt::hook_function(cpu, va, expect, n, stack_args, fn, name, in_line)` ([src/runtime/rt_host.h](../src/runtime/rt_host.h)):
- **It checks** that the function starts with `expect`, so any other Game.exe is left alone.
- **It registers** `fn` as a shim, and writes a 5-byte `jmp` to the shim over the function's start.
- **What the shim sees:** the guest's registers and stack arrive exactly as the original would see them, `ecx`/`edx` included.
  - `c.arg(i)` reads the i-th stack argument, `c.reg(R_EAX)` a register.
  - The return value becomes `eax`.
  - `stack_args` dwords are popped on return, like the original's `ret n`. Use 0 for cdecl.
- **`in_line = true`** serves the shim from inside the translated code without leaving the dynarec loop, which saves most of the call's fixed cost.
  - Use it only for natives that never call back into the game (`rt::guest_call`) and never block.
  - `FloorTile` and `LightBlock` call back, so they aren't in line.
- **The `HOOK` macro** in `install_d2_native` adds a bit to `D2_NATIVE_MASK` (see [Checking](#checking-against-the-games-own-code)).
  - Add new hooks **at the end** of the list, so earlier hooks keep their bits. The mask is 64 bits; it was 32 and silently skipped every hook past the 32nd.
- **Game memory** is read and written through the guest-to-host helpers in `d2_native.cpp`: `H(va)` (a host pointer), `U32(va)`, `I32(va)`, `put32(va, v)`. The game's structures keep their x86 layout: the 3DS is 32-bit and little-endian like x86.

### 2. A hot loop inside a function: in-line sites

Some routines spend their time in one loop over mostly empty entries, such as the 1024-slot table, the cell grid, the floor records, the walls' cells, or the units under the cursor. Replacing the whole function would copy far more than needed. Instead (`install_slot_steps`):
- The loop's own bytes are checked against the original, as above.
- `rt::native_entry(name, 0, fn)` registers the native and returns its trap address, and `c.set_inline_shim(trap)` lets the translated code serve it in line.
- A `call trap` is written over the loop's step, padded with `nop`s, and `c.invalidate_code(va, n)` drops the old translation.
- The native skips the empty entries and leaves the registers as the game's loop would have at its next live entry.

### 3. A call redirected: `redirect_call`

To run native code around a call without replacing either side, the call's target is changed to a native that does its work and then goes on to the original target. Examples are the floor queue's arm and flush points around the world's floor pass (`install_floor_queue`).

### 4. Intrinsics in the backend

The game's `EnterCriticalSection`/`LeaveCriticalSection` and `GetTickCount`/`timeGetTime` are served from the backend's in-line call path without the runtime's lock when uncontended (#39, #48). These are backend patches in `tests/cpu/patches/armv6/`, not `HOOK`s.

## Handing a call back to the game

A native may handle the common case and hand the rest back to the game's code. It does that with `resume_original(c, va, esp, stack_args)`, which sets ESP and tells the bridge to continue at `va` instead of returning.

**The hook's `jmp` overwrote the function's first 5 bytes.** So `va` must be the first whole instruction **after** those bytes, and the native must first do what the skipped instructions did. For example, `UnitPickFilter` (0x466870):

```cpp
// push ebp; mov ebp, esp; sub esp, 0x34 done here (the jmp covers 466870-466874), on at 466876.
const uint32_t esp = c.reg(R_ESP);
put32(esp - 4, c.reg(R_EBP));      // push ebp
c.set_reg(R_EBP, esp - 4);         // mov ebp, esp
resume_original(c, 0x00466876, esp - 4 - 0x34, 2);   // sub esp, 0x34; 2 stack args
return c.reg(R_EAX);
```

Resuming at the function's start would run straight into the hook's `jmp` again. Resuming inside the `jmp`'s bytes would crash.

**Hand back before writing anything.** A native that hands a call back must not have changed game memory first. `UnitWalk` and `PathSearch` plan without side effects, and give the whole call to the game's code for the cases they don't cover: size classes other than 1, 3 and 5, or a room's edge.

## Checking against the game's own code

Every native has a check mode on the m4 lane: `D2_NATIVE_CHECK=1`. For each call it:
1. runs the native and keeps its result: `eax`, and the bytes it wrote;
2. puts memory back as it was;
3. runs the game's own routine from a **trampoline**: a copy of the original's first whole instructions (5 bytes or more), followed by a `jmp` back into the original past them;
4. compares the two results. The game's result is the one kept, and differences are logged as `native check: <name> <calls> calls, <n> differ`.

The table is `g_remap_check` (name, address, and how many whole-instruction bytes the trampoline copies). `install_remap_check` builds the trampolines before the hooks go in. A new checked native needs:
- an entry at the end of the table (and the table's size raised);
- a `*_checked` wrapper that uses `rt::guest_call(c, k.tramp, {args})`;
- `remap_check ? x_checked : x` in its `HOOK`.

Checked wrappers call back into the game, so they are never served in line.

**Run it:**

```sh
D2_NATIVE_CHECK=1 M4_SCRIPT=tests/m4/scripts/fight.txt scripts/docker.sh test m4 <game-dir> <guest-ms>
grep -r "native check" build/m4/
```

**Narrow it down:**
- `D2_NATIVE_MASK=<bits>` turns hooks on and off (bit k = the k-th `HOOK`), to find which native changes a result.
- `M4_FRAMEHASH=<file>` writes one hash per presented frame. The same script must give the same hashes with and without a native that is meant to be exact.
- The scripts in [tests/m4/scripts/](../tests/m4/scripts/) cover the Act I town walk, a fight, the Cold Plains, area loads, and Act II's town, oasis and the way east.

The natives in `g_remap_check`, and the sprite cache's and the locked lookups' own checks, all ran with no difference before they were merged. Their call counts are in their PRs and in [hardware-test.md](hardware-test.md). Planned additions: d2decomped's emulator-made cases (#108), and generated cases for every native (#109).

## Measuring

- **The lane's exact block counter shows where the game's own code spends its time:** `WX86_HOTPROF=1 D2_NOLINK=1`, with the console's video settings (`M4_FAST=1`) and decoder (`D2_DCC=1`).
  - It counts executed blocks times their sizes, rolled up into functions: `python3 -I tools/hot_report.py build/m4/err.txt dist/game/Game.exe`.
  - `M4_HOTPROF_LEVEL=<level id>` starts the count once that level is reached.
  - The total ("4.52 billion bytes of x86 for the same 2,507 frames") is the before/after measure for a native.
- **The timer profiles can't find hot translated code.** Both the console's guest code profile and the lane's `prof eip` sample where a thread is at its next block boundary. Chained translated blocks never stop at one, but every return from a native does, so translated time is charged to the native that comes next. `SpriteCacheTouch` once looked like 12% of a crowd frame; its own timing is 3–5%.
- **Names.** The profile tools name functions where a name is known (`tools/game_names.py`, #107): a native's from the `HOOK` list in `d2_native.cpp`, then d2decomped's (`third_party/d2d`'s `names.tsv`, then Ghidra's in its `game-functions.tsv`), as `0x67b850` (PathSearch). Without the submodule they print bare addresses.
  - `tools/function_starts.py dist/game/Game.exe` compares our function starts with d2d's list, and says how the binary reaches the starts only d2d has.
- **On the console,** a profiler run with **guest code profile** ticked gives each native's time per frame:
  - `tools/profile_report.py --windows fights|quiet --routines <Name>,0x<addr>`;
  - add the new native's name to `NATIVES` in `tools/profile_report.py`.
  - Post the figures on the issue.

## A new native, step by step

1. **Find it** with the lane's block counter, then read the routine: its arguments, register use and side effects. d2decomped's code and notes often describe it already (#105). Cite the address in comments.
2. **Write it** on the game's structures, with a hand-back for what it doesn't cover.
3. **Hook it** at the end of the `HOOK` list, with its first bytes. Use `in_line` if it never calls back or blocks.
4. **Add its check** (`g_remap_check`, `*_checked`). Run `D2_NATIVE_CHECK=1` over the scripts that reach it, until it has many calls and no difference.
5. **Measure it:** the lane's bytes before and after, then on the console if it is large. Add its name to `tools/profile_report.py`.
6. **Document it:** a line in the PR with its checked call count and measurements, and the figures as a comment on the issue.
