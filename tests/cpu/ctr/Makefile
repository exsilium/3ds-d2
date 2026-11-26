# 3DS build of the differential CPU lane: WinVita's Box86 backend with the
# armv6 patch series, compiled by devkitARM (hard-float, ARMv6K, VFPv2), linked
# with cpu_runner.cpp into a .3dsx that runs every x86 fixture and writes the
# results to sdmc:/3ds/3ds-d2/cpu-lane/. Run through `scripts/docker.sh build-cpu-lane`.
#
# Sources come from ../../../build/cpu/armv6/winx86, prepared by
# `make -C tests/cpu VARIANT=armv6` (pristine submodule + patch series).
ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not set; build through scripts/docker.sh)
endif

ROOT   := ../../..
W      := $(ROOT)/build/cpu/armv6/winx86
B86    := $(W)/third_party/box86-dynarec
SHIM   := $(W)/src/dynarec86/shim
CTR    := $(SHIM)/ctr
DYN86  := $(W)/src/dynarec86
RT     := $(W)/src/runtime
X86    := $(ROOT)/build/x86
OUT    := $(ROOT)/build/cpu/ctr
# CPU: the game app's CPU backend, chosen when it is built (issue #138,
# docs/aot.md). box86: the dynarec, as ever. aot: CpuAot (src/runtime/
# rt_cpu_aot.h), the AOT code alone, needing AOT=<dir>: the dynarec's objects
# and its code pool aren't in the app, whose memory is then the game's. Its
# objects are in a folder of their own.
CPU ?= box86
AOT_OPT ?= -O2 -g
ifeq ($(CPU),aot)
OUT    := $(ROOT)/build/cpu/ctr-aot
CPUDEF := -DD2RT_CPU_AOT
# The functions off the recompiler's hot list (AOT_FNC, src/game/aot_rt.h) for
# size: 81% of the code's bytes ran in none of the profiles (issue #153).
AOTDEF := -DAOT_COLD_SIZE
# AOT_SAMPLE=1: a profile of where compiled code is (src/game/aot_rt.h's
# AOT_SAMPLE_ON), in a build directory of its own.
ifneq ($(AOT_SAMPLE),)
OUT    := $(ROOT)/build/cpu/ctr-aot-sample
CPUDEF += -DAOT_SAMPLE_ON
AOTDEF += -DAOT_SAMPLE_ON
endif
# AOT_OPT: the generated C's optimisation (its 24 MiB of code are the heap's
# loss: docs/aot.md), with AOT_TAG naming a build directory of its own.
ifneq ($(AOT_TAG),)
OUT    := $(OUT)-$(AOT_TAG)
endif
else
CPUDEF := -DD2RT_CPU_BOX86
endif
CTRULIB := $(DEVKITPRO)/libctru

PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
CC     := $(PREFIX)gcc
CXX    := $(PREFIX)g++

ARCH   := -marm -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -mtp=soft
DEFS   := -D__3DS__ -DDYNAREC -DARM -DUSE_MMAP -DTRACE_MEMSTAT \
          '-D__clear_cache(b,e)=dyn86_ctr_clear_cache(b,e)'
INC    := -I$(CTR) -I$(SHIM) -I$(DYN86) -I$(B86)/include -I$(B86) -I$(B86)/dynarec -I$(CTRULIB)/include
# GCC 16 rejects Box86's uint32_t*/uintptr_t* mixing; both are 32-bit on ARM.
CFLAGS := -O2 -g $(ARCH) -std=gnu17 $(DEFS) $(INC) -ffile-prefix-map=$(abspath $(ROOT))=. \
          -Wno-error=incompatible-pointer-types -w
CXXFLAGS := -O2 -g $(ARCH) -std=gnu++17 -D__3DS__ $(CPUDEF) -I$(CTR) -I$(W)/src -I$(DYN86) \
          -I$(SHIM) -I$(B86)/include -I$(B86) -I$(CTRULIB)/include -ffile-prefix-map=$(abspath $(ROOT))=. -w
# Header dependencies (-MMD): without them a changed header (guest_region.h
# gained members) left older objects compiled against the old class layout,
# and the app crashed at its first HeapAlloc.
CFLAGS += -MMD -MP
CXXFLAGS += -MMD -MP
# WinVita's PROF_COUNTERS (run/shim time split, EIP samples per guest
# thread), in every build: counting only while a profiler run's guest code
# profile asks for it (rt::set_profiling, issue #19), a branch per shim call
# and per slice otherwise.
CFLAGS += -DPROF_COUNTERS
CXXFLAGS += -DPROF_COUNTERS
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -L$(CTRULIB)/lib -Wl,-Map,$(OUT)/cpu-lane.map

