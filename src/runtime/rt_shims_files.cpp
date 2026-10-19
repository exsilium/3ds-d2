// KERNEL32 file system: paths, files, directories, find, INI files, mappings.
//
// The guest sees one drive, C:\, whose root is the program's directory. A guest
// path resolves against two host roots: write_root (saves, settings, anything
// the program creates) and data_root (the read-only game install). Reads take
// the first root holding the name; writes always land in write_root.
//
// Windows names are case-insensitive. The 3DS SD card (FAT) already is, so
// there a path is used as-is. A case-sensitive file system (the Linux lanes,
// a CIA's RomFS) matches each component against a cached directory listing.
//
// Cheap on the ARM11: ReadFile/WriteFile move bytes straight between the host
// stream and guest memory (no staging buffer), file handles are a fixed-size
// table, and directory listings are read once.
//
// Read-only files from the game install (the MPQs) bypass stdio. The game
// reads them in small sector-sized pieces with a seek before most of them,
// and stdio dropped its small buffer at every fseek: on the console each
// call became an SD access, at 400-750 KiB/s. Such a file is an unbuffered
// descriptor under a per-handle block cache (ReadCache), and its position is
// kept here, so SetFilePointer never reaches the file system. Files the
// guest may write keep stdio, so no cache can go stale against a write.
#include "runtime/rt_host.h"
#include "runtime/rt_layout.h"

#include "runtime/bridge.h"
#include "runtime/cpu.h"
#include "runtime/guest_region.h"
#include "runtime/guest_sync.h"
#include "runtime/guest_thread_ctx.h"
#include "runtime/host_clock.h"
#include "runtime/sched_cooperative.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

using namespace d2rt;

namespace rt {
namespace {

constexpr uint32_t kInvalidHandle = 0xFFFFFFFFu;
constexpr uint32_t kStdInput = 0xFFFFFFF6u, kStdOutput = 0xFFFFFFF5u, kStdError = 0xFFFFFFF4u;
constexpr uint32_t kInvalidAttributes = 0xFFFFFFFFu;
constexpr uint32_t kAttrReadOnly = 0x01, kAttrDirectory = 0x10, kAttrArchive = 0x20;

enum : uint32_t {
    ERROR_FILE_NOT_FOUND = 2,
    ERROR_PATH_NOT_FOUND = 3,
    ERROR_ACCESS_DENIED = 5,
    ERROR_INVALID_HANDLE = 6,
    ERROR_NO_MORE_FILES = 18,
    ERROR_HANDLE_EOF = 38,
    ERROR_FILE_EXISTS = 80,
    ERROR_INVALID_PARAMETER = 87,
    ERROR_INSUFFICIENT_BUFFER = 122,
    ERROR_ALREADY_EXISTS = 183,
};

std::string lower(std::string s)
{
    for (char &ch : s)
        ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// ---- paths -----------------------------------------------------------------------

std::string g_cwd;   // current directory, relative to C:\ ('/'-separated, no leading or trailing slash)

// Guest path -> '/'-separated path relative to C:\, with "." and ".." applied.
// Every drive letter maps to the one drive.
std::string normalize(const std::string &guest)
{
    std::string p = guest;
    if (p.compare(0, 4, "\\\\?\\") == 0)
        p.erase(0, 4);
    bool absolute = false;
    if (p.size() >= 2 && p[1] == ':') {
        p.erase(0, 2);
        absolute = !p.empty() && (p[0] == '\\' || p[0] == '/');
    } else {
        absolute = !p.empty() && (p[0] == '\\' || p[0] == '/');
    }
    std::vector<std::string> parts;
    auto split_into = [&](const std::string &s) {
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find_first_of("\\/", i);
            if (j == std::string::npos)
                j = s.size();
            std::string comp = s.substr(i, j - i);
            // Windows drops trailing dots and spaces from a component.
            while (!comp.empty() && comp != "." && comp != ".." && (comp.back() == ' ' || comp.back() == '.'))
                comp.pop_back();
            if (comp == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!comp.empty() && comp != ".") {
                parts.push_back(comp);
            }
            i = j + 1;
        }
    };
    if (!absolute)
        split_into(g_cwd);
    split_into(p);
    std::string out;
    for (const std::string &c : parts) {
        if (!out.empty())
            out += '/';
        out += c;
    }
    return out;
}

std::string to_guest(const std::string &rel)
{
    std::string g = "C:\\" + rel;
    for (char &ch : g)
        if (ch == '/')
            ch = '\\';
    return g;
}

std::string parent_of(const std::string &rel)
{
    const size_t k = rel.rfind('/');
    return k == std::string::npos ? std::string() : rel.substr(0, k);
}

std::string leaf_of(const std::string &rel)
{
    const size_t k = rel.rfind('/');
    return k == std::string::npos ? rel : rel.substr(k + 1);
}

bool host_stat(const std::string &host, struct stat &st) { return ::stat(host.c_str(), &st) == 0; }

// Directory listings, lower-case name -> actual name, keyed by host directory.
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_dirs;

const std::unordered_map<std::string, std::string> *listing(const std::string &dir)
{
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end())
        return &it->second;
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return nullptr;
    auto &m = g_dirs[dir];
    while (dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n != "." && n != "..")
            m.emplace(lower(n), n);
    }
    ::closedir(d);
    return &m;
}

// The 3DS SD card (FAT) is case-insensitive already: a path is used as given.
// Everything else (the Linux lanes, a CIA's RomFS) matches each component
// against a cached directory listing.
bool case_insensitive_fs(const std::string &root)
{
#ifdef __3DS__
    return root.compare(0, 5, "sdmc:") == 0;
#else
    (void)root;
    return false;
#endif
}

bool find_ci(const std::string &root, const std::string &rel, std::string &host)
{
    struct stat st;
    if (case_insensitive_fs(root)) {
        host = rel.empty() ? root : root + "/" + rel;
        return host_stat(host, st);
    }
    host = root;
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos)
            j = rel.size();
        const auto *names = listing(host);
        if (!names)
            return false;
        auto it = names->find(lower(rel.substr(i, j - i)));
        if (it == names->end())
            return false;
        host += "/" + it->second;
        i = j + 1;
    }
    return host_stat(host, st);
}

void forget_dir(const std::string &dir) { g_dirs.erase(dir); }

const std::string &write_root() { return options().write_root; }
const std::string &data_root() { return options().data_root; }
bool split_roots() { return write_root() != data_root(); }

bool resolve_read(const std::string &rel, std::string &host)
{
    if (find_ci(write_root(), rel, host))
        return true;
    return split_roots() && find_ci(data_root(), rel, host);
}

bool copy_host_file(const std::string &from, const std::string &to)
{
    FILE *in = std::fopen(from.c_str(), "rb");
    if (!in)
        return false;
    FILE *out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    static char buf[16384];
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0)
        ok = ok && std::fwrite(buf, 1, n, out) == n;
    std::fclose(in);
    return std::fclose(out) == 0 && ok;
}

// Host directory in write_root for `rel`, created if the guest could see it
// (in either root). Empty when it exists in neither.
bool ensure_write_dir(const std::string &rel, std::string &host)
{
    if (find_ci(write_root(), rel, host))
        return true;
    std::string data;
    if (!split_roots() || !find_ci(data_root(), rel, data))
        return false;
    std::string parent;
    if (!ensure_write_dir(parent_of(rel), parent))
        return false;
    host = parent + "/" + leaf_of(data.substr(data_root().size()));
    ::mkdir(host.c_str(), 0777);
    forget_dir(parent);
    return true;
}

