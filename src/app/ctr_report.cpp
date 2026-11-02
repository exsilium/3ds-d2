// Crash and session reports on the console (issue #26). Nothing here runs
// during play: it runs before the game starts and after it has ended, when
// the guest's memory is free.
//
//   sdmc:/3ds/3ds-d2/reports/<time>-<build>/   one session's report:
//       game.log, stderr.txt   the session's logs, as they were
//       sprites.trc            a profiler run's sprite trace (issue #18)
//       files.trc              a profiler run's file-read trace (issue #21)
//       crash_dump_*.dmp       Luma3DS's dumps saved during it (copies)
//       report.txt             why there is a report (key=value)
//       note.txt               the player's description
//       topic.txt              the topic the player set on it, or it was sent with
//       screenshots.txt        the screenshot pairs the player chose for it (issue #61)
//       sent.txt / declined.txt  the player's answer (none yet: offered at start)
//
// A session that never wrote its closing line to game.log (a crash Luma3DS
// caught, the backend's fatal exit, a freeze ended with the power button)
// is found at the next start, before game.log and stderr.txt are opened for
// the new session: both move to a report folder. A session that ended with
// a fatal error, or for which the player tapped INFO's report button, is
// copied there after the game. Either way the top screen then shows what
// would be sent; nothing leaves the console without A (Send). The folder
// stays on the card: it is also what to attach to a GitHub issue by hand.
//
// The upload is a Sentry envelope POSTed over the console's sockets, with
// TLS from a module loaded only for it (ctr_tls.cpp). D2_REPORT_DSN (a build
// variable) names the project; without it nothing is offered or sent, and
// the crashed session's logs are only kept. The DSN sits in a fixed-size
// array, so the code and data addresses (the build ID's ELF, for Luma dumps)
// are the same with or without one.
#include "game/osk.h"
#include "game/report.h"
#include "game/second_screen.h"

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace tls {
bool post(const std::string &url, const std::string &headers, const std::string &body, int &status,
          std::string &answer, std::string &err);
}

namespace shot {   // ctr_screenshot.cpp
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgb;
};
// A JPEG or a BMP as RGB, scaled down to fit max_w x max_h (0: as it is).
bool load(const std::string &path, uint32_t max_w, uint32_t max_h, Image &out, std::string &msg);
bool to_jpeg(const Image &img, std::string &out, std::string &msg);
}  // namespace shot

#ifndef D2_BUILD_ID
#define D2_BUILD_ID "unversioned"
#endif
#ifndef D2_VERSION
#define D2_VERSION "0.0.0"
#endif
#ifndef D2_REPORT_DSN
#define D2_REPORT_DSN ""
#endif

extern "C" const uint8_t default_font_bin[];

namespace report {

// What ctr_game.cpp lends the reports: its screen messages and consoles.
struct Host {
    PrintConsole *log;               // the bottom screen's message window
    void (*message)(const char *);   // a line there and in game.log
    void (*restore_bottom)();        // the bottom screen's consoles again, after the fallback keyboard
    const char *app;                 // "cia", "3dsx"
    const char *game_files;          // "sdcard", "romfs-slim", "romfs-full"
};

namespace {

using game::report::LogSummary;

const char kReports[] = "sdmc:/3ds/3ds-d2/reports";
const char kLogDir[] = "sdmc:/3ds/3ds-d2/logs";
const char kLumaDumps[] = "sdmc:/luma/dumps/arm11";
// The player's topic for their reports (issue #61): the title's own part,
// after the version ("#24"), until they change it.
const char kTopic[] = "sdmc:/3ds/3ds-d2/report-topic.txt";
constexpr int kTopicMax = 40;   // characters
// Screenshot pairs to offer (game/report.h): the game's list
// (ctr_screenshot.cpp), and Luma3DS's folder.
const char kShotList[] = "sdmc:/3ds/3ds-d2/screenshots/pairs.txt";
const char kLumaShots[] = "sdmc:/luma/screenshots";
constexpr uint64_t kShotMax = 8u << 20;   // a picture larger is not one of ours: left out
const char kDsn[256] = D2_REPORT_DSN;
constexpr int kKeepWaiting = 32;   // reports to send: older ones are removed (prune_reports)
constexpr int kKeepAnswered = 8;   // sent, or not to be sent: older ones are removed
// What of each file goes into the report. game.log goes whole up to 32 MiB
// (a 4-hour session wrote 1.1 MiB; 32 MiB compresses to ~4 MiB, far under
// Sentry's 40 MB per request); beyond that its first 1 MiB (the console's
// memory setup) and the last 31 MiB (what led to the end).
constexpr uint64_t kLogMax = 32u << 20, kLogHead = 1u << 20;
constexpr uint64_t kStderrMax = 4u << 20, kDumpMax = 8u << 20;
// The sprite trace (src/game/sprite_trace.h): its first 32 MiB, whole
// records. Half an hour of play writes a few MiB; it compresses about 3:1.
constexpr uint64_t kTraceMax = 32u << 20, kTraceRecord = 16;
// The file-read trace (rt::set_file_trace): text, its first 16 MiB. A line
// per read of the game's; it compresses about 5:1.
constexpr uint64_t kFileTraceMax = 16u << 20;

void say(const Host &host, const std::string &s)
{
    consoleSelect(host.log);   // the top screen's console may be the current one
    host.message(s.c_str());
}

// osGetTime: milliseconds since 1900, in the console's local time (as are
// the SD card's file times).
uint64_t unix_now() { return osGetTime() / 1000 - 2208988800ull; }

// A time from unix_now() as the console showed it.
std::string clock_text(uint64_t when)
{
    const time_t t = (time_t)when;
    struct tm tm;
    gmtime_r(&t, &tm);   // already local time: no zone to apply
    char s[32];
    std::strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", &tm);
    return s;
}

// Small files only (report.txt, note.txt): logs are streamed.
bool read_all(const std::string &path, std::string &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[512];
    out.clear();
    while (size_t n = std::fread(buf, 1, sizeof buf, f))
        out.append(buf, n);
    std::fclose(f);
    return true;
}

bool write_all(const std::string &path, const std::string &data)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

bool copy_file(const std::string &from, const std::string &to)
{
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in)
        return false;
    FILE *out = std::fopen(to.c_str(), "wb");
    bool ok = out != nullptr;
    std::vector<char> buf(64 << 10);   // on the heap: .bss is resident during play
    while (ok)
        if (const size_t n = std::fread(buf.data(), 1, buf.size(), in))
            ok = std::fwrite(buf.data(), 1, n, out) == n;
        else
            break;
    std::fclose(in);
    return out && std::fclose(out) == 0 && ok;
}

bool move_file(const std::string &from, const std::string &to)
{
    if (std::rename(from.c_str(), to.c_str()) == 0)
        return true;
    return copy_file(from, to) && std::remove(from.c_str()) == 0;
}

bool exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

long file_size(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

std::vector<std::string> list_dir(const char *dir)
{
    std::vector<std::string> names;
    if (DIR *d = opendir(dir)) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.')
                names.push_back(e->d_name);
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void remove_tree(const std::string &dir)
{
    for (const std::string &n : list_dir(dir.c_str()))
        std::remove((dir + "/" + n).c_str());
    rmdir(dir.c_str());
}

bool summarize(const std::string &path, LogSummary &s)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f)
        return false;
    char line[1024];
    while (std::fgets(line, sizeof line, f))
        s.feed(line);
    std::fclose(f);
    return true;
}

// key=value lines.
std::string kv_get(const std::string &text, const char *key)
{
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        if (text.compare(at, k.size(), k) == 0)
            return text.substr(at + k.size(), end - at - k.size());
        at = end + 1;
    }
    return "";
}

