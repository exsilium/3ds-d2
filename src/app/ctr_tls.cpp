// One HTTP request over the console's sockets, for problem reports (issue
// #26): https through the TLS module (src/tls/tls_module.h), plain http as
// it is (a relay). The console's own HTTPS (httpc) cannot complete a
// handshake with Sentry's servers, which take TLS 1.2 with SNI only.
//
// The module is mbedTLS, about 320 KiB. It is read from the RomFS into the
// heap, made executable the way the backend's JIT makes its code chunks
// (mman_ctr.c), used for the one request, and freed: none of it is in
// memory during play. Requests are only made before the game starts or
// after it has ended.
#include "tls/tls_module.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace tls {
namespace {

constexpr const char kModulePath[] = "romfs:/tls.bin";
constexpr int kTimeoutMs = 30000;     // a socket that stays silent this long is given up
constexpr size_t kReplyMax = 4096;    // the answer's start: its status line and Sentry's reply

bool is_emulator()
{
    s64 v = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) && v != 0;
}

Result protect(void *p, size_t size, u32 perm)
{
    Handle h;
    Result rc = svcDuplicateHandle(&h, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(rc)) {
        rc = svcControlProcessMemory(h, (u32)p, 0, size, MEMOP_PROT, perm);
        svcCloseHandle(h);
    }
    return rc;
}

// Luma3DS's svcInvalidateInstructionCacheRange (0x93); Azahar has it too.
void invalidate_icache(u32 addr, u32 size)
{
    register u32 r0 __asm__("r0") = addr;
    register u32 r1 __asm__("r1") = size;
    __asm__ volatile("svc 0x93" : "+r"(r0) : "r"(r1) : "memory");
}

// The module in memory, from load() to its destructor.
class Module {
public:
    ~Module() { unload(); }

    bool load(std::string &err)
    {
        FILE *f = std::fopen(kModulePath, "rb");
        if (!f) {
            err = "the TLS module (tls.bin) is missing";
            return false;
        }
        TlsModuleFile h;
        bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == TLS_MODULE_MAGIC && h.abi == TLS_MODULE_ABI &&
                  h.entry < h.image_size;
        if (ok) {
            size_ = (h.image_size + h.bss_size + 0xFFF) & ~0xFFFu;
            mem_ = static_cast<u8 *>(memalign(0x1000, size_));
            std::vector<u32> relocs(h.reloc_count);
            ok = mem_ && std::fread(mem_, 1, h.image_size, f) == h.image_size &&
                 (relocs.empty() || std::fread(relocs.data(), 4, relocs.size(), f) == relocs.size());
            if (ok) {
                std::memset(mem_ + h.image_size, 0, size_ - h.image_size);
                for (u32 at : relocs) {
                    if (at > h.image_size - 4 || at % 4) {
                        ok = false;
                        break;
                    }
                    *reinterpret_cast<u32 *>(mem_ + at) += (u32)(uintptr_t)mem_;
                }
            }
        }
        std::fclose(f);
        if (!ok) {
            err = mem_ ? "the TLS module (tls.bin) is damaged or of another build" : "no memory for the TLS module";
            unload();
            return false;
        }
        // Executable, as the JIT's code chunks are: on a console with
        // svcControlProcessMemory; Azahar runs code from any mapped page.
        if (!is_emulator()) {
            const Result rc = protect(mem_, size_, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
            if (rc != 0) {
                char b[80];
                std::snprintf(b, sizeof b, "the TLS module could not be made executable (%08lx)", (unsigned long)rc);
                err = b;
                std::free(mem_);   // still read-write: safe to return to the heap
                mem_ = nullptr;
                return false;
            }
            executable_ = true;
        }
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)mem_, size_);
        invalidate_icache((u32)mem_, size_);
        const TlsModuleEntry entry = reinterpret_cast<TlsModuleEntry>(mem_ + h.entry);
        if (!entry(TLS_MODULE_ABI, &api_)) {
            err = "the TLS module (tls.bin) is of another build";
            unload();
            return false;
        }
        return true;
    }

    void unload()
    {
        if (!mem_)
            return;
        // Never back to the heap while executable (as jitmem_ctr.c).
        if (!executable_ || R_SUCCEEDED(protect(mem_, size_, MEMPERM_READ | MEMPERM_WRITE)))
            std::free(mem_);
        mem_ = nullptr;
        executable_ = false;
    }

    const TlsModule &api() const { return api_; }

private:
    u8 *mem_ = nullptr;
    size_t size_ = 0;
    bool executable_ = false;
    TlsModule api_ = {};
};

// ---- What the module takes from the app ----------------------------------------------------

int host_random(unsigned char *out, size_t len)
{
    // The SSL service's generator (ssl:C; the CIA has no ps:ps).
    if (R_FAILED(sslcInit(0)))
        return -1;
    const Result rc = sslcGenerateRandomData(out, len);
    sslcExit();
    return R_SUCCEEDED(rc) ? 0 : -1;
}

