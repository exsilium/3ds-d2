// 3ds-d2 bootstrap application (plan increment 1): M0 dual-target hello world
// and the M1 executable-memory and mapping probes.
//
// On launch it identifies the build and target, runs every probe, and logs to
// sdmc:/3ds/3ds-d2/logs/<target>-<build>-<flavor>.log. It then echoes input on
// the bottom screen; START records the input result and exits.
#include "platform/ctr/log.h"
#include "platform/ctr/probes.h"
#include "platform/ctr/target.h"
#include "probe/jitprobe.h"
#include "runtime/jitmem.h"

#include <3ds.h>
#include <stdio.h>

int main(void)
{
    gfxInitDefault();
    PrintConsole top, bottom;
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);
    consoleSelect(&top);

    target_info ti;
    target_detect(&ti);

    char logname[96];
    snprintf(logname, sizeof logname, "%s-%s-%s", ti.name, D2_BUILD_ID, D2_FLAVOR);
    bool logging = log_open(logname);

    plat_log("3ds-d2 build_id=%s flavor=%s\n", D2_BUILD_ID, D2_FLAVOR);
    plat_log("TARGET name=%s emulator=%d new3ds=%d luma=%d luma_version=%08lx speedup=%d emu_build=\"%s\" "
             "emu_commit=\"%s\"\n",
             ti.name, ti.is_emulator, ti.is_new3ds, ti.luma, (unsigned long)ti.luma_version, ti.speedup,
             ti.emu_build, ti.emu_commit);
    plat_log("RESULT m0.log_file %s path=%s\n", logging ? "PASS" : "FAIL", logging ? log_path() : "-");

    probe_tally t = {logging ? 1u : 0u, logging ? 0u : 1u, 0};
    sdfixture_run(&t);
    memprobe_run(&t);

    jitprobe_config cfg = {.pool_size = 256 * 1024, .stress_rounds = 8, .yield_count = 16};
    jitprobe_result jr;
    jitprobe_run(&cfg, &jr);
    t.passed += jr.passed;
    t.failed += jr.failed;
    t.info += jr.info;

    mapprobe_run(&t);

    plat_log("RESULT probe.summary %s passed=%u failed=%u info=%u\n", t.failed ? "FAIL" : "PASS", t.passed,
             t.failed, t.info);
    plat_log("Press buttons / touch to test input. START exits.\n");

    consoleSelect(&bottom);
    printf("\x1b[1;1H3ds-d2 %s (%s)\n%s\n", D2_BUILD_ID, D2_FLAVOR, ti.name);
    u32 seen = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld();
        seen |= down;
        touchPosition tp;
        hidTouchRead(&tp);
        printf("\x1b[5;1Hheld=%08lx seen=%08lx\x1b[K\n", (unsigned long)held, (unsigned long)seen);
        printf("\x1b[6;1Htouch=%3u,%3u\x1b[K\n", tp.px, tp.py);
        if (down & KEY_START)
            break;
        gspWaitForVBlank();
    }

    consoleSelect(&top);
    // START itself counts, so any non-START key proves general input delivery.
    plat_log("RESULT m0.input %s keys_seen=%08lx\n", (seen & ~KEY_START) ? "PASS" : "INFO",
             (unsigned long)seen);
    log_close();
    gfxExit();
    return 0;
}
