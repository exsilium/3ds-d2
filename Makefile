#---------------------------------------------------------------------------------
# 3ds-d2 application build. Adapted from the devkitPro 3DS application template.
# Invoke through scripts/docker.sh; it sets FLAVOR and BUILD_ID.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM is not set; build through scripts/docker.sh")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

FLAVOR		?=	debug
BUILD_ID	?=	unversioned

TARGET		:=	3ds-d2
BUILD		:=	build/$(FLAVOR)/obj
OUTDIR		:=	build/$(FLAVOR)
SOURCES		:=	src src/platform/ctr src/probe src/runtime
DATA		:=
INCLUDES	:=	src

APP_TITLE	:=	3ds-d2
APP_DESCRIPTION	:=	D2 runtime feasibility probe
APP_AUTHOR	:=	3ds-d2

#---------------------------------------------------------------------------------
# Code generation. -ffile-prefix-map and the absence of __DATE__/__TIME__ keep
# two clean builds byte-identical.
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm

ifeq ($(FLAVOR),release)
OPT	:=	-O2
else ifeq ($(FLAVOR),debug)
OPT	:=	-Og -DD2_DEBUG=1
else
$(error FLAVOR must be debug or release)
endif

CFLAGS	:=	-g $(OPT) -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
			-mword-relocations -ffunction-sections \
			-ffile-prefix-map=$(TOPDIR)=. \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DD2_BUILD_ID=\"$(BUILD_ID)\" -DD2_FLAVOR=\"$(FLAVOR)\"

ASFLAGS	:=	-g $(ARCH) -ffile-prefix-map=$(TOPDIR)=.
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(OUTPUT).map -Wl,--build-id=none

LIBS	:= -lctru -lm
LIBDIRS	:= $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(notdir $(BUILD)),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(OUTDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export FLAVOR BUILD_ID

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:=	$(CC)
export OFILES	:=	$(sort $(CFILES:.c=.o) $(SFILES:.s=.o))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON		:=	$(CTRULIB)/default_icon.png
export _3DSXDEPS	:=	$(OUTPUT).smdh
export _3DSXFLAGS	+=	--smdh=$(OUTPUT).smdh

.PHONY: all clean print-flags

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -fr build/$(FLAVOR)

print-flags:
	@echo $(filter-out -I%,$(CFLAGS)) $(LDFLAGS)

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf	:	$(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