// A new report folder, named so that the names sort by time.
std::string make_report_dir(uint64_t when, const std::string &build)
{
    mkdir(kReports, 0777);
    const time_t t = (time_t)when;
    struct tm tm;
    gmtime_r(&t, &tm);   // already local time: no zone to apply
    char name[48];
    std::strftime(name, sizeof name, "%Y%m%d-%H%M%S", &tm);
    std::string dir = std::string(kReports) + "/" + name + "-" + (build.empty() ? "unknown" : build);
    for (int i = 2; exists(dir); i++)
        dir = std::string(kReports) + "/" + name + "-" + std::to_string(i);
    mkdir(dir.c_str(), 0777);
    return dir;
}

// Reports still to be sent stay, up to kKeepWaiting (sessions played
// without the internet, for the next connection); of the answered ones (sent,
// or not to be sent), only the newest kKeepAnswered.
void prune_reports()
{
    const std::vector<std::string> names = list_dir(kReports);
    int waiting = 0, answered = 0;
    for (auto n = names.rbegin(); n != names.rend(); ++n) {
        const std::string dir = std::string(kReports) + "/" + *n;
        const bool open = exists(dir + "/report.txt") && !exists(dir + "/sent.txt") && !exists(dir + "/declined.txt");
        if (open ? ++waiting > kKeepWaiting : ++answered > kKeepAnswered)
            remove_tree(dir);
    }
}

// Luma3DS's ARM11 dumps written since `since` (unix seconds; a few seconds'
// slack for the clock's granularity), copied into `dir`.
int copy_new_dumps(const std::string &dir, uint64_t since)
{
    int n = 0;
    for (const std::string &name : list_dir(kLumaDumps)) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".dmp") != 0)
            continue;
        const std::string path = std::string(kLumaDumps) + "/" + name;
        u64 mtime = 0;
        if (R_FAILED(archive_getmtime(path.c_str(), &mtime)) || mtime + 5 < since)
            continue;
        if (copy_file(path, dir + "/" + name))
            n++;
    }
    return n;
}

std::string reason_of(const LogSummary &s, int dumps)
{
    for (const std::string &l : s.crash_lines())
        if (l.compare(0, 6, "crash:") == 0)
            return "crashed (CPU exception)";
    if (dumps)
        return "crashed (Luma3DS dump)";
    if (!s.crash_lines().empty())
        return "stopped on a fatal error";
    return "did not end normally (froze, or was switched off?)";
}

bool have_dsn()
{
    game::report::Dsn d;
    return game::report::parse_dsn(kDsn, d);
}

// ---- the consent screen (top) -------------------------------------------------------

PrintConsole g_top;

// `text` in lines of at most `width` columns, broken at spaces where possible.
std::vector<std::string> wrap(const std::string &text, size_t width)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos)
            nl = text.size();
        std::string para = text.substr(at, nl - at);
        while (para.size() > width) {
            size_t cut = para.rfind(' ', width);
            if (cut == std::string::npos || cut == 0)
                cut = width;
            out.push_back(para.substr(0, cut));
            para.erase(0, cut + (para[cut] == ' ' ? 1 : 0));
        }
        out.push_back(para);
        at = nl + 1;
    }
    return out;
}

// The swkbd's text is UTF-8; the console font has ASCII only.
std::string printable(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 0x80) {
            if ((c & 0xC0) == 0xC0)
                o += '?';   // one per character, not per byte
        } else if (c >= 0x20 || c == '\n') {
            o += (char)c;
        }
    }
    return o;
}

std::string kib(long bytes)
{
    char b[24];
    std::snprintf(b, sizeof b, "%ld KiB", (bytes + 1023) / 1024);
    return b;
}

struct Offer {
    std::string dir, reason, build, note;
    std::string topic;      // the player's part of the title (issue #61)
    std::vector<game::report::ShotPair> shots;   // the screenshot pairs chosen (screenshots.txt)
    LogSummary log;
    std::vector<std::string> dumps;
    std::string state;      // "" (to send), "sent", "not sent"
    std::string event_id;   // Sentry's, once sent
    std::string status;     // the last action's outcome
    bool confirm_delete = false;
};

// A report the player asked for (or a profiler run): titled by the version
// and the player's topic. Any other report is titled by what happened.
bool asked_for(const Offer &o) { return o.reason == "asked for by the player" || o.reason == "a profiler run"; }

// The version the session was played on. A log from before 0.4.0 does not
// say it: only if it is this build's is it known.
std::string version_of(const Offer &o)
{
    return !o.log.version().empty() ? o.log.version() : o.build == D2_BUILD_ID ? std::string(D2_VERSION) : "";
}

// The bottom of every report screen: a message in rows 24-25 (wrapped, two
// lines at most), and the buttons in rows 26-29, two to a row, each column's
// keys and labels lined up. Columns 0 and 25, labels of at most 16: nothing
// reaches column 49, where the cursor would wrap (and the last row scroll).
// What is above them ends by row 23.
void draw_status(const std::string &text)
{
    const std::vector<std::string> lines = wrap(text, 49);
    for (size_t i = 0; i < lines.size() && i < 2; i++)
        std::printf("\x1b[%u;0H\x1b[33m%s\x1b[0m", (unsigned)(24 + i), lines[i].c_str());
}

void draw_keys(std::initializer_list<std::pair<const char *, const char *>> keys)
{
    unsigned i = 0;
    for (const auto &k : keys) {
        std::printf("\x1b[%u;%uH\x1b[33m%-7s\x1b[0m %.16s", 26 + i / 2, i % 2 ? 25u : 0u, k.first, k.second);
        i++;
    }
}

// The frame drawn so far, on both screens.
void present()
{
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, false);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    gspWaitForVBlank();
}

