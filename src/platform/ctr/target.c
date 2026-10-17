#include "platform/ctr/target.h"
#include "runtime/jitmem.h"

#include <string.h>

static target_info g_target;

// Emulator info strings are packed 7 characters per s64.
static void emu_string(char *out, size_t cap, s32 first_param, int parts)
{
    size_t n = 0;
    for (int i = 0; i < parts && n + 8 <= cap; i++) {
        s64 v = 0;
        if (R_FAILED(svcGetSystemInfo(&v, 0x20000, first_param + i)))
            break;
        memcpy(out + n, &v, 8);
        n += strnlen(out + n, 8);
    }
    out[n < cap ? n : cap - 1] = '\0';
}

void target_detect(target_info *t)
{
    memset(t, 0, sizeof *t);
    t->real_app_region = -1;

    // EMULATOR_ID: 1 = Citra, 2 = Azahar (EmulatorIDs in Azahar's svc.cpp; the
    // comment there saying "always 1" is stale). Hardware rejects type 0x20000.
    s64 v = 0;
    s64 emu_id = R_SUCCEEDED(svcGetSystemInfo(&v, 0x20000, 0)) ? v : 0;
    t->is_emulator = emu_id != 0;
    if (t->is_emulator) {
        emu_string(t->emu_build, sizeof t->emu_build, 11, 1);
        emu_string(t->emu_commit, sizeof t->emu_commit, 40, 2);
    }

    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    t->is_new3ds = n3ds;

    // Luma answers type 0x10000 itself; the stock kernel rejects it. Azahar also
    // answers some 0x10000 parameters, so Luma is only inferred on hardware.
    if (!t->is_emulator && R_SUCCEEDED(svcGetSystemInfo(&v, 0x10000, 0))) {
        t->luma = true;
        t->luma_version = (u32)v;
    }
    if (R_SUCCEEDED(svcGetSystemInfo(&v, 0x10000, 0x80)))
        t->real_app_region = v;

    // Deliberate New 3DS CPU configuration: 804 MHz and L2 cache.
    if (n3ds) {
        osSetSpeedupEnable(true);
        t->speedup = true;
    }

    t->name = emu_id == 2 ? "azahar" : emu_id ? "citra" : (n3ds ? "n3ds-hw" : "o3ds-hw");
    g_target = *t;
}

const target_info *target_get(void) { return &g_target; }

bool memory_has_perm(u32 addr, u32 size, u32 perm)
{
    u32 end = addr + size;
    while (addr < end) {
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || mi.state == MEMSTATE_FREE || (mi.perm & perm) != perm ||
            mi.size == 0)
            return false;
        addr = mi.base_addr + mi.size;
    }
    return true;
}

// --- probe support services -----------------------------------------------------

uint64_t plat_ticks(void) { return svcGetSystemTick(); }
uint64_t plat_ticks_per_sec(void) { return SYSCLOCK_ARM11; }
void plat_yield_ms(unsigned ms) { svcSleepThread((s64)ms * 1000000); }

typedef struct thread_tramp {
    void (*fn)(void *);
    void *arg;
} thread_tramp;

static void tramp_entry(void *p)
{
    thread_tramp *t = p;
    t->fn(t->arg);
}

int plat_run_on_thread(void (*fn)(void *), void *arg)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    thread_tramp t = {fn, arg};
    // Same core as the main thread, one step higher priority so it runs now.
    Thread th = threadCreate(tramp_entry, &t, 32 * 1024, prio > 0x18 ? prio - 1 : prio, -2, false);
    if (!th)
        return -1;
    threadJoin(th, U64_MAX);
    threadFree(th);
    return 0;
}
