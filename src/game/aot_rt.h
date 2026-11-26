/* What the AOT spike's generated C (issue #113, tools/aot/recompile)
 * calls and includes. The generated file is made from the user's own
 * Game.exe under build/aot/ and is never committed; this header and
 * aot_spike.cpp are ours.
 *
 * The guest's memory model is the natives': guest address va is host
 * aot_mem + va (d2_native.cpp's H()), little-endian, unaligned access
 * through memcpy. Registers are uint32_t locals. Flags are kept lazily: a
 * flag-setting instruction whose flags are read stores its operands and
 * result (F_A, F_B, F_R, and F_C for a carry in or an overflow), and the
 * reader works the condition out with aot_cc() for that instruction's
 * kind and width, constant at compile time, so gcc folds it into a
 * compare.
 *
 * The generated files (aot_spike.c, the tables; aot_spike_NN.c, the
 * functions, issue #135) are each compiled several times (the build's
 * AOT_VARIANT):
 *   default: aot_* symbols;
 *   AOT_JOURNAL: aotj_*, every store journaled first (the check mode);
 *   AOT_NOINLINE: aotn_*, no compiled function inlined into another, so
 *   every call between compiled functions is a real call (to measure what
 *   the signatures save).
 */
#ifndef D2_GAME_AOT_RT_H
#define D2_GAME_AOT_RT_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t r[8];   /* eax ecx edx ebx esp ebp esi edi */
} AotRegs;
typedef uint32_t (*AotRootFn)(uint32_t *r);
typedef struct {
    uint32_t va;
    AotRootFn sig, ctx;   /* with register signatures; all eight through a context */
    const char *name;
    uint8_t keep;         /* the whole instructions (5 bytes or more) a trampoline copies */
    uint8_t pops;         /* the bytes its ret pops after the return address */
    uint8_t head[16];     /* the game's first bytes there */
} AotRoot;
/* The functions' table (issues #114, #115; AOT_SYM(funcs) in each build):
 * every compiled function, entered with all eight registers as a root is.
 * keep 0: its entry can't be hooked (the recompiler's hookable(): a branch in
 * its first bytes or into them, or pops not known). */
typedef struct {
    uint32_t va;
    AotRootFn sig;        /* its register-signature code (journaled in aotj_funcs) */
    const char *name;
    uint8_t keep, pops;   /* pops AOT_POPS_LOOSE: a loose function (below) */
    uint8_t out;          /* bit k: r[k] is an output its compiled callers read */
    uint8_t preserved;    /* bit k: r[k] comes back as it went in */
    uint8_t root;         /* also in aot_roots */
    uint8_t head[16];
    /* Its code as compiled (issue #115): FNV-1a of the bytes of its ranges
     * (lo, hi pairs), and the indexes in this table of the compiled
     * functions it calls in C. A function whose bytes changed since (a
     * native's patch) runs under Box86, with every compiled caller that
     * reaches it in C. */
    uint32_t hash;
    uint16_t nranges, ncallees;
    const uint32_t *ranges;
    const uint16_t *callees;
    /* 1: its C never calls Box86, nor does any compiled callee's (no nested
     * run), so Box86 may enter it in line (the backend's in-line shim). 0
     * under AOT_SPLIT, where every call between compiled functions is
     * Box86's. */
    uint8_t leaf;
    /* 1: its C, or a compiled callee's, may exit (aot_exit, issue #115):
     * hand the rest of the call to Box86 at a call that may block, an
     * import or an indirect call. Its own exits alone under AOT_SPLIT. */
    uint8_t may_exit;
    /* 1: ... other than at a callout (aot_call_out), the rest of the code
     * cut there. One that can't may run where there's no trap to exit to (a
     * native's guest_call), its callouts' fallbacks nested runs there. */
    uint8_t may_hard;
    /* 1: it has x87 code (issue #136), on the CPU's own x87 state. */
    uint8_t x87;
} AotFunc;
/* A loose function (issue #138, the recompiler's --loose, for a build without
 * Box86): esp at its rets, or what its tail jump's target pops, isn't known
 * when it is compiled (004bae60's pop ebp; jmp eax through a vtable). Its sig
 * takes all eight registers and checks nothing at a ret: r[4] comes back as
 * the ret or the jump left it, and what sig returns is where control goes on
 * (the return address it popped, the jump's target). Only the dispatcher runs
 * one; compiled callers reach it by a callout to its address. */
