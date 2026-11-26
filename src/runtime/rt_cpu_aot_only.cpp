// A build with CpuAot (D2RT_CPU_AOT, issue #138) links no Box86: the dynarec's
// objects are in WinVita's library, and the linker takes one only for a
// symbol something refers to. These are the ones the runtime, the natives
// and the app refer to for Box86's own things (its memory, its translation-
// time intrinsics, its block profile, its options): here they are what "no
// dynarec" answers, so none of its objects is linked and none of its memory
// taken.
#ifdef D2RT_CPU_AOT

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "dyn86_intrin.h"
#include "runtime/rt_host.h"

extern "C" {

// The dynarec's memory: none.
size_t d2rt_box86_custommalloc_kb(void) { return 0; }
size_t d2rt_box86_dynarec_kb(void) { return 0; }
size_t d2rt_box86_dynarec_used_kb(void) { return 0; }
size_t d2rt_box86_jmptbl_kb(void) { return 0; }
uint32_t dyn86_jit_allocfail = 0;
unsigned d2rt_meta_reserve_refusals = 0;   // its blocks' records refused for the reserve (the app's count)

// No translated block to name at a fault, no block profile.
int dyn86_block_of_native(uintptr_t, uint32_t *, uint32_t *) { return 0; }
void dyn86_hotprof_dump(FILE *, int) {}
void dyn86_hotprof_reset(void) {}

// The translation-time intrinsics (a function's body replaced where the
// dynarec translates its call): nothing is translated.
int dyn86_intrin_n = 0;
int dyn86_intrin_on = 0;
dyn86_intrin_t dyn86_intrin_tbl[DYN86_INTRIN_MAX];
int dyn86_intrin_add(uintptr_t, dyn86_intrin_fn, uint16_t, uint16_t, uint16_t, const char *) { return 0; }
int dyn86_intrin_report(char *out, unsigned cap)
{
    if (cap)
        out[0] = 0;
    return 0;
}
int dyn86_memintrin = 0;
void dyn86_mi_arm(int, uintptr_t, uintptr_t) {}
void dyn86_mi_set_span(uint32_t) {}

// Its options and its cpuid's feature bits (aot_cpuid has the runtime's own).
int box86_dynarec_callret = 0, dyn86_nopend = 0, box86_dynarec_budgettail = 0, box86_dynarec_forward = 0;
void wx86_set_cpuid1_edx(uint32_t) {}

// What WinVita's own runtime objects refer to in it: the Bridge's base for a
// time profile's addresses, the scheduler's dump of the dynarec's last
// transfers at a fault.
uint32_t d2rt_timeprof_base = 0;
void dyn86_dump_xfer(void) {}
// ... the scheduler's time in translation (D2_JITPROF), and the features the
// kernel32 shims say the CPU has: what cpuid reports (aot_cpuid).
int dyn86_jitprof = 0;
uint64_t dyn86_jp_run_us = 0;
uint64_t dyn86_jp_now_us(void) { return 0; }
int dyn86_protectdb(void) { return 0; }   // no page of translated code to unprotect at a write
void wx86_cpuid_features(uint32_t *edx, uint32_t *ecx)
{
    if (edx)
        *edx = rt::cpuid1_edx();
    if (ecx)
        *ecx = 0;
}

}   // extern "C"

#endif   // D2RT_CPU_AOT
