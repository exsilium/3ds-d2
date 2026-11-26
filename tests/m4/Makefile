# M4 game lane: Diablo II 1.14d's Game.exe through 3ds-d2's own Win32 runtime
# (src/runtime/rt_*.cpp + WinVita bridge/shims/scheduler) on the ARMv6 Box86
# backend, under qemu's ARM11 MPCore model, with the 3DS's settings. Run through
# `scripts/docker.sh test m4 <game-dir> [guest-ms] [command line]`; the game
# directory is mounted read-only at /game and never copied.
ROOT  := ../..
W     := $(ROOT)/build/cpu/armv6/winx86
# M4_OUT=<dir> (scripts/docker.sh): another output directory, so that lanes can run side by side.
OUT   ?= $(ROOT)/build/m4
# BIN_OUT: where the binary and its AOT objects go (OUT's by default). A lane
# that runs several scripts at once (scripts/docker.sh test aot) builds once
# there, then gives each run an OUT of its own (run-built: no build step, so
# the runs never race on one).
BIN_OUT ?= $(OUT)
CXX   := arm-linux-gnueabi-g++
ARCH  := -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp
INC   := -I$(ROOT)/src -I$(W)/src -I$(W)/src/dynarec86 -I$(W)/src/dynarec86/shim \
         -I$(W)/third_party/box86-dynarec/include -I$(W)/third_party/box86-dynarec
