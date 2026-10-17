// Differential CPU lane: runs the x86 lane's fixtures (tests/x86) through a
// d2rt::Cpu backend and prints STATE/MEM lines in the golden-file format, so
// the output diffs directly against tests/x86/golden/qemu-i386.txt.
//
//   cpu_runner <fixtures.bin> <fixtures.sym>
//
// Each case: reset the FPU (fx__reset), seed scratch memory, load the input
// registers, push the trap sentinel as return address, run until the fixture's
// RET enters the trap window, then read the registers and memory back.
#include "runtime/cpu.h"

extern "C" {
#include "../x86/cases.h"
}

#ifdef __3DS__
// 3DS build: the fixtures are linked in (bin2s), results go to the SD card.
#include <3ds.h>
#include <sys/stat.h>
extern "C" {
extern const uint8_t fixtures_bin[], fixtures_bin_end[];
extern const uint8_t fixtures_sym[], fixtures_sym_end[];
}
#define LANE_DIR "sdmc:/3ds/3ds-d2/cpu-lane"
#endif

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace d2rt;

namespace {

constexpr uint32_t kTrapLo = X86_RETURN_TRAP;  // return sentinel window (one page)
constexpr uint32_t kMemSize = X86_STACK_TOP - X86_SCRATCH_BASE;
// One unused page above the stack top: the backend's fault reporter dumps the
// guest stack upward and would otherwise fault a second time past the top.
constexpr uint32_t kGuardSize = 0x1000;

struct Fixture {
    const char *name;
    uint32_t mask;
    int cases;
};

#define ROW(n, m, g) {#n, m, g},
const Fixture kFixtures[] = {X86_FIXTURES(ROW)};

std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return v;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

std::map<std::string, uint32_t> parse_symbols(const char *text, size_t len)
{
    std::map<std::string, uint32_t> m;
    std::string t(text, len);
    size_t pos = 0;
    while (pos < t.size()) {
        size_t eol = t.find('\n', pos);
        if (eol == std::string::npos)
            eol = t.size();
        char name[128];
        unsigned addr;
        char type;
        if (std::sscanf(t.substr(pos, eol - pos).c_str(), "%x %c %127s", &addr, &type, name) == 3)
            m[name] = addr;
        pos = eol + 1;
    }
    return m;
}

// Runs one call: registers from `in`, ESP at the stack top minus the return slot.
bool call(Cpu &cpu, uint32_t entry, const uint32_t in[8], const char **fault)
{
    static const int order[8] = {R_EAX, R_ECX, R_EDX, R_EBX, R_EBP, R_ESI, R_EDI, R_EFLAGS};
    for (int r = 0; r < 8; r++)
        cpu.set_reg(order[r], in[r]);
    cpu.set_reg(R_ESP, X86_STACK_TOP - 4);
    cpu.write_u32(X86_STACK_TOP - 4, kTrapLo);
    return cpu.run(entry, fault);
}

}  // namespace

int main(int argc, char **argv)
{
#ifdef __3DS__
    (void)argc; (void)argv;
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    std::printf("3ds-d2 CPU lane (Box86 ARMv6)\nresults: " LANE_DIR "/results.txt\n");
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3ds-d2", 0777);
    mkdir(LANE_DIR, 0777);
    FILE *out = std::fopen(LANE_DIR "/results.txt", "w");
    FILE *err = std::freopen(LANE_DIR "/runner.stderr", "w", stderr);
    if (!out || !err) {
        std::printf("FATAL cannot open " LANE_DIR "\n");
        for (int i = 0; i < 300; i++) gspWaitForVBlank();
        gfxExit();
        return 1;
    }
    setvbuf(err, nullptr, _IONBF, 0);
    static char outbuf[1 << 16];
    setvbuf(out, outbuf, _IOFBF, sizeof outbuf);
    std::vector<uint8_t> code(fixtures_bin, fixtures_bin_end);
    std::map<std::string, uint32_t> sym =
        parse_symbols((const char *)fixtures_sym, (size_t)(fixtures_sym_end - fixtures_sym));
#else
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fixtures.bin> <fixtures.sym>\n", argv[0]);
        return 2;
    }
    FILE *out = stdout;
    setvbuf(out, nullptr, _IOLBF, 0);
    std::vector<uint8_t> code = read_file(argv[1]);
    std::vector<uint8_t> symtext = read_file(argv[2]);
    std::map<std::string, uint32_t> sym = parse_symbols((const char *)symtext.data(), symtext.size());
