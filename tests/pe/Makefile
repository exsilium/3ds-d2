# Self-authored PE32 fixtures for the Win32 runtime (plan §5 M3), built with
# mingw-w64 in the host-test image. No C runtime: freestanding code against
# kernel32/user32 import libraries only.
CC      := i686-w64-mingw32-gcc
ROOT    := ../..
OUT     := $(ROOT)/build/pe
CFLAGS  := -O1 -g0 -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror \
           -Wno-cast-function-type -march=pentium-mmx
LDFLAGS := -nostdlib -Wl,--entry,_start@0 -Wl,--subsystem,windows -Wl,--image-base,0x400000 \
           -Wl,--dynamicbase -Wl,--no-insert-timestamp

.PHONY: all clean
all: $(OUT)/m3test.exe

$(OUT)/m3test.exe: m3test.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lkernel32 -luser32
	@i686-w64-mingw32-objdump -p $@ | grep -E 'DLL Name|^\s+[0-9a-f]+\s+[0-9]+\s+[A-Za-z]' | head -60

clean:
	rm -rf $(OUT)
