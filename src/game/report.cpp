// See report.h.
#include "game/report.h"

#include "game/profiler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace game {
namespace report {

std::string Dsn::envelope_url() const { return scheme + "://" + host + "/api/" + project + "/envelope/"; }

std::string Dsn::text() const { return scheme + "://" + key + "@" + host + "/" + project; }

bool parse_dsn(const std::string &s, Dsn &out)
{
    const size_t colon = s.find("://");
    if (colon == std::string::npos)
        return false;
    Dsn d;
    d.scheme = s.substr(0, colon);
    if (d.scheme != "https" && d.scheme != "http")
        return false;
    const std::string rest = s.substr(colon + 3);
    const size_t at = rest.find('@'), slash = rest.rfind('/');
    if (at == std::string::npos || slash == std::string::npos || slash < at)
        return false;
    d.key = rest.substr(0, at);
    const size_t secret = d.key.find(':');   // the old key:secret form; the secret is not used
    if (secret != std::string::npos)
        d.key.resize(secret);
    d.host = rest.substr(at + 1, slash - at - 1);   // may carry a path prefix (a relay's)
    d.project = rest.substr(slash + 1);
    if (d.key.empty() || d.host.empty() || d.project.empty())
        return false;
    for (char c : d.project)
        if (c < '0' || c > '9')
            return false;
    out = d;
    return true;
}

// ---- game.log --------------------------------------------------------------------------

namespace {

// The number after `key` in `line`, if `key` is there.
bool field(const char *line, const char *key, uint64_t &v)
{
    const char *p = std::strstr(line, key);
    if (!p)
        return false;
    p += std::strlen(key);
    if (*p < '0' || *p > '9')
        return false;
    v = std::strtoull(p, nullptr, 10);
    return true;
}

// "<key><used>/<peak>": the peak.
bool peak_field(const char *line, const char *key, uint64_t &v)
{
    const char *p = std::strstr(line, key);
    if (!p)
        return false;
    p = std::strchr(p, '/');
    if (!p)
        return false;
    v = std::strtoull(p + 1, nullptr, 10);
    return true;
}

bool is_crash_line(const char *s)
{
    return std::strncmp(s, "crash:", 6) == 0 || std::strncmp(s, "FATAL", 5) == 0 || std::strstr(s, "[rt] FATAL") ||
           std::strstr(s, "game: fatal exit(") || std::strncmp(s, "Diablo II ran out of memory", 27) == 0 ||
           std::strncmp(s, "Diablo II stopped on an internal error", 38) == 0;
}

float pct(std::vector<float> v, float p)
{
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)(v.size() * p);
    return v[i < v.size() ? i : v.size() - 1];
}

}  // namespace

