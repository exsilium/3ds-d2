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
#include "game/storm_reads.h"

#include "runtime/cpu.h"
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

}  // namespace

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
