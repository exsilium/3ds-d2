# Game.exe lane: src/game/game_exe.cpp (1.14d's Game.exe told from the others,
# issue #77) under qemu's ARM11 MPCore model. Run through
# `scripts/docker.sh test gameexe`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/gameexe
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/gameexe-tests
	$(QEMU) $(OUT)/gameexe-tests

$(OUT)/gameexe-tests: gameexe_test.cpp $(ROOT)/src/game/game_exe.cpp $(ROOT)/src/game/game_exe.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ gameexe_test.cpp $(ROOT)/src/game/game_exe.cpp -static

clean:
	rm -rf $(OUT)
