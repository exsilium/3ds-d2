# Pool lane: src/runtime/pool_fit.h (where a shared pool's VirtualAlloc face
# can place a block without its spare, issue #70) against the backend's
# GuestRegion, under qemu's ARM11 MPCore model. Needs the prepared backend
# tree (build/cpu/armv6/winx86: any build of the game prepares it). Run
# through `scripts/docker.sh test pool`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/pool
BACKEND := $(ROOT)/build/cpu/armv6/winx86/src
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -march=armv6k -marm -I$(ROOT)/src -I$(BACKEND)

.PHONY: run clean
run: $(OUT)/pool-tests
	$(QEMU) $(OUT)/pool-tests

$(OUT)/pool-tests: pool_test.cpp $(ROOT)/src/runtime/pool_fit.h $(BACKEND)/runtime/guest_region.h $(BACKEND)/runtime/guest_region.cpp
	@test -f $(BACKEND)/runtime/guest_region.h || { echo "no backend tree: build the game first"; exit 1; }
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ pool_test.cpp $(BACKEND)/runtime/guest_region.cpp -static

clean:
	rm -rf $(OUT)
