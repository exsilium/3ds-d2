// The x86 lane's single definition of fixtures, cases, scratch seeding and
// state hashing, shared by every executor (harness.c on qemu-i386,
// cpu_runner.cpp on a CPU backend). Header-only; C and C++.
#ifndef D2_TESTS_X86_CASES_H
#define D2_TESTS_X86_CASES_H

#include "abi.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { X86_EAX, X86_ECX, X86_EDX, X86_EBX, X86_EBP, X86_ESI, X86_EDI, X86_EFL };
enum { C_ARITH, C_SHIFT, C_DIV, C_IDIV, C_BITS, C_GEN, C_X87 };

// Flag masks follow the Intel SDM's "undefined" entries for each instruction.
#define NOAF (FL_ALL & ~FL_AF)
#define SHIFTCL (FL_CF | FL_PF | FL_ZF | FL_SF | FL_DF)  // OF only defined for count 1; AF undefined
#define ROTCL (FL_CF | FL_DF)                            // OF only defined for count 1
#define MULF (FL_CF | FL_OF | FL_DF)
#define BITF (FL_CF | FL_DF)
#define DIVF FL_DF

// X(name, compared flag mask, case generator). Order is the output order.
#define X86_FIXTURES(X)                                                                         \
    X(add, FL_ALL, C_ARITH) X(adc, FL_ALL, C_ARITH) X(sub, FL_ALL, C_ARITH)                     \
    X(sbb, FL_ALL, C_ARITH) X(cmp, FL_ALL, C_ARITH) X(and, NOAF, C_ARITH) X(or, NOAF, C_ARITH)  \
    X(xor, NOAF, C_ARITH) X(test, NOAF, C_ARITH) X(inc, FL_ALL, C_ARITH)                        \
    X(dec, FL_ALL, C_ARITH) X(neg, FL_ALL, C_ARITH) X(not, FL_ALL, C_ARITH)                     \
    X(add8, FL_ALL, C_ARITH) X(sub8, FL_ALL, C_ARITH) X(add16, FL_ALL, C_ARITH)                 \
    X(adc16, FL_ALL, C_ARITH) X(highregs, 0, C_ARITH) X(mul, MULF, C_ARITH)                     \
    X(imul1, MULF, C_ARITH) X(imul2, MULF, C_ARITH) X(imul3, MULF, C_ARITH) X(div, DIVF, C_DIV) \
    X(idiv, DIVF, C_IDIV) X(signext, FL_ALL, C_ARITH) X(setcc, FL_ALL, C_ARITH)                 \
    X(cmovcc, FL_ALL, C_ARITH) X(jcc, FL_ALL, C_ARITH) X(shl_cl, SHIFTCL, C_SHIFT)              \
    X(shr_cl, SHIFTCL, C_SHIFT) X(sar_cl, SHIFTCL, C_SHIFT) X(shl1, NOAF, C_SHIFT)              \
    X(shr1, NOAF, C_SHIFT) X(sar1, NOAF, C_SHIFT) X(rol_cl, ROTCL, C_SHIFT)                     \
    X(ror_cl, ROTCL, C_SHIFT) X(rcl_cl, ROTCL, C_SHIFT) X(rcr_cl, ROTCL, C_SHIFT)               \
    X(shld_cl, SHIFTCL, C_SHIFT) X(shrd_cl, SHIFTCL, C_SHIFT) X(bt, BITF, C_SHIFT)              \
    X(bts, BITF, C_SHIFT) X(btr, BITF, C_SHIFT) X(btc, BITF, C_SHIFT) X(bt_mem, BITF, C_SHIFT)  \
    X(bsf, FL_ZF | FL_DF, C_BITS) X(bsr, FL_ZF | FL_DF, C_BITS) X(bswap, FL_ALL, C_ARITH)       \
    X(movx, FL_ALL, C_ARITH) X(lea, FL_ALL, C_ARITH) X(cmpxchg, FL_ALL, C_ARITH)                \
    X(xadd, FL_ALL, C_ARITH) X(unaligned, FL_ALL, C_ARITH) X(pushpop, FL_ALL, C_GEN)            \
    X(pushad, NOAF, C_GEN) X(call_ret, FL_ALL, C_GEN) X(stdcall, FL_ALL, C_GEN)                 \
    X(recursion, MULF, C_GEN) X(callind, NOAF, C_GEN) X(jumptable, NOAF, C_GEN)                 \
    X(callreg, NOAF, C_GEN) X(loop, FL_ALL, C_GEN) X(enter_leave, FL_ALL, C_GEN)                \
    X(rep_movsb, FL_ALL, C_GEN) X(rep_movsd, FL_ALL, C_GEN) X(movsd_backward, FL_ALL, C_GEN)    \
    X(rep_stosd, FL_ALL, C_GEN) X(repne_scasb, FL_ALL, C_GEN) X(repe_cmpsb, FL_ALL, C_GEN)      \
    X(lodsb_sum, FL_ALL, C_GEN) X(fs_teb, FL_ALL, C_GEN) X(x87_arith, FL_ALL, C_X87)            \
    X(x87_round, FL_ALL, C_X87) X(x87_compare, FL_DF, C_X87) X(x87_const, FL_ALL, C_GEN)        \
    X(x87_trans, FL_ALL, C_X87) X(x87_80bit, FL_ALL, C_GEN) X(x87_status, FL_ALL, C_GEN) \
    X(x87_int64, FL_ALL, C_X87) X(x87_unaligned, FL_ALL, C_X87) X(x87_misc, FL_ALL, C_X87)              \
    X(lock_ops, FL_ALL, C_ARITH) X(rep_long, FL_ALL, C_GEN) \
    X(bsf_mem, FL_ZF | FL_DF, C_BITS) X(bsr_mem, FL_ZF | FL_DF, C_BITS) X(bsf_same, FL_ZF | FL_DF, C_BITS)

