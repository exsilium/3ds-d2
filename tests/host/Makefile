# Host-test lane; run through `scripts/docker.sh test host`.
# The binary is built for ARMv6K and executed on qemu's ARM11 MPCore model, so
# any ARMv7-only instruction in shared code faults instead of passing silently.
CC      := arm-linux-gnueabi-gcc
QEMU    := qemu-arm -cpu arm11mpcore
ROOT    := ../..
OUT     := $(ROOT)/build/host
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter \
           -march=armv6k -marm -mfloat-abi=soft -I$(ROOT)/src
LDFLAGS := -static -pthread

SRCS := test_main.c plat_linux.c ref_insns.S $(ROOT)/src/probe/jitprobe.c

.PHONY: all run clean
all: $(OUT)/host-tests

$(OUT)/host-tests: $(SRCS) $(wildcard $(ROOT)/src/runtime/*.h) $(ROOT)/src/probe/jitprobe.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

run: $(OUT)/host-tests
	@status=0; $(QEMU) $(OUT)/host-tests > $(OUT)/host-tests.log 2>&1 || status=$$?; \
	cat $(OUT)/host-tests.log; \
	grep '^RESULT host.summary' $(OUT)/host-tests.log > $(ROOT)/build/host-test-summary.txt \
	  || echo "RESULT host.summary FAIL crashed exit=$$status" > $(ROOT)/build/host-test-summary.txt; \
	exit $$status

clean:
	rm -rf $(OUT)
