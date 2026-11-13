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

.PHONY: run prepare export FORCE
FORCE:

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
