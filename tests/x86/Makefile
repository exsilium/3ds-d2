# x86 reference lane; run through `scripts/docker.sh test x86`.
#
#   run     build, execute on qemu-i386, compare with the committed golden file
#   golden  regenerate golden/qemu-i386.txt (only after a deliberate fixture change)
#
# Also exports the fixture bytes for other executors:
#   build/x86/fixtures.bin  raw .fixtures section, to load at X86_FIXTURE_BASE
#   build/x86/fixtures.sym  fixture entry addresses
CC      := i686-linux-gnu-gcc
OBJCOPY := i686-linux-gnu-objcopy
NM      := i686-linux-gnu-nm
QEMU    := qemu-i386 -cpu pentium3
ROOT    := ../..
OUT     := $(ROOT)/build/x86
CFLAGS  := -std=gnu11 -O1 -g -Wall -Wextra -Werror -m32 -march=pentium3 -fno-pie
LDFLAGS := -static -no-pie -Wl,--section-start=.fixtures=0x00400000

SRCS := harness.c trampoline.S fixtures.S

.PHONY: all run golden clean
all: $(OUT)/x86-ref

$(OUT)/x86-ref: $(SRCS) abi.h cases.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	$(OBJCOPY) -O binary --only-section=.fixtures $@ $(OUT)/fixtures.bin
	$(NM) $@ | grep ' fx_' | sort > $(OUT)/fixtures.sym

$(OUT)/results.txt: $(OUT)/x86-ref
	$(QEMU) $< > $@

run: $(OUT)/results.txt
	@grep -c '^STATE' $< | sed 's/^/cases: /'
	@grep -E '^(LANE|SKIP|DONE)' $< || true
	@if diff -q golden/qemu-i386.txt $< >/dev/null; then \
	  echo "RESULT x86.golden PASS (identical to golden/qemu-i386.txt)"; \
	else \
	  echo "RESULT x86.golden FAIL"; diff golden/qemu-i386.txt $< | head -20; exit 1; \
	fi

golden: $(OUT)/results.txt
	@mkdir -p golden
	cp $< golden/qemu-i386.txt
	@echo "wrote golden/qemu-i386.txt"

clean:
	rm -rf $(OUT)
