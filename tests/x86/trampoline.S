// void run_case(const uint32_t in[8], uint32_t out[9], void (*code)(void))
//   in:  eax ecx edx ebx ebp esi edi eflags
//   out: eax ecx edx ebx esp ebp esi edi eflags
// Runs `code` on the fixed scratch stack with exactly the given registers.
// All state handoff goes through absolute globals, so no register is reserved.
#include "abi.h"
    .text
    .globl run_case
    .type run_case, @function
run_case:
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov 20(%esp), %eax
    mov %eax, g_in
    mov 24(%esp), %eax
    mov %eax, g_out
    mov 28(%esp), %eax
    mov %eax, g_code
    mov %esp, g_host_esp

    mov $X86_STACK_TOP, %esp
    mov g_in, %eax
    pushl 28(%eax)
    popfl
    mov 4(%eax), %ecx
    mov 8(%eax), %edx
    mov 12(%eax), %ebx
    mov 16(%eax), %ebp
    mov 20(%eax), %esi
    mov 24(%eax), %edi
    mov 0(%eax), %eax
    call *g_code

    pushfl
    mov %eax, g_save_eax
    pop %eax
    mov %eax, g_save_flags
    mov g_out, %eax
    mov %ecx, 4(%eax)
    mov %edx, 8(%eax)
    mov %ebx, 12(%eax)
    mov %esp, 16(%eax)
    mov %ebp, 20(%eax)
    mov %esi, 24(%eax)
    mov %edi, 28(%eax)
    mov g_save_flags, %ecx
    mov %ecx, 32(%eax)
    mov g_save_eax, %ecx
    mov %ecx, 0(%eax)

    mov g_host_esp, %esp
    cld
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret

    .bss
    .align 4
g_in:         .long 0
g_out:        .long 0
g_code:       .long 0
g_host_esp:   .long 0
g_save_eax:   .long 0
g_save_flags: .long 0
    .section .note.GNU-stack, "", @progbits
