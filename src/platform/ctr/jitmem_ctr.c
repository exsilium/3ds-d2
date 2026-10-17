// Executable memory on the 3DS.
//
// Hardware (Luma3DS): heap pages are re-protected RWX with svcControlProcessMemory
// on a duplicated process handle. This needs SVC 0x70 access, which the probe
// records rather than assumes.
//
// Azahar: svcControlProcessMemory is unimplemented (it logs and returns with r0
// unchanged, which reads as success) and svcControlMemory rejects execute
// permission. Azahar fetches instructions from any mapped page without an
// execute check, so plain RW heap pages run there. That is an emulator-specific
// capability, labelled as such in the method name.
//
// Synchronisation on both: svcFlushProcessDataCache (stock SVC 0x54, a no-op in
// Azahar) followed by Luma's svcInvalidateInstructionCacheRange (0x93), which in
// Azahar also invalidates the dynarmic translation cache.
#include "runtime/jitmem.h"
#include "platform/ctr/target.h"

#include <malloc.h>
#include <stdlib.h>

static const char *g_method = "none";

const char *jit_method(void) { return g_method; }

static Result protect(void *p, size_t size, u32 perm)
{
    Handle h;
    Result rc = svcDuplicateHandle(&h, CUR_PROCESS_HANDLE);
    if (R_FAILED(rc))
        return rc;
    rc = svcControlProcessMemory(h, (u32)p, 0, size, MEMOP_PROT, perm);
    svcCloseHandle(h);
    if (R_SUCCEEDED(rc) && !memory_has_perm((u32)p, size, perm))
        rc = (Result)0xE0E01BF4; // reported success, permission unchanged
    return rc;
}

int32_t jit_alloc(jit_region *r, size_t size)
{
    size = (size + 0xFFF) & ~(size_t)0xFFF;
    void *p = memalign(0x1000, size);
    if (!p)
        return (int32_t)0xD8601FF3; // out of memory, application-defined
    if (target_get()->is_emulator) {
        g_method = "azahar:rw-heap-no-nx+svc54+svc93";
    } else {
        plat_log("CHECKPOINT svcControlProcessMemory(PROT,RWX) size=%zu\n", size);
        Result rc = protect(p, size, MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
        if (R_FAILED(rc)) {
            free(p);
            g_method = "hw:svc70-prot-failed";
            return rc;
        }
        g_method = "hw:svc70-prot-rwx+svc54+svc93";
    }
    r->rw = r->rx = p;
    r->size = size;
    r->priv = 0;
    return 0;
}

void jit_free(jit_region *r)
{
    if (!r->rw)
        return;
    if (!target_get()->is_emulator && R_FAILED(protect(r->rw, r->size, MEMPERM_READWRITE))) {
        // Never return pages to the heap while they are still executable.
        plat_log("WARN jit_free: could not restore RW; leaking %zu bytes\n", r->size);
    } else {
        free(r->rw);
    }
    r->rw = r->rx = 0;
}

int32_t jit_sync_dcache_only(const jit_region *r, size_t off, size_t len)
{
    return svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(r->rw + off), len);
}

int32_t jit_sync(const jit_region *r, size_t off, size_t len)
{
    Result rc = jit_sync_dcache_only(r, off, len);
    if (R_FAILED(rc))
        return rc;
    return svcLumaInvalidateInstructionCacheRange((u32)(r->rx + off), len);
}
