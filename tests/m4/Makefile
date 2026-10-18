# M4 game lane: Diablo II 1.14d's Game.exe through 3ds-d2's own Win32 runtime
# (src/runtime/rt_*.cpp + WinVita bridge/shims/scheduler) on the ARMv6 Box86
# backend, under qemu's ARM11 MPCore model, with the 3DS's settings. Run through
# `scripts/docker.sh test m4 <game-dir> [guest-ms] [command line]`; the game
# directory is mounted read-only at /game and never copied.
ROOT  := ../..
W     := $(ROOT)/build/cpu/armv6/winx86
OUT   := $(ROOT)/build/m4
CXX   := arm-linux-gnueabi-g++
ARCH  := -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp
INC   := -I$(ROOT)/src -I$(W)/src -I$(W)/src/dynarec86 -I$(W)/src/dynarec86/shim \
         -I$(W)/third_party/box86-dynarec/include -I$(W)/third_party/box86-dynarec
SRCS  := game_main.cpp $(wildcard $(ROOT)/src/runtime/rt_*.cpp) $(wildcard $(ROOT)/src/game/*.cpp) \
         $(wildcard $(ROOT)/src/game/bink/*.cpp)
# M4_QEMU_FLAGS=-one-insn-per-tb makes qemu slow enough that the game is
# CPU-bound, as on the 3DS: frames per second then compare the work per frame.
QEMU  := qemu-arm -cpu arm11mpcore $(M4_QEMU_FLAGS)
GAME  ?= /game
MS    ?= 20000
CMD   ?= game.exe -w

.PHONY: FORCE run host clean
host: $(OUT)/m4_host

run: $(OUT)/m4_host
	@test -f $(GAME)/Game.exe || { echo "no Game.exe in $(GAME)"; exit 2; }
	@rm -rf $(OUT)/shots && mkdir -p $(OUT)/write
	@status=0; timeout 1800 $(QEMU) $(OUT)/m4_host $(GAME) $(OUT)/write $(MS) "$(CMD)" \
	   > $(OUT)/out.txt 2> $(OUT)/err.txt || status=$$?; \
	 echo "exit status $$status"; grep -E '^\[rt\]' $(OUT)/err.txt | head -150; \
	 echo "--- stdout:"; tail -40 $(OUT)/out.txt; echo "--- write root:"; find $(OUT)/write -type f | head -40; echo "--- shots:"; ls $(OUT)/shots

# Always delegated: the library build is incremental and knows its own inputs.
$(W)/build-arm/libwinx86.a: FORCE
	$(MAKE) -C $(ROOT)/tests/cpu VARIANT=armv6 lib

$(OUT)/m4_host: $(SRCS) png.h $(wildcard $(ROOT)/src/runtime/rt_*.h) $(wildcard $(ROOT)/src/game/*.h) \
               $(wildcard $(ROOT)/src/game/bink/*.h) $(W)/build-arm/libwinx86.a
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -g $(ARCH) -DD2RT_CPU_BOX86 -Wall -Wextra -Wno-unused-parameter $(INC) \
	  -static -o $@ $(SRCS) $(W)/build-arm/libwinx86.a -lpthread -lm

clean:
	rm -rf $(OUT)
