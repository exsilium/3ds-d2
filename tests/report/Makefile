# Report lane: src/game/report.cpp (game.log summary, Sentry envelope) under
# qemu's ARM11 MPCore model. Run through `scripts/docker.sh test report`.
CXX     := arm-linux-gnueabi-g++
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/report
CXXFLAGS := -std=gnu++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -march=armv6k -marm -I$(ROOT)/src

.PHONY: run clean
run: $(OUT)/report-tests
	$(QEMU) $(OUT)/report-tests

$(OUT)/report-tests: report_test.cpp $(ROOT)/src/game/report.cpp $(ROOT)/src/game/report.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ report_test.cpp $(ROOT)/src/game/report.cpp -static

clean:
	rm -rf $(OUT)