void draw_offer(const Offer &o)
{
    consoleSelect(&g_top);
    consoleClear();
    if (o.state.empty())
        std::printf("\x1b[33m3ds-d2: send a problem report?\x1b[0m\n");
    else
        std::printf("\x1b[33m3ds-d2: a problem report (%s)\x1b[0m\n", o.state.c_str());
    // A player's report: its title, the version faint (it is not the
    // player's to change) and their topic. Any other: the topic only, a tag.
    const std::string topic = printable(game::report::topic_text(o.topic));
    if (asked_for(o)) {
        const std::string fixed = game::report::player_title(version_of(o), "");
        std::printf("Title: \x1b[2m%s\x1b[0m ", fixed.c_str());
        const int room = 48 - 7 - (int)fixed.size() - 1;   // within the line, as draw_keys
        if (!topic.empty())
            std::printf("\x1b[36m%.*s\x1b[0m\n\n", room > 0 ? room : 0, topic.c_str());
        else
            std::printf("\x1b[2m%s\x1b[0m\n\n", o.state == "sent" ? "" : "(R: add a topic)");
    } else if (!topic.empty()) {
        std::printf("Topic: \x1b[36m%.41s\x1b[0m\n\n", topic.c_str());
    } else {
        std::printf("\n");
    }
    for (const std::string &l : wrap("The session of build " + o.build + ": " + o.reason + ".", 49))
        std::printf("%s\n", l.c_str());
    if (!o.log.crash_lines().empty())
        std::printf("\x1b[31m%.49s\x1b[0m\n", o.log.crash_lines().front().c_str());
    std::printf(o.state == "sent" ? "\nWhat was sent to the developer via Sentry:\n"
                                  : "\nWhat would be sent to the developer via Sentry:\n");
    const long log = file_size(o.dir + "/game.log");
    std::printf(" game.log (%s%s)\n", kib(log < (long)kLogMax ? log : (long)kLogMax).c_str(),
                log > (long)kLogMax ? ", its start and end" : "");
    if (exists(o.dir + "/stderr.txt"))
        std::printf(" stderr.txt\n");
    const long trace = file_size(o.dir + "/sprites.trc");
    if (trace > 0)
        std::printf(" sprites.trc (%s%s)\n", kib(trace < (long)kTraceMax ? trace : (long)kTraceMax).c_str(),
                    trace > (long)kTraceMax ? ", its start" : "");
    const long reads = file_size(o.dir + "/files.trc");
    if (reads > 0)
        std::printf(" files.trc (%s%s)\n", kib(reads < (long)kFileTraceMax ? reads : (long)kFileTraceMax).c_str(),
                    reads > (long)kFileTraceMax ? ", its start" : "");
    for (const std::string &d : o.dumps)
        std::printf(" %.48s\n", d.c_str());
    if (!o.shots.empty())
        std::printf(" \x1b[36m%u screenshot pair%s\x1b[0m (top and bottom screen)\n", (unsigned)o.shots.size(),
                    o.shots.size() == 1 ? "" : "s");
    else if (o.state != "sent")
        std::printf(" \x1b[2mno screenshots (L: add up to %d pairs)\x1b[0m\n", game::report::kMaxShotPairs);
    std::printf(" build, version, model, region, firmware\n");
    const LogSummary::Figures f = o.log.figures();
    if (f.seconds)
        std::printf(" %u min played, %.1f fps median, memory figures\n", (unsigned)(f.seconds / 60), f.fps_median);
    std::printf(" No saves, no character names.\n\n");
    std::printf("Your description:\n");
    const std::vector<std::string> note = wrap(printable(o.note), 48);
    if (note.empty() || o.note.empty()) {
        std::printf("\x1b[36m  %s\x1b[0m\n", o.state == "sent" ? "(none)" : "(none yet: Y to write one)");
    } else {
        for (size_t i = 0; i < note.size() && i < 3; i++)
            std::printf("\x1b[36m  %s\x1b[0m\n", note[i].c_str());
        if (note.size() > 3)
            std::printf("\x1b[36m  ...\x1b[0m\n");
    }
    // The last action's outcome; once sent, Sentry's ID for it.
    draw_status(!o.status.empty() || o.state != "sent" ? o.status : "Sent. Sentry's event ID: " + o.event_id);
    if (o.state == "sent")
        draw_keys({{"SELECT", "delete"}, {"B", "back to the list"}});
    else if (o.state.empty())
        draw_keys({{"A", "send"}, {"Y", "edit description"}, {"R", asked_for(o) ? "edit title" : "edit topic"},
                   {"L", "screenshots"}, {"X", "don't send"}, {"SELECT", "delete"}, {"B", "back to the list"}});
    else
        draw_keys({{"A", "send"}, {"Y", "edit description"}, {"R", asked_for(o) ? "edit title" : "edit topic"},
                   {"L", "screenshots"}, {"SELECT", "delete"}, {"B", "back to the list"}});
}

// The game's own keyboard on the bottom screen: when the system's keyboard
// cannot be started (osk.h's open_text).
void osk_input(const Host &host, std::string &text, const char *label)
{
    game::Osk osk;
    osk.open_text(&text);
    bool prev_touch = false;
    while (osk.is_open() && aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        uint32_t pressed = 0;
        if (down & KEY_B)
            pressed |= game::kB;
        if (down & KEY_Y)
            pressed |= game::kY;
        if (down & KEY_START)
            pressed |= game::kStart;
        if (down & KEY_SELECT)
            pressed |= game::kSelect;
        osk.buttons(pressed);
        const bool touching = (hidKeysHeld() & KEY_TOUCH) != 0;
        if (touching && !prev_touch) {
            touchPosition t;
            hidTouchRead(&t);
            osk.tap(t.px, t.py - game::SecondScreen::kOskY);
        }
        prev_touch = touching;
        osk.tick(osGetTime());
        if (osk.dirty()) {
            game::BottomView v;
            v.kind = game::BottomView::kKeyboard;
            v.lines.push_back({label, 0xFFE0});
            std::vector<std::string> lines = wrap(printable(text) + "_", 40);
            const size_t first = lines.size() > 11 ? lines.size() - 11 : 0;
            for (size_t i = first; i < lines.size(); i++)
                v.lines.push_back({lines[i], 0x07FF});
            v.osk_title = osk.title();
            v.osk_keys = osk.keys();
            game::draw_bottom((uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr), v, nullptr, 0, 0,
                              nullptr, default_font_bin);
            osk.clear_dirty();
        }
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        gspWaitForVBlank();
    }
    host.restore_bottom();
    consoleSelect(&g_top);
}

void describe(const Host &host, Offer &o)
{
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, 500);
    swkbdSetHintText(&kb, "What were you doing? What went wrong?");
    swkbdSetInitialText(&kb, o.note.c_str());
    swkbdSetFeatures(&kb, SWKBD_MULTILINE | SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
    std::vector<char> buf(2048);   // 500 UTF-16 units as UTF-8
    const SwkbdButton b = swkbdInputText(&kb, buf.data(), buf.size());
    if (b == SWKBD_BUTTON_RIGHT) {
        o.note = buf.data();
    } else if (b == SWKBD_BUTTON_NONE) {
        const SwkbdResult r = swkbdGetResult(&kb);
        if (r == SWKBD_INVALID_INPUT || r == SWKBD_OUTOFMEM) {
            say(host, "report: the system keyboard did not start; the game's keyboard instead");
            osk_input(host, o.note, "Your description (Start: done)");
        }
    }
    write_all(o.dir + "/note.txt", o.note);
}

