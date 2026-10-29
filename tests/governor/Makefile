# Governor lane: src/game/sprite_governor.cpp (the sprite cache's limit,
# issue #18) under qemu's ARM11 MPCore model. Run through
# `scripts/docker.sh test governor`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/governor
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/governor-tests
	$(QEMU) $(OUT)/governor-tests

$(OUT)/governor-tests: governor_test.cpp $(ROOT)/src/game/sprite_governor.cpp $(ROOT)/src/game/sprite_governor.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ governor_test.cpp $(ROOT)/src/game/sprite_governor.cpp -static

clean:
	rm -rf $(OUT)
