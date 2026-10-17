// x86-32 fixtures for the differential CPU lane. Every fixture is a function
// called with the registers of one case (see cases in harness.c) and ESP =
// X86_STACK_TOP - 4. ESI/EDI/EBP point into scratch memory. Fixtures live in
// .fixtures, linked at X86_FIXTURE_BASE, so any executor can load the same
// bytes at the same address.
//
// Covered: plan §5 M2 list — arithmetic and flags, shifts, stack/calls/returns,
// indirect branches, string operations, unaligned accesses, FS/TLS, x87.
// MMX/SSE are added once the 1.14d opcode inventory says which are used.
#include "abi.h"
    .intel_syntax noprefix
    .section .fixtures, "ax", @progbits
    .p2align 4

.macro FX name
    .p2align 4
    .globl fx_\name
    .type fx_\name, @function
fx_\name:
.endm

// ---- arithmetic and flags ------------------------------------------------------
FX add
    add eax, ecx
    ret
FX adc
    adc eax, ecx
    ret
FX sub
    sub eax, ecx
    ret
FX sbb
    sbb eax, ecx
    ret
FX cmp
    cmp eax, ecx
    ret
FX and
    and eax, ecx
    ret
FX or
    or eax, ecx
    ret
FX xor
    xor eax, ecx
    ret
FX test
    test eax, ecx
    ret
FX inc
    inc eax
    ret
FX dec
    dec eax
    ret
FX neg
    neg eax
    ret
FX not
    not eax
    ret
FX add8
    add al, cl
    ret
FX sub8
    sub al, cl
    ret
FX add16
    add ax, cx
    ret
FX adc16
    adc ax, cx
    ret
FX highregs
    add ah, cl
    sub ch, al
    xchg ah, al
    ret
FX mul
    mul ecx
    ret
FX imul1
    imul ecx
    ret
FX imul2
    imul eax, ecx
    ret
FX imul3
    imul eax, ecx, -7
    ret
FX div
    div ecx
    ret
FX idiv
    idiv ecx
    ret
FX signext
    mov edx, eax
    cbw
    mov ebx, eax
    mov eax, edx
    cwde
    mov esi, eax
    mov eax, ecx
    cdq
    ret

// Every condition code, after one CMP: 16 SETcc bytes and 16 CMOVcc dwords.
FX setcc
    cmp eax, ecx
    seto  byte ptr [edi+0]
    setno byte ptr [edi+1]
    setb  byte ptr [edi+2]
    setae byte ptr [edi+3]
    sete  byte ptr [edi+4]
    setne byte ptr [edi+5]
    setbe byte ptr [edi+6]
    seta  byte ptr [edi+7]
    sets  byte ptr [edi+8]
    setns byte ptr [edi+9]
    setp  byte ptr [edi+10]
    setnp byte ptr [edi+11]
    setl  byte ptr [edi+12]
    setge byte ptr [edi+13]
    setle byte ptr [edi+14]
    setg  byte ptr [edi+15]
    ret
.macro CMOVSTORE cc, off
    mov edx, 0x11111111
    cmov\cc edx, ebx
    mov dword ptr [edi+\off], edx
.endm
FX cmovcc
    cmp eax, ecx
    CMOVSTORE o, 0
    CMOVSTORE no, 4
    CMOVSTORE b, 8
    CMOVSTORE ae, 12
    CMOVSTORE e, 16
    CMOVSTORE ne, 20
    CMOVSTORE be, 24
    CMOVSTORE a, 28
    CMOVSTORE s, 32
    CMOVSTORE ns, 36
    CMOVSTORE p, 40
    CMOVSTORE np, 44
    CMOVSTORE l, 48
    CMOVSTORE ge, 52
    CMOVSTORE le, 56
    CMOVSTORE g, 60
    ret
// Conditional branches build a bit mask in EDX (one bit per taken branch).
.macro JBIT cc, bit
    j\cc 1f
    jmp 2f