#define AOT_POPS_LOOSE 0xffu
/* The natives' hand-backs compiled (issue #138; AOT_SYM(resumes)): `func`'s
 * code entered at `va`, where a native that did its first instructions hands
 * the call back (resume_original), with all eight registers as there; r[4]
 * comes back past the function's ret. */
typedef struct {
    uint32_t va, func;
    uint32_t sp;          /* esp at va less the entry's: the return address at esp - sp */
    AotRootFn fn;
} AotResume;
/* The return sites' entries (issue #138; AOT_SYM(reents)): `func`'s code
 * entered at `va` (fn(R, va), one entry per function for all its sites), with
 * all eight registers as there; R->r[4] comes back past the function's ret. */
/* The trampolines with a branch moved (issue #138; AOT_SYM(tramps)): a
 * function whose first bytes (keep) have a call, jmp or jcc has its
 * trampoline's code here, each rel32 at rel[] written as the absolute
 * target. */
typedef struct {
    uint32_t va;
    uint8_t len, nrel;
    uint8_t rel[4];
    uint8_t code[27];
} AotTramp;
typedef void (*AotReentFn)(AotRegs *R, uint32_t at);
typedef struct {
    uint32_t va, func;
    uint32_t sp;          /* as AotResume's */
    AotReentFn fn;
} AotReent;
#ifdef AOT_SPLIT
#define AOT_LEAF(x) 0
#define AOT_EXIT(own, tree) (own)
#else
#define AOT_LEAF(x) (x)
#define AOT_EXIT(own, tree) (tree)
#endif

extern uint8_t *aot_mem;   /* host address of guest 0 */
/* By the functions' table's rows (issue #115): 1 when the app found that
 * function changed since compiled (a native's hook or patch). A compiled
 * call to it (call_, the recompiler) then calls the native (aot_call_stale,
 * from stale_, out of the way), and its callers stay compiled. */
extern uint8_t aot_stale[];
#define AOT_STALE(k) __builtin_expect(aot_stale[(k)], 0)
#define AOT_COLD __attribute__((cold, noinline))

/* aot_spike.cpp */
uint32_t aot_fs_base(void);                       /* the running thread's TEB */
void aot_guest_call(uint32_t va, AotRegs *r);     /* a callee not compiled: Box86, nested */
void aot_call_stale(uint32_t va, AotRegs *r);     /* a compiled callee a native changed (AOT_LIVE): its native */
__attribute__((noreturn)) void aot_bad_ret(uint32_t fn, uint32_t esp0, uint32_t esp);
__attribute__((noreturn)) void aot_unreachable(uint32_t va);
/* Box86 goes on at `target` with these registers (issue #115): back to the
 * trap that entered compiled code, the C frames above dropped. The guest's
 * stack holds each compiled caller's return address, so Box86 runs the rest
 * of each from the game's own code. */
__attribute__((noreturn)) void aot_exit(uint32_t target, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx,
                                        uint32_t esp, uint32_t ebp, uint32_t esi, uint32_t edi);
/* An indirect or import call (issue #115): `x` called in C when it's our
 * shim or a compiled function, with *r as at the call (esp at the return
 * address pushed), *r then as after it. Else, or where the callee popped
 * other than `pops` bytes, changed a callee-saved register or asked to yield
 * or to run guest code, an exit: Box86 goes on at x, or at the return. */
void aot_call_out(uint32_t x, AotRegs *r, uint32_t pops);
__attribute__((noreturn)) void aot_fault(uint32_t va);
/* cpuid, the leaf in eax: what the Box86 backend reports (a Pentium III, the
 * runtime's feature bits). */
void aot_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
/* rdtsc: the backend's clock for it (ReadTSC: the host's monotonic clock in
 * ns, the system tick on the 3DS). A checked run that reads it isn't compared. */
uint64_t aot_rdtsc(void);
/* The flags register's word for pushfd and popfd (not the arithmetic flags). */
extern uint32_t aot_eflags;
void aot_journal(uint32_t va, uint32_t n);        /* AOT_JOURNAL: before a store */
/* d2_native.cpp: our natives, called in C. The high word: 1 if the native
 * covered the call (the low word is its eax), 0 if it hands back. */
uint64_t aot_native_463740(uint32_t ecx, uint32_t edx, uint32_t y);
uint64_t aot_native_64d4e0(uint32_t esi, uint32_t room, uint32_t x, uint32_t y);