// The player's part of the title (issue #61): one line, kept in kTopic for
// every later report until changed, and in the report's own topic.txt.
void edit_topic(const Host &host, Offer &o)
{
    const std::string fixed = game::report::player_title(version_of(o), "");
    const std::string hint = asked_for(o) ? fixed + " ... (a topic, e.g. #24)" : "A topic, e.g. #24";
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, kTopicMax);
    swkbdSetHintText(&kb, hint.c_str());
    swkbdSetInitialText(&kb, o.topic.c_str());
    swkbdSetFeatures(&kb, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetButton(&kb, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&kb, SWKBD_BUTTON_RIGHT, "OK", true);
    std::vector<char> buf(4 * kTopicMax + 1);
    const SwkbdButton b = swkbdInputText(&kb, buf.data(), buf.size());
    if (b == SWKBD_BUTTON_RIGHT) {
        o.topic = game::report::topic_text(buf.data());
    } else if (b == SWKBD_BUTTON_NONE) {
        const SwkbdResult r = swkbdGetResult(&kb);
        if (r != SWKBD_INVALID_INPUT && r != SWKBD_OUTOFMEM)
            return;
        say(host, "report: the system keyboard did not start; the game's keyboard instead");
        osk_input(host, o.topic, "The report's topic (Start: done)");
        o.topic = game::report::topic_text(o.topic);
    } else {
        return;
    }
    write_all(kTopic, o.topic);
    write_all(o.dir + "/topic.txt", o.topic);
    say(host, o.topic.empty() ? "report: no topic" : "report: topic " + printable(o.topic));
}

// ---- screenshots (issue #61) ---------------------------------------------------------

using game::report::ShotPair;

// The session's time by the console's clock: from its start line to the
// last write of its game.log (a heartbeat every 10 s; a report's copy is
// made at the session's end), with a minute's slack. {0, 0}: the log does
// not say when it started.
void session_window(const Offer &o, uint64_t &from, uint64_t &to)
{
    from = to = 0;
    if (!o.log.start_time())
        return;
    from = o.log.start_time();
    u64 mtime = 0;   // unix seconds, local time, as osGetTime
    const bool known = R_SUCCEEDED(archive_getmtime((o.dir + "/game.log").c_str(), &mtime)) && mtime >= from;
    to = (known ? (uint64_t)mtime : from + o.log.figures().seconds) + 60;
}

bool in_window(const ShotPair &p, uint64_t from, uint64_t to) { return from && p.time >= from && p.time <= to; }

// What can be attached, newest first: the game's pairs still on the card,
// and Luma3DS's taken during the session. Never the Camera album as such.
std::vector<ShotPair> shot_choices(uint64_t from, uint64_t to)
{
    std::string text;
    read_all(kShotList, text);
    std::vector<ShotPair> out;
    for (const ShotPair &p : game::report::parse_shot_list(text))
        if (exists(p.top))
            out.push_back(p);
    if (from)
        for (const ShotPair &p : game::report::luma_pairs(kLumaShots, list_dir(kLumaShots), from, to))
            out.push_back(p);
    std::sort(out.begin(), out.end(), [](const ShotPair &a, const ShotPair &b) {
        return a.time != b.time ? a.time > b.time : a.ms > b.ms;
    });
    return out;
}

// One of a pair's pictures on the bottom screen, fitted and centred (the
// game's 640x480 frame at half its size). False: there is none, or it
// cannot be read.
bool draw_preview(const ShotPair &p, bool bottom)
{
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    std::memset(fb, 0, 320 * 240 * sizeof(uint16_t));
    const std::string &path = bottom ? p.bot : p.top;
    shot::Image img;
    std::string msg;
    if (path.empty() || !shot::load(path, 320, 240, img, msg))
        return false;
    const uint32_t x0 = (320 - img.width) / 2, y0 = (240 - img.height) / 2;
    for (uint32_t y = 0; y < img.height; y++) {
        const uint8_t *s = img.rgb.data() + (size_t)y * img.width * 3;
        for (uint32_t x = 0; x < img.width; x++, s += 3)   // the framebuffer: columns, bottom to top
            fb[(size_t)(x0 + x) * 240 + (239 - (y0 + y))] =
                (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | (s[2] >> 3));
    }
    return true;
}

constexpr size_t kPickRows = 20;   // rows 3..22

void draw_picker(const std::vector<ShotPair> &all, const std::vector<ShotPair> &chosen, size_t sel, size_t first,
                 uint64_t from, uint64_t to, const std::string &status, bool bottom)
{
    consoleSelect(&g_top);
    consoleClear();
    std::printf("\x1b[33m3ds-d2: screenshots for the report\x1b[0m\n");
    std::printf("%u of at most %d pairs chosen. *: this session's\n\n", (unsigned)chosen.size(),
                game::report::kMaxShotPairs);
    if (all.empty())
        for (const std::string &l :
             wrap("No screenshots to offer. In the game, L+R+Select saves a pair (both screens). So does Luma3DS's "
                  "menu (L+Down+Select), Take screenshot, while the game runs.",
                  49))
            std::printf("%s\n", l.c_str());
    for (size_t i = first; i < all.size() && i < first + kPickRows; i++) {
        const ShotPair &p = all[i];
        const bool on = std::find(chosen.begin(), chosen.end(), p) != chosen.end();
        std::printf("%s%c [%c] %s  %-7s %c\x1b[0m\n", on ? "\x1b[36m" : "", i == sel ? '>' : ' ', on ? 'x' : ' ',
                    game::report::shot_time_text(p.time).c_str(), p.luma ? "Luma3DS" : "game",
                    in_window(p, from, to) ? '*' : ' ');
    }
    draw_status(!status.empty() || all.empty() ? status
                                               : std::string("Below: its ") + (bottom ? "bottom" : "top") + " screen.");
    draw_keys({{"A", "choose"}, {"Y", bottom ? "show the top" : "show the bottom"}, {"Up/Down", "move"},
               {"B", "done"}});
}