CORE := dynarec/dynarec.c dynarec/dynablock.c dynarec/dynarec_arm.c dynarec/dynarec_arm_functions.c \
        dynarec/dynarec_arm_jmpnext.c custommem.c tools/rbtree.c emu/x87emu_private.c emu/x86primop.c \
        emu/x86compstrings.c
ASM  := dynarec/arm_prolog.S dynarec/arm_epilog.S dynarec/arm_next.S dynarec/arm_lock_helper.S
PASS := helper pass emit_tests emit_math emit_logic emit_shift 00 0f 64 65 66 67 d8 d9 da db dc dd de df f0 660f 66f0 f20f f30f
SHIMSRC := $(SHIM)/shim_impl.c $(SHIM)/x86run_flags.c $(DYN86)/dyn86.c $(DYN86)/alt_table.c \
           $(DYN86)/dyn86_emitprof.c $(DYN86)/dyn86_memintrin.c $(DYN86)/dyn86_intrin.c $(CTR)/mman_ctr.c
RTSRC := cpu.cpp cpu_box86.cpp gil.cpp host_clock.cpp prof.cpp
# The full Win32 runtime for the M3 app: everything build.sh compiles except
# the native-thread scheduler and sockets (not used; they need Linux APIs).
RTFULL := $(filter-out sched_native.cpp win32_shims_wsock32.cpp,\
          $(notdir $(shell sed -n '/^RT_SRC="/,/^"/p' $(W)/build.sh | grep -o '\$$RT/[a-z0-9_]*\.cpp')))

CPUOBJ := $(addprefix $(OUT)/core/,$(notdir $(CORE:.c=.o))) \
       $(addprefix $(OUT)/core/,$(notdir $(ASM:.S=.o))) \
       $(addprefix $(OUT)/shim/,$(notdir $(SHIMSRC:.c=.o))) \
       $(foreach s,0 1 2 3,$(addprefix $(OUT)/pass$(s)/dynarec_arm_,$(addsuffix .o,$(PASS))))
OBJ := $(CPUOBJ) $(addprefix $(OUT)/rt/,$(RTSRC:.cpp=.o)) \
       $(OUT)/runner/cpu_runner.o $(OUT)/runner/ctr_stubs.o $(OUT)/runner/fixtures_bin.o $(OUT)/runner/fixtures_sym.o
M3SRC := $(notdir $(wildcard $(ROOT)/src/runtime/rt_*.cpp))
M3OBJ := $(CPUOBJ) $(addprefix $(OUT)/rt/,$(RTFULL:.cpp=.o)) $(addprefix $(OUT)/m3/,$(M3SRC:.cpp=.o)) \
         $(OUT)/m3/ctr_main.o $(OUT)/runner/ctr_stubs.o $(OUT)/m3/m3test_exe.o