// Host path in write_root for writing `rel`. copy_up: an existing data_root
// file is copied first (open for update, append, rename).
bool resolve_write(const std::string &rel, bool copy_up, std::string &host)
{
    if (find_ci(write_root(), rel, host))
        return true;
    std::string dir;
    if (!ensure_write_dir(parent_of(rel), dir))
        return false;
    std::string data;
    const bool in_data = split_roots() && find_ci(data_root(), rel, data);
    host = dir + "/" + (in_data ? leaf_of(data) : leaf_of(rel));
    if (in_data && copy_up && !copy_host_file(data, host))
        return false;
    forget_dir(dir);
    return true;
}

// ---- handles -----------------------------------------------------------------------

FileStats g_file_stats;

// A read-only file's recently read blocks. kBlock-aligned blocks of the file,
// filled whole by one read each; least recently used is replaced. Reads of a
// block or more go straight to their destination.
//
// Sizes, from the console's own SD costs (sd bench: ~1.85 ms per read plus
// ~0.09 ms per KiB) replayed over the game's reads (RT_FILETRACE on the
// Linux lane): 16 KiB blocks, 8 per file, and a miss on the block right
// after the previous fill reading the next one too, take ~27% less SD time
// than 8 KiB blocks without read-ahead; larger blocks or more read-ahead
// read too much the game never uses. Memory is tight (4 MiB of blocks ran
// the host heap out in Blood Moor), so blocks are allocated on first use
// and capped in total (kCap): past it, a file reuses its own blocks.
class ReadCache {
public:
    static constexpr uint32_t kBlock = 16 * 1024, kBlocks = 8;
    static constexpr uint64_t kCap = 1u << 20;

    ReadCache(int fd, uint64_t size) : fd_(fd), size_(size) {}
    ~ReadCache()
    {
        for (const Block &b : blocks_)
            if (b.data)
                g_file_stats.cache_bytes -= kBlock;
    }

    // Threads: one thread fills blocks (read(): the game's own thread, or,
    // when reads are asynchronous, the I/O worker alone); try_read() may run
    // on another at the same time. The lock covers the block table and try_read's copy,
    // never a disk read: a block being filled is unlisted (base ~0) until its
    // bytes are in.
    std::atomic<int> inflight{0};   // asynchronous requests on this file not yet done

    // Copies n bytes at `pos` to dst if every byte is already cached (or
    // past EOF); false, with nothing done, otherwise.
    bool try_read(uint64_t pos, uint8_t *dst, uint32_t n, uint32_t &got)
    {
        got = 0;
        if (pos >= size_)
            return true;
        if (n > size_ - pos)
            n = (uint32_t)(size_ - pos);
        if (n >= kBlock)
            return false;
        Guard g(lock_);
        for (uint64_t at = pos - pos % kBlock; at < pos + n; at += kBlock)
            if (!find(at))
                return false;
        while (got < n) {
            const uint64_t at = pos + got;
            Block *b = find(at - at % kBlock);
            const uint32_t off = (uint32_t)(at - b->base);
            if (off >= b->len)
                break;
            const uint32_t take = b->len - off < n - got ? b->len - off : n - got;
            std::memcpy(dst + got, b->data.get() + off, take);
            b->used = ++clock_;
            g_file_stats.hits++;
            got += take;
        }
        return true;
    }

    // Copies up to n bytes at `pos` to dst; returns how many (fewer at EOF).
    // Fills blocks from the file as needed: the filling thread only.
    uint32_t read(uint64_t pos, uint8_t *dst, uint32_t n)
    {
        if (pos >= size_)
            return 0;
        if (n > size_ - pos)
            n = (uint32_t)(size_ - pos);
        if (n >= kBlock) {
            g_file_stats.direct++;
            return disk_read(pos, dst, n);
        }
        uint32_t done = 0;
        while (done < n) {
            const uint64_t at = pos + done;
            const Block *b = block_at(at - at % kBlock);
            if (!b)
                break;
            const uint32_t off = (uint32_t)(at - b->base);
            if (off >= b->len)
                break;
            const uint32_t take = b->len - off < n - done ? b->len - off : n - done;
            std::memcpy(dst + done, b->data.get() + off, take);
            done += take;
        }
        return done;
    }

private:
    struct Block {
        uint64_t base = ~0ull;
        uint32_t len = 0, used = 0;
        std::unique_ptr<uint8_t[]> data;
    };

    struct Guard {
        std::atomic_flag &f;
        explicit Guard(std::atomic_flag &flag) : f(flag)
        {
            while (f.test_and_set(std::memory_order_acquire)) {
            }
        }
        ~Guard() { f.clear(std::memory_order_release); }
    };

    Block *find(uint64_t base)
    {
        for (Block &b : blocks_)
            if (b.base == base && b.data)
                return &b;
        return nullptr;
    }

    // The block at `base`, read from the file if it isn't cached. Only the
    // filling thread changes blocks, so a block it returns stays valid.
    // A block to fill: the least recently used, allocated while under the
    // cap. Not `keep` (the other block of the same fill). Null: none.
    Block *victim(const Block *keep)
    {
        Block *any = nullptr, *with_memory = nullptr;
        for (Block &b : blocks_) {
            if (&b == keep)
                continue;
            if (!any || b.used < any->used)
                any = &b;
            if (b.data && (!with_memory || b.used < with_memory->used))
                with_memory = &b;
        }
        if (any && !any->data && with_memory && g_file_stats.cache_bytes + kBlock > kCap)
            any = with_memory;   // at the cap: reuse a block instead of allocating
        if (any && !any->data) {
            any->data.reset(new (std::nothrow) uint8_t[kBlock]);
            if (!any->data)
                return with_memory;
            g_file_stats.cache_bytes += kBlock;
        }
        return any;
    }

    // The block at `base`, read from the file if it isn't cached. A miss on
    // the block right after the previous fill (a file read in order) reads
    // the following block too, in the same call. Only the filling thread
    // changes blocks, so a block it returns stays valid.
    const Block *block_at(uint64_t base)
    {
        Guard g(lock_);
        if (Block *b = find(base)) {
            b->used = ++clock_;
            g_file_stats.hits++;
            return b;
        }
        Block *first = victim(nullptr);
        if (!first)
            return nullptr;
        const bool ahead = base == last_fill_ + kBlock && base + kBlock < size_ && !find(base + kBlock);
        Block *second = ahead ? victim(first) : nullptr;
        first->base = ~0ull;   // unlisted while they are filled
        if (second)
            second->base = ~0ull;
        const uint64_t left = size_ - base;
        const uint32_t want = (uint32_t)std::min<uint64_t>(left, second ? 2 * kBlock : kBlock);
        lock_.clear(std::memory_order_release);
        uint32_t len;
        if (second) {   // one read for both blocks
            static uint8_t span[2 * kBlock];   // the filling thread's alone
            len = disk_read(base, span, want);
            std::memcpy(first->data.get(), span, std::min(len, kBlock));
            if (len > kBlock)
                std::memcpy(second->data.get(), span + kBlock, len - kBlock);
        } else {
            len = disk_read(base, first->data.get(), want);
        }
        while (lock_.test_and_set(std::memory_order_acquire)) {
        }
        first->len = std::min(len, kBlock);
        first->base = base;
        first->used = ++clock_;
        last_fill_ = base;
        if (second && len > kBlock) {
            second->len = len - kBlock;
            second->base = base + kBlock;
            second->used = clock_;   // older than `first` is once that is read
            last_fill_ = base + kBlock;
            g_file_stats.fills++;
        }
        g_file_stats.fills++;
        return first;
    }

