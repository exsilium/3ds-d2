// jitmem.h implementation for the host-test lane (Linux armel under qemu-arm).
// qemu detects writes to translated pages itself, so this exercises emitter
// encodings and probe logic on an ARMv6 CPU model, not cache maintenance.
#define _GNU_SOURCE
#include "runtime/jitmem.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

const char *jit_method(void) { return "linux:mmap-rwx+__clear_cache"; }

int32_t jit_alloc(jit_region *r, size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size = (size + page - 1) & ~(page - 1);
    void *p = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    r->rw = r->rx = p;
    r->size = size;
    r->priv = 0;
    return 0;
}

void jit_free(jit_region *r)
{
    if (r->rw)
        munmap(r->rw, r->size);
    r->rw = r->rx = 0;
}

int32_t jit_sync(const jit_region *r, size_t off, size_t len)
{
    __builtin___clear_cache((char *)r->rx + off, (char *)r->rx + off + len);
    return 0;
}

int32_t jit_sync_dcache_only(const jit_region *r, size_t off, size_t len) { return 0; }

uint64_t plat_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t plat_ticks_per_sec(void) { return 1000000000ull; }

void plat_yield_ms(unsigned ms) { usleep(ms * 1000); }

typedef struct tramp {
    void (*fn)(void *);
    void *arg;
} tramp;

static void *tramp_entry(void *p)
{
    tramp *t = p;
    t->fn(t->arg);
    return 0;
}

int plat_run_on_thread(void (*fn)(void *), void *arg)
{
    pthread_t th;
    tramp t = {fn, arg};
    if (pthread_create(&th, 0, tramp_entry, &t))
        return -1;
    return pthread_join(th, 0);
}

void plat_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}
