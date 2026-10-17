@ Luma3DS custom SVC wrappers; libctru 2.7.0 does not declare these.
@ Azahar implements 0x93 and 0x94 (src/core/hle/kernel/svc.cpp).
    .arm
    .align 2

    .section .text.svcLumaInvalidateInstructionCacheRange, "ax", %progbits
    .global svcLumaInvalidateInstructionCacheRange
    .type svcLumaInvalidateInstructionCacheRange, %function
svcLumaInvalidateInstructionCacheRange:
    svc 0x93
    bx lr

    .section .text.svcLumaInvalidateEntireInstructionCache, "ax", %progbits
    .global svcLumaInvalidateEntireInstructionCache
    .type svcLumaInvalidateEntireInstructionCache, %function
svcLumaInvalidateEntireInstructionCache:
    svc 0x94
    bx lr