    uint32_t disk_read(uint64_t pos, uint8_t *dst, uint32_t n)
    {
        const uint64_t t0 = wx86_now_us();
        uint32_t done = 0;
        if (::lseek(fd_, (off_t)pos, SEEK_SET) == (off_t)pos)
            while (done < n) {
                const ssize_t got = ::read(fd_, dst + done, n - done);
                if (got <= 0)
                    break;
                done += (uint32_t)got;
            }
        g_file_stats.disk_reads++;
        g_file_stats.disk_bytes += done;
        g_file_stats.disk_us += wx86_now_us() - t0;
        return done;
    }

    int fd_;
    uint64_t size_;
    uint32_t clock_ = 0;
    uint64_t last_fill_ = ~0ull - kBlock;   // the last block filled: a miss right after it reads ahead
    Block blocks_[kBlocks];
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

struct RtFile : Waitable {
    FILE *fp = nullptr;              // files the guest may write: stdio
    int fd = -1;                     // read-only game files: a descriptor under `cache`
    uint64_t pos = 0, size = 0;      // ...whose position is kept here and size fixed at open
    std::unique_ptr<ReadCache> cache;
    std::string host;
    ~RtFile() override
    {
        // Asynchronous reads still in flight write into this file's cache.
        while (cache && cache->inflight.load(std::memory_order_acquire) > 0)
            if (options().sleep_ms)
                options().sleep_ms(1);
        if (fp)
            std::fclose(fp);
        if (fd >= 0)
            ::close(fd);
    }
    bool try_acquire(GuestThread *) override { return true; }
    bool ready(GuestThread *) override { return true; }
    const char *kind() const override { return "file"; }
};

// ---- asynchronous reads (RunOptions::io_start) --------------------------------------
//
// A ReadFile that misses the cache goes to one host worker thread: the guest
// thread waits on the request, the scheduler runs the other guest threads,
// and the reader wakes with ReadFile's TRUE once the bytes are in its buffer.
// Requests come from the game's thread (one producer) and the worker serves
// them in order (one consumer). While a file has requests in flight, only the
// worker fills its cache; the game's thread then only copies cached bytes
// (ReadCache::try_read).
struct IoRequest : Waitable {
    ReadCache *cache = nullptr;
    uint64_t pos = 0;
    uint8_t *dst = nullptr;
    uint32_t n = 0;
    std::atomic<int> state{0};   // 0 free, 1 queued, 2 done; the waiter's acquire frees it
    bool try_acquire(GuestThread *) override
    {
        int done = 2;
        return state.compare_exchange_strong(done, 0, std::memory_order_acq_rel);
    }
    bool ready(GuestThread *) override { return state.load(std::memory_order_acquire) == 2; }
    uint32_t result() const override { return 1; }   // ReadFile's TRUE
    const char *kind() const override { return "fileio"; }
};

constexpr uint32_t kIoMax = 16;   // requests in flight; past that, reads wait their turn synchronously
IoRequest g_io[kIoMax];
IoRequest *g_io_ring[kIoMax];
std::atomic<uint32_t> g_io_head{0}, g_io_tail{0};
std::atomic<bool> g_io_quit{false};
bool g_io_on = false;

void io_worker_main()
{
    for (;;) {
        const uint32_t t = g_io_tail.load(std::memory_order_relaxed);
        if (t == g_io_head.load(std::memory_order_acquire)) {
            if (g_io_quit.load(std::memory_order_acquire))
                return;
            options().io_wait();
            continue;
        }
        IoRequest *r = g_io_ring[t % kIoMax];
        r->cache->read(r->pos, r->dst, r->n);
        r->cache->inflight.fetch_sub(1, std::memory_order_acq_rel);
        g_io_tail.store(t + 1, std::memory_order_release);
        r->state.store(2, std::memory_order_release);   // before the count: the scheduler polls while it is nonzero
        wx86_host_pending(-1);
        if (options().io_done)
            options().io_done();
    }
}

// Queues a read; null when every request is in flight.
IoRequest *io_submit(ReadCache *cache, uint64_t pos, uint8_t *dst, uint32_t n)
{
    for (IoRequest &r : g_io) {
        if (r.state.load(std::memory_order_acquire) != 0)
            continue;
        r.cache = cache;
        r.pos = pos;
        r.dst = dst;
        r.n = n;
        r.state.store(1, std::memory_order_relaxed);
        cache->inflight.fetch_add(1, std::memory_order_acq_rel);
        wx86_host_pending(1);
        const uint32_t h = g_io_head.load(std::memory_order_relaxed);
        g_io_ring[h % kIoMax] = &r;
        g_io_head.store(h + 1, std::memory_order_release);
        options().io_wake();
        return &r;
    }
    return nullptr;
}

// Waiting on the I/O worker from the game's own thread (a full request
// pool, or a read that must complete before ReadFile returns).
void io_pause()
{
    if (options().sleep_ms)
        options().sleep_ms(1);
}

struct FindEntry {
    std::string name;
    uint32_t attrs;
    uint64_t size;
    uint64_t mtime;   // FILETIME
};

struct RtFind : Waitable {
    std::vector<FindEntry> entries;
    size_t next = 0;
    bool try_acquire(GuestThread *) override { return false; }
    bool ready(GuestThread *) override { return false; }
    const char *kind() const override { return "find"; }
};

struct RtMapping : Waitable {
    std::string host;
    uint32_t size = 0;
    bool writable = false;
    bool try_acquire(GuestThread *) override { return false; }
    bool ready(GuestThread *) override { return false; }
    const char *kind() const override { return "fmap"; }
};

struct View {
    uint32_t va, size, offset;
    RtMapping *map;
};
std::vector<View> g_views;

template <class T> T *object_of(uint32_t h, const char *kind)
{
    Waitable *w = wx86_handle_find(h);
    return (w && wx86_is_kind(w, kind)) ? static_cast<T *>(w) : nullptr;
}

RtFile *file_of(uint32_t h) { return object_of<RtFile>(h, "file"); }

uint64_t filetime_of(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }

uint32_t attrs_of(const struct stat &st, bool in_data)
{
    if (S_ISDIR(st.st_mode))
        return kAttrDirectory;
    return in_data && split_roots() ? (kAttrArchive | kAttrReadOnly) : kAttrArchive;
}

// Moves n bytes between a host stream and guest memory: straight into the
// arena when the backend exposes it (always, in the single-arena model).
size_t stream_read(Cpu &c, FILE *fp, uint32_t va, uint32_t n)
{
    if (!n)
        return 0;
    if (void *p = c.hostptr(va, n))
        return std::fread(p, 1, n, fp);
    std::vector<uint8_t> tmp(n);
    const size_t got = std::fread(tmp.data(), 1, n, fp);
    if (got)
        c.write(va, tmp.data(), (uint32_t)got);
    return got;
}

size_t stream_write(Cpu &c, FILE *fp, uint32_t va, uint32_t n)
{
    if (!n)
        return 0;
    if (void *p = c.hostptr(va, n))
        return std::fwrite(p, 1, n, fp);
    std::vector<uint8_t> tmp(n);
    c.read(va, tmp.data(), n);
    return std::fwrite(tmp.data(), 1, n, fp);
}

// ReadFile on a cached file: from its position, into guest memory.
uint32_t cached_read(Cpu &c, RtFile &f, uint32_t va, uint32_t n)
{
    // RT_FILETRACE=1: every read on stderr ("ftrace <fd> <pos> <n>"), for
    // sizing the cache against the game's real access pattern.
    static const bool trace = std::getenv("RT_FILETRACE") != nullptr;
    if (trace)
        std::fprintf(stderr, "ftrace %d %llu %u\n", f.fd, (unsigned long long)f.pos, n);
    if (!n)
        return 0;
    uint32_t got;
    if (void *p = c.hostptr(va, n)) {
        got = f.cache->read(f.pos, static_cast<uint8_t *>(p), n);
    } else {
        std::vector<uint8_t> tmp(n);
        got = f.cache->read(f.pos, tmp.data(), n);
        if (got)
            c.write(va, tmp.data(), got);
    }
    f.pos += got;
    return got;
}

// ---- wildcard match (FindFirstFile: '*', '?', case-insensitive; "*.*" is "*") ----

bool wild(const char *p, const char *s)
{
    if (!*p)
        return !*s;
    if (*p == '*') {
        while (p[1] == '*')
            p++;
        if (!p[1])
            return true;
        for (; *s; s++)
            if (wild(p + 1, s))
                return true;
        // "name.*" also matches "name" (no extension), as on Windows.
        return std::strcmp(p + 1, ".*") == 0 ? true : wild(p + 1, s);
    }
    if (!*s)
        return std::strcmp(p, ".*") == 0;
    if (*p == '?' || std::tolower((unsigned char)*p) == std::tolower((unsigned char)*s))
        return wild(p + 1, s + 1);
    return false;
}

void collect(const std::string &root, const std::string &dir_rel, const std::string &pattern, bool in_data,
             std::vector<FindEntry> &out)
{
    std::string dir;
    if (!find_ci(root, dir_rel, dir))
        return;
    DIR *d = ::opendir(dir.c_str());
    if (!d)
        return;
    const std::string pat = pattern == "*.*" ? "*" : pattern;
    while (dirent *e = ::readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == ".." || !wild(pat.c_str(), n.c_str()))
            continue;
        bool dup = false;
        for (const FindEntry &f : out)
            dup = dup || lower(f.name) == lower(n);
        struct stat st;
        if (dup || !host_stat(dir + "/" + n, st))
            continue;
        out.push_back({n, attrs_of(st, in_data), (uint64_t)st.st_size, filetime_of(st.st_mtime)});
    }
    ::closedir(d);
}