int wait(int fd, short events)
{
    pollfd p = {fd, events, 0};
    return poll(&p, 1, kTimeoutMs);
}

int host_send(void *io, const unsigned char *buf, size_t len)
{
    const int fd = *static_cast<int *>(io);
    if (wait(fd, POLLOUT) <= 0)
        return -1;
    const ssize_t n = send(fd, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

int host_recv(void *io, unsigned char *buf, size_t len)
{
    const int fd = *static_cast<int *>(io);
    if (wait(fd, POLLIN) <= 0)
        return -1;
    const ssize_t n = recv(fd, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

int host_vsnprintf(char *s, size_t n, const char *fmt, va_list ap) { return std::vsnprintf(s, n, fmt, ap); }
time_t host_time(time_t *t) { return std::time(t); }
struct tm *host_gmtime_r(const time_t *t, struct tm *out) { return gmtime_r(t, out); }
void *host_calloc(size_t n, size_t size) { return std::calloc(n, size); }
void host_free(void *p) { std::free(p); }

const TlsHost kHost = {host_calloc, host_free, host_vsnprintf, host_time, host_gmtime_r, host_random, host_send, host_recv};

// ---- The request ---------------------------------------------------------------------------

// The console's sockets, from construction to destruction.
class Sockets {
public:
    Sockets()
    {
        buf_ = static_cast<u32 *>(memalign(0x1000, kSize));
        no_memory_ = !buf_;
        if (buf_ && R_FAILED(socInit(buf_, kSize))) {
            std::free(buf_);
            buf_ = nullptr;
        }
    }
    ~Sockets()
    {
        if (buf_) {
            socExit();
            std::free(buf_);
        }
    }
    bool ok() const { return buf_ != nullptr; }
    bool no_memory() const { return no_memory_; }

private:
    static constexpr u32 kSize = 0x100000;
    u32 *buf_ = nullptr;
    bool no_memory_ = false;
};

int connect_to(const std::string &host, const std::string &port, std::string &err)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        err = "could not look up " + host;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        err = "could not connect to " + host + ":" + port;
    return fd;
}

}  // namespace

// POSTs `body` to `url` with `headers` (each "Name: value\r\n"). True if an
// answer came: its status in `status`, its body's start in `answer`.
// Otherwise `err` says what failed.
bool post(const std::string &url, const std::string &headers, const std::string &body, int &status,
          std::string &answer, std::string &err)
{
    status = 0;
    answer.clear();
    const size_t colon = url.find("://");
    const std::string scheme = colon == std::string::npos ? "" : url.substr(0, colon);
    if (scheme != "https" && scheme != "http") {
        err = "not an http(s) address: " + url;
        return false;
    }
    const bool https = scheme == "https";
    const size_t host_at = colon + 3, path_at = url.find('/', host_at);
    const std::string hostport = url.substr(host_at, path_at == std::string::npos ? std::string::npos : path_at - host_at);
    const std::string path = path_at == std::string::npos ? "/" : url.substr(path_at);
    const size_t pc = hostport.rfind(':');
    const std::string host = pc == std::string::npos ? hostport : hostport.substr(0, pc);
    const std::string port = pc == std::string::npos ? (https ? "443" : "80") : hostport.substr(pc + 1);

    Module module;
    if (https && !module.load(err))
        return false;
    Sockets sockets;
    if (!sockets.ok()) {
        err = sockets.no_memory() ? "no memory for the network service" : "the network service did not start";
        return false;
    }
    int fd = connect_to(host, port, err);
    if (fd < 0)
        return false;

    const std::string head = "POST " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\n" + headers +
                             "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    std::vector<unsigned char> reply(kReplyMax);
    size_t got = 0;
    bool ok;
    if (https) {
        char why[256];
        const int r = module.api().exchange(&kHost, &fd, host.c_str(), (const unsigned char *)head.data(), head.size(),
                                            (const unsigned char *)body.data(), body.size(), reply.data(),
                                            reply.size(), &got, why, sizeof why);
        ok = r == 0;
        if (!ok)
            err = why;
    } else {
        ok = host_send(&fd, (const unsigned char *)head.data(), head.size()) == (int)head.size();
        for (size_t at = 0; ok && at < body.size();) {
            const int n = host_send(&fd, (const unsigned char *)body.data() + at, body.size() - at);
            ok = n > 0;
            at += ok ? (size_t)n : 0;
        }
        while (ok && got < reply.size()) {
            const int n = host_recv(&fd, reply.data() + got, reply.size() - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (!ok)
            err = "sending failed";
    }
    close(fd);
    if (!ok)
        return false;

    const std::string text(reinterpret_cast<const char *>(reply.data()), got);
    if (text.compare(0, 5, "HTTP/") != 0 || text.find(' ') == std::string::npos) {
        err = got ? "not an HTTP answer" : "no answer";
        return false;
    }
    status = std::atoi(text.c_str() + text.find(' ') + 1);
    const size_t end = text.find("\r\n\r\n");
    if (end != std::string::npos)
        answer = text.substr(end + 4);
    return true;
}

}  // namespace tls