# The game app (src/app/ctr_game.cpp): the same runtime plus the controls.
# Its host heap's growth goes through ctr_game.cpp's __wrap__sbrk_r (issue #24), and abort()
# through its __wrap_abort (issue #66).
GAMELDFLAGS := -Wl,--wrap=_sbrk_r -Wl,--wrap=abort
GAMESRC := $(notdir $(wildcard $(ROOT)/src/game/*.cpp))
BINKSRC := $(notdir $(wildcard $(ROOT)/src/game/bink/*.cpp))
# With CPU=aot, of the backend's own objects only its memory services (the
# guest's arena is mmap'd through them) and the 80-bit conversions aot_rt.h
# uses; and no cpu_box86.cpp.
ifeq ($(CPU),aot)
GAMECPUOBJ := $(OUT)/shim/mman_ctr.o $(OUT)/core/x87emu_private.o
GAMERT := $(filter-out cpu_box86.cpp,$(RTFULL))
else
GAMECPUOBJ := $(CPUOBJ)
GAMERT := $(RTFULL)
endif
GAMEOBJ := $(GAMECPUOBJ) $(addprefix $(OUT)/rt/,$(GAMERT:.cpp=.o)) $(addprefix $(OUT)/m3/,$(M3SRC:.cpp=.o)) \
           $(addprefix $(OUT)/game/,$(GAMESRC:.cpp=.o)) $(addprefix $(OUT)/game/bink/,$(BINKSRC:.cpp=.o)) $(OUT)/game/ctr_game.o $(OUT)/game/ctr_audio.o $(OUT)/game/ctr_screenshot.o $(OUT)/game/ctr_writer.o $(OUT)/game/ctr_report.o $(OUT)/game/ctr_tls.o $(OUT)/runner/ctr_stubs.o

# AOT=<dir> (scripts/docker.sh package-cia with D2_AOT_BUILD=1): the AOT
# spike's C, generated there from the user's Game.exe (issue #113,
# tools/aot/), in the game app twice (src/game/aot_rt.h: inlined and not).
# aot.cfg relinks the app when it comes or goes.
AOT ?=
ifneq ($(AOT),)
AOTSRC := $(sort $(wildcard $(AOT)/aot_spike*.c))   # the tables and the functions' parts (issue #135)
# (CPU=aot: once. The build with no function inlined into another is the spike's SigCalls modes'.)
AOTOBJ := $(foreach v,$(if $(filter aot,$(CPU)),d,d n),$(patsubst $(AOT)/%.c,$(OUT)/aot/$(v)/%.o,$(AOTSRC)))
GAMEOBJ += $(AOTOBJ)
endif
$(OUT)/aot/d/%.o: $(AOT)/%.c $(ROOT)/src/game/aot_rt.h
	@mkdir -p $(@D); $(CC) $(AOT_OPT) $(ARCH) -std=gnu17 -I$(ROOT)/src -w $(AOTDEF) -c $< -o $@
$(OUT)/aot/n/%.o: $(AOT)/%.c $(ROOT)/src/game/aot_rt.h
	@mkdir -p $(@D); $(CC) -O2 -g $(ARCH) -std=gnu17 -I$(ROOT)/src -w -DAOT_NOINLINE -c $< -o $@
$(OUT)/aot.cfg: FORCE
	@mkdir -p $(@D); echo '$(AOT)' | cmp -s - $@ || echo '$(AOT)' > $@
# The AOT objects alone (scripts/docker.sh builds them first at AOT_JOBS:
# each part takes gcc about 1 GB, too many for the app's -j8).
aot-objs: $(AOTOBJ)

.PHONY: all clean FORCE aot-objs

# The compile flags: when they change (PROF_COUNTERS joining every build),
# every object is built again instead of old and new ones linked together.
FLAGS_CFG := $(subst ',,$(CFLAGS) | $(CXXFLAGS))
$(OUT)/flags.cfg: FORCE
	@mkdir -p $(@D); echo '$(FLAGS_CFG)' | cmp -s - $@ || echo '$(FLAGS_CFG)' > $@
$(sort $(OBJ) $(M3OBJ) $(GAMEOBJ) $(OUT)/game/ctr_game_cia.o): $(OUT)/flags.cfg
all: $(OUT)/cpu-lane.3dsx $(OUT)/m3.3dsx $(OUT)/3ds-d2-game.3dsx $(OUT)/3ds-d2-game-cia.elf

vpath %.c $(B86)/dynarec $(B86) $(B86)/tools $(B86)/emu $(SHIM) $(DYN86) $(CTR)
vpath %.S $(B86)/dynarec

$(OUT)/core/%.o: %.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -c $< -o $@
$(OUT)/core/%.o: %.S
	@mkdir -p $(@D); $(CC) $(ARCH) -c $< -o $@
$(OUT)/shim/%.o: %.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -c $< -o $@
define PASS_RULE
$(OUT)/pass$(1)/dynarec_arm_%.o: $(B86)/dynarec/dynarec_arm_%.c
	@mkdir -p $$(@D); $$(CC) $$(CFLAGS) -DSTEP=$(1) -c $$< -o $$@
endef
$(foreach s,0 1 2 3,$(eval $(call PASS_RULE,$(s))))
$(OUT)/rt/%.o: $(RT)/%.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/runner/cpu_runner.o: ../cpu_runner.cpp ../../x86/cases.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -c $< -o $@
$(OUT)/runner/ctr_stubs.o: ctr_stubs.c
	@mkdir -p $(@D); $(CC) $(CFLAGS) -c $< -o $@
$(OUT)/runner/fixtures_%.o: $(X86)/fixtures.%
	@mkdir -p $(@D); cp $< $(OUT)/runner/fixtures.$*; cd $(OUT)/runner && bin2s fixtures.$* > fixtures_$*.s && $(CC) $(ARCH) -c fixtures_$*.s -o fixtures_$*.o

$(OUT)/m3/%.o: $(ROOT)/src/runtime/%.cpp $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -I$(ROOT)/src -c $< -o $@
$(OUT)/m3/ctr_main.o: $(ROOT)/tests/m3/ctr_main.cpp $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -I$(ROOT)/src -c $< -o $@
$(OUT)/m3/m3test_exe.o: $(ROOT)/build/pe/m3test.exe
	@mkdir -p $(@D); cp $< $(OUT)/m3/m3test.exe; cd $(OUT)/m3 && bin2s m3test.exe > m3test_exe.s && $(CC) $(ARCH) -c m3test_exe.s -o m3test_exe.o
$(OUT)/m3.elf: $(M3OBJ)
	$(CXX) $(LDFLAGS) $^ -lctru -lm -o $@
$(OUT)/m3.smdh:
	@mkdir -p $(@D); smdhtool --create "3ds-d2 M3" "PE32 through the Win32 runtime" "3ds-d2" $(CTRULIB)/default_icon.png $@
$(OUT)/m3.3dsx: $(OUT)/m3.elf $(OUT)/m3.smdh
	3dsxtool $< $@ --smdh=$(OUT)/m3.smdh
	@echo "built $@"

$(OUT)/game/%.o: $(ROOT)/src/game/%.cpp $(wildcard $(ROOT)/src/game/*.h) $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# The Bink decoder: hot integer code, built for speed like the blitters.
$(OUT)/game/bink/%.o: $(ROOT)/src/game/bink/%.cpp $(wildcard $(ROOT)/src/game/bink/*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
$(OUT)/game/ctr_game.o: $(ROOT)/src/app/ctr_game.cpp $(ROOT)/src/app/ctr_writer.h $(wildcard $(ROOT)/src/game/*.h) $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# The CIA flavour: the same objects, with the app built for the New 3DS's
# extended memory mode and its RomFS (scripts/package-cia.sh packs it).
$(OUT)/game/ctr_audio.o: $(ROOT)/src/app/ctr_audio.cpp $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# The SD card's writer thread (issue #71).
$(OUT)/game/ctr_writer.o: $(ROOT)/src/app/ctr_writer.cpp $(ROOT)/src/app/ctr_writer.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# Screenshots into the Camera album: libjpeg-turbo from the pinned image's portlibs.
PORTLIBS := $(DEVKITPRO)/portlibs/3ds
$(OUT)/game/ctr_screenshot.o: $(ROOT)/src/app/ctr_screenshot.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -I$(PORTLIBS)/include -c $< -o $@
# Crash and session reports (issue #26): the build ID, the title version and
# the Sentry DSN go into ctr_report.o only. report.cfg changes when they do,
# so it is rebuilt then. Unless given, they are scripts/build-id.sh's ID (so
# every build of other sources reports as its own), the CIA's title version
# as a -dev build (docker.sh passes scripts/version.sh's, which knows a release
# by its tag; issue #61), and the project's DSN from packaging/cia/report.dsn; D2_REPORT_DSN= (empty)
# builds an app that never offers or sends anything.
# Both report objects are built for size: they never run during play, and
# their code is resident all the same.
ifeq ($(origin BUILD_ID),undefined)
BUILD_ID := $(or $(shell sh $(ROOT)/scripts/build-id.sh 2>/dev/null),unversioned)
endif
ifeq ($(origin D2_VERSION),undefined)
D2_VERSION := $(or $(shell sed -n 's/^version=\([0-9.]*\).*/\1/p' $(ROOT)/scripts/package-cia.sh),0.0.0)-dev
endif
ifeq ($(origin D2_REPORT_DSN),undefined)
D2_REPORT_DSN := $(shell tr -d '\r\n' < $(ROOT)/packaging/cia/report.dsn 2>/dev/null)
endif
REPORT_CFG := $(BUILD_ID) $(D2_VERSION) $(D2_REPORT_DSN)
$(OUT)/game/report.cfg: FORCE
	@mkdir -p $(@D); echo '$(REPORT_CFG)' | cmp -s - $@ || echo '$(REPORT_CFG)' > $@
$(OUT)/game/ctr_report.o: $(ROOT)/src/app/ctr_report.cpp $(wildcard $(ROOT)/src/game/*.h) $(OUT)/game/report.cfg
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -Os -I$(ROOT)/src -I$(PORTLIBS)/include -DD2_BUILD_ID='"$(BUILD_ID)"' \
	    -DD2_VERSION='"$(D2_VERSION)"' -DD2_REPORT_DSN='"$(D2_REPORT_DSN)"' -c $< -o $@
$(OUT)/game/report.o: $(ROOT)/src/game/report.cpp $(ROOT)/src/game/report.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -Os -I$(ROOT)/src -c $< -o $@
$(OUT)/game/ctr_tls.o: $(ROOT)/src/app/ctr_tls.cpp $(ROOT)/src/tls/tls_module.h
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -Os -I$(ROOT)/src -c $< -o $@

# The TLS module (src/tls/tls_module.h): mbedTLS and the root certificates,
# kept out of the app and loaded only to send a problem report. It links
# neither newlib nor libctru (-nostdlib; tls_libc.c), and is shipped in the
# RomFS: package-cia.sh copies it into the CIA's, the .3dsx gets its own.
TLSFLAGS := $(ARCH) -Os -Wall -Wextra -ffunction-sections -fdata-sections -fno-common -I$(ROOT)/src \
            -I$(PORTLIBS)/include -ffile-prefix-map=$(abspath $(ROOT))=.
$(OUT)/tls/tls_module.o: $(ROOT)/src/tls/tls_module.c $(ROOT)/src/tls/tls_module.h $(ROOT)/src/tls/root_ca.inc
	@mkdir -p $(@D); $(CC) $(TLSFLAGS) -c $< -o $@
$(OUT)/tls/tls_libc.o: $(ROOT)/src/tls/tls_libc.c
	@mkdir -p $(@D); $(CC) $(TLSFLAGS) -O2 -ffreestanding -fno-tree-loop-distribute-patterns -c $< -o $@
$(OUT)/tls/tls_module.elf: $(OUT)/tls/tls_module.o $(OUT)/tls/tls_libc.o $(ROOT)/src/tls/module.ld
	$(CC) $(ARCH) -nostdlib -nostartfiles -static -Wl,--gc-sections -Wl,--emit-relocs -Wl,--no-undefined \
	    -Wl,--no-warn-rwx-segments -T $(ROOT)/src/tls/module.ld $(filter %.o,$^) -L$(PORTLIBS)/lib \
	    -lmbedtls -lmbedx509 -lmbedcrypto -lgcc -o $@
$(OUT)/tls.bin: $(OUT)/tls/tls_module.elf $(ROOT)/src/tls/pack_module.py
	python3 -I $(ROOT)/src/tls/pack_module.py $< $@
$(OUT)/romfs-3dsx/tls.bin: $(OUT)/tls.bin
	@mkdir -p $(@D); cp $< $@
$(OUT)/game/ctr_game_cia.o: $(ROOT)/src/app/ctr_game.cpp $(wildcard $(ROOT)/src/game/*.h) $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -DD2_CIA -I$(ROOT)/src -c $< -o $@
$(OUT)/3ds-d2-game-cia.elf: $(filter-out $(OUT)/game/ctr_game.o,$(GAMEOBJ)) $(OUT)/game/ctr_game_cia.o $(OUT)/aot.cfg
	$(CXX) $(LDFLAGS) $(GAMELDFLAGS) $(filter %.o,$^) -L$(PORTLIBS)/lib -ljpeg -lz -lctru -lm -o $@

$(OUT)/3ds-d2-game.elf: $(GAMEOBJ) $(OUT)/aot.cfg
	$(CXX) $(LDFLAGS) $(GAMELDFLAGS) $(filter %.o,$^) -L$(PORTLIBS)/lib -ljpeg -lz -lctru -lm -o $@
$(OUT)/3ds-d2-game.smdh:
	@mkdir -p $(@D); smdhtool --create "3ds-d2" "Diablo II LoD 1.14d on the n3DS" "3ds-d2" $(CTRULIB)/default_icon.png $@
$(OUT)/3ds-d2-game.3dsx: $(OUT)/3ds-d2-game.elf $(OUT)/3ds-d2-game.smdh $(OUT)/romfs-3dsx/tls.bin
	3dsxtool $< $@ --smdh=$(OUT)/3ds-d2-game.smdh --romfs=$(OUT)/romfs-3dsx
	@echo "built $@"

$(OUT)/cpu-lane.elf: $(OBJ)
	$(CXX) $(LDFLAGS) $^ -lctru -lm -o $@
$(OUT)/cpu-lane.smdh:
	@mkdir -p $(@D); smdhtool --create "3ds-d2 CPU lane" "Box86 ARMv6 differential lane" "3ds-d2" $(CTRULIB)/default_icon.png $@
$(OUT)/cpu-lane.3dsx: $(OUT)/cpu-lane.elf $(OUT)/cpu-lane.smdh
	3dsxtool $< $@ --smdh=$(OUT)/cpu-lane.smdh
	@echo "built $@"

-include $(shell find $(OUT) -name '*.d' 2>/dev/null)

clean:
	rm -rf $(OUT)
