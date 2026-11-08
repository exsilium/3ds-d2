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
//   A       mov byte [F],1; mov ebx,[0x6cc214]; jmp 0x41a672
//   B       mov byte [F],0; cmp dword [esi+0x28],0; je 0x41a763; jmp 0x41a72b
#include "game/storm_reads.h"

#include "runtime/cpu.h"
#include "runtime/guest_scratch.h"
#include "runtime/rt_host.h"

#include <cstring>

namespace game {
namespace {

constexpr uint32_t kDeadline = 0x0041aeb2, kDeadlineNext = 0x0041aeb7;
constexpr uint8_t kDeadlineOrig[] = {0x68, 0xff, 0xff, 0xff, 0x7f};
constexpr uint32_t kStub = 0x006cb780, kStubEnd = 0x006cb7a0;

void put_rel32(uint8_t *at, uint32_t insn_end, uint32_t target)
{
    const uint32_t rel = target - insn_end;
    std::memcpy(at, &rel, 4);
}

constexpr uint32_t kDirectA = 0x006cb7a0, kDirectB = 0x006cb7b8,
                   kDirectEnd = 0x006cb7e0;   // kDirectB + 22 <= the end; d2_native's checks follow
constexpr uint32_t kTakeAt = 0x0041a66c, kTakeNext = 0x0041a672, kReadDone = 0x0041a725,
                   kReadDoneNext = 0x0041a72b, kNoCallback = 0x0041a763;
constexpr uint32_t kAsyncCheck = 0x0041ace2, kQueuePath = 0x0041adf3;
constexpr uint8_t kTakeOrig[] = {0x8b, 0x1d, 0x14, 0xc2, 0x6c, 0x00};          // mov ebx,[0x6cc214]
constexpr uint8_t kReadDoneOrig[] = {0x83, 0x7e, 0x28, 0x00, 0x74, 0x38};      // cmp [esi+0x28],0; je 0x41a763
constexpr uint8_t kAsyncOrig[] = {0x83, 0xba, 0x10, 0x01, 0x00, 0x00, 0x00,     // cmp dword [edx+0x110],0
                                  0x0f, 0x85, 0x04, 0x01, 0x00, 0x00};          // jne 0x41adf3
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
    uint8_t take[sizeof kTakeOrig], done[sizeof kReadDoneOrig], check[sizeof kAsyncOrig];
    uint8_t slack[kDirectEnd - kDirectA];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kTakeAt, take, sizeof take) || std::memcmp(take, kTakeOrig, sizeof take) != 0 ||
        !c.read(kReadDone, done, sizeof done) || std::memcmp(done, kReadDoneOrig, sizeof done) != 0 ||
        !c.read(kAsyncCheck, check, sizeof check) || std::memcmp(check, kAsyncOrig, sizeof check) != 0 ||
        !c.read(kDirectA, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
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
    // A: the flag set, the instruction it replaces, back.
    uint8_t a[18] = {0xc6, 0x05, 0, 0, 0, 0, 0x01};
    std::memcpy(a + 2, &g_flag, 4);
    std::memcpy(a + 7, kTakeOrig, sizeof kTakeOrig);
    a[13] = 0xe9;
    put_rel32(a + 14, kDirectA + 18, kTakeNext);
    // B: the flag cleared, the two instructions it replaces, back.
    uint8_t b[22] = {0xc6, 0x05, 0, 0, 0, 0, 0x00, 0x83, 0x7e, 0x28, 0x00, 0x0f, 0x84};
    std::memcpy(b + 2, &g_flag, 4);
    put_rel32(b + 13, kDirectB + 17, kNoCallback);
    b[17] = 0xe9;
    put_rel32(b + 18, kDirectB + 22, kReadDoneNext);
    static_assert(kDirectB + 22 <= kDirectEnd, "B fits");
    c.write(kDirectA, a, sizeof a);
    c.write(kDirectB, b, sizeof b);
    // The jumps to them, and the check.
    uint8_t ja[sizeof kTakeOrig] = {0xe9, 0, 0, 0, 0, 0x90};
    put_rel32(ja + 1, kTakeAt + 5, kDirectA);
    uint8_t jb[sizeof kReadDoneOrig] = {0xe9, 0, 0, 0, 0, 0x90};
    put_rel32(jb + 1, kReadDone + 5, kDirectB);
    uint8_t chk[sizeof kAsyncOrig] = {0xe8, 0, 0, 0, 0, 0x85, 0xc9, 0x0f, 0x85};
    put_rel32(chk + 1, kAsyncCheck + 5, decide);
    put_rel32(chk + 9, kAsyncCheck + 13, kQueuePath);
    c.write(kTakeAt, ja, sizeof ja);
    c.write(kReadDone, jb, sizeof jb);
    c.write(kAsyncCheck, chk, sizeof chk);
    c.invalidate_code(kDirectA, kDirectEnd - kDirectA);
    c.invalidate_code(kTakeAt, sizeof ja);
    c.invalidate_code(kReadDone, sizeof jb);
    c.invalidate_code(kAsyncCheck, sizeof chk);
    rt::logf("storm reads: a waiting caller reads itself while the read thread is not reading, at %08x", kAsyncCheck);
    return true;
}

bool install_storm_read_priority(d2rt::Cpu &c)
{
    uint8_t have[sizeof kDeadlineOrig];
    uint8_t slack[kStubEnd - kStub];
    static const uint8_t kZero[sizeof slack] = {};
    if (!c.read(kDeadline, have, sizeof have) || std::memcmp(have, kDeadlineOrig, sizeof have) != 0 ||
        !c.read(kStub, slack, sizeof slack) || std::memcmp(slack, kZero, sizeof slack) != 0)
        return false;
    uint8_t s[23] = {0x83, 0x7d, 0x18, 0x00, 0x75, 13 - 6, 0x6a, 0x00, 0xe9};
    put_rel32(s + 9, kStub + 13, kDeadlineNext);
    std::memcpy(s + 13, kDeadlineOrig, sizeof kDeadlineOrig);
    s[18] = 0xe9;
    put_rel32(s + 19, kStub + 23, kDeadlineNext);
    uint8_t jump[sizeof kDeadlineOrig] = {0xe9};
    put_rel32(jump + 1, kDeadline + 5, kStub);
    c.write(kStub, s, sizeof s);
    c.write(kDeadline, jump, sizeof jump);
    c.invalidate_code(kStub, sizeof s);
    c.invalidate_code(kDeadline, sizeof jump);
    rt::logf("storm reads: a waiting caller's read first, at %08x", kDeadline);
    return true;
}

}  // namespace game