static const uint32_t x86_vals[] = {0x00000000, 0x00000001, 0x0000007f, 0x00008000, 0x7fffffff,
                                    0x80000000, 0xfffffffe, 0xffffffff, 0x12345678, 0x9abcdef0};
#define X86_NVALS (int)(sizeof x86_vals / sizeof x86_vals[0])
static const uint32_t x86_shift_vals[] = {0x80000001, 0x12345678, 0xffffffff, 0x40000000, 0x00000001};
static const uint32_t x86_shift_counts[] = {0, 1, 2, 7, 15, 31, 32, 33};
// {edx, eax, ecx}: quotient fits, divisor non-zero.
static const uint32_t x86_div_cases[][3] = {
    {0, 0, 1},          {0, 100, 7},          {0, 0xffffffff, 0xffffffff}, {1, 0, 2},
    {0x7ffffffe, 0xffffffff, 0x7fffffff}, {0x1234, 0x56789abc, 0x87654321}, {0, 0x80000000, 3},
};
static const uint32_t x86_idiv_cases[][3] = {
    {0, 100, 7},        {0xffffffff, 0xffffff9c, 7}, {0, 100, 0xfffffff9}, {0xffffffff, 0x80000000, 2},
    {0, 0x7fffffff, 0xffffffff}, {0xffffffff, 0xfffffffb, 0xfffffffe},
};
static const uint32_t x86_bits_vals[] = {0x00000001, 0x80000000, 0x00010000, 0x7ffffff0, 0x12345678, 0xffffffff};
static const uint32_t x86_x87_pairs[][2] = {
    {0, 1}, {1, 3}, {7, 2}, {0xfffffff9, 2}, {12345678, 1000}, {0x7fffffff, 0x7fffffff},
    {0x80000000, 3}, {5, 0xfffffffd}, {1000000, 7}, {2, 2},
};

static inline int x86_ncases(int gen)
{
    switch (gen) {
    case C_ARITH: return X86_NVALS * X86_NVALS * 2;
    case C_SHIFT: return 5 * 8 * 2;
    case C_DIV: return (int)(sizeof x86_div_cases / sizeof x86_div_cases[0]);
    case C_IDIV: return (int)(sizeof x86_idiv_cases / sizeof x86_idiv_cases[0]);
    case C_BITS: return 6;
    case C_GEN: return 6;
    case C_X87: return (int)(sizeof x86_x87_pairs / sizeof x86_x87_pairs[0]);
    }
    return 0;
}

