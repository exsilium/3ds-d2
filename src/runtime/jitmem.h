// Platform contract for executable memory, implemented per target:
//   src/platform/ctr/jitmem_ctr.c  3DS hardware and Azahar
//   tests/host/plat_linux.c        host-test lane (qemu-arm)
#ifndef D2_RUNTIME_JITMEM_H
#define D2_RUNTIME_JITMEM_H

#include <stdint.h>
#include <stddef.h>

typedef struct jit_region {
    uint8_t *rw;   // address the emitter writes through
    uint8_t *rx;   // address code executes from (== rw for a single mapping)
    size_t size;   // page-rounded
    void *priv;    // backend bookkeeping
} jit_region;

// Returns 0 on success, otherwise a backend error code (3DS: a Result).
int32_t jit_alloc(jit_region *r, size_t size);
void jit_free(jit_region *r);

// Makes [off, off+len) written through rw visible to instruction fetch at rx:
// clean the data cache, then invalidate instruction caches, including any
// emulator translation cache. Returns 0 on success.
int32_t jit_sync(const jit_region *r, size_t off, size_t len);

// Data-cache clean only, without instruction-cache invalidation. Used solely to
// observe whether stale translated code is still reachable.
int32_t jit_sync_dcache_only(const jit_region *r, size_t off, size_t len);

// Human-readable name of the permission and synchronisation method in use.
const char *jit_method(void);

// Support services the probe uses.
uint64_t plat_ticks(void);
uint64_t plat_ticks_per_sec(void);
void plat_yield_ms(unsigned ms);
// Runs fn(arg) on a separate thread and waits for it. Returns 0 on success.
int plat_run_on_thread(void (*fn)(void *), void *arg);

void plat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
