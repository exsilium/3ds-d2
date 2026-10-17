// Shared layout of the x86 reference lane. Every executor (qemu-i386 here,
// a CPU backend under test later) must place fixtures and scratch memory at
// these guest addresses, so results compare byte for byte.
#ifndef D2_TESTS_X86_ABI_H
#define D2_TESTS_X86_ABI_H

#define X86_FIXTURE_BASE 0x00400000u  // .fixtures section (code + jump tables)
#define X86_SCRATCH_BASE 0x00600000u  // 64 KiB data, seeded before every case
#define X86_SCRATCH_SIZE 0x00010000u
#define X86_TEB_BASE     0x00608000u  // FS base for TLS fixtures; [+0x18] = self
#define X86_STACK_LOW    0x00610000u
#define X86_STACK_TOP    0x00620000u  // ESP at fixture entry, minus the return address
#define X86_RETURN_TRAP  0x00700000u  // return address pushed by non-native executors
// Everything lies below 8 MiB, so an executor can back the whole guest span
// with one small block (the single-arena model the 3DS needs).
#define X86_GUEST_SPAN   0x00800000u

// Defined status flags: CF PF AF ZF SF DF OF. IF and reserved bits are never compared.
#define FL_CF 0x0001u
#define FL_PF 0x0004u
#define FL_AF 0x0010u
#define FL_ZF 0x0040u
#define FL_SF 0x0080u
#define FL_DF 0x0400u
#define FL_OF 0x0800u
#define FL_ALL (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_DF | FL_OF)

#endif
