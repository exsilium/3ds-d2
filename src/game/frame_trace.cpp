#include "game/frame_trace.h"

#include "runtime/rt_host.h"

namespace game {
namespace {

FILE *g_file = nullptr;
uint64_t g_start_us = 0;
bool g_have_last = false;
uint32_t g_named = 0;   // threads named in a note (bit per id)
FrameSample g_last;

// rt::frame_thread_us's kinds: run, ready, host, sleep, then the waits
// (critsec, event, fileio, multi, thread, other).
enum { kRun, kReady, kHost, kSleep, kCritsec, kEvent, kFileio, kMulti, kThread, kOther, kKinds };

}  // namespace

void frame_trace_start(FILE *f)
{
    g_file = f;
    g_have_last = false;
    g_named = 0;
    if (g_file)
        std::fputs("# 3ds-d2 frame trace 2\n"
                   "# t gap tick run ready host sleep wait io dec dec_us skip who\n",
                   g_file);
}

void frame_trace_stop() { g_file = nullptr; }

bool frame_trace_on() { return g_file != nullptr; }

void frame_trace_frame(const FrameSample &s)
{
    if (!g_file)
        return;
    if (!g_have_last) {   // the first frame only sets the marks
        g_last = s;
        g_have_last = true;
        g_start_us = s.host_us;
        return;
    }
    uint64_t d[kKinds];
    for (int i = 0; i < kKinds; i++)
        d[i] = s.thread_us[i] - g_last.thread_us[i];
    const uint64_t wait = d[kCritsec] + d[kEvent] + d[kMulti] + d[kThread] + d[kOther];
    // The thread that ran longest while this one was ready.
    int who = -1;
    uint64_t most = 0;
    for (int i = 0; i < 16; i++)
        if (s.ready_by[i] - g_last.ready_by[i] > most) {
            most = s.ready_by[i] - g_last.ready_by[i];
            who = i;
        }
    if (who >= 0 && !(g_named & (1u << who))) {
        g_named |= 1u << who;
        std::fprintf(g_file, "# thread %d entry %08x\n", who, (unsigned)rt::guest_thread_entry((uint32_t)who));
    }
    std::fprintf(g_file, "%llu %llu %u %llu %llu %llu %llu %llu %llu %u %llu %u %d\n",
                 (unsigned long long)((s.host_us - g_start_us) / 1000), (unsigned long long)(s.host_us - g_last.host_us),
                 (unsigned)(s.tick_ms - g_last.tick_ms), (unsigned long long)d[kRun], (unsigned long long)d[kReady],
                 (unsigned long long)d[kHost], (unsigned long long)d[kSleep], (unsigned long long)wait,
                 (unsigned long long)d[kFileio], (unsigned)(s.decodes - g_last.decodes),
                 (unsigned long long)(s.decode_us - g_last.decode_us), (unsigned)(s.skipped - g_last.skipped), who);
    g_last = s;
}

void frame_trace_note(const char *text)
{
    if (g_file)
        std::fprintf(g_file, "# %s\n", text);
}

}  // namespace game