// The player picks the pairs to attach (none chosen to begin with); kept in
// the report's screenshots.txt, oldest first.
void pick_shots(const Host &host, Offer &o)
{
    uint64_t from, to;
    session_window(o, from, to);
    const std::vector<ShotPair> all = shot_choices(from, to);
    std::vector<ShotPair> chosen;
    for (const ShotPair &p : o.shots)   // those still on the card
        if (std::find(all.begin(), all.end(), p) != all.end())
            chosen.push_back(p);
    size_t sel = 0, first = 0;
    bool bottom = false, redraw = true, preview = true;
    std::string status;
    while (aptMainLoop()) {
        if (preview && !all.empty()) {
            if (!draw_preview(all[sel], bottom))
                status = bottom && all[sel].bot.empty() ? "This pair has no bottom picture."
                                                        : "This picture cannot be read.";
            preview = false;
            redraw = true;
        }
        if (redraw) {
            draw_picker(all, chosen, sel, first, from, to, status, bottom);
            redraw = false;
        }
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_B)
            break;
        if (!all.empty() && (k & (KEY_UP | KEY_DOWN | KEY_A | KEY_Y))) {
            status.clear();
            if ((k & KEY_DOWN) && sel + 1 < all.size()) {
                sel++;
                preview = true;
            } else if ((k & KEY_UP) && sel > 0) {
                sel--;
                preview = true;
            } else if (k & KEY_Y) {
                bottom = !bottom;
                preview = true;
            } else if (k & KEY_A) {
                const auto at = std::find(chosen.begin(), chosen.end(), all[sel]);
                if (at != chosen.end())
                    chosen.erase(at);
                else if (chosen.size() < (size_t)game::report::kMaxShotPairs)
                    chosen.push_back(all[sel]);
                else
                    status = "At most " + std::to_string(game::report::kMaxShotPairs) + " pairs: A on one unchooses it.";
            }
            if (sel < first)
                first = sel;
            else if (sel >= first + kPickRows)
                first = sel - kPickRows + 1;
            redraw = true;
        }
        present();
    }
    std::sort(chosen.begin(), chosen.end(), [](const ShotPair &a, const ShotPair &b) {
        return a.time != b.time ? a.time < b.time : a.ms < b.ms;
    });
    o.shots = chosen;
    write_all(o.dir + "/screenshots.txt", game::report::shot_list_text(chosen));
    host.restore_bottom();
    consoleSelect(&g_top);
}

// ---- sending ------------------------------------------------------------------------

std::string event_id()
{
    // Unique enough: the clock and the tick counter, mixed.
    uint64_t x = osGetTime() ^ (svcGetSystemTick() << 17);
    char id[33];
    for (int half = 0; half < 2; half++) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        std::snprintf(id + half * 16, 17, "%016llx", (unsigned long long)z);
    }
    return id;
}

std::string model_name(u8 m)
{
    static const char *const kModels[] = {"Old 3DS", "Old 3DS XL", "New 3DS", "Old 2DS", "New 3DS XL", "New 2DS XL"};
    return m < 6 ? kModels[m] : "unknown (" + std::to_string(m) + ")";
}

std::string region_name(u8 r)
{
    static const char *const kRegions[] = {"JPN", "USA", "EUR", "AUS", "CHN", "KOR", "TWN"};
    return r < 7 ? kRegions[r] : "unknown";
}

// The request body: the envelope, gzipped as it is made (sent with
// Content-Encoding: gzip, so Sentry keeps the attachments readable). Files
// are read in chunks: only the compressed result is ever held whole, and a
// long session's log compresses about 9:1.
class Gzip {
public:
    Gzip() { ok_ = deflateInit2(&z_, 6, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK; }
    ~Gzip() { deflateEnd(&z_); }
    void add(const void *p, size_t n) { run(p, n, Z_NO_FLUSH); }
    void add(const std::string &s) { add(s.data(), s.size()); }
    // `n` bytes of `f` from `from`. A file that comes up short (still being
    // written) leaves its item shorter than its header says: Sentry then
    // refuses the envelope, which shows as a failed send.
    void add_file(FILE *f, uint64_t from, uint64_t n)
    {
        if (n && std::fseek(f, (long)from, SEEK_SET) != 0)
            return;
        std::vector<uint8_t> buf(64 << 10);   // on the heap: .bss is resident during play
        while (n) {
            const size_t got = std::fread(buf.data(), 1, n < buf.size() ? (size_t)n : buf.size(), f);
            if (!got)
                return;
            add(buf.data(), got);
            n -= got;
        }
    }
    // The finished body; false if zlib failed (out of memory).
    bool finish(std::string &out)
    {
        run(nullptr, 0, Z_FINISH);
        out.swap(out_);
        return ok_;
    }

private:
    void run(const void *p, size_t n, int flush)
    {
        z_.next_in = (Bytef *)p;
        z_.avail_in = (uInt)n;
        uint8_t chunk[16 << 10];
        while (ok_) {
            z_.next_out = chunk;
            z_.avail_out = sizeof chunk;
            const int rc = deflate(&z_, flush);
            if (rc == Z_STREAM_ERROR) {
                ok_ = false;
                break;
            }
            out_.append((const char *)chunk, sizeof chunk - z_.avail_out);
            if (flush == Z_FINISH ? rc == Z_STREAM_END : z_.avail_in == 0 && z_.avail_out != 0)
                break;
        }
    }

    z_stream z_ = {};
    bool ok_ = false;
    std::string out_;
};

// A file as an attachment item: all of it up to `max`, else its first
// `head` bytes and the rest of `max` from its end. Nothing if it is empty
// or cannot be read.
void add_file_item(Gzip &gz, const std::string &path, const std::string &name, const char *type, uint64_t max,
                   uint64_t head)
{
    const long size = file_size(path);
    FILE *f = size > 0 ? std::fopen(path.c_str(), "rb") : nullptr;
    if (!f)
        return;
    const game::report::FileCut c = game::report::cut_file((uint64_t)size, max, head);
    gz.add(game::report::attachment_header(name, type, c.length()));
    gz.add_file(f, 0, c.head);
    gz.add(c.note);
    gz.add_file(f, c.tail_from, c.size - c.tail_from);
    gz.add("\n", 1);
    std::fclose(f);
}

// The sprite trace: binary records, so no note in it, only whole records
// from its start.
void add_trace_item(Gzip &gz, const std::string &path)
{
    const long size = file_size(path);
    FILE *f = size > 0 ? std::fopen(path.c_str(), "rb") : nullptr;
    if (!f)
        return;
    uint64_t n = (uint64_t)size < kTraceMax ? (uint64_t)size : kTraceMax;
    n -= n > 32 ? (n - 32) % kTraceRecord : 0;   // after the 32-byte header
    gz.add(game::report::attachment_header("sprites.trc", "application/octet-stream", n));
    gz.add_file(f, 0, n);
    gz.add("\n", 1);
    std::fclose(f);
}

// A screenshot as a JPEG attachment (issue #61): the game's as it is,
// Luma3DS's BMP made one. Nothing if it is gone or cannot be read.
void add_picture(Gzip &gz, const std::string &path, bool luma, const std::string &name)
{
    const long size = path.empty() ? 0 : file_size(path);
    if (size <= 0 || (uint64_t)size > kShotMax)
        return;
    if (!luma) {
        add_file_item(gz, path, name, "image/jpeg", kShotMax, kShotMax);
        return;
    }
    shot::Image img;
    std::string jpeg, msg;
    if (!shot::load(path, 0, 0, img, msg) || !shot::to_jpeg(img, jpeg, msg))
        return;
    gz.add(game::report::attachment_header(name, "image/jpeg", jpeg.size()));
    gz.add(jpeg);
    gz.add("\n", 1);
}

bool build_envelope(const Host &host, const Offer &o, const game::report::Dsn &dsn, std::string &body)
{
    namespace r = game::report;
    r::Event e;
    e.event_id = event_id();
    // No timestamp: Sentry's time of receipt. The console's clock is local
    // time in an unknown zone (a time ahead of Sentry's is flagged as
    // invalid); the session's start by that clock goes along as text.
    if (o.log.start_time())
        e.started = clock_text(o.log.start_time());
    const bool crashed = o.reason.compare(0, 7, "crashed") == 0;
    const bool asked = asked_for(o);
    e.level = crashed ? "fatal" : asked ? "info" : "error";
    // The release is the version the session was played on ("0.4.0-dev"
    // for any build but the release's own, issue #61); its build is the
    // dist (and the build_id tag), so a release gathers all its builds.
    const std::string version = version_of(o);
    e.version = version;
    if (!version.empty())
        e.release = "3ds-d2@" + version;
    e.dist = o.build;
    e.note = o.note;
    e.topic = o.topic;
    e.player_report = asked;
    e.reason = "Session: " + o.reason + ".";
    e.crash_lines = o.log.crash_lines();
    e.figures = o.log.figures();
    e.have_figures = e.figures.seconds != 0;
    // The first Luma3DS dump titles and groups a crash the app did not catch (issue #61).
    for (const std::string &d : o.dumps) {
        std::string bytes;
        if (read_all(o.dir + "/" + d, bytes) && r::parse_luma_dump(bytes, e.dump)) {
            e.have_dump = true;
            break;
        }
    }

    u8 model = 0xFF, region = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        CFGU_SecureInfoGetRegion(&region);
        cfguExit();
    }
    // The kernel's version (2.58-0), and the system's as Settings shows it
    // (11.17.0-50E), from the NAND's version titles.
    const u32 kernel = osGetFirmVersion();
    char kernel_s[24];
    std::snprintf(kernel_s, sizeof kernel_s, "%lu.%lu-%lu", (unsigned long)GET_VERSION_MAJOR(kernel),
                  (unsigned long)GET_VERSION_MINOR(kernel), (unsigned long)GET_VERSION_REVISION(kernel));
    char system_s[32] = "";
    OS_VersionBin nver = {}, cver = {};
    if (R_FAILED(osGetSystemVersionDataString(&nver, &cver, system_s, sizeof system_s)))
        system_s[0] = 0;
    e.tags = {{"build_id", o.build},
              {"version", version.empty() ? "unknown" : version},
              {"app", host.app},
              {"mode", host.game_files},
              {"profile", o.log.profiled().empty() ? "none" : o.log.profiled()},   // a profiler run's options
              {"model", model_name(model)},
              {"region", region_name(region)},
              {"kernel", kernel_s},
              {"reason", o.reason},
              {"app_build_id", D2_BUILD_ID},
              {"app_version", D2_VERSION}};
    if (system_s[0])
        e.tags.push_back({"firmware", system_s});
    e.device = {{"model", model_name(model)}, {"family", "Nintendo 3DS"}, {"arch", "armv6k"}};
    e.screenshots = o.shots;