#if defined(AOT_JOURNAL)
#define AOT_SYM(x) aotj_##x
#define AOT_STORE_HOOK(a, n) aot_journal((a), (n))
#elif defined(AOT_NOINLINE)
#define AOT_SYM(x) aotn_##x
#define AOT_STORE_HOOK(a, n) ((void)0)
#else
#define AOT_SYM(x) aot_##x
#define AOT_STORE_HOOK(a, n) ((void)0)
#endif
/* Not static (issue #135): the functions are split over several files
 * (aot_spike_NN.c), and a call may go from one to another. Hidden, so the
 * three builds' symbols (AOT_SYM) stay inside the binary. */
#ifdef AOT_NOINLINE
#define AOT_FN __attribute__((visibility("hidden"), noinline))
#else
#define AOT_FN __attribute__((visibility("hidden")))
#endif

/* A function off the recompiler's AOT_HOT list (issue #153): one that ran in
 * none of the profiles. AOT_COLD_SIZE (the console's AOT build): compiled
 * for size, its code being the heap's loss. */
#ifdef AOT_COLD_SIZE
#define AOT_FNC AOT_FN __attribute__((optimize("Os")))
#else
#define AOT_FNC AOT_FN
#endif

#define AOT_INL static inline __attribute__((always_inline))

/* AOT_TRACE_ON (debugging the recompiler): each compiled function's entry
 * and registers, to compare with the game's run under Unicorn. */
#if defined(AOT_SAMPLE_ON)
/* AOT_SAMPLE_ON (a profile of a build with no Box86, docs/aot.md): each
 * compiled function writes its address at its entry, and a host thread reads
 * the word every 2 ms (aot_sample). */
extern volatile uint32_t aot_at;
#define AOT_TRACE(va, ...) (aot_at = (va))
#elif defined(AOT_TRACE_ON)
void aot_trace(uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esp, uint32_t ebp,
               uint32_t esi, uint32_t edi);
#define AOT_TRACE(...) aot_trace(__VA_ARGS__)
#else
#define AOT_TRACE(...) ((void)0)
#endif

/* M: a function's local copy of aot_mem, so stores (char-typed, which may
 * alias anything) don't make gcc reload it. */
AOT_INL uint32_t aot_ld8(uint8_t *m, uint32_t a) { return m[a]; }
AOT_INL uint32_t aot_ld16(uint8_t *m, uint32_t a)
{
    uint16_t v;
    memcpy(&v, m + a, 2);
    return v;
}
AOT_INL uint32_t aot_ld32(uint8_t *m, uint32_t a)
{
    uint32_t v;
    memcpy(&v, m + a, 4);
    return v;
}
AOT_INL void aot_st8(uint8_t *m, uint32_t a, uint32_t v)
{
    AOT_STORE_HOOK(a, 1);
    m[a] = (uint8_t)v;
}
AOT_INL void aot_st16(uint8_t *m, uint32_t a, uint32_t v)
{
    uint16_t x = (uint16_t)v;
    AOT_STORE_HOOK(a, 2);
    memcpy(m + a, &x, 2);
}
AOT_INL void aot_st32(uint8_t *m, uint32_t a, uint32_t v)
{
    AOT_STORE_HOOK(a, 4);
    memcpy(m + a, &v, 4);
}
#define LD8(a) aot_ld8(M, (a))
#define LD16(a) aot_ld16(M, (a))
#define LD32(a) aot_ld32(M, (a))
#define ST8(a, v) aot_st8(M, (a), (v))
#define ST16(a, v) aot_st16(M, (a), (v))
#define ST32(a, v) aot_st32(M, (a), (v))

/* x87 (issue #136): compiled code works on the CPU's own x87 state, as
 * Box86 keeps it (Cpu::x87_state(), backend patch 0065: its x86emu_t fields
 * from cw to fpu_ll, regs.h's packed types, so the doubles are only
 * word-aligned). The stack's depth needn't be known where it's compiled, a
 * callee may take or leave values on it, and a callout, an exit or Box86
 * entering compiled code finds it as Box86 would.
 *
 * What each instruction does is what Box86's dynarec does on this port
 * (patch 0066: every value a double), not the interpreter's helpers: the
 * arithmetic in the FPSCR's mode (cw's rounding control only for fist and
 * frndint), fist's out-of-range values, the int64 and 80-bit shadows that
 * make fild/fistp m64 and fld/fstp m80 exact, the transcendentals from libm. */
