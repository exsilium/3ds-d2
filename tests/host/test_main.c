// Host-test lane: checks emitter encodings against values produced by the
// assembler, then runs the shared JIT probe on qemu's ARM11 MPCore model.
#include "probe/jitprobe.h"
#include "runtime/armemit.h"
#include "runtime/jitmem.h"

#include <inttypes.h>
#include <stdio.h>

// Reference encodings assembled by GNU as for -march=armv6k -marm.
extern const uint32_t ref_insns[];
extern const uint32_t ref_insns_count;

static unsigned failures;

static void expect(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    printf("RESULT emit.%s %s got=0x%08" PRIx32 " want=0x%08" PRIx32 "\n", what, ok ? "PASS" : "FAIL", got, want);
    if (!ok)
        failures++;
}

int main(void)
{
    if (ref_insns_count != 6) {
        printf("RESULT emit.reference FAIL count=%" PRIu32 "\n", ref_insns_count);
        return 1;
    }
    expect("ldr_pc_r0_0", arm_ldr_pc_imm(ARM_R0, 0), ref_insns[0]);
    expect("ldr_pc_r3_4092", arm_ldr_pc_imm(ARM_R3, 4092), ref_insns[1]);
    expect("bx_lr", ARM_BX_LR, ref_insns[2]);
    expect("add_r0_r0_r1", arm_add_reg(ARM_R0, ARM_R0, ARM_R1), ref_insns[3]);
    // ref_insns[4] is "b .+0x100" and [5] is "b .-0x100", each at its own address.
    expect("b_forward", arm_b((uintptr_t)&ref_insns[4], (uintptr_t)&ref_insns[4] + 0x100), ref_insns[4]);
    expect("b_backward", arm_b((uintptr_t)&ref_insns[5], (uintptr_t)&ref_insns[5] - 0x100), ref_insns[5]);
    expect("b_misaligned_rejected", arm_b(0x1000, 0x1002), 0);
    expect("b_out_of_range_rejected", arm_b(0x1000, 0x1000 + 0x4000000), 0);

    jitprobe_config cfg = {.pool_size = 64 * 1024, .stress_rounds = 4, .yield_count = 2};
    jitprobe_result r;
    jitprobe_run(&cfg, &r);

    unsigned total_failed = failures + r.failed;
    printf("RESULT host.summary %s emit_failures=%u probe_failed=%u probe_passed=%u\n",
           total_failed ? "FAIL" : "PASS", failures, r.failed, r.passed);
    return total_failed ? 1 : 0;
}
