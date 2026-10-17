@ Reference encodings for tests/host/test_main.c, produced by the assembler.
    .arch armv6k
    .arm
    .section .rodata
    .align 2
    .global ref_insns
ref_insns:
    ldr r0, [pc, #0]
    ldr r3, [pc, #4092]
    bx lr
    add r0, r0, r1
    b . + 0x100
    b . - 0x100

    .global ref_insns_count
    .align 2
ref_insns_count:
    .word 6