1:  or edx, (1 << \bit)
2:
.endm
FX jcc
    xor edx, edx
    cmp eax, ecx
    JBIT o, 0
    JBIT b, 1
    JBIT e, 2
    JBIT be, 3
    JBIT s, 4
    JBIT p, 5
    JBIT l, 6
    JBIT le, 7
    JBIT a, 8
    JBIT g, 9
    ret

// ---- shifts and rotates ----------------------------------------------------------
FX shl_cl
    shl eax, cl
    ret
FX shr_cl
    shr eax, cl
    ret
FX sar_cl
    sar eax, cl
    ret
FX shl1
    shl eax, 1
    ret
FX shr1
    shr eax, 1
    ret
FX sar1
    sar eax, 1
    ret
FX rol_cl
    rol eax, cl
    ret
FX ror_cl
    ror eax, cl
    ret
FX rcl_cl
    rcl eax, cl
    ret
FX rcr_cl
    rcr eax, cl
    ret
FX shld_cl
    shld eax, edx, cl
    ret
FX shrd_cl
    shrd eax, edx, cl
    ret

// ---- bit operations and data movement ---------------------------------------------
FX bt
    bt eax, ecx
    ret
FX bts
    bts eax, ecx
    ret
FX btr
    btr eax, ecx
    ret
FX btc
    btc eax, ecx
    ret
FX bt_mem
    and ecx, 0x3ff
    bts dword ptr [edi], ecx
    ret
FX bsf
    bsf edx, eax
    ret
FX bsr
    bsr edx, eax
    ret
FX bswap
    bswap eax
    ret
FX movx
    movzx edx, al
    movsx ebx, ah
    movzx esi, cx
    movsx ebp, cx
    ret
FX lea
    lea eax, [eax+ecx*4+0x1234]
    lea edx, [ecx+ecx*2-8]
    lea ebx, [ebx+ebp]
    ret
FX cmpxchg
    mov dword ptr [edi], ecx
    cmpxchg dword ptr [edi], ebx
    mov edx, dword ptr [edi]
    ret
FX xadd
    mov dword ptr [edi+4], ecx
    xadd dword ptr [edi+4], eax
    mov edx, dword ptr [edi+4]
    ret
FX unaligned
    mov dword ptr [edi+1], eax
    mov edx, dword ptr [edi+3]
    mov word ptr [edi+7], cx
    movzx ebx, word ptr [edi+5]
    mov dword ptr [edi+0xfff], ecx
    ret

// ---- stack, calls and control flow ------------------------------------------------
FX pushpop
    push eax
    push ecx
    push 0x1234
    pop edx
    pop ebx
    pop esi
    ret
FX pushad
    pushad
    xor eax, eax
    xor ecx, ecx
    not edx
    popad
    pushfd
    pop edx
    and edx, 0x8d5
    ret
FX call_ret
    call 1f
    jmp 2f
1:  add eax, ecx
    ret
2:  ret
FX stdcall
    push ecx
    push eax
    call fx_stdcall_callee
    ret
fx_stdcall_callee:
    mov eax, dword ptr [esp+4]
    sub eax, dword ptr [esp+8]
    ret 8
FX recursion
    and ecx, 7
    mov eax, 1
    call fx_fact
    ret
fx_fact:
    test ecx, ecx
    jz 1f
    imul eax, ecx
    dec ecx
    call fx_fact
1:  ret
FX callind
    and ecx, 3
    call dword ptr [fx_ct_table + ecx*4]
    ret
fx_ci0: add eax, 0x10
    ret
fx_ci1: sub eax, 0x20
    ret
fx_ci2: xor eax, 0x30
    ret
fx_ci3: lea eax, [eax+eax*2]
    ret
    .p2align 2
fx_ct_table:
    .long fx_ci0, fx_ci1, fx_ci2, fx_ci3
FX jumptable
    and ecx, 3
    jmp dword ptr [fx_jt_table + ecx*4]
fx_jt0: mov edx, 0xa0
    ret
fx_jt1: mov edx, 0xa1
    ret
fx_jt2: mov edx, 0xa2
    ret
fx_jt3: mov edx, 0xa3
    ret
    .p2align 2
fx_jt_table:
    .long fx_jt0, fx_jt1, fx_jt2, fx_jt3