// WIN32_FIND_DATAA (320 bytes) / WIN32_FIND_DATAW (592 bytes).
void put_find_data(Cpu &c, uint32_t va, const FindEntry &e, bool wide)
{
    uint8_t buf[592] = {};
    auto put32 = [&](uint32_t off, uint32_t v) { std::memcpy(buf + off, &v, 4); };
    put32(0, e.attrs);
    for (uint32_t off : {4u, 12u, 20u}) {
        put32(off, (uint32_t)e.mtime);
        put32(off + 4, (uint32_t)(e.mtime >> 32));
    }
    put32(28, (uint32_t)(e.size >> 32));
    put32(32, (uint32_t)e.size);
    const size_t n = e.name.size() < 259 ? e.name.size() : 259;
    if (wide) {
        for (size_t i = 0; i < n; i++)
            buf[44 + 2 * i] = (uint8_t)e.name[i];   // names on the game's media are ASCII
        c.write(va, buf, 592);
    } else {
        std::memcpy(buf + 44, e.name.data(), n);
        c.write(va, buf, 320);
    }
}

uint32_t find_first(Cpu &c, const std::string &guest, uint32_t data, bool wide)
{
    const std::string rel = normalize(guest);
    const std::string dir = parent_of(rel), pattern = leaf_of(rel);
    auto *f = new RtFind();
    collect(write_root(), dir, pattern, false, f->entries);
    if (split_roots())
        collect(data_root(), dir, pattern, true, f->entries);
    if (f->entries.empty()) {
        delete f;
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return kInvalidHandle;
    }
    put_find_data(c, data, f->entries[0], wide);
    f->next = 1;
    return wx86_handle_add(f);
}

// ---- INI files (GetPrivateProfile*) ------------------------------------------------

struct IniLine {
    std::string section, key, value, raw;   // key empty: a section header, comment or blank line
};

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        a++;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        b--;
    return s.substr(a, b - a);
}

std::vector<IniLine> ini_load(const std::string &guest_file)
{
    std::vector<IniLine> lines;
    std::string host;
    // A bare name lives in the Windows directory on Windows; here, the program's.
    if (!resolve_read(normalize(guest_file), host))
        return lines;
    FILE *f = std::fopen(host.c_str(), "rb");
    if (!f)
        return lines;
    char buf[1024];
    std::string section;
    while (std::fgets(buf, sizeof buf, f)) {
        IniLine l;
        l.raw = buf;
        while (!l.raw.empty() && (l.raw.back() == '\n' || l.raw.back() == '\r'))
            l.raw.pop_back();
        const std::string t = trim(l.raw);
        if (t.size() >= 2 && t[0] == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
        } else if (!t.empty() && t[0] != ';') {
            const size_t eq = t.find('=');
            if (eq != std::string::npos) {
                l.key = trim(t.substr(0, eq));
                l.value = trim(t.substr(eq + 1));
                if (l.value.size() >= 2 && (l.value[0] == '"' || l.value[0] == '\'') && l.value.back() == l.value[0])
                    l.value = l.value.substr(1, l.value.size() - 2);
            }
        }
        l.section = section;
        lines.push_back(l);
    }
    std::fclose(f);
    return lines;
}

bool ieq(const std::string &a, const std::string &b) { return lower(a) == lower(b); }

// Writes a NUL-separated, double-NUL-terminated list; returns the
// GetPrivateProfileString result (characters copied, without the final NUL).
uint32_t put_list(Cpu &c, uint32_t buf, uint32_t size, const std::vector<std::string> &items)
{
    if (size < 2)
        return 0;
    std::string out;
    for (const std::string &s : items) {
        if (out.size() + s.size() + 2 > size)
            break;
        out += s;
        out += '\0';
    }
    out += '\0';
    c.write(buf, out.data(), (uint32_t)out.size());
    return (uint32_t)out.size() - 1;
}

uint32_t ini_get_string(Cpu &c, const std::string &app, uint32_t key_va, const std::string &def, uint32_t buf,
                        uint32_t size, const std::string &file, bool app_null)
{
    const std::vector<IniLine> lines = ini_load(file);
    if (app_null) {
        std::vector<std::string> sections;
        for (const IniLine &l : lines) {
            bool seen = false;
            for (const std::string &s : sections)
                seen = seen || ieq(s, l.section);
            if (!seen && !l.section.empty())
                sections.push_back(l.section);
        }
        return put_list(c, buf, size, sections);
    }
    if (!key_va) {
        std::vector<std::string> keys;
        for (const IniLine &l : lines)
            if (!l.key.empty() && ieq(l.section, app))
                keys.push_back(l.key);
        return put_list(c, buf, size, keys);
    }
    const std::string key = read_cstr(c, key_va);
    std::string value = trim(def);
    for (const IniLine &l : lines)
        if (!l.key.empty() && ieq(l.section, app) && ieq(l.key, key)) {
            value = l.value;
            break;
        }
    if (!size)
        return 0;
    if (value.size() >= size)
        value.resize(size - 1);
    c.write(buf, value.c_str(), (uint32_t)value.size() + 1);
    return (uint32_t)value.size();
}

