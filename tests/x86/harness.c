// x86 reference lane: runs every fixture over its cases on an i686 executor
// (qemu-i386 in tests/x86/Makefile) and prints one STATE line per case:
//
//   STATE <fixture> <case> eax=.. ecx=.. edx=.. ebx=.. esp=.. ebp=.. esi=.. edi=..
//         fl=<eflags & mask> mem=<fnv64 of scratch> stk=<fnv64 of used stack>
//
// Fixtures, cases, seeding and hashing are defined once in cases.h, so another
// executor (cpu_runner.cpp) reproduces each case exactly.
#include "cases.h"

#include <asm/ldt.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

void run_case(const uint32_t in[8], uint32_t out[9], void (*code)(void));

#define DECL(n, m, g) extern void fx_##n(void);
X86_FIXTURES(DECL)

typedef struct fixture {
    const char *name;
    void (*code)(void);
    uint32_t mask;
    int cases;
} fixture;

#define ROW(n, m, g) {#n, fx_##n, m, g},
static const fixture fixtures[] = {X86_FIXTURES(ROW)};

static int setup_fs(void)
{
    struct user_desc d = {0};
    d.entry_number = (unsigned)-1;
    d.base_addr = X86_TEB_BASE;
    d.limit = 0xfff;
    d.seg_32bit = 1;
    d.useable = 1;
    if (syscall(SYS_set_thread_area, &d) != 0)
        return -1;
    unsigned short sel = (unsigned short)(d.entry_number << 3 | 3);
    __asm__ volatile("mov %0, %%fs" ::"r"(sel));
    return 0;
}

int main(void)
{
    uint8_t *mem = mmap((void *)X86_SCRATCH_BASE, X86_STACK_TOP - X86_SCRATCH_BASE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mem != (uint8_t *)X86_SCRATCH_BASE) {
        printf("FATAL scratch mmap\n");
        return 1;
    }
    int fs_ok = setup_fs() == 0;
    printf("LANE x86-reference v1 fixture_base=%08x scratch=%08x stack_top=%08x fs=%s\n", X86_FIXTURE_BASE,
           X86_SCRATCH_BASE, X86_STACK_TOP, fs_ok ? "ok" : "unavailable");

    unsigned total = 0;
    for (size_t f = 0; f < sizeof fixtures / sizeof fixtures[0]; f++) {
        const fixture *fx = &fixtures[f];
        printf("FIXTURE %s addr=%08" PRIx32 " mask=%04" PRIx32 " cases=%d\n", fx->name,
               (uint32_t)(uintptr_t)fx->code, fx->mask, x86_ncases(fx->cases));
        if (!fs_ok && fx->code == fx_fs_teb) {
            printf("SKIP %s no FS segment\n", fx->name);
            continue;
        }
        for (int i = 0; i < x86_ncases(fx->cases); i++) {
            uint32_t in[8], out[9];
            x86_make_case(fx->cases, i, in);
            x86_seed_memory(mem, x86_case_seed(f, i));
            __asm__ volatile("fninit");
            run_case(in, out, fx->code);
            uint64_t h_mem = x86_fnv64(mem, X86_SCRATCH_SIZE);
            uint64_t h_stk = x86_fnv64(mem + (X86_STACK_LOW - X86_SCRATCH_BASE), X86_STACK_TOP - 4 - X86_STACK_LOW);
            printf("STATE %s %d eax=%08" PRIx32 " ecx=%08" PRIx32 " edx=%08" PRIx32 " ebx=%08" PRIx32
                   " esp=%08" PRIx32 " ebp=%08" PRIx32 " esi=%08" PRIx32 " edi=%08" PRIx32 " fl=%04" PRIx32
                   " mem=%016" PRIx64 " stk=%016" PRIx64 "\n",
                   fx->name, i, out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7],
                   out[8] & fx->mask, h_mem, h_stk);
            if (x86_dumps_memory(fx->name)) {
                const uint8_t *m = mem + (in[X86_EDI] - X86_SCRATCH_BASE);
                printf("MEM %s %d ", fx->name, i);
                for (int b = 0; b < 64; b++)
                    printf("%02x", m[b]);
                printf("\n");
            }
            total++;
        }
    }
    printf("DONE fixtures=%zu cases=%u\n", sizeof fixtures / sizeof fixtures[0], total);
    return 0;
}
