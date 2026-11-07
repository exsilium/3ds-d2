#include "app/ctr_writer.h"

#include <3ds.h>

#include <algorithm>
#include <cstring>

namespace app {
namespace {

constexpr size_t kStreams = 4;
// A write at most this long: the FS service takes one request at a time,
// and the game's own reads (the loader, on this core too) wait behind it.
constexpr size_t kChunk = 4 * 1024;
constexpr s64 kRoundNs = 500 * 1000000LL;   // a round at least this often

struct Stream {
    bool used = false;
    const char *name = nullptr;
    FILE *file = nullptr;
    FILE *front = nullptr;   // the funopen stream the callers write to
    char *ring = nullptr;
    size_t cap = 0;
    size_t head = 0, tail = 0;   // bytes queued and written since the start (mod 2^32: differences only)
    LightLock ring_lock;         // head, tail, lost
    LightLock io_lock;           // one writer of `file` at a time
    uint64_t bytes = 0, us = 0;
    uint32_t writes = 0, longest_us = 0, lost = 0;
};

Stream g_streams[kStreams];
LightLock g_list_lock;
LightEvent g_wake;
Thread g_thread = nullptr;
volatile bool g_quit = false, g_paused = false, g_crashed = false;
bool g_init = false;

void init_once()
{
    if (g_init)
        return;
    g_init = true;
    LightLock_Init(&g_list_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    // Once: the writer may be waiting on a closed stream's lock when its
    // slot is taken again.
    for (Stream &s : g_streams) {
        LightLock_Init(&s.ring_lock);
        LightLock_Init(&s.io_lock);
    }
}

uint64_t now_us() { return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000); }

// One chunk of `s`'s queue to its file; false if there is none. The caller
// holds io_lock.
bool write_chunk(Stream &s)
{
    LightLock_Lock(&s.ring_lock);
    const size_t h = s.head, t = s.tail;
    LightLock_Unlock(&s.ring_lock);
    if (h == t || !s.file)
        return false;
    const size_t off = t % s.cap;
    const size_t len = std::min({h - t, s.cap - off, kChunk});
    const uint64_t t0 = now_us();
    std::fwrite(s.ring + off, 1, len, s.file);
    std::fflush(s.file);
    const uint64_t dt = now_us() - t0;
    s.bytes += len;
    s.writes++;
    s.us += dt;
    s.longest_us = std::max<uint32_t>(s.longest_us, (uint32_t)std::min<uint64_t>(dt, UINT32_MAX));
    LightLock_Lock(&s.ring_lock);
    s.tail = t + len;   // written or failed: either way it is not queued any more
    LightLock_Unlock(&s.ring_lock);
    return true;
}

// Everything queued in `s`. The writer thread stops at a pause or a crash.
void write_out(Stream &s, bool by_writer)
{
    LightLock_Lock(&s.io_lock);
    if (s.used)
        while (!(by_writer && (g_paused || g_crashed)) && write_chunk(s)) {
        }
    LightLock_Unlock(&s.io_lock);
}

// A crash: the queue and `buf` straight to the file, without waiting for a
// lock the crashed thread may hold.
void write_now(Stream &s, const char *buf, size_t n)
{
    const bool locked = LightLock_TryLock(&s.io_lock) == 0;
    while (write_chunk(s)) {
    }
    if (n && s.file) {
        std::fwrite(buf, 1, n, s.file);
        std::fflush(s.file);
    }
    if (locked)
        LightLock_Unlock(&s.io_lock);
}

// newlib's funopen: the size's type is _READ_WRITE_BUFSIZE_TYPE (size_t here).
int stream_write(void *cookie, const char *buf, _READ_WRITE_BUFSIZE_TYPE size)
{
    Stream &s = *static_cast<Stream *>(cookie);
    const int n = (int)size;
    if (n <= 0)
        return 0;
    if (g_crashed || !g_thread) {
        write_now(s, buf, (size_t)n);
        return n;
    }
    LightLock_Lock(&s.ring_lock);
    const size_t room = s.cap - (s.head - s.tail);
    if ((size_t)n > room) {
        s.lost += (uint32_t)n;   // dropped, counted: the game does not wait for the card
    } else {
        const size_t off = s.head % s.cap, first = std::min((size_t)n, s.cap - off);
        std::memcpy(s.ring + off, buf, first);
        std::memcpy(s.ring, buf + first, (size_t)n - first);
        s.head += (size_t)n;
    }
    LightLock_Unlock(&s.ring_lock);
    return n;
}

int stream_close(void *cookie)
{
    Stream &s = *static_cast<Stream *>(cookie);
    LightLock_Lock(&s.io_lock);
    while (write_chunk(s)) {
    }
    const int rc = s.file ? std::fclose(s.file) : 0;
    LightLock_Lock(&g_list_lock);
    s.used = false;
    s.file = nullptr;
    s.front = nullptr;
    LightLock_Unlock(&g_list_lock);
    LightLock_Unlock(&s.io_lock);
    return rc;
}

void writer_main(void *)
{
    while (!g_quit) {
        LightEvent_WaitTimeout(&g_wake, kRoundNs);
        for (Stream &s : g_streams)
            if (s.used)
                write_out(s, true);
    }
}

}  // namespace

bool writer_start(int core, int prio)
{
    init_once();
    if (g_thread)
        return true;
    g_quit = false;
    g_thread = threadCreate(writer_main, nullptr, 8 * 1024, prio, core, false);
    return g_thread != nullptr;
}

void writer_stop()
{
    if (!g_thread)
        return;
    g_quit = true;
    LightEvent_Signal(&g_wake);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = nullptr;
    writer_drain();
}

FILE *writer_wrap(FILE *file, char *ring, size_t bytes, const char *name)
{
    init_once();
    if (!file || !ring || !bytes)
        return nullptr;
    LightLock_Lock(&g_list_lock);
    Stream *s = nullptr;
    for (Stream &c : g_streams)
        if (!c.used && !c.front) {
            s = &c;
            break;
        }
    if (s) {
        LightLock_Lock(&s->io_lock);
        s->head = s->tail = 0;
        s->bytes = s->us = 0;
        s->writes = s->longest_us = s->lost = 0;
        s->name = name;
        s->file = file;
        s->ring = ring;
        s->cap = bytes;
        s->front = funopen(s, nullptr, stream_write, nullptr, stream_close);
        if (s->front) {
            // Each write reaches the ring as it is made: the lines keep their
            // order across threads, and fflush has nothing left to do.
            std::setvbuf(s->front, nullptr, _IONBF, 0);
            std::setvbuf(file, nullptr, _IONBF, 0);   // the ring's chunks go straight to the card
            s->used = true;
        }
        LightLock_Unlock(&s->io_lock);
    }
    LightLock_Unlock(&g_list_lock);
    return s ? s->front : nullptr;
}

void writer_kick()
{
    if (g_thread)
        LightEvent_Signal(&g_wake);
}

void writer_drain()
{
    for (Stream &s : g_streams)
        if (s.used)
            write_out(s, false);
}

void writer_pause(bool paused)
{
    if (paused) {
        g_paused = true;   // the writer stops after its chunk in flight
        writer_drain();
    } else {
        g_paused = false;
        writer_kick();
    }
}

void writer_crash()
{
    g_crashed = true;
    for (Stream &s : g_streams)
        if (s.used)
            write_now(s, nullptr, 0);
}

size_t writer_stats(WriterStats *out, size_t n)
{
    size_t k = 0;
    for (Stream &s : g_streams)
        if (s.used && k < n)
            out[k++] = {s.name, s.bytes, s.writes, (uint32_t)(s.us / 1000), s.longest_us / 1000, s.lost};
    return k;
}

}  // namespace app