    Gzip gz;
    gz.add(r::envelope_head(dsn, e));
    add_file_item(gz, o.dir + "/game.log", "game.log", "text/plain", kLogMax, kLogHead);
    add_file_item(gz, o.dir + "/stderr.txt", "stderr.txt", "text/plain", kStderrMax, 0);
    add_trace_item(gz, o.dir + "/sprites.trc");
    add_file_item(gz, o.dir + "/files.trc", "files.trc", "text/plain", kFileTraceMax, kFileTraceMax);
    for (const std::string &d : o.dumps)
        add_file_item(gz, o.dir + "/" + d, d, "application/octet-stream", kDumpMax, kDumpMax);
    for (size_t i = 0; i < o.shots.size(); i++) {
        const std::string n = "screenshot-" + std::to_string(i + 1);
        add_picture(gz, o.shots[i].top, o.shots[i].luma, n + "-top.jpg");
        add_picture(gz, o.shots[i].bot, o.shots[i].luma, n + "-bot.jpg");
    }
    return gz.finish(body);
}

// POSTs the envelope (ctr_tls.cpp: https through the TLS module). True on
// HTTP 200; `msg` says what happened, and goes to game.log with the address;
// `event_id` is what Sentry calls the report, if it said.
bool post(const Host &host, const game::report::Dsn &dsn, const std::string &body, std::string &msg,
          std::string &event_id)
{
    u32 wifi = 0;
    if (R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
    }
    if (!wifi) {
        msg = "No Wi-Fi connection.";
        return false;
    }
    const std::string url = dsn.envelope_url();
    const std::string headers = "User-Agent: 3ds-d2/" D2_VERSION "\r\n"
                                "Content-Type: application/x-sentry-envelope\r\n"
                                "Content-Encoding: gzip\r\n"
                                "X-Sentry-Auth: Sentry sentry_version=7, sentry_key=" +
                                dsn.key + ", sentry_client=3ds-d2/" D2_VERSION "\r\n";
    int status = 0;
    std::string answer, err;
    const uint64_t t0 = osGetTime();
    const bool answered = tls::post(url, headers, body, status, answer, err);
    const uint64_t took_ms = osGetTime() - t0;   // with screenshots, how long an upload takes (issue #61)
    if (!answered)
        msg = "Not sent: " + err + ".";
    else if (status != 200)
        msg = "Not sent: the server answered " + std::to_string(status) + ". " + printable(answer.substr(0, 120));
    else
        msg = "Sent.";
    // Sentry's answer to an accepted envelope: {"id":"<the event's ID>"}.
    event_id.clear();
    const size_t id = answer.find("\"id\":\"");
    if (status == 200 && id != std::string::npos) {
        const size_t end = answer.find('"', id + 6);
        if (end != std::string::npos && end - id - 6 <= 32)
            event_id = printable(answer.substr(id + 6, end - id - 6));
    }
    say(host, "report: " + url + " (" + std::to_string((body.size() + 1023) >> 10) + " KiB, " +
                  std::to_string(took_ms) + " ms): " + msg + (event_id.empty() ? "" : " Event " + event_id + "."));
    return answered && status == 200;
}

// After a report went: what Sentry calls it, until A.
bool confirm_sent(const Offer &o, const std::string &event_id)
{
    consoleSelect(&g_top);
    consoleClear();
    std::printf("\x1b[32m3ds-d2: report sent\x1b[0m\n\n");
    for (const std::string &l : wrap("Thank you. Sentry accepted the report, and the developer can now see it.", 49))
        std::printf("%s\n", l.c_str());
    std::printf("\n");
    if (!event_id.empty()) {
        std::printf("Report ID (Sentry's event ID):\n");
        std::printf("\x1b[36m  %s\x1b[0m\n\n", event_id.c_str());
        for (const std::string &l : wrap("To write about it on GitHub too, quote this ID.", 49))
            std::printf("%s\n", l.c_str());
        std::printf("\n");
    }
    std::printf("Its files stay on the SD card:\n");
    for (const std::string &l : wrap(o.dir.substr(o.dir.find(':') + 1), 47))
        std::printf("  %s\n", l.c_str());
    draw_keys({{"A", "continue"}});
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_A)
            return true;
        present();
    }
    return false;
}