#endif
    if (code.empty() || sym.empty() || !sym.count("fx__reset")) {
        std::fprintf(out, "FATAL cannot read fixtures\n");
        return 1;
    }

    // Single-arena memory model (guest + membase), as on the 3DS: the whole
    // guest span is one host block. An explicit WX86_ARENA wins.
    setenv("WX86_ARENA", "800000", 0);
    Cpu *cpu = make_cpu_box86();
    if (!cpu->map(X86_FIXTURE_BASE, (uint32_t)code.size(), code.data(), P_R | P_X) ||
        !cpu->map(X86_SCRATCH_BASE, kMemSize + kGuardSize, nullptr, P_RW)) {
        std::fprintf(out, "FATAL map\n");
        return 1;
    }
    cpu->set_trap(kTrapLo, kTrapLo + 0x1000, [](Cpu &, uint32_t) { return false; });
    cpu->set_fs_base(X86_TEB_BASE);

    std::fprintf(out, "LANE x86-reference v1 fixture_base=%08x scratch=%08x stack_top=%08x fs=ok\n", X86_FIXTURE_BASE,
                X86_SCRATCH_BASE, X86_STACK_TOP);

    // Debugging aids: CPU_RUNNER_ONLY=<fixture> restricts the run;
    // CPU_RUNNER_TRACE=1 reports each call on stderr before it starts.
    const char *only = std::getenv("CPU_RUNNER_ONLY");
    const bool trace = std::getenv("CPU_RUNNER_TRACE") != nullptr;

    std::vector<uint8_t> mem(kMemSize);
    unsigned total = 0, faults = 0;
    const size_t nfix = sizeof kFixtures / sizeof kFixtures[0];
    for (size_t f = 0; f < nfix; f++) {
        const Fixture &fx = kFixtures[f];
        if (only && std::strcmp(only, fx.name) != 0)
            continue;
        const uint32_t entry = sym["fx_" + std::string(fx.name)];
#ifdef __3DS__
        std::printf("\x1b[4;1H%-20s\x1b[K", fx.name);
        gspWaitForVBlank();
#endif
        std::fprintf(out, "FIXTURE %s addr=%08" PRIx32 " mask=%04" PRIx32 " cases=%d\n", fx.name, entry, fx.mask,
                    x86_ncases(fx.cases));
        for (int i = 0; i < x86_ncases(fx.cases); i++) {
            uint32_t in[8];
            const char *fault = nullptr;
            x86_make_case(fx.cases, i, in);
            if (trace)
                std::fprintf(stderr, "TRACE %s %d reset\n", fx.name, i);
            call(*cpu, sym["fx__reset"], in, &fault);
            if (trace)
                std::fprintf(stderr, "TRACE %s %d run\n", fx.name, i);
            x86_seed_memory(mem.data(), x86_case_seed(f, i));
            cpu->write(X86_SCRATCH_BASE, mem.data(), kMemSize);
            if (!call(*cpu, entry, in, &fault)) {
                std::fprintf(out, "FAULT %s %d %s at %08" PRIx32 "\n", fx.name, i, fault ? fault : "?", cpu->fault_addr());
                faults++;
                continue;
            }
            cpu->read(X86_SCRATCH_BASE, mem.data(), kMemSize);
            const uint64_t h_mem = x86_fnv64(mem.data(), X86_SCRATCH_SIZE);
            const uint64_t h_stk =
                x86_fnv64(mem.data() + (X86_STACK_LOW - X86_SCRATCH_BASE), X86_STACK_TOP - 4 - X86_STACK_LOW);
            std::fprintf(out, "STATE %s %d eax=%08" PRIx32 " ecx=%08" PRIx32 " edx=%08" PRIx32 " ebx=%08" PRIx32
                        " esp=%08" PRIx32 " ebp=%08" PRIx32 " esi=%08" PRIx32 " edi=%08" PRIx32 " fl=%04" PRIx32
                        " mem=%016" PRIx64 " stk=%016" PRIx64 "\n",
                        fx.name, i, cpu->reg(R_EAX), cpu->reg(R_ECX), cpu->reg(R_EDX), cpu->reg(R_EBX),
                        cpu->reg(R_ESP), cpu->reg(R_EBP), cpu->reg(R_ESI), cpu->reg(R_EDI),
                        cpu->reg(R_EFLAGS) & fx.mask, h_mem, h_stk);
            if (x86_dumps_memory(fx.name)) {
                const uint8_t *m = mem.data() + (in[X86_EDI] - X86_SCRATCH_BASE);
                std::fprintf(out, "MEM %s %d ", fx.name, i);
                for (int b = 0; b < 64; b++)
                    std::fprintf(out, "%02x", m[b]);
                std::fprintf(out, "\n");
            }
            total++;
        }
    }
    std::fprintf(out, "DONE fixtures=%zu cases=%u\n", nfix, total);
    if (faults)
        std::fprintf(stderr, "cpu_runner: %u faulting cases\n", faults);
    std::fflush(out);
#ifdef __3DS__
    std::fclose(out);
    std::printf("DONE fixtures=%zu cases=%u faults=%u\n", nfix, total, faults);
    for (int i = 0; i < 120; i++) gspWaitForVBlank();
    gfxExit();
    return 0;
#else
    std::_Exit(0);  // skip the backend's global destructors; this is a one-shot process
#endif
}
