// M1 memory facts: region sizes, heap split, largest single allocations, and a
// svcQueryMemory walk of the user address space. These seed docs/memory-budget.md;
// none of them is derived from the console's total RAM.
#include "platform/ctr/probes.h"
#include "platform/ctr/target.h"
#include "runtime/jitmem.h"

#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>

extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;

typedef void *(*alloc_fn)(size_t);
typedef void (*free_fn)(void *);

// Largest single allocation, to 64 KiB resolution.
static size_t largest_block(alloc_fn a, free_fn f, size_t hi)
{
    size_t lo = 0;
    while (hi - lo > 0x10000) {
        size_t mid = lo + (hi - lo) / 2;
        void *p = a(mid);
        if (p) {
            f(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void *linear_alloc_wrap(size_t n) { return linearAlloc(n); }

static const char *state_name(u32 s)
{
    switch (s) {
    case MEMSTATE_FREE: return "free";
    case MEMSTATE_IO: return "io";
    case MEMSTATE_STATIC: return "static";
    case MEMSTATE_CODE: return "code";
    case MEMSTATE_PRIVATE: return "private";
    case MEMSTATE_SHARED: return "shared";
    case MEMSTATE_CONTINUOUS: return "continuous";
    case MEMSTATE_ALIASED: return "aliased";
    case MEMSTATE_ALIAS: return "alias";
    case MEMSTATE_ALIASCODE: return "aliascode";
    case MEMSTATE_LOCKED: return "locked";
    default: return "?";
    }
}

void memprobe_run(probe_tally *t)
{
    const target_info *ti = target_get();
    plat_log("MEM app_region size=%lu used=%lu free=%lu real_app_region=%lld\n",
             (unsigned long)osGetMemRegionSize(MEMREGION_APPLICATION),
             (unsigned long)osGetMemRegionUsed(MEMREGION_APPLICATION),
             (unsigned long)osGetMemRegionFree(MEMREGION_APPLICATION), (long long)ti->real_app_region);
    plat_log("MEM ctru heap=%lu linear_heap=%lu linear_free=%lu\n", (unsigned long)__ctru_heap_size,
             (unsigned long)__ctru_linear_heap_size, (unsigned long)linearSpaceFree());

    s64 priv = -1;
    svcGetProcessInfo(&priv, CUR_PROCESS_HANDLE, 0);
    plat_log("MEM process private_and_shared=%lld\n", (long long)priv);

    size_t big_heap = largest_block(malloc, free, __ctru_heap_size);
    size_t big_linear = largest_block(linear_alloc_wrap, linearFree, __ctru_linear_heap_size);
    plat_log("RESULT mem.largest_alloc INFO heap_bytes=%zu linear_bytes=%zu\n", big_heap, big_linear);
    t->info++;

    // Address-space map, coalesced by svcQueryMemory itself.
    u32 addr = 0x00100000;
    unsigned regions = 0;
    while (addr < 0x40000000 && regions < 64) {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || mi.size == 0)
            break;
        plat_log("MAP %08lx-%08lx %c%c%c %s\n", (unsigned long)mi.base_addr,
                 (unsigned long)(mi.base_addr + mi.size), mi.perm & MEMPERM_READ ? 'r' : '-',
                 mi.perm & MEMPERM_WRITE ? 'w' : '-', mi.perm & MEMPERM_EXECUTE ? 'x' : '-', state_name(mi.state));
        if (mi.base_addr + mi.size <= addr)
            break;
        addr = mi.base_addr + mi.size;
        regions++;
    }

    // Is the default Win32 image base unoccupied in this process?
    MemInfo mi;
    PageInfo pi;
    bool free_at_base = R_SUCCEEDED(svcQueryMemory(&mi, &pi, 0x00400000)) && mi.state == MEMSTATE_FREE;
    plat_log("RESULT mem.image_base_0x00400000_free INFO free=%s region=%08lx+%lx\n", free_at_base ? "yes" : "no",
             (unsigned long)mi.base_addr, (unsigned long)mi.size);
    t->info++;
}