Offer load_offer(const std::string &dir)
{
    Offer o;
    o.dir = dir;
    std::string text;
    read_all(dir + "/report.txt", text);
    o.reason = kv_get(text, "reason");
    o.build = kv_get(text, "build");
    read_all(dir + "/note.txt", o.note);
    // The topic set on this report, else (until sent) the one the player's
    // reports carry.
    if (!read_all(dir + "/topic.txt", o.topic) && !exists(dir + "/sent.txt"))
        read_all(kTopic, o.topic);
    o.topic = game::report::topic_text(o.topic);
    std::string shots;
    if (read_all(dir + "/screenshots.txt", shots))
        o.shots = game::report::parse_shot_list(shots);
    summarize(dir + "/game.log", o.log);
    if (o.build.empty())
        o.build = o.log.build().empty() ? "unknown" : o.log.build();
    if (o.reason.empty())
        o.reason = "ended";
    for (const std::string &n : list_dir(dir.c_str()))
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".dmp") == 0)
            o.dumps.push_back(n);
    std::string sent;
    if (read_all(dir + "/sent.txt", sent)) {
        o.state = "sent";
        o.event_id = kv_get(sent, "event_id");
    } else if (exists(dir + "/declined.txt")) {
        o.state = "not sent";
    }
    return o;
}

// Compresses and POSTs one report; on success, sent.txt with Sentry's ID.
bool send(const Host &host, Offer &o, const game::report::Dsn &dsn, std::string &msg)
{
    std::string body, event_id;
    if (!build_envelope(host, o, dsn, body)) {
        msg = "Not sent: out of memory compressing the report.";
        say(host, "report: " + msg);
        return false;
    }
    if (!post(host, dsn, body, msg, event_id))
        return false;
    write_all(o.dir + "/topic.txt", o.topic);   // as it was sent, whatever the next reports carry
    write_all(o.dir + "/sent.txt", "sent=" + std::to_string(unix_now()) + "\nevent_id=" + event_id + "\n");
    o.state = "sent";
    o.event_id = event_id;
    return true;
}

// One report's screen, until B. False: the app is closing. `sent`: it was
// sent from here; `status`: what the list says about it afterwards.
bool detail(const Host &host, const game::report::Dsn &dsn, const std::string &dir, bool &sent, std::string &status)
{
    sent = false;
    Offer o = load_offer(dir);
    bool redraw = true;
    while (aptMainLoop()) {
        if (redraw) {
            draw_offer(o);
            redraw = false;
        }
        hidScanInput();
        const u32 k = hidKeysDown() & ~KEY_TOUCH;
        if (k & KEY_SELECT) {
            // Twice: the folder and everything in it.
            if (o.confirm_delete) {
                remove_tree(dir);
                say(host, "report: deleted " + dir);
                status = "Deleted.";
                return true;
            }
            o.confirm_delete = true;
            o.status = "Delete this report and its files? SELECT again deletes it; any other button keeps it.";
            redraw = true;
        } else if (k) {
            if (o.confirm_delete) {
                o.confirm_delete = false;
                o.status.clear();
                redraw = true;
            }
            if (k & KEY_B) {
                return true;
            } else if ((k & KEY_Y) && o.state != "sent") {
                describe(host, o);
                redraw = true;
            } else if ((k & KEY_R) && o.state != "sent") {
                edit_topic(host, o);
                redraw = true;
            } else if ((k & KEY_L) && o.state != "sent") {
                pick_shots(host, o);
                redraw = true;
            } else if ((k & KEY_A) && o.state != "sent") {
                o.status = "Sending...";
                draw_offer(o);
                present();
                std::string msg;
                if (send(host, o, dsn, msg)) {
                    sent = true;
                    status = "Sent: " + o.event_id;
                    return confirm_sent(o, o.event_id);
                }
                o.status = msg;
                redraw = true;
            } else if ((k & KEY_X) && o.state.empty()) {
                write_all(dir + "/declined.txt", "declined=" + std::to_string(unix_now()) + "\n");
                say(host, "report: marked not to be sent (the files stay in sdmc:/3ds/3ds-d2/reports)");
                o.state = "not sent";
                o.status = "Marked: not to be sent. A sends it after all.";
                redraw = true;
            }
        }
        present();
    }
    return false;
}

// One report folder, as the list shows it.
struct Entry {
    std::string dir, when, reason, state, event_id;
};

bool is_waiting(const Entry &e) { return e.state == "to send"; }

// Every report folder, newest first.
std::vector<Entry> list_reports()
{
    std::vector<Entry> out;
    const std::vector<std::string> names = list_dir(kReports);
    for (auto n = names.rbegin(); n != names.rend(); ++n) {
        Entry e;
        e.dir = std::string(kReports) + "/" + *n;
        std::string text;
        if (!read_all(e.dir + "/report.txt", text))
            continue;
        e.reason = kv_get(text, "reason");
        if (e.reason.empty())
            e.reason = "ended";
        // The folder's name starts with the session's time: YYYYMMDD-HHMMSS.
        e.when = n->size() >= 15 ? n->substr(0, 4) + "-" + n->substr(4, 2) + "-" + n->substr(6, 2) + " " +
                                       n->substr(9, 2) + ":" + n->substr(11, 2)
                                 : *n;
        std::string sent;
        if (read_all(e.dir + "/sent.txt", sent)) {
            e.state = "sent";
            e.event_id = kv_get(sent, "event_id");
        } else {
            e.state = exists(e.dir + "/declined.txt") ? "not sent" : "to send";
        }
        out.push_back(e);
    }
    return out;
}

constexpr size_t kListRows = 20;   // rows 4..23

void draw_list(const std::vector<Entry> &list, size_t sel, size_t first, const std::string &status)
{
    consoleSelect(&g_top);
    consoleClear();
    const size_t waiting = (size_t)std::count_if(list.begin(), list.end(), is_waiting);
    std::printf("\x1b[33m3ds-d2: problem reports\x1b[0m\n");
    std::printf("%u to send, %u in all (newest first)\n\n", (unsigned)waiting, (unsigned)list.size());
    if (list.empty())
        std::printf("  No reports on the SD card.\n");
    for (size_t i = first; i < list.size() && i < first + kListRows; i++) {
        const Entry &e = list[i];
        const char *colour = is_waiting(e) ? "\x1b[36m" : e.state == "sent" ? "\x1b[32m" : "\x1b[37m";
        std::printf("%s%c %-16.16s %-21.21s %-8s\x1b[0m\n", colour, i == sel ? '>' : ' ', e.when.c_str(),
                    e.reason.c_str(), e.state.c_str());
    }
    draw_status(status);
    draw_keys({{"A", "open"}, {"X", "send all waiting"}, {"Up/Down", "choose"}, {"START", "continue"}});
}

