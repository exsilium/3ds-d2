# Differential CPU lane; run through `scripts/docker.sh test cpu [armv7|armv6]`.
#
# Builds WinVita's Box86 backend from third_party/winx86 (a copy under
# build/cpu/<variant>, with that variant's patches applied), links
# cpu_runner.cpp against it, runs the x86 lane's fixtures on qemu-arm and
# compares the output with tests/x86/golden/qemu-i386.txt.
#
#   armv7  upstream as-is (ARMv7 + NEON), on qemu's Cortex-A9 = the Vita's CPU.
#          Baseline: any divergence here is Box86's, not the port's.
#   armv6  the 3DS port (ARMv6K, VFPv2), on qemu's ARM11 MPCore = the 3DS CPU.
#          Every ARMv7/NEON instruction the generator still emits faults there.
VARIANT ?= armv7
ROOT    := ../..
OUT     := $(ROOT)/build/cpu/$(VARIANT)
SRC     := $(OUT)/winx86
X86     := $(ROOT)/build/x86
GOLDEN  := $(ROOT)/tests/x86/golden/qemu-i386.txt

ifeq ($(VARIANT),armv7)
CXX     := arm-linux-gnueabihf-g++
ARCH    := -marm -march=armv7-a+simd -mfpu=neon -mfloat-abi=hard
QEMU    := qemu-arm -cpu cortex-a9
else ifeq ($(VARIANT),armv6)
# Soft-float ABI with VFP instructions: Debian's hard-float libraries are ARMv7
# code and would fault on the ARM11 model. The port's generated code bridges
# doubles to core registers around C calls when __ARM_PCS_VFP is undefined.
CXX     := arm-linux-gnueabi-g++
ARCH    := -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp
QEMU    := qemu-arm -cpu arm11mpcore
LIBENV  := TARGET=armv6
else
$(error VARIANT must be armv7 or armv6)
endif

WX_INC := -I$(SRC)/src -I$(SRC)/src/dynarec86 -I$(SRC)/src/dynarec86/shim \
          -I$(SRC)/third_party/box86-dynarec/include -I$(SRC)/third_party/box86-dynarec

.PHONY: run lib clean FORCE
FORCE:
# Two checks. Against the golden file: the backend's x86 fidelity (Box86's own
# known gaps are recorded in baseline/armv7-vs-golden.txt). For the port, the
# acceptance criterion: output byte-identical to upstream on ARMv7.
BASELINE := baseline/armv7-results.txt
run: $(OUT)/results.txt
	@grep -E '^(DONE|FAULT)' $< | head -20
	@python3 -I compare.py $(GOLDEN) $< | tee $(OUT)/compare.txt | grep -v '^ok'
	@if [ "$(VARIANT)" != armv7 ]; then \
	  if cmp -s $(BASELINE) $<; then echo "RESULT cpu.$(VARIANT)-vs-armv7 PASS (byte-identical to the upstream baseline)"; \
	  else echo "RESULT cpu.$(VARIANT)-vs-armv7 FAIL"; diff $(BASELINE) $< | head -20; exit 1; fi; \
	fi

lib: $(SRC)/build-arm/libwinx86.a

# Pristine copy of the pinned submodule as a git work tree (commit "pristine"),
# then the variant's patch series applied as one commit per patch. Edit there,
# commit, and `make export` writes the series back to patches/<variant>/.
GIT := git -c safe.directory="*" -c user.name=3ds-d2 -c user.email=3ds-d2@localhost -c commit.gpgsign=false
$(SRC)/.prepared:
	@rm -rf $(SRC) && mkdir -p $(SRC)
	@cd $(ROOT)/third_party/winx86 && $(GIT) archive HEAD | tar -x -C $(abspath $(SRC))
	@cd $(SRC) && $(GIT) init -q && $(GIT) add -A && $(GIT) commit -q -m pristine && $(GIT) tag pristine
	@for p in $(sort $(wildcard $(abspath patches/$(VARIANT))/*.patch)); do \
	  echo "apply $$(basename $$p)"; (cd $(SRC) && $(GIT) am -q --keep-cr $$p) || exit 1; done
	@touch $@

.PHONY: export
export:
	@rm -f patches/$(VARIANT)/*.patch && mkdir -p patches/$(VARIANT)
	@cd $(SRC) && $(GIT) format-patch -q --no-signature --zero-commit -N -o $(abspath patches/$(VARIANT)) pristine..HEAD
	@ls patches/$(VARIANT)

# build.sh is incremental itself (command fingerprints + gcc .d files), so it
# always runs; the archive only changes when an input did.
$(SRC)/build-arm/libwinx86.a: $(SRC)/.prepared FORCE
	cd $(SRC) && $(LIBENV) bash build.sh > $(abspath $(OUT))/lib-build.log 2>&1 || (grep -E 'error|Error' $(abspath $(OUT))/lib-build.log | head -40; exit 1)

$(OUT)/cpu_runner: cpu_runner.cpp $(ROOT)/tests/x86/cases.h $(SRC)/build-arm/libwinx86.a
	$(CXX) -std=gnu++17 -static -O2 -g $(ARCH) -DD2RT_CPU_BOX86 $(WX_INC) \
	  -o $@ cpu_runner.cpp $(SRC)/build-arm/libwinx86.a -lpthread -lm

$(X86)/fixtures.bin:
	$(MAKE) -C $(ROOT)/tests/x86 all

$(OUT)/results.txt: $(OUT)/cpu_runner $(X86)/fixtures.bin
	timeout 1200 $(QEMU) $< $(X86)/fixtures.bin $(X86)/fixtures.sym > $@ 2> $(OUT)/runner.stderr || \
	  (echo "runner exit $$?"; tail -5 $(OUT)/runner.stderr; true)

clean:
	rm -rf $(OUT)
