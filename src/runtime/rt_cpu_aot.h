// CpuAot (issue #138, docs/aot.md "Threads that wait in place"): a Cpu backend
// with no Box86. The game's code is the AOT recompiler's C; each guest thread
// runs it on a host stack of its own (a fiber), so a shim that waits stops
// the thread where it is. What it can't run (code that isn't compiled) stops
// the run with a report: the strict lane.
#pragma once

#include "runtime/cpu.h"

#include <string>
#include <vector>

namespace d2rt {

// The backend (one a process), or nullptr where fibers aren't available.
Cpu *make_cpu_aot();
// Whether `c` is it.
bool cpu_is_aot(Cpu &c);

// The code at `eip` run until control is at `until` (0: until a trap stops
// the run): the game layer's (aot_spike.cpp), which has the compiled
// functions. False when a trap stopped the run (the thread's sentinel).
using CpuAotDispatch = bool (*)(Cpu &c, uint32_t eip, uint32_t until);
void cpu_aot_set_dispatch(CpuAotDispatch fn);

// The trap at `va` (the Bridge's: a shim's slot, the sentinel), with the
// registers as they are in `c`. False when it stops the run.
bool cpu_aot_trap(Cpu &c, uint32_t va);
// Whether `va` is in the trap range.
bool cpu_aot_is_trap(Cpu &c, uint32_t va);
// The running thread stops here until the scheduler runs it again (a yield or
// a wait the trap just asked for); the registers are `c`'s on both sides.
void cpu_aot_yield(Cpu &c);
// Where the threads' host stacks come from and go back to (8-byte aligned);
// set before a thread runs. Not set: the heap (the lane maps its own).
void cpu_aot_set_stack_alloc(void *(*alloc)(size_t), void (*release)(void *));
// The running thread's x87 state (416 bytes, aot_rt.h's AotFpu), with its
// xmm0..7 after it (128 bytes, AotXmm).
void *cpu_aot_fpu(Cpu &c);
// Code changed since this was last asked (a native's hook): true once.
bool cpu_aot_take_code_changed(Cpu &c);
// The image's span: invalidate_code outside it changes no code (the heap reports its copies).
void cpu_aot_set_code_span(Cpu &c, uint32_t lo, uint32_t hi);
// After a trap that asked for no yield: whether the running thread's slice is
// over (its share of trap calls made). True once; cpu_aot_yield then.
bool cpu_aot_slice_over(Cpu &c);
// The in-line intrinsic set for the trap at `va` (Cpu::set_intrinsic_nogil),
// or null. True from it: the call is done, eax and the bytes its ret pops in
// the frame; false: the trap as ever. A call it serves still counts for the
// slice (cpu_aot_slice_over).
Cpu::IntrinNoGilFn cpu_aot_intrinsic(Cpu &c, uint32_t va);
// Whether code changed and cpu_aot_take_code_changed hasn't been asked yet.
bool cpu_aot_code_changed(Cpu &c);
// The most any live thread has used of its host stack, and a stack's size,
// in bytes (INFO's line). It reads each stack's unused part: not every frame.
void cpu_aot_stack(uint32_t *peak, uint32_t *size);
// Each live thread's host stack and the most of it used ("cpu_aot: ..."),
// for the lane's end; empty without CpuAot.
std::vector<std::string> cpu_aot_lines();
// The run can't go on: `what` logged with the address. On the 3DS, from a
// thread's fiber, the thread stops there for good and the scheduler is asked
// to shut down (exit code 86), so the host's run returns and the app ends as
// it does after any game; elsewhere the process ends.
[[noreturn]] void cpu_aot_stop(const char *what, uint32_t at);
// What stopped the run ("<what> <address>"), or null.
const char *cpu_aot_stop_text();
// A test: the running thread's next trap call is a stop.
void cpu_aot_stop_test();

}  // namespace d2rt