SRCS  := game_main.cpp $(wildcard $(ROOT)/src/runtime/rt_*.cpp) $(wildcard $(ROOT)/src/game/*.cpp) \
         $(wildcard $(ROOT)/src/game/bink/*.cpp)
# M4_QEMU_FLAGS=-one-insn-per-tb makes qemu slow enough that the game is
# CPU-bound, as on the 3DS: frames per second then compare the work per frame.
QEMU  := qemu-arm -cpu arm11mpcore $(M4_QEMU_FLAGS)
# M4_PROF=1: WinVita's PROF_COUNTERS, counting from the start, as a console
# profiler run's guest code profile (issue #19): the 10 s profile report
# goes to err.txt. A separate library flavour and binary, so the two builds
# never mix.
ifeq ($(M4_PROF),1)
LIBTAG := _prof
PROFDEF := -DPROF_COUNTERS
HOST := $(BIN_OUT)/m4_host_prof
else
HOST := $(BIN_OUT)/m4_host
endif
LIB   := $(W)/build-arm/libwinx86$(LIBTAG).a
# AOT=<dir> (scripts/docker.sh with D2_AOT_BUILD=1): the AOT spike's C,
# generated there from the user's Game.exe (issue #113, tools/aot/), compiled
# three ways (src/game/aot_rt.h) and linked into a binary of its own.
# AOT_SPLIT=1 (issue #115): compiled functions call each other through Box86
# (the #ifdef at each call the recompiler writes), in objects and a binary of
# their own.
AOT   ?=
# AOT_VARIANTS: the generated C's builds to make and link (d: as it is; n: no
# function inlined into another, the spike's SigCalls modes; j: journaled, the
# check's). AOT_OPT: their optimisation. A run that needs only compiled code
# to be there (CpuAot's, issue #138) builds in a sixth of the time with
# AOT_VARIANTS=d AOT_OPT=-O1, in an OUT of its own: the runtime's lookups of
# the other two are weak.
AOT_VARIANTS ?= d n j
# CPU: the CPU backend the binary is built with, at compile time (issue #138):
# box86, or aot (CpuAot, rt_cpu_aot.h: the AOT code alone, the dynarec not
# linked; needs AOT=).
CPU ?= box86
CPUDEF := $(if $(filter aot,$(CPU)),-DD2RT_CPU_AOT,-DD2RT_CPU_BOX86)
AOT_OPT ?= -O2 -g
AOT_SPLIT ?=
AOTDIR := $(BIN_OUT)/aot$(if $(AOT_SPLIT),-split)
ifneq ($(AOT),)
HOST   := $(HOST)_aot$(if $(AOT_SPLIT),_split)
AOTSRC := $(sort $(wildcard $(AOT)/aot_spike*.c))   # the tables and the functions' parts (issue #135)
AOTOBJ := $(foreach v,$(AOT_VARIANTS),$(patsubst $(AOT)/%.c,$(AOTDIR)/$(v)/%.o,$(AOTSRC)))
endif
AOTCC := arm-linux-gnueabi-gcc -std=gnu17 $(AOT_OPT) $(ARCH) -I$(ROOT)/src -w $(if $(AOT_SPLIT),-DAOT_SPLIT)
GAME  ?= /game
MS    ?= 20000
# TIMEOUT: seconds before a run is cut (exit status 124); scripts/docker.sh
# test aot gives more, its scripts sharing the host's cores.
TIMEOUT ?= 1800
CMD   ?= game.exe -w

.PHONY: FORCE run run-built host clean
host: $(HOST)

define RUN_GAME
	@test -f $(GAME)/Game.exe || { echo "no Game.exe in $(GAME)"; exit 2; }
	@test -x $(HOST) || { echo "no $(HOST): make host first"; exit 2; }
	@rm -rf $(OUT)/shots && mkdir -p $(OUT)/write
	@status=0; timeout $(TIMEOUT) $(QEMU) $(HOST) $(GAME) $(OUT)/write $(MS) "$(CMD)" \
	   > $(OUT)/out.txt 2> $(OUT)/err.txt || status=$$?; \
	 echo "exit status $$status"; grep -E '^\[rt\]' $(OUT)/err.txt | head -150; \
	 echo "--- stdout:"; tail -40 $(OUT)/out.txt; echo "--- write root:"; find $(OUT)/write -type f | head -40; echo "--- shots:"; ls $(OUT)/shots
endef

run: $(HOST)
	$(RUN_GAME)

# The binary as built (make host), not built again: for runs side by side.
run-built:
	$(RUN_GAME)

# Always delegated: the library build is incremental and knows its own inputs.
$(W)/build-arm/libwinx86.a: FORCE
	$(MAKE) -C $(ROOT)/tests/cpu VARIANT=armv6 OUT=$(ROOT)/build/cpu/armv6 lib   # W's, not $(OUT)'s (a lane's OUT=)
$(W)/build-arm/libwinx86_prof.a: $(W)/build-arm/libwinx86.a FORCE
	@mkdir -p $(OUT)
	cd $(W) && TARGET=armv6 LIBTAG=_prof EXTRA=-DPROF_COUNTERS bash build.sh > $(abspath $(OUT))/lib-prof-build.log 2>&1 \
	  || (grep -E 'error|Error' $(abspath $(OUT))/lib-prof-build.log | head -40; exit 1)

$(AOTDIR)/d/%.o: $(AOT)/%.c $(ROOT)/src/game/aot_rt.h
	@mkdir -p $(@D); $(AOTCC) -c $< -o $@
$(AOTDIR)/n/%.o: $(AOT)/%.c $(ROOT)/src/game/aot_rt.h
	@mkdir -p $(@D); $(AOTCC) -DAOT_NOINLINE -c $< -o $@
$(AOTDIR)/j/%.o: $(AOT)/%.c $(ROOT)/src/game/aot_rt.h
	@mkdir -p $(@D); $(AOTCC) -DAOT_JOURNAL -c $< -o $@

$(HOST): $(SRCS) png.h $(wildcard $(ROOT)/src/runtime/rt_*.h) $(wildcard $(ROOT)/src/game/*.h) \
               $(wildcard $(ROOT)/src/game/bink/*.h) $(LIB) $(AOTOBJ)
	@mkdir -p $(BIN_OUT)
	$(CXX) -std=gnu++17 -O2 -g $(ARCH) $(CPUDEF) $(PROFDEF) -Wall -Wextra -Wno-unused-parameter $(INC) \
	  -static -Wl,-Map=$@.map -o $@ $(SRCS) $(AOTOBJ) $(LIB) -lpthread -lm

clean:
	rm -rf $(OUT)