FX callreg
    lea edx, [fx_ci2]
    call edx
    ret
FX loop
    mov ecx, 10
1:  add eax, ecx
    loop 1b
    jecxz 2f
    mov eax, 0xbad
2:  ret
FX enter_leave
    enter 16, 0
    mov dword ptr [ebp-4], eax
    mov dword ptr [ebp-8], ecx
    mov edx, dword ptr [ebp-4]
    add edx, dword ptr [ebp-8]
    mov ebx, esp
    leave
    ret

// ---- string operations (ESI/EDI in scratch) -------------------------------------------
FX rep_movsb
    mov ecx, 37
    rep movsb
    ret
FX rep_movsd
    mov ecx, 9
    rep movsd
    ret
FX movsd_backward
    add esi, 64
    add edi, 64
    std
    mov ecx, 5
    rep movsd
    cld
    ret
FX rep_stosd
    mov eax, 0xdeadbeef
    mov ecx, 13
    rep stosd
    ret
FX repne_scasb
    mov ecx, 200
    repne scasb
    ret
FX repe_cmpsb
    push esi
    push edi
    mov ecx, 32
    rep movsb
    pop edi
    pop esi
    mov byte ptr [edi+20], 0x55
    mov byte ptr [esi+20], 0xaa
    mov ecx, 64
    repe cmpsb
    ret
FX lodsb_sum
    xor edx, edx
    mov ecx, 50
1:  lodsb
    movzx eax, al
    add edx, eax
    loop 1b
    ret

// ---- TLS: FS points at a TEB at X86_TEB_BASE whose +0x18 holds its own address ----------
FX fs_teb
    mov eax, dword ptr fs:[0x18]
    mov edx, dword ptr fs:[0]
    mov dword ptr fs:[4], ecx
    mov ebx, dword ptr [eax+4]
    ret

// ---- x87: results go to memory at [EDI], as the game stores them ----------------------
FX x87_arith
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    fild dword ptr [edi]
    fiadd dword ptr [edi+4]
    fstp qword ptr [edi+8]
    fild dword ptr [edi]
    fimul dword ptr [edi+4]
    fstp qword ptr [edi+16]
    fild dword ptr [edi]
    fisub dword ptr [edi+4]
    fstp qword ptr [edi+24]
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    fst qword ptr [edi+32]
    fstp dword ptr [edi+40]
    fild dword ptr [edi]
    fabs
    fsqrt
    fstp qword ptr [edi+48]
    fnstsw ax
    ret
FX x87_round
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    fnstcw word ptr [edi+8]
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    mov dx, word ptr [edi+8]
    .irp rc, 0, 1, 2, 3
    and dx, 0xf3ff
    or dx, (\rc << 10)
    mov word ptr [edi+10], dx
    fldcw word ptr [edi+10]
    fist dword ptr [edi+12+4*\rc]
    frndint
    fst qword ptr [edi+32+8*\rc]
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    fxch
    fstp st(0)
    .endr
    fstp st(0)
    fldcw word ptr [edi+8]
    ret
FX x87_compare
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    fild dword ptr [edi+4]
    fild dword ptr [edi]
    fcom st(1)
    fnstsw ax
    mov word ptr [edi+8], ax
    sahf
    setb byte ptr [edi+10]
    sete byte ptr [edi+11]
    seta byte ptr [edi+12]
    fcomip st, st(1)
    setb byte ptr [edi+13]
    sete byte ptr [edi+14]
    fstp st(0)
    ret
FX x87_const
    fldpi
    fstp qword ptr [edi]
    fldl2e
    fstp qword ptr [edi+8]
    fldl2t
    fstp qword ptr [edi+16]
    fldlg2
    fstp qword ptr [edi+24]
    fldln2
    fstp qword ptr [edi+32]
    fld1
    fstp qword ptr [edi+40]
    fldz
    fstp qword ptr [edi+48]
    ret
FX x87_trans
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], 1000
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    fld st(0)
    fsin
    fstp qword ptr [edi+8]
    fld st(0)
    fcos
    fstp qword ptr [edi+16]
    fld st(0)
    fptan
    fstp st(0)
    fstp qword ptr [edi+24]
    fld1
    fld st(1)
    fpatan
    fstp qword ptr [edi+32]
    fabs
    fld1
    fxch
    fyl2x
    fstp qword ptr [edi+40]
    ret
