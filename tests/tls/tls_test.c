// The TLS module (tls.bin, src/tls/tls_module.h) end to end, under qemu-arm:
// loaded and relocated as the console's loader does (src/app/ctr_tls.cpp),
// then one envelope POSTed to Sentry through it over a real socket, and a
// handshake with a local server whose certificate none of the module's roots
// signed, which must be refused. Run through `scripts/docker.sh test tls`
// (tests/tls/run.sh; needs the network).
//
// The module is built for the 3DS's C library: its time_t is 64-bit and its
// struct tm is newlib's. This program is built against glibc, so the host
// functions take and give those layouts explicitly.
//
// Usage: tls_test <tls.bin> <ip> <server name> <path> <sentry key> [<local untrusted port>]
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// tls_module.h's types, with the 3DS's time_t and struct tm.
typedef int64_t ctr_time_t;
struct ctr_tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
typedef struct {
    uint32_t magic, abi, image_size, bss_size, entry, reloc_count;
} TlsModuleFile;
typedef struct {
    void *(*calloc)(size_t, size_t);
    void (*free)(void *);
    int (*vsnprintf)(char *, size_t, const char *, va_list);
    ctr_time_t (*time)(ctr_time_t *);
    struct ctr_tm *(*gmtime_r)(const ctr_time_t *, struct ctr_tm *);
    int (*random)(unsigned char *, size_t);
    int (*send)(void *, const unsigned char *, size_t);
    int (*recv)(void *, unsigned char *, size_t);
} TlsHost;
typedef int (*TlsExchange)(const TlsHost *, void *, const char *, const unsigned char *, size_t,
                           const unsigned char *, size_t, unsigned char *, size_t, size_t *, char *, size_t);
typedef struct {
    TlsExchange exchange;
    const char *version;
} TlsModule;
typedef int (*TlsModuleEntry)(uint32_t, TlsModule *);

