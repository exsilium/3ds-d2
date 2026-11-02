// src/game/report.cpp on game.log excerpts: a closed session, a crashed one,
// a v0.3.2 log, the play figures, the DSN and the envelope's framing.
// Run through `scripts/docker.sh test report`.
#include "game/report.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace game::report;

static int g_fail = 0;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

static LogSummary feed(const char *log)
{
    LogSummary s;
    std::string all(log);
    size_t at = 0;
    while (at < all.size()) {
        size_t nl = all.find('\n', at);
        if (nl == std::string::npos)
            nl = all.size();
        s.feed(all.substr(at, nl - at + 1).c_str());
        at = nl + 1;
    }
    return s;
}

static const char kPlay[] =
    "3ds-d2: Diablo II on the 3DS\n"
    "session: start 1793000000 build 0123456789ab\n"
    "[rt] game state: menus (ui 0)\n"
    "[rt] prof: 10000 ms, frames 250, idle 10 ms, switches 900, cache syncs 3 (40 us)\n"
    "[rt] t=10s peeks=1 files=2 read=3KiB frames=250 heap=100/200KiB va=300/400KiB free=5000KiB jit=10 meta=1 jmptbl=2KiB host=900KiB\n"
    "[rt] prof hitch: frames 250, <=50ms 240, <=100ms 4, <=250ms 5, >250ms 1, longest 900 ms, sprites in 0 out 0, "
    "wait 0 ms, ready 120 ms, host 310 ms, heartbeat 12 ms, skipped 0, famine 0\n"
    "[rt] game state: in game (ui 1)\n"
    "profiler run: start sprites,guest\n"
    "[rt] prof: 10000 ms, frames 200, idle 0 ms, switches 900, cache syncs 3 (40 us)\n"
    "[rt] t=20s peeks=1 files=2 read=3KiB frames=450 heap=150/250KiB va=300/600KiB free=700KiB jit=30 meta=1 jmptbl=2KiB host=1200KiB\n"
    "[rt] prof hitch: frames 200, <=50ms 190, <=100ms 7, <=250ms 2, >250ms 1, longest 412 ms, sprites in 120 out 98, "
    "wait 830 ms, ready 200 ms, host 280 ms, heartbeat 15 ms, skipped 3, famine 0\n"
    "profiler run: stop\n"
    "profiler run: start guest,files\n"
    "[rt] sprite frames 5000/6400 KiB in 60 blocks, 700 units asking 4700 KiB, blocks taken 80 given 20, "
    "over 256 KiB 2, to the pool 0\n"
    "[rt] sprite frames 7000/7300 KiB in 70 blocks, 900 units asking 6600 KiB, blocks taken 95 given 25, "
    "over 256 KiB 3, to the pool 1\n"
    "[rt] prof: 10000 ms, frames 100, idle 0 ms, switches 900, cache syncs 3 (40 us)\n"
    "[rt] prof hitch: frames 100, <=50ms 95, <=100ms 0, <=250ms 5, >250ms 0, longest 180 ms, sprites in 0 out 0, "
    "wait 0 ms, ready 90 ms, host 300 ms, heartbeat 11 ms, skipped 0, famine 2\n"
    "[rt] t=30s peeks=1 files=2 read=3KiB frames=550 heap=150/250KiB va=300/600KiB free=900KiB jit=20 meta=1 jmptbl=2KiB host=1100KiB\n"
    "[rt] prof: 10000 ms, frames 0, idle 10000 ms, switches 0, cache syncs 0 (0 us)\n"
    "[rt] prof: 10000 ms, frames 150, idle 0 ms, switches 900, cache syncs 3 (40 us)\n";