FX x87_80bit
    // Round-trip an 80-bit value whose low mantissa bits do not fit a double.
    mov dword ptr [edi], 0x89abcdef
    mov dword ptr [edi+4], 0xc1234567
    mov word ptr [edi+8], 0x3fff
    fld tbyte ptr [edi]
    fstp tbyte ptr [edi+16]
    fld tbyte ptr [edi]
    fstp qword ptr [edi+32]
    ret
FX x87_status
    mov dword ptr [edi+12], 0
    fld1
    fidiv dword ptr [edi+12]
    fstp qword ptr [edi]
    fnstsw ax
    mov word ptr [edi+8], ax
    fnclex
    fnstsw ax
    mov word ptr [edi+10], ax
    ret

// Not a fixture: executors without their own FPU reset run this before each case.
FX _reset
    fninit
    ret

// ---- batch 2: appended after fx__reset so earlier fixtures keep their addresses ----

// 64/16-bit integer x87 transfers. EAX:ECX form the 64-bit input (ECX high).
FX x87_int64
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    fild qword ptr [edi]
    fst qword ptr [edi+8]
    fistp qword ptr [edi+16]
    fild word ptr [edi]
    fistp word ptr [edi+24]
    fild dword ptr [edi+4]
    fchs
    fistp qword ptr [edi+32]
    fild qword ptr [edi]
    fild qword ptr [edi]
    fmulp st(1), st
    fistp qword ptr [edi+40]
    ret
// x87 loads and stores at unaligned addresses (x86 permits them).
FX x87_unaligned
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    fst qword ptr [edi+9]
    fst dword ptr [edi+21]
    fstp st(0)
    fld qword ptr [edi+9]
    fstp qword ptr [edi+32]
    fld dword ptr [edi+21]
    fstp qword ptr [edi+40]
    fild dword ptr [edi]
    fistp dword ptr [edi+49]
    ret
// FSCALE, FPREM, F2XM1, FRNDINT, FXCH, FCHS, FTST.
FX x87_misc
    mov dword ptr [edi], eax
    mov dword ptr [edi+4], ecx
    mov dword ptr [edi+8], 3
    fild dword ptr [edi+8]
    fild dword ptr [edi]
    fscale
    fstp qword ptr [edi+16]
    fstp st(0)
    fild dword ptr [edi+4]
    fild dword ptr [edi]
    fprem
    fstp qword ptr [edi+24]
    fstp st(0)
    fild dword ptr [edi]
    fidiv dword ptr [edi+4]
    fld st(0)
    frndint
    fxch
    fsub st, st(1)
    fabs
    f2xm1
    fstp qword ptr [edi+32]
    fchs
    ftst
    fnstsw ax
    mov word ptr [edi+40], ax
    fstp qword ptr [edi+48]
    ret
// LOCK-prefixed read-modify-writes and the implicitly locked XCHG.
FX lock_ops
    mov dword ptr [edi], ecx
    lock cmpxchg dword ptr [edi], ebx
    lock xadd dword ptr [edi+4], eax
    lock inc dword ptr [edi+8]
    lock or dword ptr [edi+12], ecx
    lock sub dword ptr [edi+16], eax
    xchg dword ptr [edi+20], ecx
    mov edx, dword ptr [edi]
    ret
// Long string copies and fills.
FX rep_long
    mov ecx, 300
    rep movsd
    mov eax, 0x5a
    mov ecx, 777
    rep stosb
    ret

// ---- batch 3: memory and aliased operand forms of instructions the ARMv6
// port re-implements (found by running the game: BSF with a memory operand) ----
FX bsf_mem
    mov dword ptr [edi], eax
    bsf edx, dword ptr [edi]
    ret
FX bsr_mem
    mov dword ptr [edi], eax
    bsr edx, dword ptr [edi]
    ret
FX bsf_same
    bsf eax, eax
    ret

    .section .note.GNU-stack, "", @progbits
