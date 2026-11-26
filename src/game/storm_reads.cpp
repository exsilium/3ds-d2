// See storm_reads.h. Storm's SFileReadFile (0x41aad0) on an archive opened for
// asynchronous reads (its +0x114 -> +0x110 set) does not read itself: it
// queues the read with Storm's read thread (0x41a550, started at 0x41a8ae)
// through 0x419790, one request per sector run (the loop at 0x41ae50), and
// a caller without OVERLAPPED then leaves Storm's lock and waits on an event
// of its own (0x41af07) until the thread has read it.
//
// The thread picks its next request with 0x41a450: of the queued requests,
// the one nearest the archive's last position, and the one whose deadline
// (+0x18: GetTickCount() at queuing plus 0x419790's argument at [ebp+0x24])
// is nearest. The nearest deadline is taken if it is at most 1,000 ms away
// (0x41a600), else the nearest position. Every request from 0x41ae50 is
// queued with 0x7fffffff (push 0x7fffffff at 0x41aeb2), so the deadline
// never decides for them: a waiting caller's small read (the drawing
// thread's, 0x5fe990 reading a DCC file's header when a new monster
// appears) is served only once it is the nearest, behind the game's
// asynchronous loads of whole DCC files queued before it. Meanwhile the
// drawing thread draws nothing: in 3DS-D2-B, the slow windows that were not
// area loads left core 0 idle for 1.4 to 4.3 s of their 10 s, while the
// drawing thread waited on no read of its own.
//
// The stub gives a read without OVERLAPPED ([ebp+0x18] 0) the deadline now
// (an argument of 0), so it is taken next; the game's asynchronous loads
// keep theirs. The flags are not live at 0x41aeb2 (pushes and a mov until
// the call). In .text's slack after the sprite decode timer's:
//   S+0   cmp dword [ebp+0x18], 0; jne S+13
//   S+6   push 0; jmp 0x41aeb7
//   S+13  push 0x7fffffff; jmp 0x41aeb7
//
// Direct reads (install_storm_direct_reads). SFileReadFile holds Storm's
// lock (0x779068) throughout; on an asynchronous archive (its +0x114 ->
// +0x110 set, the check at 0x41ace2) it queues the read and leaves the lock
// to wait (above); else it reads itself, 0x417040 under the lock (0x41acef).
// The read thread takes a request under the same lock, leaves it (0x41a677),
// reads with the same 0x417040 (or 0x415560 for a loose file), and only then
// runs the request's callback (from 0x41a725 on), which for the game's
// asynchronous loads decodes the sprite group just read (0x5ff6e0 ->
// 0x60bff0, guest code, 30-60 ms in Azahar). A waiting caller's read, taken
// next, waited for all of that.
//
// The read thread marks its read: a flag set before it leaves the lock
// (0x41a66c) and cleared once the read is done (0x41a725, where every path
// after the read meets). A caller with the lock then reads itself when the
// flag is clear and it has no OVERLAPPED: the read thread is not in 0x417040
// and cannot enter it before the caller leaves the lock, so the archive's
// handle and position are the caller's alone meanwhile, as on a synchronous
// archive. A caller that finds the flag set queues, as before: waiting for
// the read thread's read instead (sleeping 1 ms between checks, the lock
// held) made the frames worse in Azahar, 15.6 and 22.3% past 50 ms against
// 8.3 and 11.3% without direct reads. The check at 0x41ace2 (cmp [edx+0x110],0; jne 0x41adf3, 13 bytes)
// becomes call <native>; test ecx,ecx; jne 0x41adf3; the native leaves EAX
// (the size, pushed at 0x41acef) as it was and sets ECX: 1 queues.
//   F       the flag: a dword of guest scratch memory (wx86_scratch_alloc), not
//           .text: the read thread writes it, and a write to a page of code
//           would throw that page's translations away
//   P       the data dword with F's address (0x6cb7dc, in .text's slack), so
//           the stubs' bytes are the same on every run (native_sites_table.h)
//   A       mov ebx,[P]; mov byte [ebx],1; mov ebx,[0x6cc214]; jmp 0x41a672
//   B       push eax; mov eax,[P]; mov byte [eax],0; pop eax;
//           cmp dword [esi+0x28],0; je 0x41a763; jmp 0x41a72b
#include "game/storm_reads.h"