static int g_fail;
#define CHECK(c, ...)                                  \
    do {                                               \
        if (!(c)) {                                    \
            printf("FAIL %s: ", #c);                   \
            printf(__VA_ARGS__);                       \
            printf("\n");                              \
            g_fail++;                                  \
        }                                              \
    } while (0)

static ctr_time_t host_time(ctr_time_t *t)
{
    const ctr_time_t now = (ctr_time_t)time(NULL);
    if (t)
        *t = now;
    return now;
}

static struct ctr_tm *host_gmtime_r(const ctr_time_t *t, struct ctr_tm *out)
{
    const time_t tt = (time_t)*t;
    struct tm tm;
    if (!gmtime_r(&tt, &tm))
        return NULL;
    *out = (struct ctr_tm){tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon,
                           tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    return out;
}

static int host_random(unsigned char *out, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    const int ok = f && fread(out, 1, len, f) == len;
    if (f)
        fclose(f);
    return ok ? 0 : -1;
}

static int host_send(void *io, const unsigned char *buf, size_t len)
{
    struct pollfd p = {*(int *)io, POLLOUT, 0};
    if (poll(&p, 1, 30000) <= 0)
        return -1;
    const ssize_t n = send(*(int *)io, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

static int host_recv(void *io, unsigned char *buf, size_t len)
{
    struct pollfd p = {*(int *)io, POLLIN, 0};
    if (poll(&p, 1, 30000) <= 0)
        return -1;
    const ssize_t n = recv(*(int *)io, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

static const TlsHost kHost = {calloc, free, vsnprintf, host_time, host_gmtime_r, host_random, host_send, host_recv};

static TlsModule load(const char *path)
{
    TlsModule m = {0};
    FILE *f = fopen(path, "rb");
    TlsModuleFile h;
    if (!f || fread(&h, sizeof h, 1, f) != 1 || h.magic != 0x444d4c54u || h.abi != 1u) {
        printf("FAIL: %s is not a TLS module\n", path);
        exit(1);
    }
    const size_t size = (h.image_size + h.bss_size + 0xFFF) & ~0xFFFu;
    unsigned char *mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint32_t *relocs = malloc(h.reloc_count * 4 + 4);
    if (mem == MAP_FAILED || fread(mem, 1, h.image_size, f) != h.image_size ||
        fread(relocs, 4, h.reloc_count, f) != h.reloc_count) {
        printf("FAIL: reading %s\n", path);
        exit(1);
    }
    fclose(f);
    for (uint32_t i = 0; i < h.reloc_count; i++)
        *(uint32_t *)(mem + relocs[i]) += (uint32_t)(uintptr_t)mem;
    free(relocs);
    __builtin___clear_cache((char *)mem, (char *)mem + size);
    if (!((TlsModuleEntry)(mem + h.entry))(1u, &m)) {
        printf("FAIL: the module refused ABI 1\n");
        exit(1);
    }
    printf("loaded %s at %p: %u bytes, %u relocations, mbedTLS %s\n", path, (void *)mem, (unsigned)size,
           (unsigned)h.reloc_count, m.version);
    return m;
}

static int dial(const char *ip, int port)
{
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("FAIL: connecting to %s\n", ip);
        exit(1);
    }
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: tls_test <tls.bin> <ip> <server name> <path> <sentry key> [<local untrusted port>]\n");
        return 2;
    }
    const TlsModule m = load(argv[1]);
    const char *ip = argv[2], *server = argv[3], *path = argv[4], *key = argv[5];

    {   // A small envelope, as the app's (no gzip: Sentry takes both).
        char id[33];
        unsigned char r[16];
        host_random(r, sizeof r);
        for (int i = 0; i < 16; i++)
            sprintf(id + 2 * i, "%02x", r[i]);
        char ev[512], body[1024], head[1024];
        snprintf(ev, sizeof ev,
                 "{\"event_id\":\"%s\",\"platform\":\"native\",\"level\":\"info\",\"logger\":\"3ds-d2\","
                 "\"environment\":\"test\",\"message\":{\"formatted\":"
                 "\"TLS module test: an envelope through tls.bin under qemu-arm (issue #26)\"}}",
                 id);
        snprintf(body, sizeof body, "{\"event_id\":\"%s\"}\n{\"type\":\"event\",\"length\":%zu}\n%s\n", id, strlen(ev), ev);
        snprintf(head, sizeof head,
                 "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: 3ds-d2/tls-test\r\n"
                 "Content-Type: application/x-sentry-envelope\r\n"
                 "X-Sentry-Auth: Sentry sentry_version=7, sentry_key=%s, sentry_client=3ds-d2/tls-test\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                 path, server, key, strlen(body));
        int fd = dial(ip, 443);
        unsigned char reply[4096];
        size_t got = 0;
        char err[256];
        const int rc = m.exchange(&kHost, &fd, server, (const unsigned char *)head, strlen(head),
                                  (const unsigned char *)body, strlen(body), reply, sizeof reply - 1, &got, err,
                                  sizeof err);
        close(fd);
        reply[got] = 0;
        CHECK(rc == 0, "exchange: %d %s", rc, err);
        CHECK(strncmp((char *)reply, "HTTP/1.1 200", 12) == 0, "answer: %.80s", (char *)reply);
        const char *b = strstr((char *)reply, "\r\n\r\n");
        printf("event %s: %.12s ... %s\n", id, (char *)reply, b ? b + 4 : "");
    }
    if (argc > 6) {   // A certificate none of the roots signed: the check must refuse it.
        int fd = dial("127.0.0.1", atoi(argv[6]));
        unsigned char reply[256];
        size_t got = 0;
        char err[256];
        const char head[] = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
        const int rc = m.exchange(&kHost, &fd, "localhost", (const unsigned char *)head, sizeof head - 1, NULL, 0, reply,
                                  sizeof reply, &got, err, sizeof err);
        close(fd);
        CHECK(rc != 0 && strstr(err, "certificate"), "an untrusted certificate was accepted: %d %s", rc, err);
        printf("untrusted server: %s\n", err);
    }
    printf("RESULT tls %s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
