// Minimal ARMv6 (ARM state) instruction encoders used by the M1 executable-memory
// probe. Deliberately restricted to encodings available on ARM11 MPCore: no
// MOVW/MOVT (ARMv6T2), no Thumb-2. tests/host checks them under qemu's
// arm11mpcore model.
#ifndef D2_RUNTIME_ARMEMIT_H
#define D2_RUNTIME_ARMEMIT_H

#include <stdint.h>
#include <stddef.h>

#define ARM_BX_LR 0xE12FFF1Eu

enum { ARM_R0 = 0, ARM_R1 = 1, ARM_R2 = 2, ARM_R3 = 3 };

// ADD rd, rn, rm
static inline uint32_t arm_add_reg(unsigned rd, unsigned rn, unsigned rm)
{
    return 0xE0800000u | (rn & 15u) << 16 | (rd & 15u) << 12 | (rm & 15u);
}

// LDR rd, [pc, #imm12]  (pc reads as the instruction address + 8)
static inline uint32_t arm_ldr_pc_imm(unsigned rd, uint32_t imm12)
{
    return 0xE59F0000u | (rd & 15u) << 12 | (imm12 & 0xFFFu);
}

// B target, assembled at address `from`. Returns 0 when the target is
// misaligned or out of the +/-32 MiB range; 0 is never a valid branch here
// because the condition field is AL.
static inline uint32_t arm_b(uintptr_t from, uintptr_t to)
{
    intptr_t delta = (intptr_t)to - (intptr_t)(from + 8);
    if ((delta & 3) != 0 || delta < -(intptr_t)0x2000000 || delta > (intptr_t)0x1FFFFFC)
        return 0;
    return 0xEA000000u | ((uint32_t)(delta >> 2) & 0xFFFFFFu);
}

// Emits `uint32_t f(void) { return value; }` as LDR-literal + BX LR + literal.
// Always exactly three words, so a rewrite occupies the same bytes.
#define ARM_RET_CONST_WORDS 3
static inline size_t arm_emit_ret_const(uint32_t *p, uint32_t value)
{
    p[0] = arm_ldr_pc_imm(ARM_R0, 0); // loads p[2]
    p[1] = ARM_BX_LR;
    p[2] = value;
    return ARM_RET_CONST_WORDS;
}

// Emits `uint32_t f(uint32_t a, uint32_t b) { return a + b; }`.
static inline size_t arm_emit_add_args(uint32_t *p)
{
    p[0] = arm_add_reg(ARM_R0, ARM_R0, ARM_R1);
    p[1] = ARM_BX_LR;
    return 2;
}

#endif