void LogSummary::feed(const char *line)
{
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    const char *l = s.c_str();
    const size_t start_len = sizeof kSessionStart - 1;
    if (std::strncmp(l, kSessionStart, start_len) == 0) {
        started_ = true;
        uint64_t t = 0;
        if (std::sscanf(l + start_len, " %llu", (unsigned long long *)&t) == 1)
            start_time_ = t;
        // Each a word: the build ID, then the app's version (0.4.0 and later).
        if (const char *b = std::strstr(l, " build "))
            build_ = std::string(b + 7, std::strcspn(b + 7, " "));
        if (const char *v = std::strstr(l, " version "))
            version_ = std::string(v + 9, std::strcspn(v + 9, " "));
        return;
    }
    if (std::strncmp(l, kLegacyStart, sizeof kLegacyStart - 1) == 0) {
        started_ = true;
        return;
    }
    if (std::strncmp(l, kSessionClosed, sizeof kSessionClosed - 1) == 0 ||
        std::strncmp(l, kLegacyEnd, sizeof kLegacyEnd - 1) == 0) {
        closed_ = true;
        return;
    }
    if (is_crash_line(l)) {
        if (crash_.size() < 40)
            crash_.push_back(s);
        return;
    }
    if (const char *st = std::strstr(l, "game state: ")) {
        in_game_ = std::strncmp(st + 12, "in game", 7) == 0;
        return;
    }
    const size_t run_len = sizeof kProfilerRunLine - 1;
    if (std::strncmp(l, kProfilerRunLine, run_len) == 0) {
        if (std::strncmp(l + run_len, "start ", 6) == 0)
            for (const char *p = l + run_len + 6; *p;) {
                const std::string name(p, std::strcspn(p, ","));
                p += name.size() + (p[name.size()] ? 1 : 0);
                if (!name.empty() && ("," + profiled_ + ",").find("," + name + ",") == std::string::npos)
                    profiled_ += (profiled_.empty() ? "" : ",") + name;
            }
        return;
    }
    uint64_t slow = 0, stalled = 0, longest = 0;
    if (std::strstr(l, "prof hitch: ") && field(l, "<=250ms ", slow) && field(l, ">250ms ", stalled) &&
        field(l, "longest ", longest)) {
        if (in_game_) {
            f_.hitches_100ms += (uint32_t)(slow + stalled);
            f_.longest_frame_ms = std::max(f_.longest_frame_ms, (uint32_t)longest);
        }
        return;
    }
    uint64_t ms = 0, frames = 0, switches = 0;
    if (std::strstr(l, "prof: ") && field(l, "prof: ", ms) && field(l, "frames ", frames) &&
        field(l, "switches ", switches)) {
        // A window with neither frames nor thread switches is a suspend
        // (the HOME menu, the lid closed).
        if (in_game_ && frames && ms)
            play_fps_.push_back(frames * 1000.0f / ms);
        return;
    }
    uint64_t in_use = 0, held = 0;
    if (std::strstr(l, "sprite frames ") && field(l, "sprite frames ", in_use) && peak_field(l, "sprite frames ", held)) {
        uint64_t v = 0;
        f_.frames_held_peak_kib = std::max(f_.frames_held_peak_kib, (uint32_t)held);
        if (held > in_use)
            f_.frames_unused_peak_kib = std::max(f_.frames_unused_peak_kib, (uint32_t)(held - in_use));
        if (field(l, "over 256 KiB ", v))
            f_.frames_over_256k = (uint32_t)v;
        if (field(l, "to the pool ", v))
            f_.frames_to_pool = (uint32_t)v;
        return;
    }
    uint64_t t = 0;
    if (std::strstr(l, "t=") && field(l, "t=", t) && std::strstr(l, " heap=")) {
        uint64_t v = 0;
        f_.seconds = (uint32_t)t;
        if (peak_field(l, " heap=", v))
            f_.heap_peak_kib = std::max(f_.heap_peak_kib, (uint32_t)v);
        if (peak_field(l, " va=", v))
            f_.va_peak_kib = std::max(f_.va_peak_kib, (uint32_t)v);
        if (field(l, " free=", v))
            f_.pool_free_min_kib = have_beat_ ? std::min(f_.pool_free_min_kib, (uint32_t)v) : (uint32_t)v;
        if (field(l, " jit=", v))
            f_.jit_kib = std::max(f_.jit_kib, (uint32_t)v);
        if (field(l, " host=", v))
            f_.host_peak_kib = std::max(f_.host_peak_kib, (uint32_t)v);
        have_beat_ = true;
    }
}

LogSummary::Figures LogSummary::figures() const
{
    Figures f = f_;
    f.play_windows = (uint32_t)play_fps_.size();
    f.fps_median = pct(play_fps_, 0.5f);
    f.fps_p10 = pct(play_fps_, 0.1f);
    return f;
}

FileCut cut_file(uint64_t size, uint64_t max, uint64_t head)
{
    FileCut c;
    c.size = size;
    if (size <= max) {
        c.head = size;
        c.tail_from = size;
        return c;
    }
    c.head = head < max ? head : max;
    char note[96];
    std::snprintf(note, sizeof note, "\n[... %llu bytes left out of the report ...]\n",
                  (unsigned long long)(size - max));
    c.note = note;
    c.tail_from = c.head + (size - max);   // what is left of the budget, from the end
    return c;
}

std::string attachment_header(const std::string &filename, const std::string &content_type, uint64_t length)
{
    return "{\"type\":\"attachment\",\"length\":" + std::to_string(length) + ",\"filename\":" + json_string(filename) +
           ",\"content_type\":" + json_string(content_type) + "}\n";
}

// ---- Sentry ----------------------------------------------------------------------------

std::string json_string(const std::string &s)
{
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char u[8];
                std::snprintf(u, sizeof u, "\\u%04x", c);
                o += u;
            } else {
                o += (char)c;   // UTF-8 passes through
            }
        }
    }
    return o + "\"";
}

