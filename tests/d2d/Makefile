# d2d (psi29a/d2decomped, GPL-3.0-or-later) for tests and tools only; run
# through `scripts/docker.sh test d2d`. Issue #106, part of #105.
#
# Like the Box86 backend (tests/cpu/Makefile), d2d is used from a work tree,
# build/d2d/d2d: a pristine copy of the third_party/d2d submodule as a git
# work tree (commit and tag "pristine"), then this project's patch series
# (patches/*.patch) applied as one commit per patch. Edit there, commit, and
# `make -C tests/d2d export` writes the series back. tests/cpu/prepare.sh
# prepares it again whenever the submodule's commit or the series changes.
#
# Nothing here is linked into a 3DS build. d2d's files stay in its submodule
# and in this work tree; they are read from there, never copied into ours.
ROOT    := ../..
OUT     := $(ROOT)/build/d2d
SRC     := $(OUT)/d2d
SUBMOD  := $(ROOT)/third_party/d2d

.PHONY: run prepare export cases FORCE
FORCE:

# d2d's emulator-made cases against our natives (issue #108): needs the
# user's Game.exe (GAME, mounted read-only by scripts/docker.sh test d2d
# <game-dir>). Built as the m4 lane builds the game (ARMv6K, run on qemu's
# ARM11 model), the test file including d2_native.cpp in place of its own
# object, and d2d's case files read from the work tree.
W       := $(ROOT)/build/cpu/armv6/winx86
CXX     := arm-linux-gnueabi-g++
ARCH    := -marm -march=armv6k -mtune=mpcore -mfpu=vfp -mfloat-abi=softfp
INC     := -I$(ROOT)/src -I$(W)/src -I$(W)/src/dynarec86 -I$(W)/src/dynarec86/shim \
           -I$(W)/third_party/box86-dynarec/include -I$(W)/third_party/box86-dynarec -I$(SRC)/tests
QEMU    := qemu-arm -cpu arm11mpcore
GAME    ?= /game
NATIVE_SRCS := $(filter-out $(ROOT)/src/game/d2_native.cpp,$(wildcard $(ROOT)/src/runtime/rt_*.cpp) \
               $(wildcard $(ROOT)/src/game/*.cpp) $(wildcard $(ROOT)/src/game/bink/*.cpp))
LIB     := $(W)/build-arm/libwinx86.a

$(LIB): FORCE
	$(MAKE) -C ../cpu VARIANT=armv6 lib

$(OUT)/walk_back: walk_back.cpp $(SRC)/.prepared $(NATIVE_SRCS) $(wildcard $(ROOT)/src/game/*.h) \
                  $(wildcard $(ROOT)/src/runtime/rt_*.h) $(LIB)
	$(CXX) -std=gnu++17 -O2 -g $(ARCH) -DD2RT_CPU_BOX86 -Wall -Wextra -Wno-unused-parameter -Wno-psabi $(INC) \
	  -static -o $@ walk_back.cpp $(NATIVE_SRCS) $(LIB) -lpthread -lm

cases: $(OUT)/walk_back
	@test -f $(GAME)/Game.exe || { echo "no Game.exe in $(GAME)"; exit 2; }
	@$(QEMU) $(OUT)/walk_back $(GAME)/Game.exe

run: $(SRC)/.prepared
	@echo "d2d $$(cat $(OUT)/prepare.id) in build/d2d/d2d ($$(ls patches/*.patch 2>/dev/null | wc -l) patches)"
	@python3 -I -c "import unicorn, capstone, pefile; from importlib.metadata import version as v; print('tools/emu: unicorn', v('unicorn'), '| capstone', v('capstone'), '| pefile', v('pefile'))"
	@echo "RESULT d2d.prepare PASS"

prepare: $(SRC)/.prepared

GIT := git -c safe.directory="*" -c user.name=3ds-d2 -c user.email=3ds-d2@localhost -c commit.gpgsign=false
# Without this check, an uninitialised submodule's empty folder would make git
# answer for the 3ds-d2 repository itself.
CHECK_SUBMODULE = @[ -e $(SUBMOD)/.git ] || { echo "third_party/d2d is not checked out: git submodule update --init third_party/d2d" >&2; exit 1; }
PREPARE_ID = $(shell $(GIT) -C $(SUBMOD) rev-parse HEAD 2>/dev/null)-$(shell \
  cd patches 2>/dev/null && sha256sum *.patch 2>/dev/null | sha256sum | cut -c1-16)
$(OUT)/prepare.id: FORCE
	$(CHECK_SUBMODULE)
	@mkdir -p $(OUT); echo '$(PREPARE_ID)' | cmp -s - $@ || echo '$(PREPARE_ID)' > $@
$(SRC)/.prepared: $(OUT)/prepare.id
	@bash ../cpu/prepare.sh $(SRC) patches $(SUBMOD) '$(PREPARE_ID)' 'make -C tests/d2d export'

export:
	@rm -f patches/*.patch
	@cd $(SRC) && $(GIT) format-patch -q --no-signature --zero-commit -N -o $(abspath patches) pristine..HEAD
	@ls patches
