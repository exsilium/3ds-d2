// The frame trace: a line per frame the game presents, with what its thread
// did since the frame before (a profiler run's "frames" option, profiler.h:
// logs/frames.trc). The heartbeat's 10 s totals say how many frames were
// slow, not what a slow frame was doing; this does, for each one. Read by
// tools/frame_report.py.
//
// The file is text:
//   # 3ds-d2 frame trace 1                       the format
//   # <anything>                                 a note (a state change), between
//                                                the frames it came between
//   t gap tick run ready host sleep wait io dec dec_us skip who
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
//   dec     sprites decoded, and dec_us the time that took (on the frame thread)
//   skip    frames not shown since: the screen was still drawing the last
//   who     the guest thread that ran longest while it was ready (-1: none);
//           a note "# thread <id> entry <va>" names it the first time
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
