# M3 lane (plan §5): a self-authored PE32 (tests/pe) runs through 3ds-d2's
# Win32 runtime (src/runtime/rt_*.cpp + WinVita bridge/shims/scheduler) on the
# ARMv6 Box86 backend, under qemu's ARM11 MPCore model. Run through
# `scripts/docker.sh test m3`. Uses the patched tree prepared by tests/cpu.
ROOT  := ../..
W     := $(ROOT)/build/cpu/armv6/winx86
OUT   := $(ROOT)/build/m3
CXX   := arm-linux-gnueabi-g++
ARCH  := -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp
INC   := -I$(ROOT)/src -I$(W)/src -I$(W)/src/dynarec86 -I$(W)/src/dynarec86/shim \
         -I$(W)/third_party/box86-dynarec/include -I$(W)/third_party/box86-dynarec
SRCS  := m3_main.cpp $(wildcard $(ROOT)/src/runtime/rt_*.cpp)
QEMU  := qemu-arm -cpu arm11mpcore

.PHONY: FORCE run clean
run: $(OUT)/m3_host $(ROOT)/build/pe/m3test.exe
	@rm -rf $(OUT)/root && mkdir -p $(OUT)/root
	@status=0; timeout 600 $(QEMU) $(OUT)/m3_host $(ROOT)/build/pe/m3test.exe $(OUT)/root \
	   > $(OUT)/out.txt 2> $(OUT)/err.txt || status=$$?; \
	 cat $(OUT)/out.txt; grep -E '^\[rt\]' $(OUT)/err.txt | head -20; \
	 if [ $$status -eq 0 ] && grep -q '^RESULT m3 ' $(OUT)/out.txt; then echo "RESULT m3.lane PASS"; \
	 else echo "RESULT m3.lane FAIL (exit $$status)"; exit 1; fi

$(ROOT)/build/pe/m3test.exe: $(ROOT)/tests/pe/m3test.c
	$(MAKE) -C $(ROOT)/tests/pe

# Always delegated: the library build is incremental and knows its own inputs.
$(W)/build-arm/libwinx86.a: FORCE
	$(MAKE) -C $(ROOT)/tests/cpu VARIANT=armv6 lib

$(OUT)/m3_host: $(SRCS) $(wildcard $(ROOT)/src/runtime/rt_*.h) $(W)/build-arm/libwinx86.a
	@mkdir -p $(OUT)
	$(CXX) -std=gnu++17 -O2 -g $(ARCH) -DD2RT_CPU_BOX86 -Wall -Wextra -Wno-unused-parameter $(INC) \
	  -static -o $@ $(SRCS) $(W)/build-arm/libwinx86.a -lpthread -lm

clean:
	rm -rf $(OUT)