// The reports: the list, and from it each report's screen. With `open_dir`,
// that report's screen first (a session that just ended). Leaves when START
// is pressed, or after a send that left nothing waiting. False: the app is
// closing.
bool manage(const Host &host, const std::string &open_dir)
{
    game::report::Dsn dsn;
    game::report::parse_dsn(kDsn, dsn);
    consoleInit(GFX_TOP, &g_top);
    std::vector<Entry> list = list_reports();
    size_t sel = 0, first = 0;
    std::string status;
    bool open = true, leave = false;
    const auto nothing_waiting = [&] { return std::none_of(list.begin(), list.end(), is_waiting); };
    if (!open_dir.empty()) {
        bool sent = false;
        open = detail(host, dsn, open_dir, sent, status);
        list = list_reports();
        leave = sent && nothing_waiting();
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].dir == open_dir)
                sel = i;
    }
    bool redraw = true;
    while (open && !leave && (open = aptMainLoop())) {
        if (sel >= list.size())
            sel = list.empty() ? 0 : list.size() - 1;
        if (sel < first)
            first = sel;
        if (sel >= first + kListRows)
            first = sel + 1 - kListRows;
        if (redraw) {
            draw_list(list, sel, first, status);
            redraw = false;
        }
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_START)
            break;
        if ((k & (KEY_DOWN | KEY_CPAD_DOWN)) && sel + 1 < list.size()) {
            sel++;
            redraw = true;
        } else if ((k & (KEY_UP | KEY_CPAD_UP)) && sel > 0) {
            sel--;
            redraw = true;
        } else if ((k & KEY_R) && !list.empty()) {
            sel = std::min(sel + kListRows, list.size() - 1);
            redraw = true;
        } else if (k & KEY_L) {
            sel = sel > kListRows ? sel - kListRows : 0;
            redraw = true;
        } else if ((k & KEY_A) && !list.empty()) {
            bool sent = false;
            status.clear();
            open = detail(host, dsn, list[sel].dir, sent, status);
            list = list_reports();
            leave = sent && nothing_waiting();
            redraw = true;
        } else if (k & KEY_X) {
            // Every waiting report, oldest first, until one fails (no
            // Wi-Fi: the rest wait for the next time).
            std::vector<std::string> todo;
            for (auto e = list.rbegin(); e != list.rend(); ++e)
                if (is_waiting(*e))
                    todo.push_back(e->dir);
            size_t done = 0;
            std::string msg;
            for (const std::string &dir : todo) {
                draw_list(list, sel, first,
                          "Sending " + std::to_string(done + 1) + " of " + std::to_string(todo.size()) + "...");
                present();
                Offer o = load_offer(dir);
                if (!send(host, o, dsn, msg))
                    break;
                done++;
            }
            if (todo.empty())
                status = "Nothing waiting to be sent.";
            else if (done == todo.size())
                status = "Sent " + std::to_string(done) + " of " + std::to_string(done) +
                         ". Each report's screen shows its ID.";
            else
                status = "Sent " + std::to_string(done) + " of " + std::to_string(todo.size()) + ". " + msg;
            list = list_reports();
            redraw = true;
        }
        present();
    }
    // The top screen back to the game's: double-buffered, black; messages
    // on the bottom screen again.
    consoleSelect(&g_top);
    consoleClear();
    gfxSetDoubleBuffering(GFX_TOP, true);
    for (int i = 0; i < 2; i++) {
        std::memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr), 0, 400 * 240 * 2);
        gfxFlushBuffers();
        gfxScreenSwapBuffers(GFX_TOP, false);
        gspWaitForVBlank();
    }
    consoleSelect(host.log);
    return open;
}

}  // namespace

// game.log's first line: when and which build, for the next start.
std::string start_line()
{
    return std::string(game::report::kSessionStart) + " " + std::to_string(unix_now()) + " build " + D2_BUILD_ID +
           " version " + D2_VERSION;
}

const char *closed_line() { return game::report::kSessionClosed; }

// Before game.log and stderr.txt are opened for the new session: if the
// last one did not close, its logs and Luma3DS's dumps go to a report
// folder. Returns the folder, or "".
std::string keep_unclosed_session()
{
    const std::string log = std::string(kLogDir) + "/game.log";
    LogSummary s;
    if (!summarize(log, s) || !s.started() || s.closed())
        return "";
    const std::string build = s.build().empty() ? "unknown" : s.build();
    const std::string dir = make_report_dir(s.start_time() ? s.start_time() : unix_now(), build);
    move_file(log, dir + "/game.log");
    move_file(std::string(kLogDir) + "/stderr.txt", dir + "/stderr.txt");
    move_file(std::string(kLogDir) + "/sprites.trc", dir + "/sprites.trc");
    move_file(std::string(kLogDir) + "/files.trc", dir + "/files.trc");
    const int dumps = s.start_time() ? copy_new_dumps(dir, s.start_time()) : 0;
    write_all(dir + "/report.txt", "reason=" + reason_of(s, dumps) + "\nbuild=" + build + "\n");
    if (!have_dsn())
        write_all(dir + "/declined.txt", "declined=no DSN in this build\n");
    prune_reports();
    return dir;
}

// After the game, with game.log closed: a copy of the session's logs, for
// `reason`. Returns the folder.
std::string keep_session(const char *reason)
{
    std::fflush(stderr);
    const std::string dir = make_report_dir(unix_now(), D2_BUILD_ID);
    copy_file(std::string(kLogDir) + "/game.log", dir + "/game.log");
    copy_file(std::string(kLogDir) + "/stderr.txt", dir + "/stderr.txt");
    copy_file(std::string(kLogDir) + "/sprites.trc", dir + "/sprites.trc");
    copy_file(std::string(kLogDir) + "/files.trc", dir + "/files.trc");
    write_all(dir + "/report.txt", std::string("reason=") + reason + "\nbuild=" + D2_BUILD_ID + "\n");
    prune_reports();
    return dir;
}

bool enabled() { return have_dsn(); }

// After a session: `dir`'s screen, then the list. False: the app is closing.
bool offer_one(const Host &host, const std::string &dir)
{
    if (!have_dsn())
        return true;
    return manage(host, dir);
}

// At the start: the list, if a report waits to be sent (one kept from an
// earlier session without the internet, or one cut short by a crash), or if
// SELECT is held. False: the app is closing.
bool offer_pending(const Host &host)
{
    if (!have_dsn())
        return true;
    hidScanInput();
    const bool asked = (hidKeysHeld() & KEY_SELECT) != 0;
    const std::vector<Entry> list = list_reports();
    if (!asked && std::none_of(list.begin(), list.end(), is_waiting))
        return true;
    return manage(host, "");
}

}  // namespace report
