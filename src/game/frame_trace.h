// The frame trace: a line per frame the game presents, with what its thread
// did since the frame before (a profiler run's "frames" option, profiler.h:
// logs/frames.trc). The heartbeat's 10 s totals say how many frames were
// slow, not what a slow frame was doing; this does, for each one. Read by
// tools/frame_report.py.
//
// The file is text:
//   # 3ds-d2 frame trace 4                       the format
//   # <anything>                                 a note (a state change), between
//                                                the frames it came between
//   t gap tick run ready host sleep wait io dec dec_us skip who cs ev wsite wown wus wfrom sl stall
// one line per frame, each field the change since the line before:
//   t       the host clock at the present, ms since the trace started
//   gap     us since the last present
//   tick    ms the game's clock (GetTickCount) moved meanwhile
//   run     us the frame thread ran guest code
//   ready   us it was ready while another guest thread ran
//   host    us the host's own work held it (the heartbeat, the service call)
//   sleep   us it slept (Sleep)
//   wait    us it waited on a lock, an event or a thread
//   io      us it waited on a file read
//   dec     sprites the frame thread decoded, and dec_us the time that took
//           (Storm's read thread decodes too: its time is the frames' ready)
//   skip    frames not shown since: the screen was still drawing the last
//   who     the guest thread that ran longest while it was ready (-1: none);
//           a note "# thread <id> entry <va>" names it the first time
//   cs, ev  of wait: us on a critical section, and on an event
//   wsite   the game's code it waited from longest (hex, 0: no wait), wown
//           the thread that held that critical section (0: none or an
//           event), wus the us of that one wait, wfrom the game's calls it
//           waited under (up to 4 return addresses on its EBP chain, hex,
//           comma-separated; -)
//   sl      the frame thread's scheduler slices: many in a long frame, its
//           guest code ran long; few, a host call held it
//   stall   the frame thread held 150 ms or more in one slice (a host call or
//           a guest loop): its EIP and the first three dwords on its stack,
//           hex, as eip/a,b,c (- if not); in a shim or a native, EIP is its
//           trap and a the game's call site
// (version 3 had no sl or stall, version 2 no cs..wfrom, version 1 no who either)
//
// Platform-independent: the platform samples, opens and closes the file.
#ifndef D2_GAME_FRAME_TRACE_H
#define D2_GAME_FRAME_TRACE_H

#include <cstdint>
#include <cstdio>

namespace game {

struct FrameSample {
    uint64_t host_us = 0;        // the present's host clock
    uint32_t tick_ms = 0;        // the game's clock (rt::tick_ms)
    uint64_t thread_us[10] = {}; // the frame thread's time by kind since the start (rt::frame_thread_us)
    uint32_t decodes = 0;        // sprites decoded since the start, and their time (us)
    uint64_t decode_us = 0;
    uint32_t skipped = 0;        // frames not shown since the start
    uint64_t ready_by[16] = {};  // its ready time by the thread that ran (rt::frame_thread_ready_by)
    uint64_t wait_us = 0;        // its longest wait since the last frame (rt::frame_thread_wait_top)
    uint32_t wait_site = 0, wait_owner = 0;
    uint32_t wait_from[4] = {};   // rt::kWaitFrom
    uint32_t slices = 0;          // rt::frame_thread_slices
    bool stalled = false;         // rt::take_stall: the frame thread held 150 ms in one slice
    uint32_t stall[4] = {};       // ...its EIP and the first three dwords on its stack
};

// Starts writing to `f` (the caller keeps it, and closes it after stop).
void frame_trace_start(FILE *f);
void frame_trace_stop();
bool frame_trace_on();
// One frame, at its present.
void frame_trace_frame(const FrameSample &s);
// A "# " line, after the last frame's.
void frame_trace_note(const char *text);

}  // namespace game

#endif
