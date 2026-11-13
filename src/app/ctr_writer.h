// Files the game's threads write to the SD card without waiting on it
// (issue #71). On the console a write to game.log cost about 20 ms per KiB:
// the heartbeat's ~11 KiB held the game ~213 ms every 10 s while profiling,
// a log line written in play carried the buffered lines with it, and one
// write took 2.4 s (3DS-D2-13, 2026-11-07).
//
// writer_wrap() puts a stdio stream in front of an open file: what is
// written to it goes into a ring in memory, and a thread of its own (on the
// drawing core, below the drawing thread) writes the ring out. The callers
// keep their stdio calls; a write costs a copy. A ring that is full drops
// what does not fit and counts it. The writer writes a file in batches of
// 16 KiB, or 5 s after its last write: the card's cost is per write, and a
// long session of small ones ended in stalls of many seconds (3DS-D2-14).
//
// The SD card's archive is closed while the HOME menu has the system: the
// writer is paused around that (writer_pause), after writing out what is
// queued. A crash (writer_crash) writes the queued bytes and every write
// after it from the crashing thread, as before.
#ifndef D2_APP_CTR_WRITER_H
#define D2_APP_CTR_WRITER_H

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace app {

// Starts the writer thread on `core` at priority `prio`. Without it, the
// wrapped streams write to their files at once.
bool writer_start(int core, int prio);

// A stream in front of `file`, whose writes go through `ring` (`bytes`,
// owned by the caller, alive until the stream is closed). `name` labels the
// stream's counters. fclose on the stream writes out what is queued, then
// closes `file`. Null (and `file` left open) if it cannot be made.
FILE *writer_wrap(FILE *file, char *ring, size_t bytes, const char *name);

// Writes out what is queued and ends the thread; the streams then write at
// once. At the app's end, after the last line.
void writer_stop();

// How soon queued bytes reach the card (the INFO checklist's "log writes"
// row): Batched, a file at 16 KiB queued or 5 s after its last write (the
// default); EverySecond, what is queued once a second; AtOnce, each write
// from the calling thread before it goes on, so that a crash, a freeze or
// a power-off loses nothing written before it (slower: for chasing crashes).
enum class WriterMode : uint8_t { Batched, EverySecond, AtOnce };
void writer_set_mode(WriterMode mode);

// Wakes the writer to look at its streams before its next round (a file is
// still written by the mode's rule).
void writer_kick();

// Writes out every stream's queued bytes from the calling thread.
void writer_drain();

// While paused the writer writes nothing; streams keep queueing. Pausing
// writes out what is queued first.
void writer_pause(bool paused);

// From now on each write goes to its file at once, from the calling thread,
// after what was queued.
void writer_crash();

// Per stream, since the start: bytes written, writes, the time in them, the
// longest one, and the bytes dropped for want of room.
struct WriterStats {
    const char *name;
    uint64_t bytes;
    uint32_t writes, ms, longest_ms, lost;
};
// Fills up to `n` stats, one per open stream; returns how many.
size_t writer_stats(WriterStats *out, size_t n);

}  // namespace app

#endif