namespace {

std::string json_object(const std::vector<std::pair<std::string, std::string>> &kv)
{
    std::string o = "{";
    for (size_t i = 0; i < kv.size(); i++)
        o += (i ? "," : "") + json_string(kv[i].first) + ":" + json_string(kv[i].second);
    return o + "}";
}

std::string num(double v, int decimals)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

// The exception for a crash line: its kind as the type, the line as the value.
std::pair<std::string, std::string> exception_of(const std::string &line)
{
    const char *l = line.c_str();
    if (std::strncmp(l, "crash: ", 7) == 0) {
        const char *at = std::strstr(l, " at pc ");
        if (at)
            return {std::string(l + 7, at), line};
        return {"crash", line};
    }
    if (std::strstr(l, "out of memory"))
        return {"out of memory", line};
    if (std::strstr(l, "fatal exit") || std::strstr(l, "internal error"))
        return {"fatal exit", line};
    return {"FATAL", line};
}

}  // namespace

std::string event_json(const Event &e)
{
    std::string o = "{\"event_id\":" + json_string(e.event_id);
    if (e.timestamp)
        o += ",\"timestamp\":" + std::to_string(e.timestamp);
    o += ",\"platform\":\"native\",\"logger\":\"3ds-d2\"";
    o += ",\"level\":" + json_string(e.level);
    if (!e.release.empty())
        o += ",\"release\":" + json_string(e.release);
    if (!e.dist.empty())
        o += ",\"dist\":" + json_string(e.dist);
    // The player's words are the message; without them, the reason.
    o += ",\"message\":{\"formatted\":" + json_string(e.message.empty() ? e.reason : e.message) + "}";
    if (!e.crash_lines.empty()) {
        // The first `crash:` line names the fault; otherwise the first line.
        size_t first = 0;
        for (size_t i = 0; i < e.crash_lines.size(); i++)
            if (e.crash_lines[i].compare(0, 7, "crash: ") == 0) {
                first = i;
                break;
            }
        const auto ex = exception_of(e.crash_lines[first]);
        o += ",\"exception\":{\"values\":[{\"type\":" + json_string(ex.first) + ",\"value\":" + json_string(ex.second) +
             "}]}";
    }
    o += ",\"tags\":" + json_object(e.tags);
    o += ",\"contexts\":{\"device\":" + json_object(e.device);
    if (e.have_figures) {
        const LogSummary::Figures &f = e.figures;
        o += ",\"session\":{\"type\":\"session\",\"seconds\":" + std::to_string(f.seconds) +
             ",\"play_windows\":" + std::to_string(f.play_windows) + ",\"fps_median_play\":" + num(f.fps_median, 1) +
             ",\"fps_p10_play\":" + num(f.fps_p10, 1) + ",\"pool_free_min_kib\":" + std::to_string(f.pool_free_min_kib) +
             ",\"guest_heap_peak_kib\":" + std::to_string(f.heap_peak_kib) +
             ",\"guest_va_peak_kib\":" + std::to_string(f.va_peak_kib) +
             ",\"host_peak_kib\":" + std::to_string(f.host_peak_kib) + ",\"jit_kib\":" + std::to_string(f.jit_kib) +
             ",\"hitches_100ms_play\":" + std::to_string(f.hitches_100ms) +
             ",\"longest_frame_ms_play\":" + std::to_string(f.longest_frame_ms);
        if (f.frames_held_peak_kib)
            o += ",\"sprite_frames_held_peak_kib\":" + std::to_string(f.frames_held_peak_kib) +
                 ",\"sprite_frames_unused_peak_kib\":" + std::to_string(f.frames_unused_peak_kib) +
                 ",\"sprite_frames_over_256k\":" + std::to_string(f.frames_over_256k) +
                 ",\"sprite_frames_to_pool\":" + std::to_string(f.frames_to_pool);
        o += "}";
    }
    o += "}";
    std::string lines;
    for (const std::string &l : e.crash_lines)
        lines += l + "\n";
    o += ",\"extra\":{\"reason\":" + json_string(e.reason);
    if (!e.started.empty())
        o += ",\"session_started_console_clock\":" + json_string(e.started);
    if (!lines.empty())
        o += ",\"crash_lines\":" + json_string(lines);
    o += "}}";
    return o;
}

std::string envelope_head(const Dsn &dsn, const Event &e)
{
    const std::string ev = event_json(e);
    return "{\"event_id\":" + json_string(e.event_id) + ",\"dsn\":" + json_string(dsn.text()) + "}\n" +
           "{\"type\":\"event\",\"length\":" + std::to_string(ev.size()) + "}\n" + ev + "\n";
}

std::string envelope(const Dsn &dsn, const Event &e, const std::vector<Attachment> &attachments)
{
    std::string o = envelope_head(dsn, e);
    for (const Attachment &a : attachments)
        o += attachment_header(a.filename, a.content_type, a.data.size()) + a.data + "\n";
    return o;
}

}  // namespace report
}  // namespace game
