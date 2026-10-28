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
CXXFLAGS := -O2 -g $(ARCH) -std=gnu++17 -D__3DS__ -DD2RT_CPU_BOX86 -I$(CTR) -I$(W)/src -I$(DYN86) \
          -I$(SHIM) -I$(B86)/include -I$(B86) -I$(CTRULIB)/include -ffile-prefix-map=$(abspath $(ROOT))=. -w
# Header dependencies (-MMD): without them a changed header (guest_region.h
# gained members) left older objects compiled against the old class layout,
# and the app crashed at its first HeapAlloc.
CFLAGS += -MMD -MP
CXXFLAGS += -MMD -MP
# PROF=1 (package-cia --prof): the profiling build. WinVita's PROF_COUNTERS
# (run/shim time split, EIP samples per guest thread) reported to game.log
# every 10 s; objects in their own directory so the two builds never mix.
ifeq ($(PROF),1)
OUT := $(ROOT)/build/cpu/ctr-prof
CFLAGS += -DPROF_COUNTERS
CXXFLAGS += -DPROF_COUNTERS
endif
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
GAMESRC := $(notdir $(wildcard $(ROOT)/src/game/*.cpp))
BINKSRC := $(notdir $(wildcard $(ROOT)/src/game/bink/*.cpp))
GAMEOBJ := $(CPUOBJ) $(addprefix $(OUT)/rt/,$(RTFULL:.cpp=.o)) $(addprefix $(OUT)/m3/,$(M3SRC:.cpp=.o)) \
           $(addprefix $(OUT)/game/,$(GAMESRC:.cpp=.o)) $(addprefix $(OUT)/game/bink/,$(BINKSRC:.cpp=.o)) $(OUT)/game/ctr_game.o $(OUT)/game/ctr_audio.o $(OUT)/game/ctr_screenshot.o $(OUT)/game/ctr_report.o $(OUT)/game/ctr_tls.o $(OUT)/runner/ctr_stubs.o

.PHONY: all clean FORCE
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
$(OUT)/game/ctr_game.o: $(ROOT)/src/app/ctr_game.cpp $(wildcard $(ROOT)/src/game/*.h) $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# The CIA flavour: the same objects, with the app built for the New 3DS's
# extended memory mode and its RomFS (scripts/package-cia.sh packs it).
$(OUT)/game/ctr_audio.o: $(ROOT)/src/app/ctr_audio.cpp $(wildcard $(ROOT)/src/runtime/rt_*.h)
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(ROOT)/src -c $< -o $@
# Screenshots into the Camera album: libjpeg-turbo from the pinned image's portlibs.
PORTLIBS := $(DEVKITPRO)/portlibs/3ds
$(OUT)/game/ctr_screenshot.o: $(ROOT)/src/app/ctr_screenshot.cpp
	@mkdir -p $(@D); $(CXX) $(CXXFLAGS) -O2 -I$(PORTLIBS)/include -c $< -o $@
# Crash and session reports (issue #26): the build ID, the title version and
# the Sentry DSN go into ctr_report.o only. report.cfg changes when they do,
# so it is rebuilt then. Unless given, they are scripts/build-id.sh's ID (so
# every build of other sources reports as its own), the CIA's title version,
# and the project's DSN from packaging/cia/report.dsn; D2_REPORT_DSN= (empty)
# builds an app that never offers or sends anything.
# Both report objects are built for size: they never run during play, and
# their code is resident all the same.
ifeq ($(origin BUILD_ID),undefined)
BUILD_ID := $(or $(shell sh $(ROOT)/scripts/build-id.sh 2>/dev/null),unversioned)
endif
ifeq ($(origin D2_VERSION),undefined)
D2_VERSION := $(or $(shell sed -n 's/^version=\([0-9.]*\).*/\1/p' $(ROOT)/scripts/package-cia.sh),0.0.0)
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
$(OUT)/3ds-d2-game-cia.elf: $(filter-out $(OUT)/game/ctr_game.o,$(GAMEOBJ)) $(OUT)/game/ctr_game_cia.o
	$(CXX) $(LDFLAGS) $^ -L$(PORTLIBS)/lib -ljpeg -lz -lctru -lm -o $@

$(OUT)/3ds-d2-game.elf: $(GAMEOBJ)
	$(CXX) $(LDFLAGS) $^ -L$(PORTLIBS)/lib -ljpeg -lz -lctru -lm -o $@
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