typedef struct {
    uint16_t cw, sw;           /*   0 */
    uint32_t st[8][2];         /*   4: x87[8], doubles */
    uint32_t mmx[8][2];        /*  68 */
    uint32_t top;              /* 132 */
    int32_t stack;             /* 136: fpu_stack */
    uint32_t tags;             /* 140: fpu_tags, 2 bits a register from st0, 11 empty */
    uint8_t ld[8][18];         /* 144: fpu_ld, an 80-bit value loaded (10 bytes) and its double */
    uint32_t ll[8][4];         /* 288: fpu_ll, a double from fild m64 and its int64 */
} AotFpu;                      /* 416 */
#ifdef __cplusplus
static_assert(sizeof(AotFpu) == 416, "AotFpu: Box86's x87 fields");
#else
_Static_assert(sizeof(AotFpu) == 416, "AotFpu: Box86's x87 fields");
#endif
extern AotFpu *aot_fpu;   /* aot_spike.cpp: the running emu's */
/* xmm0..7 (issue #138, a build without Box86: the C runtime's SSE2 pow, log
 * and exp, which it takes where cpuid reports SSE2). Each is two 64-bit
 * halves; a double is its bits. No MXCSR is kept: every exception masked,
 * round to nearest, as a thread starts and as that code leaves it. */