// Input registers of case i, in X86_EAX..X86_EFL order.
static inline void x86_make_case(int gen, int i, uint32_t r[8])
{
    r[X86_EAX] = 0;
    r[X86_ECX] = 0;
    r[X86_EDX] = 0x0f0f0f0fu ^ (uint32_t)i;
    r[X86_EBX] = 0x5a5a5a5au + (uint32_t)i;
    r[X86_EBP] = X86_SCRATCH_BASE + 0x400;
    r[X86_ESI] = X86_SCRATCH_BASE + 0x100;
    r[X86_EDI] = X86_SCRATCH_BASE + 0x800;
    r[X86_EFL] = 0x202;
    switch (gen) {
    case C_ARITH:
        r[X86_EAX] = x86_vals[(i >> 1) / X86_NVALS];
        r[X86_ECX] = x86_vals[(i >> 1) % X86_NVALS];
        r[X86_EFL] |= (uint32_t)(i & 1);  // CF in
        break;
    case C_SHIFT:
        r[X86_EAX] = x86_shift_vals[(i >> 1) / 8];
        r[X86_ECX] = x86_shift_counts[(i >> 1) % 8];
        r[X86_EDX] = 0xcafef00du;
        r[X86_EFL] |= (uint32_t)(i & 1);
        break;
    case C_DIV:
        r[X86_EDX] = x86_div_cases[i][0], r[X86_EAX] = x86_div_cases[i][1], r[X86_ECX] = x86_div_cases[i][2];
        break;
    case C_IDIV:
        r[X86_EDX] = x86_idiv_cases[i][0], r[X86_EAX] = x86_idiv_cases[i][1], r[X86_ECX] = x86_idiv_cases[i][2];
        break;
    case C_BITS:
        r[X86_EAX] = x86_bits_vals[i];
        break;
    case C_GEN:
        r[X86_EAX] = 0x1000u * (uint32_t)i + 3;
        r[X86_ECX] = (uint32_t)i * 5 + 1;
        r[X86_EFL] |= (uint32_t)(i & 1);
        break;
    case C_X87:
        r[X86_EAX] = x86_x87_pairs[i][0];
        r[X86_ECX] = x86_x87_pairs[i][1];
        break;
    }
}

// Fills the guest region [X86_SCRATCH_BASE, X86_STACK_TOP) as seen through
// `mem` (host pointer to its first byte): xorshift32 scratch seeded from the
// case id, a zeroed stack, and the TEB words.
static inline void x86_seed_memory(uint8_t *mem, uint32_t seed)
{
    uint32_t s = 0x9e3779b9u ^ seed;
    for (uint32_t i = 0; i < X86_SCRATCH_SIZE / 4; i++) {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        memcpy(mem + 4 * i, &s, 4);
    }
    memset(mem + (X86_STACK_LOW - X86_SCRATCH_BASE), 0, X86_STACK_TOP - X86_STACK_LOW);
    uint32_t end_of_chain = 0xffffffffu, self = X86_TEB_BASE;
    memcpy(mem + (X86_TEB_BASE - X86_SCRATCH_BASE), &end_of_chain, 4);
    memcpy(mem + (X86_TEB_BASE - X86_SCRATCH_BASE) + 0x18, &self, 4);
}

static inline uint32_t x86_case_seed(size_t fixture_index, int case_index)
{
    return (uint32_t)(fixture_index * 1000 + (size_t)case_index);
}

static inline uint64_t x86_fnv64(const uint8_t *b, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// Fixtures whose results live in memory also print 64 bytes at the input EDI.
static inline int x86_dumps_memory(const char *name)
{
    return !strncmp(name, "x87", 3) || !strcmp(name, "setcc") || !strcmp(name, "cmovcc") ||
           !strcmp(name, "lock_ops");
}

#endif