#include "game/native_sites.h"
#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"
#include "runtime/rt_host.h"

#include <cstring>

namespace game {
namespace {

constexpr uint32_t kDeadline = 0x0041aeb2;
constexpr uint32_t kAsyncCheck = 0x0041ace2;
constexpr uint32_t kFlagAt = 0x006cb7dc;   // the data dword with F's address, in .text's slack
uint32_t g_direct = 0, g_queued = 0;
uint32_t g_flag = 0;   // F's guest address

}  // namespace

void storm_direct_counts(uint32_t &direct, uint32_t &queued)
{
    direct = g_direct;
    queued = g_queued;
}

bool install_storm_direct_reads(d2rt::Cpu &c)
{
    g_flag = wx86_scratch_alloc(4);
    if (!g_flag)
        return false;
    const uint32_t zero = 0;
    c.write(g_flag, &zero, 4);
    const uint32_t decide = rt::native_entry("StormDirectRead", 0, [](d2rt::Cpu &cpu) -> uint32_t {
        const uint32_t archive = cpu.reg(d2rt::R_EDX), ebp = cpu.reg(d2rt::R_EBP);
        uint32_t queue = cpu.read_u32(archive + 0x110) != 0;   // the original test: an asynchronous archive
        if (queue && cpu.read_u32(ebp + 0x18) == 0) {         // ...and no OVERLAPPED
            uint8_t reading = 0;
            cpu.read(g_flag, &reading, 1);
            queue = reading != 0;
            (queue ? g_queued : g_direct)++;
            if (g_direct + g_queued == 1 || ((g_direct + g_queued) & 0x3ff) == 0)   // the lane's sign of it
                rt::logf("storm reads: %u read by their caller, %u queued", (unsigned)g_direct, (unsigned)g_queued);
        }
        cpu.set_reg(d2rt::R_ECX, queue);
        return cpu.reg(d2rt::R_EAX);   // the size, pushed next
    });
    if (!decide)
        return false;
    // The stubs, the jumps to them and the check: native_sites_table.h's
    // storm_direct rows, which the AOT recompiler reads too (issue #138). The
    // stubs read the flag's address from the data dword at kFlagAt:
    //   A  mov ebx,[kFlagAt]; mov byte [ebx],1; mov ebx,[0x6cc214]; jmp 0x41a672
    //   B  push eax; mov eax,[kFlagAt]; mov byte [eax],0; pop eax;
    //      cmp dword [esi+0x28],0; je 0x41a763; jmp 0x41a72b
    uint32_t slot = 0;
    if (!c.read(kFlagAt, &slot, 4) || slot != 0)
        return false;
    c.write(kFlagAt, &g_flag, 4);
    if (!apply_sites(c, "storm_direct", {{"StormDirectRead", decide}})) {
        c.write(kFlagAt, &zero, 4);
        return false;
    }
    rt::logf("storm reads: a waiting caller reads itself while the read thread is not reading, at %08x", kAsyncCheck);
    return true;
}

bool install_storm_read_priority(d2rt::Cpu &c)
{
    // native_sites_table.h's storm_priority rows (issue #138).
    if (!apply_sites(c, "storm_priority", {}))
        return false;
    rt::logf("storm reads: a waiting caller's read first, at %08x", kDeadline);
    return true;
}

uint32_t g_storm_rate = 0;

bool set_storm_read_rate(d2rt::Cpu &c, uint32_t bytes_per_s)
{
    // 0x40a3b8: push 0x40000; call 0x4164a0 (Storm's, which keeps its
    // argument at 0x77904c, ret 4). The call goes to a native that keeps
    // the run's rate there: native_sites_table.h's storm_rate row.
    constexpr uint32_t kRateAt = 0x0077904c;
    g_storm_rate = bytes_per_s ? bytes_per_s : kStormReadRate;
    const uint32_t native = rt::native_entry("StormRate", 1, [](d2rt::Cpu &c) -> uint32_t {
        c.write_u32(kRateAt, g_storm_rate);
        return g_storm_rate;
    });
    if (!native || !apply_sites(c, "storm_rate", {{"StormRate", native}}))
        return false;
    rt::logf("storm reads: the read thread's rate limit %u KiB/s (the game's 256), at %08x", (unsigned)(g_storm_rate >> 10),
             0x0040a3bdu);
    return true;
}

}  // namespace game