typedef struct {
    uint64_t q[2];
} AotXmm;
extern AotXmm *aot_xmm;   /* aot_spike.cpp: the running thread's */
AOT_INL double aot_xd(uint64_t q)
{
    double d;
    memcpy(&d, &q, 8);
    return d;
}
AOT_INL uint64_t aot_xq(double d)
{
    uint64_t q;
    memcpy(&q, &d, 8);
    return q;
}
/* 16 bytes of guest memory (aot_ld64 and aot_st64 are below). */
AOT_INL AotXmm aot_xld(uint8_t *m, uint32_t a)
{
    AotXmm x;
    memcpy(&x, m + a, 16);
    return x;
}
AOT_INL void aot_xst(uint8_t *m, uint32_t a, AotXmm x)
{
    AOT_STORE_HOOK(a, 16);
    memcpy(m + a, &x, 16);
}
/* pmaxsw: the greater of each pair of signed words. */
AOT_INL AotXmm aot_xmaxsw(AotXmm a, AotXmm b)
{
    int16_t x[8], y[8];
    memcpy(x, &a, 16);
    memcpy(y, &b, 16);
    for (int k = 0; k < 8; k++)
        if (y[k] > x[k])
            x[k] = y[k];
    memcpy(&a, x, 16);
    return a;
}
/* pcmpeqd: all ones in each dword that is equal. */
AOT_INL AotXmm aot_xcmpeqd(AotXmm a, AotXmm b)
{
    uint32_t x[4], y[4];
    memcpy(x, &a, 16);
    memcpy(y, &b, 16);
    for (int k = 0; k < 4; k++)
        x[k] = x[k] == y[k] ? 0xffffffffu : 0u;
    memcpy(&a, x, 16);
    return a;
}
/* pmovmskb: each byte's top bit. */
AOT_INL uint32_t aot_xmovmskb(AotXmm a)
{
    uint8_t x[16];
    uint32_t r = 0;
    memcpy(x, &a, 16);
    for (int k = 0; k < 16; k++)
        r |= (uint32_t)(x[k] >> 7) << k;
    return r;
}
/* cvtsd2si (round to nearest even) and cvttsd2si: out of range or NaN is 0x80000000. */
AOT_INL uint32_t aot_xcvt(double d, int trunc)
{
    const double r = trunc ? __builtin_trunc(d) : __builtin_nearbyint(d);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
/* ucomisd and comisd as AOT_EFL's word: unordered ZF PF CF, below CF, equal ZF. */
AOT_INL uint32_t aot_xcomi(double a, double b)
{
    return a < b ? 1u : a > b ? 0u : a == b ? 0x40u : 0x45u;
}
/* cmpsd's and cmppd's predicate (0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord): all ones or none. */
AOT_INL uint64_t aot_xcmp(double a, double b, int p)
{
    int t;
    switch (p & 7) {
    case 0: t = a == b; break;
    case 1: t = a < b; break;
    case 2: t = a <= b; break;
    case 3: t = a != a || b != b; break;
    case 4: t = !(a == b); break;
    case 5: t = !(a < b); break;
    case 6: t = !(a <= b); break;
    default: t = a == a && b == b; break;
    }
    return t ? ~(uint64_t)0 : 0;
}
/* Box86's LD2D and D2LD (x87emu_private.c), 80-bit <-> double. */
void aot_ld2d(const void *ld, double *d);
void aot_d2ld(const double *d, void *ld);

#define C87_C0 0x0100u
#define C87_C1 0x0200u
#define C87_C2 0x0400u
#define C87_C3 0x4000u

AOT_INL double aot_fget(const AotFpu *F, uint32_t i)
{
    double d;
    memcpy(&d, F->st[(F->top + i) & 7], 8);
    return d;
}
AOT_INL void aot_fset(AotFpu *F, uint32_t i, double d) { memcpy(F->st[(F->top + i) & 7], &d, 8); }
/* A push and a pop as the dynarec's write-back of its cache does them
 * (x87_purgecache: fpu_stack and the tags by the count, no limit). */
AOT_INL void aot_fpush_n(AotFpu *F)
{
    F->stack++;
    F->tags = (F->tags << 2) & 0xffffu;
    F->top = (F->top - 1u) & 7u;
}
AOT_INL void aot_fpush(AotFpu *F, double d)
{
    aot_fpush_n(F);
    aot_fset(F, 0, d);
}
AOT_INL void aot_fpop(AotFpu *F)
{
    F->stack--;
    F->tags = ((F->tags | 0xffff0000u) >> 2) & 0xffffu;
    F->top = (F->top + 1u) & 7u;
}
AOT_INL double aot_ldf32(uint8_t *m, uint32_t a)
{
    float f;
    memcpy(&f, m + a, 4);
    return (double)f;
}
AOT_INL double aot_ldf64(uint8_t *m, uint32_t a)
{
    double d;
    memcpy(&d, m + a, 8);
    return d;
}
AOT_INL void aot_stf32(uint8_t *m, uint32_t a, double d)
{
    float f = (float)d;
    AOT_STORE_HOOK(a, 4);
    memcpy(m + a, &f, 4);
}
AOT_INL void aot_stf64(uint8_t *m, uint32_t a, double d)
{
    AOT_STORE_HOOK(a, 8);
    memcpy(m + a, &d, 8);
}
AOT_INL void aot_st64(uint8_t *m, uint32_t a, uint64_t v)
{
    AOT_STORE_HOOK(a, 8);
    memcpy(m + a, &v, 8);
}
AOT_INL uint64_t aot_ld64(uint8_t *m, uint32_t a)
{
    uint64_t v;
    memcpy(&v, m + a, 8);
    return v;
}
/* fcom and its kin: C3 C2 C0 from the compare, C1 clear. */
AOT_INL void aot_fcom(AotFpu *F, double a, double b)
{
    uint16_t c = a < b ? C87_C0 : a > b ? 0 : a == b ? C87_C3 : (C87_C3 | C87_C2 | C87_C0);
    F->sw = (uint16_t)((F->sw & ~(C87_C0 | C87_C1 | C87_C2 | C87_C3)) | c);
}
/* fnstsw: sw with top in bits 11-13 (sw itself keeps its own). */
AOT_INL uint32_t aot_fnstsw(const AotFpu *F) { return (uint32_t)((F->sw & ~0x3800u) | ((F->top & 7u) << 11)); }
/* cw's rounding control (00 nearest, 01 down, 10 up, 11 toward zero). */
AOT_INL double aot_fround(const AotFpu *F, double d)
{
    switch ((F->cw >> 10) & 3u) {
    case 0: return __builtin_nearbyint(d);
    case 1: return __builtin_floor(d);
    case 2: return __builtin_ceil(d);
    default: return __builtin_trunc(d);
    }
}
/* fist m32 (VCVTR in cw's mode): NaN or out of range is 0x80000000. */
AOT_INL uint32_t aot_fist32(const AotFpu *F, double d)
{
    const double r = aot_fround(F, d);
    if (!(r >= -2147483648.0 && r <= 2147483647.0))
        return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
/* fist m16: the same, then 0x8000 out of int16's range too. */
AOT_INL uint32_t aot_fist16(const AotFpu *F, double d)
{
    const double r = aot_fround(F, d);
    if (!(r >= -32768.0 && r <= 32767.0))
        return 0x8000u;
    return (uint32_t)(uint16_t)(int16_t)r;
}
/* VCVT.S32.F64 (fscale's count): toward zero, saturated, NaN 0. */
AOT_INL int32_t aot_cvt_s32(double d)
{
    if (d != d)
        return 0;
    if (d >= 2147483648.0)
        return 2147483647;
    if (d <= -2147483649.0)
        return (int32_t)0x80000000u;
    return (int32_t)d;
}
/* fild m64: the value made as the dynarec makes it (|v|'s high word times
 * 2^32 plus its low word, one rounding), and the int64 kept beside it. */
AOT_INL void aot_fild64(AotFpu *F, uint64_t v)
{
    const int neg = (int)(v >> 63);
    const uint64_t u = neg ? 0u - v : v;
    double d = (double)(uint32_t)(u >> 32) * 4294967296.0 + (double)(uint32_t)u;
    if (neg)
        d = -d;
    aot_fpush(F, d);
    uint32_t *ll = F->ll[F->top & 7];
    memcpy(ll, &d, 8);
    memcpy(ll + 2, &v, 8);
}
/* fistp m64 (arm_fist64_*): the int64 fild loaded, if st0 is still its
 * value; else rounded in cw's mode, 0x8000000000000000 out of range. */
AOT_INL uint64_t aot_fist64(const AotFpu *F, double d)
{
    double ref;
    memcpy(&ref, F->ll[F->top & 7], 8);
    if (d == ref) {
        uint64_t v;
        memcpy(&v, F->ll[F->top & 7] + 2, 8);
        return v;
    }
    if (__builtin_isgreater(d, (double)(int64_t)0x7fffffffffffffffLL) ||
        __builtin_isless(d, -(double)(uint64_t)0x8000000000000000ULL) || !__builtin_isfinite(d))
        return 0x8000000000000000ULL;
    return (uint64_t)(int64_t)aot_fround(F, d);
}
/* fld m80: the 10 bytes kept beside their double (LD2D truncates). */
AOT_INL void aot_fld80(AotFpu *F, uint8_t *m, uint32_t a)
{
    aot_fpush_n(F);
    uint8_t *ld = F->ld[F->top & 7];
    double d;
    memcpy(ld, m + a, 10);
    aot_ld2d(ld, &d);
    memcpy(ld + 10, &d, 8);
    aot_fset(F, 0, d);
}
/* fnsave (issue #138: the C runtime's 687d71 around its error handler): the
 * 108 bytes of protected mode's 32-bit form (cw, sw, the tag word, 16 bytes
 * of pointers left zero, st0..st7 as 80-bit values), then the state fninit
 * leaves. frstor: that back. */
AOT_INL void aot_fnsave(AotFpu *F, uint8_t *m, uint32_t a)
{
    uint32_t tw = 0;
    AOT_STORE_HOOK(a, 108);
    memset(m + a, 0, 108);
    for (uint32_t i = 0; i < 8; i++) {
        const double d = aot_fget(F, i);
        if (((F->tags >> (2 * i)) & 3u) == 3u)
            tw |= 3u << (2 * ((F->top + i) & 7u));
        aot_d2ld(&d, m + a + 28 + 10 * i);
    }
    const uint16_t cw = F->cw, sw = (uint16_t)((F->sw & ~0x3800u) | ((F->top & 7u) << 11)), t16 = (uint16_t)tw;
    memcpy(m + a, &cw, 2);
    memcpy(m + a + 4, &sw, 2);
    memcpy(m + a + 8, &t16, 2);
    F->cw = 0x37f;
    F->sw = 0;
    F->top = 0;
    F->stack = 0;
    F->tags = 0xffffu;
}
AOT_INL void aot_frstor(AotFpu *F, uint8_t *m, uint32_t a)
{
    uint16_t cw, sw, tw;
    memcpy(&cw, m + a, 2);
    memcpy(&sw, m + a + 4, 2);
    memcpy(&tw, m + a + 8, 2);
    F->cw = cw;
    F->sw = sw;
    F->top = (sw >> 11) & 7u;
    F->tags = 0;
    F->stack = 0;
    for (uint32_t i = 0; i < 8; i++) {
        double d;
        aot_ld2d(m + a + 28 + 10 * i, &d);
        aot_fset(F, i, d);
        if (((tw >> (2 * ((F->top + i) & 7u))) & 3u) == 3u)
            F->tags |= 3u << (2 * i);
        else
            F->stack++;
    }
}
/* fstp m80: those bytes again if st0 is still their value (compared as
 * numbers, as on ARMv6), else D2LD. */
AOT_INL void aot_fstp80(AotFpu *F, uint8_t *m, uint32_t a)
{
    const double d = aot_fget(F, 0);
    const uint8_t *ld = F->ld[F->top & 7];
    double ref;
    memcpy(&ref, ld + 10, 8);
    AOT_STORE_HOOK(a, 10);
    if (d == ref)
        memcpy(m + a, ld, 10);
    else
        aot_d2ld(&d, m + a);
    aot_fpop(F);
}
/* fprem1: the quotient rounded toward +inf (the dynarec's FPSCR mode 01),
 * saturated; st0 - q * st1; C2 0, C1 C3 C0 the quotient's low bits. */
AOT_INL void aot_fprem1(AotFpu *F)
{
    const double a = aot_fget(F, 0), b = aot_fget(F, 1);
    const int32_t q = aot_cvt_s32(__builtin_ceil(a / b));
    aot_fset(F, 0, a - (double)q * b);
    const uint32_t u = (uint32_t)q;
    F->sw = (uint16_t)((F->sw & ~(C87_C0 | C87_C1 | C87_C2 | C87_C3)) | ((u & 1u) << 9) | ((u >> 1 & 1u) << 14) |
                       ((u >> 2 & 1u) << 8));
}

/* The C runtime's memset and memcpy (MSVC's memcpy handles overlap as
 * memmove does), as the memory intrinsics do them. */
AOT_INL void aot_memset(uint32_t d, uint32_t v, uint32_t n)
{
    AOT_STORE_HOOK(d, n);
    memset(aot_mem + d, (int)(v & 0xff), n);
}
AOT_INL void aot_memmove(uint32_t d, uint32_t s, uint32_t n)
{
    AOT_STORE_HOOK(d, n);
    memmove(aot_mem + d, aot_mem + s, n);
}
/* rep movs / rep stos, element by element where the copy overlaps forward
 * (as the CPU does it), with the direction flag. */
AOT_INL void aot_movs(uint32_t di, uint32_t si, uint32_t n, int size, int down)
{
    const uint32_t bytes = n * (uint32_t)size;
    if (!down && (di <= si || di >= si + bytes)) {
        AOT_STORE_HOOK(di, bytes);
        memmove(aot_mem + di, aot_mem + si, bytes);
        return;
    }
    const int32_t step = down ? -size : size;
    for (uint32_t k = 0; k < n; k++, di += (uint32_t)step, si += (uint32_t)step) {
        AOT_STORE_HOOK(di, (uint32_t)size);
        memmove(aot_mem + di, aot_mem + si, (size_t)size);
    }
}
AOT_INL void aot_stos(uint32_t di, uint32_t v, uint32_t n, int size, int down)
{
    if (size == 1) {
        const uint32_t lo = down ? di - n + 1 : di;
        AOT_STORE_HOOK(lo, n);
        memset(aot_mem + lo, (int)(v & 0xff), n);
        return;
    }
    const int32_t step = down ? -size : size;
    for (uint32_t k = 0; k < n; k++, di += (uint32_t)step) {
        AOT_STORE_HOOK(di, (uint32_t)size);
        memcpy(aot_mem + di, &v, (size_t)size);
    }
}

/* Flag kinds (the recompiler's KIND) and condition codes (CC_NUM). */
enum { AOT_ADD, AOT_SUB, AOT_LOGIC, AOT_INC, AOT_DEC, AOT_NEG, AOT_SHL, AOT_SHR, AOT_SAR, AOT_MUL, AOT_ADC, AOT_SBB, AOT_EFL };
/* AOT_EFL (issue #138): the flags as bits of eflags in F_A (CF 0x1, PF 0x4,
 * ZF 0x40, SF 0x80, OF 0x800), where no instruction's operands give them:
 * sahf's (ah; OF isn't its, and a reader of OF after it is refused), bt's
 * (CF alone), ucomisd's, and the flags a call hands on or back (aot_xf). */
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

AOT_INL uint32_t aot_mask(int w) { return w == 4 ? 0xffffffffu : (1u << (8 * w)) - 1; }
AOT_INL uint32_t aot_sbit(int w) { return 1u << (8 * w - 1); }
AOT_INL int32_t aot_sx(uint32_t v, int w) { return w == 4 ? (int32_t)v : w == 2 ? (int16_t)v : (int8_t)v; }

AOT_INL int aot_cf(int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    switch (k) {
    case AOT_ADD: return r < a;
    case AOT_SUB: return a < b;
    case AOT_NEG: return a != 0;
    case AOT_SHL: return (int)((a >> (8 * w - b)) & 1);
    case AOT_SHR: return (int)((a >> (b - 1)) & 1);
    case AOT_SAR: return (int)(((uint32_t)(aot_sx(a, w) >> (b - 1))) & 1);
    case AOT_MUL: return c != 0;
    case AOT_ADC: return (uint64_t)a + b + c > aot_mask(w);
    case AOT_SBB: return (uint64_t)a < (uint64_t)b + c;
    default: return 0;   /* logic; inc/dec leave CF (never asked: the recompiler refuses) */
    }
}
AOT_INL int aot_of(int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    const uint32_t s = aot_sbit(w);
    switch (k) {
    case AOT_ADD:
    case AOT_ADC: return (~(a ^ b) & (a ^ r) & s) != 0;
    case AOT_SUB:
    case AOT_SBB: return ((a ^ b) & (a ^ r) & s) != 0;
    case AOT_INC: return r == s;
    case AOT_DEC: return a == s;
    case AOT_NEG: return a == s;
    case AOT_SHL: return b == 1 ? (((r ^ (aot_cf(k, w, a, b, r, c) ? s : 0)) & s) != 0) : 0;
    case AOT_SHR: return b == 1 ? ((a & s) != 0) : 0;
    case AOT_MUL: return c != 0;
    default: return 0;
    }
}
AOT_INL int aot_cc(int cc, int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    const uint32_t m = aot_mask(w);
    if (k == AOT_SUB) {   /* cmp/sub: compares, which gcc emits as one */
        switch (cc) {
        case CC_B: return a < b;
        case CC_AE: return a >= b;
        case CC_E: return a == b;
        case CC_NE: return a != b;
        case CC_BE: return a <= b;
        case CC_A: return a > b;
        case CC_L: return aot_sx(a, w) < aot_sx(b, w);
        case CC_GE: return aot_sx(a, w) >= aot_sx(b, w);
        case CC_LE: return aot_sx(a, w) <= aot_sx(b, w);
        case CC_G: return aot_sx(a, w) > aot_sx(b, w);
        default: break;
        }
    }
    if (k == AOT_LOGIC) {   /* and/or/xor/test: CF = OF = 0 */
        switch (cc) {
        case CC_E:
        case CC_BE: return (r & m) == 0;
        case CC_NE:
        case CC_A: return (r & m) != 0;
        case CC_S:
        case CC_L: return aot_sx(r, w) < 0;
        case CC_NS:
        case CC_GE: return aot_sx(r, w) >= 0;
        case CC_LE: return aot_sx(r, w) <= 0;
        case CC_G: return aot_sx(r, w) > 0;
        case CC_B:
        case CC_O: return 0;
        case CC_AE:
        case CC_NO: return 1;
        default: break;
        }
    }
    int cf, zf, sf, of, pf;
    if (k == AOT_EFL) {
        cf = (int)(a & 1u), pf = (int)((a >> 2) & 1u), zf = (int)((a >> 6) & 1u), sf = (int)((a >> 7) & 1u), of = (int)((a >> 11) & 1u);
    } else {
        cf = aot_cf(k, w, a, b, r, c), zf = (r & m) == 0, sf = (r & aot_sbit(w)) != 0;
        of = aot_of(k, w, a, b, r, c), pf = !__builtin_parity(r & 0xff);
    }
    switch (cc) {
    case CC_O: return of;
    case CC_NO: return !of;
    case CC_B: return cf;
    case CC_AE: return !cf;
    case CC_E: return zf;
    case CC_NE: return !zf;
    case CC_BE: return cf || zf;
    case CC_A: return !cf && !zf;
    case CC_S: return sf;
    case CC_NS: return !sf;
    case CC_P: return pf;
    case CC_NP: return !pf;
    case CC_L: return sf != of;
    case CC_GE: return sf == of;
    case CC_LE: return zf || sf != of;
    default: return !zf && sf == of;
    }
}
/* Several kinds reach the reader: F_K holds kind * 8 + width. */
static inline int aot_cc_dyn(int cc, int kw, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    return aot_cc(cc, kw >> 3, kw & 7, a, b, r, c);
}
/* Flags that cross a call (issue #138, a build without Box86: the C
 * runtime's x87 helpers, 698795 leaving ZF for its caller's jz, 6879dd
 * entered with it): the function that hands them on stores them here as
 * AOT_EFL's word, at the call or at its ret, and the one that reads them
 * takes them from here. One guest thread runs at a time, and nothing comes
 * between the two but the call or the ret. */
extern uint32_t aot_xf;
AOT_INL uint32_t aot_flw(int k, int w, uint32_t a, uint32_t b, uint32_t r, uint32_t c)
{
    if (k == AOT_EFL)
        return a;
    return (aot_cc(CC_B, k, w, a, b, r, c) ? 1u : 0u) | (aot_cc(CC_P, k, w, a, b, r, c) ? 4u : 0u) |
           (aot_cc(CC_E, k, w, a, b, r, c) ? 0x40u : 0u) | (aot_cc(CC_S, k, w, a, b, r, c) ? 0x80u : 0u) |
           (aot_cc(CC_O, k, w, a, b, r, c) ? 0x800u : 0u);
}

#ifdef __cplusplus
}
#endif

#endif