int main()
{
    {   // A session that closed: nothing to report.
        const std::string log = std::string(kPlay) + "Game ended: exited=1 code=0 \nsession: closed\n";
        LogSummary s = feed(log.c_str());
        CHECK(s.started());
        CHECK(s.closed());
        CHECK(s.start_time() == 1793000000ull);
        CHECK(s.build() == "0123456789ab");
        CHECK(s.version().empty());   // a start line from before the version was on it
        LogSummary v = feed("session: start 1793000000 build 0123456789ab version 0.4.0\n");
        CHECK(v.build() == "0123456789ab" && v.version() == "0.4.0");
        CHECK(s.crash_lines().empty());
        const LogSummary::Figures f = s.figures();
        CHECK(f.seconds == 30);
        CHECK(f.play_windows == 3);   // the menu window and the suspend are not play
        CHECK(std::fabs(f.fps_median - 15.0f) < 0.01f);
        CHECK(std::fabs(f.fps_p10 - 10.0f) < 0.01f);
        CHECK(f.pool_free_min_kib == 700);
        CHECK(f.heap_peak_kib == 250 && f.va_peak_kib == 600 && f.host_peak_kib == 1200 && f.jit_kib == 30);
        // Hitches in play only: the menu window's 900 ms frame is left out.
        CHECK(f.hitches_100ms == 8 && f.longest_frame_ms == 412);
        // Two profiler runs: their options once each, in order.
        CHECK(s.profiled() == "sprites,guest,files");
        CHECK(feed("session: start 1 build x\n").profiled().empty());
        Event e;
        e.figures = f;
        e.have_figures = true;
        const std::string j = event_json(e);
        CHECK(j.find("\"hitches_100ms_play\":8,\"longest_frame_ms_play\":412") != std::string::npos);
        // The frames' blocks: the peak held, the most partly used room, the last counts.
        CHECK(f.frames_held_peak_kib == 7300 && f.frames_unused_peak_kib == 1400);
        CHECK(f.frames_over_256k == 3 && f.frames_to_pool == 1);
        CHECK(j.find("\"sprite_frames_held_peak_kib\":7300,\"sprite_frames_unused_peak_kib\":1400,"
                     "\"sprite_frames_over_256k\":3,\"sprite_frames_to_pool\":1}") != std::string::npos);
        Event none;
        none.have_figures = true;
        CHECK(event_json(none).find("sprite_frames") == std::string::npos);   // a build without them
        CHECK(event_json(none).find("heap_refused") == std::string::npos);    // nor the system lines
    }
    {   // The system lines (issue #24): the lowest free and room, the last refusal count.
        const char *sys =
            "[rt] system: free KiB app 440 sys 81592 base 11588 linear 990 heap 3743, heap room 1900 low 1200, heap "
            "refused 0 (first 0 B), threads 7 events 2 mutexes 0 semaphores 0 timers 0 shmem 0 arbiters 1, blocks 23\n"
            "[rt] system at video open: free KiB app 440 sys 81592 base 11588 linear 990 heap 100, heap room 50 low "
            "50, heap refused 9 (first 4096 B), threads 7 events 2 mutexes 0 semaphores 0 timers 0 shmem 0 arbiters "
            "1, blocks 23\n"
            "[rt] system: free KiB app 440 sys 81592 base 11588 linear 958 heap 1815, heap room 1500 low 700, heap "
            "refused 2 (first 8192 B), threads 7 events 2 mutexes 0 semaphores 0 timers 0 shmem 0 arbiters 1, blocks 23\n";
        const LogSummary::Figures f = feed(sys).figures();
        CHECK(f.have_system);
        CHECK(f.host_free_min_kib == 1815 && f.heap_room_min_kib == 700 && f.heap_refused == 2);
        Event e;
        e.figures = f;
        e.have_figures = true;
        CHECK(event_json(e).find("\"host_free_min_kib\":1815,\"heap_room_min_kib\":700,\"heap_refused\":2}") !=
              std::string::npos);
    }
    {   // Cut short by a CPU exception: crash lines, no closing line.
        const std::string log = std::string(kPlay) +
                                "crash: data abort at pc 00123456, fsr 00000007, far 00000000, lr 0, sp 0, cpsr 0\n"
                                "crash: r0-r12 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
                                "crash: handing the fault to Luma3DS (luma/dumps/arm11)\n";
        LogSummary s = feed(log.c_str());
        CHECK(s.started() && !s.closed());
        CHECK(s.crash_lines().size() == 3);
        Event e;
        e.event_id = "0123456789abcdef0123456789abcdef";
        e.crash_lines = s.crash_lines();
        e.note = "crashed opening the \"stash\"\nagain";
        e.dist = "0123456789ab";
        e.started = "2026-10-28 01:23:45";
        std::string j = event_json(e);
        CHECK(j.find("\"type\":\"data abort\"") != std::string::npos);
        CHECK(j.find("opening the \\\"stash\\\"\\nagain") != std::string::npos);
        // No timestamp unless given (Sentry's receipt time); the build as the dist.
        CHECK(j.find("\"timestamp\"") == std::string::npos);
        CHECK(j.find("\"dist\":\"0123456789ab\"") != std::string::npos);
        CHECK(j.find("\"session_started_console_clock\":\"2026-10-28 01:23:45\"") != std::string::npos);
        CHECK(j.find("\"release\"") == std::string::npos);   // no version known: no release
        e.timestamp = 1793000000;
        e.dist.clear();
        e.release = "3ds-d2@0.4.0";
        j = event_json(e);
        CHECK(j.find("\"timestamp\":1793000000") != std::string::npos && j.find("\"dist\"") == std::string::npos);
        CHECK(j.find("\"release\":\"3ds-d2@0.4.0\"") != std::string::npos);
        // Issue #61: the note is context and a tag, not the title; the group is the fault.
        CHECK(j.find("\"report\":{\"type\":\"report\",\"note\":") != std::string::npos);
        CHECK(j.find("\"note\":\"crashed opening the \\\"stash\\\" again\"") != std::string::npos);   // the tag: one line
        CHECK(j.find("\"fingerprint\":[\"cpu-exception\",\"data abort\",\"host\"]") != std::string::npos);
        e.crash_lines.insert(e.crash_lines.begin() + 1,
                             "crash: in translated guest code, block 00515fd0 (41 x86 bytes), xEIP 00515fd0");
        CHECK(fingerprint_of(e) == std::vector<std::string>({"cpu-exception", "data abort", "guest 00515fd0"}));
    }
    {   // Issue #61: a Luma3DS dump titles and groups a crash the app did not catch.
        // A dump as Luma3DS writes it: header, 23 registers, no code or stack, the process.
        const auto dump = [](uint32_t core, uint32_t type, uint32_t pc, const char *process) {
            std::string d;
            const auto put = [&d](uint32_t v) {
                for (int i = 0; i < 4; i++)
                    d += (char)(v >> (8 * i));
            };
            put(0xDEADC0DE), put(0xDEADCAFE), put(0x00010003);
            put(0x0000000B | core << 16);   // processor 11, core
            put(type), put(0), put(23 * 4), put(0), put(0), put(16);
            for (uint32_t r = 0; r < 23; r++)
                put(r == 15 ? pc : r == 14 ? 0x00231770 : r == 17 ? 0x805 : r == 18 ? 0x2 : r == 19 ? 0x089c202d : 0);
            std::string name(process);
            name.resize(8, '\0');
            d += name;
            put(0x00009802), put(0x00040030);
            return d;
        };
        LumaDump m;
        CHECK(parse_luma_dump(dump(0, 2, 0x00136f28, "menu"), m));
        CHECK(m.process == "menu" && m.title == 0x0004003000009802ull && m.pc == 0x00136f28 && m.core == 0);
        CHECK(std::string(m.type_name()) == "prefetch abort" && m.fsr == 0x2 && m.lr == 0x00231770);
        CHECK(!parse_luma_dump("not a dump", m));
        Event e;
        e.event_id = "0123456789abcdef0123456789abcdef";
        e.reason = "Session: crashed (Luma3DS dump).";
        e.note = "Going East crash";
        CHECK(parse_luma_dump(dump(0, 2, 0x00136f28, "menu"), e.dump));
        e.have_dump = true;
        CHECK(fingerprint_of(e) == std::vector<std::string>({"luma-dump", "menu", "prefetch abort", "00136f28"}));
        std::string j = event_json(e);
        CHECK(j.find("\"message\":{\"formatted\":\"Session: crashed (Luma3DS dump).\"}") != std::string::npos);
        CHECK(j.find("\"type\":\"Luma3DS dump: prefetch abort\"") != std::string::npos);
        CHECK(j.find("menu (title 0004003000009802), core 0: pc 00136f28") != std::string::npos);
        // 3ds-d2's own: by where it faulted (its pc moves with every build), here the kernel.
        CHECK(parse_luma_dump(dump(2, 3, 0xffff01c0, "3ds-d2"), e.dump));
        CHECK(e.dump.fsr == 0x805 && e.dump.far == 0x089c202d);
        CHECK(fingerprint_of(e) == std::vector<std::string>({"luma-dump", "3ds-d2", "data abort", "kernel"}));
        // Without a dump: a fatal error by its kind; a player's report; a session that did not end.
        Event f;
        f.crash_lines = {"[rt] FATAL: out of memory"};
        CHECK(fingerprint_of(f) == std::vector<std::string>({"fatal", "out of memory"}));
        Event p;
        p.player_report = true;
        p.note = std::string(250, 'x');
        CHECK(fingerprint_of(p) == std::vector<std::string>({"player-report"}));
        CHECK(event_json(p).find("\"note\":\"" + std::string(200, 'x') + "\"") != std::string::npos);   // the tag, cut
        Event u;
        u.reason = "Session: did not end normally (froze, or was switched off?).";
        CHECK(fingerprint_of(u) == std::vector<std::string>({"session", u.reason}));
    }
    {   // A v0.3.2 log: its own first and last lines.
        CHECK(feed("3ds-d2: Diablo II on the 3DS\nGame ended: exited=1 code=0 \n").closed());
        LogSummary s = feed("3ds-d2: Diablo II on the 3DS\n[rt] FATAL: out of guest memory\n");
        CHECK(s.started() && !s.closed() && s.crash_lines().size() == 1);
        CHECK(!feed("some other log\n").started());
    }
    {   // DSN.
        Dsn d;
        CHECK(parse_dsn("https://abc123@o42.ingest.de.sentry.io/4507", d));
        CHECK(d.key == "abc123" && d.host == "o42.ingest.de.sentry.io" && d.project == "4507");
        CHECK(d.envelope_url() == "https://o42.ingest.de.sentry.io/api/4507/envelope/");
        CHECK(parse_dsn("http://k:secret@relay.example:8080/7", d) && d.key == "k" && d.host == "relay.example:8080");
        CHECK(!parse_dsn("", d));
        CHECK(!parse_dsn("https://o1.ingest.sentry.io/42", d));   // no key
        CHECK(!parse_dsn("ftp://k@h/1", d));
    }
    {   // A file within the budget goes whole; a longer one as its start,
        // a note, and the rest of the budget from its end.
        FileCut c = cut_file(5000, 32u << 20, 1u << 20);
        CHECK(c.head == 5000 && c.tail_from == 5000 && c.note.empty() && c.length() == 5000);
        c = cut_file(100000, 10000, 1000);
        CHECK(c.head == 1000 && c.tail_from == 91000);   // 9000 bytes from the end
        CHECK(c.note.find("90000 bytes left out") != std::string::npos);
        CHECK(c.length() == 10000 + c.note.size());
        c = cut_file(100000, 10000, 20000);   // a head larger than the budget: the budget
        CHECK(c.head == 10000 && c.tail_from == 100000);
    }
    {   // Envelope: every item's length is its payload's.
        Dsn d;
        parse_dsn("https://k@h/1", d);
        Event e;
        e.event_id = "0123456789abcdef0123456789abcdef";
        const std::string bin("\x00\x01\n\x02", 4);
        const std::string env = envelope(d, e, {{"game.log", "text/plain", "a\nb\n"}, {"x.dmp", "application/octet-stream", bin}});
        size_t at = env.find('\n') + 1;   // past the envelope header
        int items = 0;
        while (at < env.size()) {
            const size_t hdr_end = env.find('\n', at);
            const std::string hdr = env.substr(at, hdr_end - at);
            const size_t lp = hdr.find("\"length\":");
            CHECK(lp != std::string::npos);
            const size_t len = std::stoul(hdr.substr(lp + 9));
            CHECK(env[hdr_end + 1 + len] == '\n');
            at = hdr_end + 1 + len + 1;
            items++;
        }
        CHECK(items == 3);
        CHECK(at == env.size());
    }
    std::printf("RESULT report %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
