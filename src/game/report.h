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
        // The 3DS app's "system:" heartbeat lines (issue #24): the host
        // heap's lowest free, the least room it had left to grow, and by
        // the end the requests to grow it that were refused.
        bool have_system = false;
        uint32_t host_free_min_kib = 0, heap_room_min_kib = 0, heap_refused = 0;
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

// A Luma3DS ARM11 exception dump (luma/dumps/arm11/*.dmp; tools/luma_dump.py):
// what a report is titled and grouped by. Layout per Luma3DS's dump header:
// two magic words, version, processor and core, the exception type, total
// size, then the sizes of the register, code, stack and additional-data
// sections, which follow in that order (the process name and title ID last).
struct LumaDump {
    std::string process;   // "3ds-d2", "menu", ...
    uint64_t title = 0;
    uint32_t core = 0, type = 0;   // type: 0 FIQ, 1 undefined instruction, 2 prefetch abort, 3 data abort
    uint32_t pc = 0, lr = 0, fsr = 0, far = 0;   // fsr: the DFSR for a data abort, else the IFSR
    const char *type_name() const;
    bool in_kernel() const { return pc >= 0xFFF00000u; }
};
bool parse_luma_dump(const std::string &data, LumaDump &out);

// ---- screenshots (issue #61) ---------------------------------------------------------
//
// A report can carry up to kMaxShotPairs screenshot pairs, each the top and
// the bottom screen's picture, chosen by the player (none by default). Two
// sources, and never the console's Camera album as such (the player's own
// photos are in it):
//  - the game's (L+R+Select): its frame, saved to the album, and the bottom
//    screen, saved to the game's own folder. The game lists the pairs it saved
//    (kShotList: the newest kKeepShotPairs), so only those are offered.
//  - Luma3DS's (Rosalina menu, "Take screenshot"):
//    sdmc:/luma/screenshots/<YYYY-MM-DD_HH-MM-SS.mmm>_top.bmp and _bot.bmp,
//    offered when taken while the session ran (suspended in the HOME Menu
//    included), so no other app's pictures.
constexpr int kMaxShotPairs = 4, kKeepShotPairs = 16;

struct ShotPair {
    uint64_t time = 0;     // when taken, unix seconds by the console's clock (local time)
    uint32_t ms = 0;       // and its milliseconds
    std::string top, bot;  // the pictures' paths (bot may be empty: the game's, without one)
    bool luma = false;     // Luma3DS's (BMP), else the game's (JPEG)
    bool operator==(const ShotPair &o) const { return top == o.top; }
};

// The game's list, a line per pair, oldest first: "<time> <ms> <top> <bot>"
// (a report's chosen pairs too: Luma3DS's are known by their .bmp).
// add_shot appends a pair and drops the oldest beyond kKeepShotPairs, giving
// back those dropped (their bottom pictures, the game's own files, go too).
std::vector<ShotPair> parse_shot_list(const std::string &text);
std::string shot_list_text(const std::vector<ShotPair> &pairs);
std::vector<ShotPair> add_shot(std::vector<ShotPair> &list, const ShotPair &p);

// Seconds since 1970 for a date and time (no time zone: the console's clock).
uint64_t unix_time(int year, int month, int day, int hour, int minute, int second);
// Luma3DS's pairs among a directory's file names: both pictures there, taken
// within [from, to] (unix seconds); newest first. `dir` prefixes the paths.
std::vector<ShotPair> luma_pairs(const std::string &dir, const std::vector<std::string> &names, uint64_t from,
                                 uint64_t to);
// "2026-11-02 14:02:31", as the picker shows a pair's time.
std::string shot_time_text(uint64_t time);

// A 24-bit BMP (Luma3DS's screenshots) as RGB rows, top row first.
bool decode_bmp(const std::string &data, uint32_t &width, uint32_t &height, std::vector<uint8_t> &rgb);

struct Event {
    std::string event_id;    // 32 lowercase hex digits
    uint64_t timestamp = 0;  // unix seconds, UTC; 0: when Sentry receives it (the console's
                             // clock is local time, in no known zone)
    std::string level;       // "fatal" (a crash), "error", "info" (asked for by the player)
    std::string version;     // the version the session was played on ("0.4.0", "0.4.0-dev"; "" unknown)
    std::string release;     // 3ds-d2@<version>
    std::string dist;        // the build ID: which build of the release
    std::string started;     // when the session started, by the console's clock (may be empty)
    std::string note;        // the player's description (may be empty): context, not the title (issue #61)
    std::string topic;       // the player's own part of the title ("#24"; may be empty), kept between reports
    // The screenshot pairs attached, in order (screenshot-<n>a-top.jpg and
    // <n>b-bot.jpg): where each came from, for the report context.
    std::vector<ShotPair> screenshots;
    std::string reason;      // why there is a report, in a few words
    std::vector<std::pair<std::string, std::string>> tags;
    std::vector<std::pair<std::string, std::string>> device;   // contexts.device
    std::vector<std::string> crash_lines;
    LumaDump dump;           // the session's first Luma3DS dump, if have_dump
    bool have_dump = false;
    bool player_report = false;   // asked for by the player, or a profiler run
    LogSummary::Figures figures;
    bool have_figures = false;
};

// A player's report is titled by the version, which the player cannot
// change, and the topic they add ("v0.4.0-dev #24"); reports with the same
// title are one Sentry issue. topic_text: the topic on one line as typed, but
// trimmed and with runs of spaces as one; topic_key: that in ASCII lowercase,
// as the fingerprint takes it ("#24 " and "#24" are one issue).
std::string player_title(const std::string &version, const std::string &topic);
std::string topic_text(const std::string &topic);
std::string topic_key(const std::string &topic);

// What Sentry groups the event by (its `fingerprint`), from what happened,
// never from the player's note (issue #61): a CPU exception by its kind and
// the guest block it hit; a Luma3DS dump by process, exception and, outside
// 3ds-d2 (whose addresses move with each build), its pc; a fatal error by
// its kind; a session that did not end; a player's own report by its title.
// The topic groups only a player's report: a crash keeps its own issue
// whatever the run was for, and carries the topic as a tag.
std::vector<std::string> fingerprint_of(const Event &e);

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
