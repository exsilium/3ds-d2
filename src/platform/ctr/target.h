#ifndef D2_PLATFORM_CTR_TARGET_H
#define D2_PLATFORM_CTR_TARGET_H

#include <3ds.h>
#include <stdbool.h>

typedef struct target_info {
    bool is_emulator;     // svcGetSystemInfo(0x20000, 0) != 0 (1 Citra, 2 Azahar)
    bool is_new3ds;
    bool luma;            // svcGetSystemInfo(0x10000, 0) succeeded on hardware
    u32 luma_version;     // raw value: major<<24 | minor<<16 | build<<8
    s64 real_app_region;  // Luma/Azahar REAL_APP_REGION_SIZE, or -1
    bool speedup;         // New 3DS 804 MHz + L2 requested
    char emu_build[32];   // emulator build version, if any
    char emu_commit[16];  // emulator git description, if any
    const char *name;     // "azahar", "citra", "n3ds-hw", "o3ds-hw"
} target_info;

void target_detect(target_info *t);
const target_info *target_get(void);

// Luma3DS custom SVCs (also implemented by Azahar). Never call 0x91/0x92 in
// Azahar: they are unimplemented there and return with r0 unchanged.
Result svcLumaInvalidateInstructionCacheRange(u32 addr, u32 size);
Result svcLumaInvalidateEntireInstructionCache(void);

// An unimplemented SVC returns with r0 unchanged, which can read as success, so
// memory-changing calls are confirmed with svcQueryMemory before use.
// True when every page of [addr, addr+size) has at least `perm` and is not free.
bool memory_has_perm(u32 addr, u32 size, u32 perm);

#endif
