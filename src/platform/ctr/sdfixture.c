// M0 gate: write a small deterministic SD file, read it back and compare. The
// previous run's build ID is reported first, which shows persistence across
// fresh launches (emulator save states are not a substitute).
#include "platform/ctr/log.h"
#include "platform/ctr/probes.h"
#include "runtime/jitmem.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_PATH D2_SD_ROOT "/fixture.bin"
#define FIXTURE_WORDS 1024
#define FIXTURE_MAGIC 0x32443344u // "D3D2"

typedef struct fixture_header {
    uint32_t magic;
    uint32_t words;
    char build_id[24];
} fixture_header;

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void fill(uint32_t *words, const char *build_id)
{
    uint32_t s = 2166136261u; // FNV-1a of the build ID seeds the pattern
    for (const char *c = build_id; *c; c++)
        s = (s ^ (uint8_t)*c) * 16777619u;
    if (!s)
        s = 1;
    for (unsigned i = 0; i < FIXTURE_WORDS; i++)
        words[i] = xorshift(&s);
}

void sdfixture_run(probe_tally *t)
{
    static uint32_t want[FIXTURE_WORDS], got[FIXTURE_WORDS];
    fixture_header h = {FIXTURE_MAGIC, FIXTURE_WORDS, {0}};
    strncpy(h.build_id, D2_BUILD_ID, sizeof h.build_id - 1);

    FILE *f = fopen(FIXTURE_PATH, "rb");
    fixture_header prev;
    if (f && fread(&prev, sizeof prev, 1, f) == 1 && prev.magic == FIXTURE_MAGIC) {
        prev.build_id[sizeof prev.build_id - 1] = '\0';
        plat_log("SD previous_fixture build_id=%s\n", prev.build_id);
    } else {
        plat_log("SD previous_fixture none\n");
    }
    if (f)
        fclose(f);

    fill(want, h.build_id);
    f = fopen(FIXTURE_PATH, "wb");
    bool wrote = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(want, sizeof want, 1, f) == 1;
    if (f)
        wrote = (fclose(f) == 0) && wrote;

    fixture_header back;
    f = fopen(FIXTURE_PATH, "rb");
    bool read = f && fread(&back, sizeof back, 1, f) == 1 && fread(got, sizeof got, 1, f) == 1;
    if (f)
        fclose(f);

    bool ok = wrote && read && memcmp(&back, &h, sizeof h) == 0 && memcmp(got, want, sizeof want) == 0;
    plat_log("RESULT m0.sd_fixture %s path=%s bytes=%zu wrote=%d read=%d\n", ok ? "PASS" : "FAIL", FIXTURE_PATH,
             sizeof h + sizeof want, wrote, read);
    if (ok)
        t->passed++;
    else
        t->failed++;
}
