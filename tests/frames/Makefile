# Frames lane: src/game/sprite_frames.cpp (the allocator of the sprite cache's frames,
# issue #44) under qemu's ARM11 MPCore model. Run through
# `scripts/docker.sh test frames`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/frames
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/frames-tests
	$(QEMU) $(OUT)/frames-tests

$(OUT)/frames-tests: frames_test.cpp $(ROOT)/src/game/sprite_frames.cpp $(ROOT)/src/game/sprite_frames.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ frames_test.cpp $(ROOT)/src/game/sprite_frames.cpp -static

clean:
	rm -rf $(OUT)