bool ini_write(const std::string &file, const std::string &app, const std::string *key, const std::string *value)
{
    std::vector<IniLine> lines = ini_load(file);
    std::vector<std::string> out;
    bool done = key == nullptr && value == nullptr ? false : false, in_app = false, app_seen = false;
    for (size_t i = 0; i < lines.size(); i++) {
        const IniLine &l = lines[i];
        const bool header = trim(l.raw).size() >= 2 && trim(l.raw)[0] == '[';
        if (header) {
            if (in_app && !done && key && value) {   // leaving the section: append the new key
                out.push_back(*key + "=" + *value);
                done = true;
            }
            in_app = ieq(l.section, app);
            app_seen = app_seen || in_app;
            if (in_app && !key)   // delete the whole section
                continue;
        } else if (in_app) {
            if (!key)
                continue;
            if (!l.key.empty() && ieq(l.key, *key)) {
                if (value && !done)
                    out.push_back(l.key + "=" + *value);
                done = true;
                continue;
            }
        }
        out.push_back(l.raw);
    }
    if (key && value && !done) {
        if (!app_seen)
            out.push_back("[" + app + "]");
        out.push_back(*key + "=" + *value);
    }
    std::string host;
    if (!resolve_write(normalize(file), false, host))
        return false;
    FILE *f = std::fopen(host.c_str(), "wb");
    if (!f)
        return false;
    for (const std::string &s : out)
        std::fprintf(f, "%s\r\n", s.c_str());
    return std::fclose(f) == 0;
}

// ---- file creation ------------------------------------------------------------------

uint32_t create_file(Cpu &c, const std::string &guest, uint32_t access, uint32_t disp)
{
    const std::string rel = normalize(guest);
    const bool write = (access & 0xC0000000u) != 0x80000000u && (access & 0x40000000u);   // GENERIC_WRITE
    std::string host;
    const bool exists = resolve_read(rel, host);
    struct stat st;
    if (exists && host_stat(host, st) && S_ISDIR(st.st_mode)) {
        set_last_error(c, ERROR_ACCESS_DENIED);
        return kInvalidHandle;
    }
    const char *mode = nullptr;
    switch (disp) {
    case 1:   // CREATE_NEW
        if (exists) { set_last_error(c, ERROR_FILE_EXISTS); return kInvalidHandle; }
        mode = "w+b"; break;
    case 2:   // CREATE_ALWAYS
        mode = "w+b"; break;
    case 3:   // OPEN_EXISTING
        if (!exists) { set_last_error(c, ERROR_FILE_NOT_FOUND); return kInvalidHandle; }
        mode = write ? "r+b" : "rb"; break;
    case 4:   // OPEN_ALWAYS
        mode = exists ? (write ? "r+b" : "rb") : "w+b"; break;
    case 5:   // TRUNCATE_EXISTING
        if (!exists) { set_last_error(c, ERROR_FILE_NOT_FOUND); return kInvalidHandle; }
        mode = "w+b"; break;
    default:
        set_last_error(c, ERROR_INVALID_PARAMETER);
        return kInvalidHandle;
    }
    if (mode[0] == 'w' || write) {
        const bool keep = mode[0] == 'r';   // opened for update: keep the data_root contents
        if (!resolve_write(rel, keep, host)) {
            set_last_error(c, ERROR_PATH_NOT_FOUND);
            return kInvalidHandle;
        }
    }
    auto *f = new RtFile();
    f->host = host;
    // Read-only and in the game install: the cached path (see the top).
    const std::string &data = data_root();
    if (std::strcmp(mode, "rb") == 0 && split_roots() && host.compare(0, data.size(), data) == 0) {
        f->fd = ::open(host.c_str(), O_RDONLY);
        struct stat fst;
        if (f->fd >= 0 && ::fstat(f->fd, &fst) == 0) {
            f->size = (uint64_t)fst.st_size;
            f->cache.reset(new ReadCache(f->fd, f->size));
        } else if (f->fd >= 0) {
            ::close(f->fd);
            f->fd = -1;
        }
    }
    if (!f->cache && !(f->fp = std::fopen(host.c_str(), mode))) {
        delete f;
        set_last_error(c, ERROR_ACCESS_DENIED);
        return kInvalidHandle;
    }
    stats().files_opened++;
    set_last_error(c, (exists && (disp == 2 || disp == 4)) ? (uint32_t)ERROR_ALREADY_EXISTS : 0u);
    return wx86_handle_add(f);
}

uint32_t get_attributes(Cpu &c, const std::string &guest)
{
    const std::string rel = normalize(guest);
    std::string host;
    struct stat st;
    if (rel.empty())
        return kAttrDirectory;
    if (!resolve_read(rel, host) || !host_stat(host, st)) {
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return kInvalidAttributes;
    }
    return attrs_of(st, split_roots() && host.compare(0, data_root().size(), data_root()) == 0);
}

uint32_t create_directory(Cpu &c, const std::string &guest)
{
    const std::string rel = normalize(guest);
    std::string host;
    if (resolve_read(rel, host)) {
        set_last_error(c, ERROR_ALREADY_EXISTS);
        return 0;
    }
    std::string parent;
    if (!ensure_write_dir(parent_of(rel), parent)) {
        set_last_error(c, ERROR_PATH_NOT_FOUND);
        return 0;
    }
    if (::mkdir((parent + "/" + leaf_of(rel)).c_str(), 0777) != 0) {
        set_last_error(c, ERROR_ACCESS_DENIED);
        return 0;
    }
    forget_dir(parent);
    return 1;
}

