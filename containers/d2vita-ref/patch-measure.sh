#!/bin/sh
# Measurement-only patches to the pinned D2Vita/WinVita trees, applied inside
# the reference image. Each adds output or an environment knob; with the knobs
# unset, the game behaves exactly as upstream. Every anchor is checked, so an
# upstream change fails the image build instead of silently skipping a patch.
set -eu
cd "${1:-/src/d2vita}"
RT=tools/rt_boot.cpp
WX=third_party/winx86
DYN=$WX/third_party/box86-dynarec/dynarec/dynarec_arm.c
SHIM=$WX/src/dynarec86/shim/shim_impl.c

need() { grep -q -F "$2" "$1" || { echo "patch-measure: anchor missing in $1: $2" >&2; exit 1; }; }

# 1. At a clean exit, print the guest allocator gauges and D2's own sprite-cache
#    counters (upstream prints them only when the game halts).
need $RT 'd2_crashlog("CLEAN EXIT (game path, frame=%d)", g_frame);'
sed -i 's|^\(    \)d2_crashlog("CLEAN EXIT (game path, frame=%d)", g_frame);|\1std::printf("[MEASURE] heap now/peak=%u/%u KB va now/peak=%u/%u KB heap-free-max=%u KB va-free-max=%u KB va-reserved=%llu KB va-committed-in=%llu KB va-committed-fresh=%llu KB\\n", g_heapA.cur()>>10, g_heapA.peak()>>10, g_vaA.cur()>>10, g_vaA.peak()>>10, g_heapA.largest_free()>>10, g_vaA.largest_free()>>10, (unsigned long long)(g_vaResB>>10), (unsigned long long)(g_vaComInB>>10), (unsigned long long)(g_vaComFrB>>10));\n\1std::printf("[D2CACHE-EXIT] sprite now/max=%u/%u KB celdata-max=%u KB bigmachine=%u celdata-now=%u KB\\n", d2rt_sprite_cache_kb(0), d2rt_sprite_cache_kb(1), d2rt_sprite_cache_kb(2), d2rt_sprite_cache_kb(3), d2rt_sprite_cache_kb(4)); std::fflush(stdout);\n&|' $RT

# 2. D2_TOTALPHYS_MB: the physical RAM GlobalMemoryStatus reports (D2 sizes
#    its sprite cache from it). Default stays D2Vita's 256 MiB.
need $RT 'wx86_mem_set_total_phys_mb(256);'
sed -i 's|wx86_mem_set_total_phys_mb(256);|wx86_mem_set_total_phys_mb(getenv("D2_TOTALPHYS_MB") ? (uint32_t)atoi(getenv("D2_TOTALPHYS_MB")) : 256u);|' $RT

# 3. WX86_CPUID_EDX=<hex>: CPUID leaf-1 EDX feature bits (and, through the same
#    macro, IsProcessorFeaturePresent). Default stays FPU CMOV MMX FXSR SSE SSE2.
need $SHIM '#define WX86_CPUID1_EDX ((1<<0)|(1<<15)|(1<<23)|(1<<24)|(1<<25)|(1<<26))'
sed -i 's@^#define WX86_CPUID1_EDX ((1<<0)|(1<<15)|(1<<23)|(1<<24)|(1<<25)|(1<<26)).*$@#include <stdlib.h>\nstatic uint32_t wx86_cpuid1_edx(void) { const char *s = getenv("WX86_CPUID_EDX"); return s ? (uint32_t)strtoul(s, 0, 16) : (uint32_t)((1<<0)|(1<<15)|(1<<23)|(1<<24)|(1<<25)|(1<<26)); }\n#define WX86_CPUID1_EDX (wx86_cpuid1_edx())@' $SHIM

# 4. WX86_BLOCKWATCH=<lo>-<hi> (hex guest VAs): log every translated block that
#    overlaps the range, i.e. proof that code there actually executed.
need $DYN '    block->x86_size = end-start;'
sed -i 's|^    block->x86_size = end-start;$|&\n    { static int w = -1; static unsigned long wlo, whi; if (w < 0) { const char *s = getenv("WX86_BLOCKWATCH"); w = s \&\& sscanf(s, "%lx-%lx", \&wlo, \&whi) == 2; } if (w \&\& (unsigned long)block->x86_addr < whi \&\& (unsigned long)block->x86_addr + block->x86_size > wlo) fprintf(stderr, "[BLOCKWATCH] %08lx+%u\\n", (unsigned long)block->x86_addr, (unsigned)block->x86_size); }|' $DYN

# 5. WX86_XFERLOG=<file> (with D2_NOLINK=1 so every transfer funnels through
#    LinkNext): append one binary record per control transfer, 11 x uint32:
#    source block, target EIP, EAX ECX EDX EBX ESP EBP ESI EDI, [ESP]. Two
#    deterministic runs (D2SCHED=coop D2_VIRTCLOCK=1) can then be compared to
#    find the first transfer where they diverge.
LN=$WX/third_party/box86-dynarec/dynarec/dynarec.c
need $LN '    // D2_EIPTRAP diag: report who chains into the suspect address (wild jump'
python3 - "$LN" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
anchor = '    // D2_EIPTRAP diag: report who chains into the suspect address (wild jump'
add = """    { static FILE* xl = (FILE*)-1;
      if (xl == (FILE*)-1) { const char* f = getenv("WX86_XFERLOG"); xl = f ? fopen(f, "wb") : NULL;
                             if (xl) setvbuf(xl, NULL, _IONBF, 0); }   /* a crash must not lose the tail */
      if (xl) { dynablock_t* sb = FindDynablockFromNativeAddress((void*)((uintptr_t)x2-8));
                uint32_t top = getProtection((uintptr_t)R_ESP) ? *(uint32_t*)DYN86_G2H(R_ESP) : 0;
                uint32_t r[11] = { sb ? (uint32_t)(uintptr_t)sb->x86_addr : 0, (uint32_t)addr,
                                   R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI, top };
                fwrite(r, sizeof r, 1, xl); } }
"""
assert s.count(anchor) == 1
s = s.replace(anchor, add + anchor)
open(p, 'w').write(s)
EOF

# 6. WX86_FAKETSC=1: RDTSC returns a counter advanced by a fixed step per call
#    instead of the host clock, so two runs see the same values (the game
#    times its CPU with RDTSC at boot).
need $SHIM '    clock_gettime(CLOCK_MONOTONIC, &ts);'
python3 - "$SHIM" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """uint64_t ReadTSC(x86emu_t* emu)
{
    (void)emu;
    struct timespec ts;"""
new = """uint64_t ReadTSC(x86emu_t* emu)
{
    (void)emu;
    static int fake = -1; static uint64_t fake_tsc = 0;
    if (fake < 0) fake = getenv("WX86_FAKETSC") != NULL;
    if (fake) return fake_tsc += 1000003ull;
    struct timespec ts;"""
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, new))
EOF

grep -c '\[MEASURE\]' $RT >/dev/null
grep -c 'D2_TOTALPHYS_MB' $RT >/dev/null
grep -c 'wx86_cpuid1_edx' $SHIM >/dev/null
grep -c 'BLOCKWATCH' $DYN >/dev/null
grep -c 'WX86_XFERLOG' $LN >/dev/null
grep -c 'WX86_FAKETSC' $SHIM >/dev/null
echo "patch-measure: 6 patches applied in $(pwd)"
