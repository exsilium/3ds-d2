// Crash and session reports (issue #26): what game.log says about a session,
// and the Sentry envelope that carries it with the player's description.
// Nothing here runs during play: the platform calls it before the game starts
// (the previous session's files) or after it has ended.
//
// Platform-neutral: the platform reads the files, collects the console's
// details and does the upload (src/app/ctr_report.cpp).
#ifndef D2_GAME_REPORT_H
#define D2_GAME_REPORT_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace game {
namespace report {

// The lines the app writes to game.log at its start and at its orderly end.
// A log with the first and not the second is a session that was cut short:
// a crash Luma3DS caught, the backend's fatal exit, a freeze ended by the
// power button, a flat battery.
constexpr const char kSessionStart[] = "session: start";   // followed by " <unix time> build <id> version <v>"
constexpr const char kSessionClosed[] = "session: closed";
// The same, as logs from before these lines (v0.3.2 and older) have them.
constexpr const char kLegacyStart[] = "3ds-d2: Diablo II on the 3DS";
constexpr const char kLegacyEnd[] = "Game ended:";

// A project's DSN: https://<key>@<host>/<project id>. http:// is accepted
// too (a relay in front of Sentry, if the console's TLS cannot reach it).
struct Dsn {
    std::string scheme, key, host, project;
    std::string envelope_url() const;   // <scheme>://<host>/api/<project>/envelope/
    std::string text() const;           // the DSN again
};
bool parse_dsn(const std::string &s, Dsn &out);

// What a session's game.log says, line by line (feed() each line, without
// or with its newline). The figures match tools/session_report.py's: 10 s
// windows from the heartbeat's `t=` lines, "in play" from the last `game
// state:` line before each.
class LogSummary {
public:
    void feed(const char *line);

    bool started() const { return started_; }
    bool closed() const { return closed_; }
    uint64_t start_time() const { return start_time_; }   // unix seconds, 0 if not logged
    const std::string &build() const { return build_; }
    const std::string &version() const { return version_; }   // "" in logs from before 0.4.0
    // The lines that say how it ended: `crash:` (the CPU exception handler),
    // FATAL, out of memory, the game's internal error. At most 40.
    const std::vector<std::string> &crash_lines() const { return crash_; }
    // What the session's profiler runs measured (profiler.h): the options of
    // every "profiler run: start" line, in the order first seen, comma
    // separated; "" without a run.
    const std::string &profiled() const { return profiled_; }

    struct Figures {
        uint32_t seconds = 0;          // the last heartbeat's game time
        uint32_t play_windows = 0;     // 10 s windows in a game with frames
        float fps_median = 0, fps_p10 = 0;   // over those windows
        uint32_t pool_free_min_kib = 0;      // the guest pool's lowest free (0: no heartbeat)
        uint32_t heap_peak_kib = 0, va_peak_kib = 0, host_peak_kib = 0, jit_kib = 0;
        // In play, from the hitch counters ("prof hitch:", issue #19):
        // frames that took over 100 ms, and the longest.
        uint32_t hitches_100ms = 0, longest_frame_ms = 0;
        // The sprite cache's frames in blocks of their own ("sprite frames",
        // issue #44): the most their blocks held, the most of that in no
        // unit (partly used blocks), and by the end the groups over 256 KiB
        // they took (the C heap's before) and those they had no block for.
        uint32_t frames_held_peak_kib = 0, frames_unused_peak_kib = 0, frames_over_256k = 0, frames_to_pool = 0;
    };
    Figures figures() const;

private:
    bool started_ = false, closed_ = false, in_game_ = false;
    uint64_t start_time_ = 0;
    std::string build_, version_, profiled_;
    std::vector<std::string> crash_;
    uint32_t last_t_ = 0, last_frames_ = 0;
    bool have_beat_ = false;
    std::vector<float> play_fps_;
    Figures f_;
};

// What of a file of `size` bytes goes into a report of at most `max`: all
// of it, or its first `head` bytes, a line saying how much was left out,
// and the rest of the budget from its end. The platform streams the parts
// (a long session's log is megabytes; it is never held whole).
struct FileCut {
    uint64_t head = 0;        // bytes from the start
    uint64_t tail_from = 0;   // then from here to the end (== size: nothing)
    uint64_t size = 0;
    std::string note;         // between them, if anything was left out
    uint64_t length() const { return head + note.size() + (size - tail_from); }
};
FileCut cut_file(uint64_t size, uint64_t max, uint64_t head);

// An attachment item's header line (newline included), for `length` bytes
// of payload; the payload and a newline follow it.
std::string attachment_header(const std::string &filename, const std::string &content_type, uint64_t length);

struct Attachment {
    std::string filename, content_type, data;
};

struct Event {
    std::string event_id;    // 32 lowercase hex digits
    uint64_t timestamp = 0;  // unix seconds, UTC; 0: when Sentry receives it (the console's
                             // clock is local time, in no known zone)
    std::string level;       // "fatal" (a crash), "error", "info" (asked for by the player)
    std::string release;     // 3ds-d2@<version>
    std::string dist;        // the build ID: which build of the release
    std::string started;     // when the session started, by the console's clock (may be empty)
    std::string message;     // the player's description (may be empty)
    std::string reason;      // why there is a report, in a few words
    std::vector<std::pair<std::string, std::string>> tags;
    std::vector<std::pair<std::string, std::string>> device;   // contexts.device
    std::vector<std::string> crash_lines;
    LogSummary::Figures figures;
    bool have_figures = false;
};

// The event as Sentry's JSON.
std::string event_json(const Event &e);
// The envelope's header and its event item; attachment items follow
// (attachment_header). envelope() is all of it at once, for small payloads.
std::string envelope_head(const Dsn &dsn, const Event &e);
std::string envelope(const Dsn &dsn, const Event &e, const std::vector<Attachment> &attachments);

// A JSON string literal (quotes included) for UTF-8 text.
std::string json_string(const std::string &s);

}  // namespace report
}  // namespace game

#endif