uint32_t copy_file(Cpu &c, const std::string &from, const std::string &to, bool fail_if_exists)
{
    std::string src, dst;
    if (!resolve_read(normalize(from), src)) {
        set_last_error(c, ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (fail_if_exists && resolve_read(normalize(to), dst)) {
        set_last_error(c, ERROR_FILE_EXISTS);
        return 0;
    }
    if (!resolve_write(normalize(to), false, dst) || !copy_host_file(src, dst)) {
        set_last_error(c, ERROR_ACCESS_DENIED);
        return 0;
    }
    return 1;
}

// Module file name: C:\<name> (the executable is C:\<exe_name>).
uint32_t module_file_name(Cpu &c, uint32_t module, uint32_t buf, uint32_t size)
{
    const std::string name = module_name_of(module ? module : exe_base());
    if (name.empty()) {
        set_last_error(c, 126);   // ERROR_MOD_NOT_FOUND
        return 0;
    }
    const std::string full = "C:\\" + name;
    if (!size)
        return 0;
    if (full.size() + 1 > size) {   // XP-style truncation: size-1 characters, terminated
        c.write(buf, full.data(), size - 1);
        c.write(buf + size - 1, "", 1);
        set_last_error(c, ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    c.write(buf, full.c_str(), (uint32_t)full.size() + 1);
    return (uint32_t)full.size();
}

}  // namespace

const FileStats &file_stats() { return g_file_stats; }

void start_async_io()
{
    g_io_quit = false;
    g_io_on = options().real_clock && options().io_start && options().io_wait && options().io_wake &&
              options().io_start(io_worker_main);
    if (g_io_on && options().io_done && options().io_idle_wait)
        wx86_set_host_waiter([](uint32_t ms) { options().io_idle_wait(ms); });
    logf("file reads: %s", g_io_on ? "asynchronous (I/O worker)" : "synchronous");
}

void stop_async_io()
{
    if (!g_io_on)
        return;
    wx86_set_host_waiter(nullptr);
    g_io_quit = true;
    options().io_wake();   // the worker drains the queue, then returns
    g_io_on = false;
}

bool host_path(const std::string &guest, bool write, std::string &host)
{
    const std::string rel = normalize(guest);
    return write ? resolve_write(rel, true, host) : resolve_read(rel, host);
}

bool read_guest_file(const std::string &guest, std::vector<uint8_t> &bytes)
{
    std::string host;
    if (!host_path(guest, false, host))
        return false;
    FILE *f = std::fopen(host.c_str(), "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes.resize(n > 0 ? (size_t)n : 0);
    const bool ok = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok;
}

std::string guest_full_path(const std::string &guest) { return to_guest(normalize(guest)); }

void install_files(Bridge &br)
{
    auto K = [&](const char *name, uint32_t argc, std::function<uint32_t(Cpu &)> fn) {
        Shim s;
        s.argc = argc;
        s.stdcall_cleanup = true;
        s.tag = std::string("KERNEL32.dll!") + name;
        s.fn = std::move(fn);
        br.register_shim("KERNEL32.dll", name, s);
    };
    g_cwd.clear();
    g_dirs.clear();

    // ---- open, read, write ----
    K("CreateFileA", 7, [](Cpu &c) { return create_file(c, read_cstr(c, c.arg(0)), c.arg(1), c.arg(4)); });
    K("CreateFileW", 7, [](Cpu &c) { return create_file(c, read_wstr(c, c.arg(0)), c.arg(1), c.arg(4)); });
    K("ReadFile", 5, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        const uint32_t buf = c.arg(1), n = c.arg(2), pread = c.arg(3), ov = c.arg(4);
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        uint32_t got;
        g_file_stats.calls++;
        if (f->cache && n && g_io_on) {
            // Asynchronous: what is cached is copied now; a miss goes to the
            // worker, the only thread that fills caches in this mode. A
            // plain read waits for it as a guest wait (the other guest
            // threads run); an OVERLAPPED one, or one without a direct
            // pointer, waits here. The file can't change size, so the count
            // and the new position are known at once.
            if (ov)   // OVERLAPPED: the offset is in the structure; completes synchronously
                f->pos = c.read_u32(ov + 8);
            std::vector<uint8_t> tmp;
            uint8_t *dst = static_cast<uint8_t *>(c.hostptr(buf, n));
            if (!dst) {
                tmp.resize(n);
                dst = tmp.data();
            }
            if (!f->cache->try_read(f->pos, dst, n, got)) {
                got = f->pos < f->size ? (uint32_t)std::min<uint64_t>(n, f->size - f->pos) : 0;
                IoRequest *r;
                while (!(r = io_submit(f->cache.get(), f->pos, dst, n)))
                    io_pause();   // every request in flight
                if (!ov && tmp.empty()) {
                    f->pos += got;
                    stats().bytes_read += got;
                    g_file_stats.async++;
                    if (pread)
                        c.write_u32(pread, got);
                    return wx86_sched()->wait(r, 0xFFFFFFFFu);   // EAX = TRUE when it wakes
                }
                while (r->state.load(std::memory_order_acquire) != 2)
                    io_pause();
                r->state.store(0, std::memory_order_release);   // free the request
            }
            if (!tmp.empty() && got)
                c.write(buf, tmp.data(), got);
            f->pos += got;
        } else if (f->cache) {
            if (ov)   // OVERLAPPED: the offset is in the structure; completes synchronously
                f->pos = c.read_u32(ov + 8);
            got = cached_read(c, *f, buf, n);
        } else {
            if (ov)
                std::fseek(f->fp, (long)c.read_u32(ov + 8), SEEK_SET);
            got = (uint32_t)stream_read(c, f->fp, buf, n);
        }
        stats().bytes_read += got;
        if (pread)
            c.write_u32(pread, got);
        if (ov) {
            c.write_u32(ov, 0);          // Internal: STATUS_SUCCESS
            c.write_u32(ov + 4, got);    // InternalHigh: bytes transferred
            if (Waitable *e = wx86_handle_find(c.read_u32(ov + 16)))
                if (wx86_is_kind(e, "event"))
                    static_cast<WxEvent *>(e)->signaled = true;
            if (!got && n) { set_last_error(c, ERROR_HANDLE_EOF); return 0; }
        }
        return 1;
    });
    K("WriteFile", 5, [](Cpu &c) -> uint32_t {
        const uint32_t h = c.arg(0), buf = c.arg(1), n = c.arg(2), pwritten = c.arg(3), ov = c.arg(4);
        if (h == kStdOutput || h == kStdError) {
            std::string s = n ? std::string(n, '\0') : std::string();
            if (n)
                c.read(buf, &s[0], n);
            if (options().out)
                options().out(s.data(), n);
            if (pwritten)
                c.write_u32(pwritten, n);
            return 1;
        }
        RtFile *f = file_of(h);
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        if (f->cache) { set_last_error(c, ERROR_ACCESS_DENIED); return 0; }   // opened for reading only
        if (ov)
            std::fseek(f->fp, (long)c.read_u32(ov + 8), SEEK_SET);
        const uint32_t put = (uint32_t)stream_write(c, f->fp, buf, n);
        if (pwritten)
            c.write_u32(pwritten, put);
        if (ov) {
            c.write_u32(ov, 0);
            c.write_u32(ov + 4, put);
        }
        return put == n;
    });
    K("SetFilePointer", 4, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
        const int whence = c.arg(3) == 1 ? SEEK_CUR : c.arg(3) == 2 ? SEEK_END : SEEK_SET;
        int64_t off = (int32_t)c.arg(1);
        if (const uint32_t phigh = c.arg(2)) {
            off = (int64_t)(((uint64_t)c.read_u32(phigh) << 32) | c.arg(1));
            c.write_u32(phigh, 0);
        }
        if (f->cache) {   // position only: nothing reaches the file system
            g_file_stats.seeks++;
            const int64_t base = whence == SEEK_CUR ? (int64_t)f->pos : whence == SEEK_END ? (int64_t)f->size : 0;
            if (base + off < 0) {
                set_last_error(c, 131);   // ERROR_NEGATIVE_SEEK
                return 0xFFFFFFFFu;
            }
            f->pos = (uint64_t)(base + off);
            set_last_error(c, 0);
            return (uint32_t)f->pos;
        }
        if (std::fseek(f->fp, (long)off, whence) != 0) {
            set_last_error(c, 131);   // ERROR_NEGATIVE_SEEK
            return 0xFFFFFFFFu;
        }
        set_last_error(c, 0);
        return (uint32_t)std::ftell(f->fp);
    });
    K("GetFileSize", 2, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0xFFFFFFFFu; }
        if (c.arg(1)) c.write_u32(c.arg(1), 0);
        if (f->cache)
            return (uint32_t)f->size;
        std::fflush(f->fp);
        struct stat st;
        if (::fstat(fileno(f->fp), &st) != 0) return 0xFFFFFFFFu;
        return (uint32_t)st.st_size;
    });
    K("GetFileType", 1, [](Cpu &c) -> uint32_t {
        const uint32_t h = c.arg(0);
        if (file_of(h)) return 1;   // FILE_TYPE_DISK
        return (h == kStdInput || h == kStdOutput || h == kStdError) ? 2u : 0u;   // FILE_TYPE_CHAR / UNKNOWN
    });
    K("FlushFileBuffers", 1, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        return f && (f->cache || std::fflush(f->fp) == 0);
    });
    K("SetEndOfFile", 1, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        if (f->cache) { set_last_error(c, ERROR_ACCESS_DENIED); return 0; }
        std::fflush(f->fp);
        return ::ftruncate(fileno(f->fp), std::ftell(f->fp)) == 0;
    });
    K("GetFileTime", 4, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        struct stat st;
        if (!f || ::fstat(f->cache ? f->fd : fileno(f->fp), &st) != 0) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        const uint64_t ft = filetime_of(st.st_mtime);
        for (int i = 1; i <= 3; i++)
            if (const uint32_t p = c.arg(i)) {
                c.write_u32(p, (uint32_t)ft);
                c.write_u32(p + 4, (uint32_t)(ft >> 32));
            }
        return 1;
    });

    // ---- names and directories ----
    K("GetFileAttributesA", 1, [](Cpu &c) { return get_attributes(c, read_cstr(c, c.arg(0))); });
    K("GetFileAttributesW", 1, [](Cpu &c) { return get_attributes(c, read_wstr(c, c.arg(0))); });
    K("CreateDirectoryA", 2, [](Cpu &c) { return create_directory(c, read_cstr(c, c.arg(0))); });
    K("CreateDirectoryW", 2, [](Cpu &c) { return create_directory(c, read_wstr(c, c.arg(0))); });
    K("RemoveDirectoryA", 1, [](Cpu &c) -> uint32_t {
        std::string host;
        if (!find_ci(write_root(), normalize(read_cstr(c, c.arg(0))), host) || ::rmdir(host.c_str()) != 0) {
            set_last_error(c, ERROR_ACCESS_DENIED);
            return 0;
        }
        forget_dir(host.substr(0, host.rfind('/')));
        return 1;
    });
    K("DeleteFileA", 1, [](Cpu &c) -> uint32_t {
        std::string host;
        const std::string rel = normalize(read_cstr(c, c.arg(0)));
        if (!find_ci(write_root(), rel, host)) {
            set_last_error(c, resolve_read(rel, host) ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
            return 0;
        }
        if (std::remove(host.c_str()) != 0) { set_last_error(c, ERROR_ACCESS_DENIED); return 0; }
        forget_dir(host.substr(0, host.rfind('/')));
        return 1;
    });
    K("MoveFileA", 2, [](Cpu &c) -> uint32_t {
        std::string src, dst;
        const std::string from = normalize(read_cstr(c, c.arg(0))), to = normalize(read_cstr(c, c.arg(1)));
        if (resolve_read(to, dst)) { set_last_error(c, ERROR_ALREADY_EXISTS); return 0; }
        if (!resolve_write(from, true, src)) { set_last_error(c, ERROR_FILE_NOT_FOUND); return 0; }
        if (!resolve_write(to, false, dst) || std::rename(src.c_str(), dst.c_str()) != 0) {
            set_last_error(c, ERROR_ACCESS_DENIED);
            return 0;
        }
        forget_dir(src.substr(0, src.rfind('/')));
        forget_dir(dst.substr(0, dst.rfind('/')));
        return 1;
    });
    K("CopyFileA", 3, [](Cpu &c) { return copy_file(c, read_cstr(c, c.arg(0)), read_cstr(c, c.arg(1)), c.arg(2) != 0); });
    K("CopyFileW", 3, [](Cpu &c) { return copy_file(c, read_wstr(c, c.arg(0)), read_wstr(c, c.arg(1)), c.arg(2) != 0); });

    K("GetModuleFileNameA", 3, [](Cpu &c) { return module_file_name(c, c.arg(0), c.arg(1), c.arg(2)); });
    br.register_shim("PSAPI.DLL", "GetModuleFileNameExA", [] {
        Shim s;
        s.argc = 4;
        s.tag = "PSAPI.DLL!GetModuleFileNameExA";
        s.fn = [](Cpu &c) { return module_file_name(c, c.arg(1), c.arg(2), c.arg(3)); };
        return s;
    }());
    K("GetCurrentDirectoryA", 2, [](Cpu &c) {
        std::string d = to_guest(g_cwd);
        return write_cstr(c, c.arg(1), c.arg(0), d);
    });
    K("SetCurrentDirectoryA", 1, [](Cpu &c) -> uint32_t {
        const std::string rel = normalize(read_cstr(c, c.arg(0)));
        std::string host;
        struct stat st;
        if (!rel.empty() && (!resolve_read(rel, host) || !host_stat(host, st) || !S_ISDIR(st.st_mode))) {
            set_last_error(c, ERROR_PATH_NOT_FOUND);
            return 0;
        }
        g_cwd = rel;
        return 1;
    });
    K("GetFullPathNameA", 4, [](Cpu &c) -> uint32_t {
        const std::string in = read_cstr(c, c.arg(0));
        std::string full = to_guest(normalize(in));
        if (!in.empty() && (in.back() == '\\' || in.back() == '/') && full.back() != '\\')
            full += '\\';
        const uint32_t buf = c.arg(2), size = c.arg(1), filepart = c.arg(3);
        const uint32_t r = write_cstr(c, buf, size, full);
        if (r < size && filepart) {
            const size_t k = full.rfind('\\');
            c.write_u32(filepart, k + 1 < full.size() ? buf + (uint32_t)k + 1 : 0);
        }
        return r;
    });
    K("GetTempPathA", 2, [](Cpu &c) {
        std::string host;
        if (!find_ci(write_root(), "temp", host))
            ::mkdir((write_root() + "/temp").c_str(), 0777), forget_dir(write_root());
        return write_cstr(c, c.arg(1), c.arg(0), "C:\\temp\\");
    });
    K("GetTempPathW", 2, [](Cpu &c) -> uint32_t {
        std::string host;
        if (!find_ci(write_root(), "temp", host))
            ::mkdir((write_root() + "/temp").c_str(), 0777), forget_dir(write_root());
        static const char kTemp[] = "C:\\temp\\";
        const uint32_t n = sizeof kTemp - 1;
        if (c.arg(0) < n + 1)
            return n + 1;
        for (uint32_t i = 0; i <= n; i++)
        {
            const uint16_t w = (uint16_t)kTemp[i];
            c.write(c.arg(1) + 2 * i, &w, 2);
        }
        return n;
    });
    K("GetTempFileNameA", 4, [](Cpu &c) -> uint32_t {
        const std::string dir = read_cstr(c, c.arg(0)), prefix = read_cstr(c, c.arg(1)).substr(0, 3);
        static uint32_t counter = 0;
        uint32_t unique = c.arg(2);
        std::string name;
        for (int tries = 0; tries < 0x10000; tries++) {
            const uint32_t u = unique ? unique : (++counter & 0xFFFF);
            char tail[16];
            std::snprintf(tail, sizeof tail, "%X.tmp", u);
            name = dir + (dir.empty() || dir.back() == '\\' ? "" : "\\") + prefix + tail;
            std::string host;
            if (unique || !resolve_read(normalize(name), host))
                break;
        }
        if (!unique) {   // the name is reserved by creating the file
            std::string host;
            if (FILE *f = resolve_write(normalize(name), false, host) ? std::fopen(host.c_str(), "wb") : nullptr)
                std::fclose(f);
        }
        write_cstr(c, c.arg(3), 260, name);
        return unique ? unique : counter;
    });
    K("GetDiskFreeSpaceA", 5, [](Cpu &c) {
        const uint32_t v[4] = {8, 512, 0x40000, 0x80000};   // 1 GiB free of 2 GiB, in 4 KiB clusters
        for (int i = 0; i < 4; i++)
            if (c.arg(1 + i))
                c.write_u32(c.arg(1 + i), v[i]);
        return 1u;
    });
    K("GetVolumeInformationA", 8, [](Cpu &c) {
        if (c.arg(1)) write_cstr(c, c.arg(1), c.arg(2), "3DS-D2");
        if (c.arg(3)) c.write_u32(c.arg(3), 0x3D5D2000u);
        if (c.arg(4)) c.write_u32(c.arg(4), 255);
        if (c.arg(5)) c.write_u32(c.arg(5), 0x2);   // FILE_CASE_PRESERVED_NAMES
        if (c.arg(6)) write_cstr(c, c.arg(6), c.arg(7), "FAT32");
        return 1u;
    });
    K("GetLogicalDriveStringsA", 2, [](Cpu &c) -> uint32_t {
        if (c.arg(0) < 5)
            return 5;
        c.write(c.arg(1), "C:\\\0", 5);
        return 4;
    });
    K("GetDriveTypeA", 1, [](Cpu &) { return 3u; });   // DRIVE_FIXED

    // ---- find ----
    K("FindFirstFileA", 2, [](Cpu &c) { return find_first(c, read_cstr(c, c.arg(0)), c.arg(1), false); });
    K("FindFirstFileW", 2, [](Cpu &c) { return find_first(c, read_wstr(c, c.arg(0)), c.arg(1), true); });
    auto next = [](Cpu &c, bool wide) -> uint32_t {
        RtFind *f = object_of<RtFind>(c.arg(0), "find");
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        if (f->next >= f->entries.size()) { set_last_error(c, ERROR_NO_MORE_FILES); return 0; }
        put_find_data(c, c.arg(1), f->entries[f->next++], wide);
        return 1;
    };
    K("FindNextFileA", 2, [next](Cpu &c) { return next(c, false); });
    K("FindNextFileW", 2, [next](Cpu &c) { return next(c, true); });
    K("FindClose", 1, [](Cpu &c) -> uint32_t {
        RtFind *f = object_of<RtFind>(c.arg(0), "find");
        if (!f) return 0;
        wx86_handle_erase(c.arg(0));
        delete f;
        return 1;
    });

    // ---- INI files ----
    K("GetPrivateProfileStringA", 6, [](Cpu &c) {
        return ini_get_string(c, read_cstr(c, c.arg(0)), c.arg(1), read_cstr(c, c.arg(2)), c.arg(3), c.arg(4),
                              read_cstr(c, c.arg(5)), c.arg(0) == 0);
    });
    K("GetPrivateProfileIntA", 4, [](Cpu &c) -> uint32_t {
        const std::string app = read_cstr(c, c.arg(0)), key = read_cstr(c, c.arg(1));
        for (const IniLine &l : ini_load(read_cstr(c, c.arg(3))))
            if (!l.key.empty() && ieq(l.section, app) && ieq(l.key, key))
                return (uint32_t)std::strtol(l.value.c_str(), nullptr, 0);
        return c.arg(2);
    });
    K("WritePrivateProfileStringA", 4, [](Cpu &c) -> uint32_t {
        const std::string app = read_cstr(c, c.arg(0)), key = read_cstr(c, c.arg(1)), value = read_cstr(c, c.arg(2));
        if (!c.arg(0)) return 0;
        return ini_write(read_cstr(c, c.arg(3)), app, c.arg(1) ? &key : nullptr, c.arg(2) ? &value : nullptr);
    });

    // ---- file mapping: a view is a copy of the file in the VirtualAlloc
    // region, written back when unmapped if the mapping is writable ----
    K("CreateFileMappingA", 6, [](Cpu &c) -> uint32_t {
        RtFile *f = file_of(c.arg(0));
        if (!f) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }   // pagefile-backed sections: not needed yet
        struct stat st;
        if (f->cache) {
            ::fstat(f->fd, &st);
        } else {
            std::fflush(f->fp);
            ::fstat(fileno(f->fp), &st);
        }
        auto *m = new RtMapping();
        m->host = f->host;
        m->size = c.arg(4) ? c.arg(4) : (uint32_t)st.st_size;
        m->writable = (c.arg(2) & 0xFF) == 0x04 || (c.arg(2) & 0xFF) == 0x40;   // PAGE_READWRITE / EXECUTE_READWRITE
        return wx86_handle_add(m);
    });
    K("MapViewOfFile", 5, [](Cpu &c) -> uint32_t {
        RtMapping *m = object_of<RtMapping>(c.arg(0), "fmap");
        if (!m) { set_last_error(c, ERROR_INVALID_HANDLE); return 0; }
        const uint32_t off = c.arg(2);
        const uint32_t n = c.arg(4) ? c.arg(4) : (off < m->size ? m->size - off : 0);
        const uint32_t va = n ? va_region().alloc(n) : 0;
        if (!va) { set_last_error(c, 8); return 0; }   // ERROR_NOT_ENOUGH_MEMORY
        if (FILE *fp = std::fopen(m->host.c_str(), "rb")) {
            std::fseek(fp, (long)off, SEEK_SET);
            const size_t got = stream_read(c, fp, va, n);
            std::fclose(fp);
            if (got < n) {
                std::vector<uint8_t> zero(n - got, 0);
                c.write(va + (uint32_t)got, zero.data(), n - (uint32_t)got);
            }
        }
        g_views.push_back({va, n, off, m});
        return va;
    });
    K("UnmapViewOfFile", 1, [](Cpu &c) -> uint32_t {
        for (size_t i = 0; i < g_views.size(); i++)
            if (g_views[i].va == c.arg(0)) {
                const View v = g_views[i];
                if (v.map->writable)
                    if (FILE *fp = std::fopen(v.map->host.c_str(), "r+b")) {
                        std::fseek(fp, (long)v.offset, SEEK_SET);
                        stream_write(c, fp, v.va, v.size);
                        std::fclose(fp);
                    }
                va_region().free(v.va);
                g_views.erase(g_views.begin() + (long)i);
                return 1;
            }
        return 0;
    });

    // ---- handles: files, finds and mappings here; the rest are engine objects ----
    K("CloseHandle", 1, [](Cpu &c) -> uint32_t {
        const uint32_t h = c.arg(0);
        if (RtFile *f = file_of(h)) {
            wx86_handle_erase(h);
            delete f;   // closes its stream or descriptor
            return 1;
        }
        if (RtMapping *m = object_of<RtMapping>(h, "fmap")) {
            wx86_handle_erase(h);
            bool viewed = false;   // views keep the mapping alive (as on Windows)
            for (const View &v : g_views)
                viewed = viewed || v.map == m;
            if (!viewed)
                delete m;
            return 1;
        }
        // Events, threads and other engine objects: drop the handle. The
        // object itself stays alive, because a scheduler wait may still point
        // at it (WinVita keeps the same rule for its per-thread waits).
        if (wx86_handle_find(h)) {
            wx86_handle_erase(h);
            return 1;
        }
        return (h == kStdInput || h == kStdOutput || h == kStdError || h == 0xFFFFFFFEu || h == 0xFFFFFFFFu) ? 1u : 0u;
    });
}

}  // namespace rt
