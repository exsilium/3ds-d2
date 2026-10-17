// M1 guest-mapping probe: can heap pages be aliased at the Win32 image base
// 0x00400000, and does code written through the heap alias execute at the
// fixed address? Each method is tried only where it can be meaningful:
//
//   svcControlMemory(MEMOP_MAP)          both targets. The stock kernel restricts
//                                        the destination range; Azahar does not
//                                        check it, so results may legitimately differ.
//   svcControlProcessMemory(MEMOP_MAP)   hardware only (unimplemented in Azahar).
//
// A success here is a capability observation, not a decision: guest pointers stay
// distinct from host pointers until M2/M3 choose a mapping strategy.
#include "platform/ctr/probes.h"
#include "platform/ctr/target.h"
#include "runtime/armemit.h"
#include "runtime/jitmem.h"

#include <3ds.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE 0x00400000u

typedef enum { VIA_CONTROL_MEMORY, VIA_CONTROL_PROCESS_MEMORY } map_via;

static Result do_map(map_via via, bool unmap, u32 dst, u32 src, u32 size)
{
    if (via == VIA_CONTROL_MEMORY) {
        u32 out = 0;
        return svcControlMemory(&out, dst, src, size, unmap ? MEMOP_UNMAP : MEMOP_MAP, MEMPERM_READWRITE);
    }
    Handle h;
    Result rc = svcDuplicateHandle(&h, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc))
        return rc;
    rc = svcControlProcessMemory(h, dst, src, size, unmap ? MEMOP_UNMAP : MEMOP_MAP,
                                 MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    svcCloseHandle(h);
    return rc;
}

static void try_map(probe_tally *t, map_via via, u32 size)
{
    const char *name = via == VIA_CONTROL_MEMORY ? "svcControlMemory" : "svcControlProcessMemory";
    u8 *src = memalign(0x1000, size);
    if (!src) {
        plat_log("RESULT map.%s.%luk INFO outcome=no-source-memory\n", name, (unsigned long)(size >> 10));
        t->info++;
        return;
    }
    memset(src, 0, 4096);

    plat_log("CHECKPOINT %s(MAP) dst=%08lx size=%lu\n", name, (unsigned long)IMAGE_BASE, (unsigned long)size);
    Result rc = do_map(via, false, IMAGE_BASE, (u32)src, size);
    if (R_SUCCEEDED(rc) && !memory_has_perm(IMAGE_BASE, size, MEMPERM_READWRITE)) {
        plat_log("RESULT map.%s.%luk INFO outcome=false-success rc=%08lx (destination still unmapped)\n", name,
                 (unsigned long)(size >> 10), (unsigned long)rc);
        t->info++;
        free(src);
        return;
    }
    if (R_FAILED(rc)) {
        plat_log("RESULT map.%s.%luk INFO outcome=refused rc=%08lx\n", name, (unsigned long)(size >> 10),
                 (unsigned long)rc);
        t->info++;
        free(src);
        return;
    }

    // Alias check in both directions, at the first and last page.
    volatile u32 *dst = (volatile u32 *)IMAGE_BASE;
    dst[0] = 0xD2D2D2D2u;
    ((volatile u32 *)src)[(size - 4) / 4] = 0x13141516u;
    bool alias_ok = ((volatile u32 *)src)[0] == 0xD2D2D2D2u && dst[(size - 4) / 4] == 0x13141516u;

    // Execute at the fixed address, code written through the heap alias. On
    // hardware via svcControlMemory the mapping is RW only; skip execution there.
    const char *exec = "skipped";
    if (target_get()->is_emulator || via == VIA_CONTROL_PROCESS_MEMORY) {
        arm_emit_ret_const((u32 *)(src + 64), 0x00400D2Du);
        __asm__ volatile("" ::: "memory");
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(src + 64), 12);
        svcLumaInvalidateInstructionCacheRange(IMAGE_BASE + 64, 12);
        u32 got = ((u32(*)(void))(IMAGE_BASE + 64))();
        exec = got == 0x00400D2Du ? "ok" : "wrong-result";
    }

    Result urc = do_map(via, true, IMAGE_BASE, (u32)src, size);
    plat_log("RESULT map.%s.%luk INFO outcome=mapped alias=%s exec=%s unmap_rc=%08lx\n", name,
             (unsigned long)(size >> 10), alias_ok ? "ok" : "broken", exec, (unsigned long)urc);
    t->info++;
    if (R_SUCCEEDED(urc))
        free(src);
    else
        plat_log("WARN unmap failed; leaking %lu source bytes\n", (unsigned long)size);
}

void mapprobe_run(probe_tally *t)
{
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, IMAGE_BASE)) || mi.state != MEMSTATE_FREE) {
        plat_log("RESULT map.image_base INFO outcome=occupied\n");
        t->info++;
        return;
    }
    static const u32 sizes[] = {64 * 1024, 16 * 1024 * 1024};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        try_map(t, VIA_CONTROL_MEMORY, sizes[i]);
        if (!target_get()->is_emulator)
            try_map(t, VIA_CONTROL_PROCESS_MEMORY, sizes[i]);
    }
}
