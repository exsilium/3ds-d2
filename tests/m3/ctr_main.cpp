// M3 lane on the 3DS (Azahar): runs the linked-in PE32 fixture (tests/pe)
// through 3ds-d2's Win32 runtime on the ARMv6 Box86 backend, in real-time
// clock mode as the game will run. Guest output goes to the top screen and to
// sdmc:/3ds/3ds-d2/m3/out.txt; guest files live in that same directory.
#include "runtime/rt_host.h"

#include <3ds.h>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

extern "C" {
extern const uint8_t m3test_exe[], m3test_exe_end[];
}

#define M3_DIR "sdmc:/3ds/3ds-d2/m3"

int main()
{
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/3ds-d2", 0777);
    mkdir(M3_DIR, 0777);
    static FILE *out = std::fopen(M3_DIR "/out.txt", "w");
    static FILE *log = std::fopen(M3_DIR "/runtime.log", "w");
    std::freopen(M3_DIR "/stderr.txt", "w", stderr);
    std::printf("3ds-d2 M3: PE32 through the Win32 runtime\n");

    rt::RunOptions opt;
    opt.data_root = M3_DIR;
    opt.out = [](const char *s, size_t n) {
        std::fwrite(s, 1, n, stdout);
        if (out) { std::fwrite(s, 1, n, out); std::fflush(out); }
    };
    opt.log = [](const char *s) {
        if (log) { std::fprintf(log, "[rt] %s\n", s); std::fflush(log); }
    };
    opt.real_clock = true;
    opt.sleep_ms = [](uint32_t ms) { svcSleepThread((s64)ms * 1000000); };

    std::vector<uint8_t> exe(m3test_exe, m3test_exe_end);
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    char line[128];
    if (!r.loaded)
        std::snprintf(line, sizeof line, "M3 FATAL %s\n", r.error.c_str());
    else
        std::snprintf(line, sizeof line, "M3 DONE exited=%d code=%u\n", r.exited, r.exit_code);
    std::printf("%s", line);
    if (out) { std::fputs(line, out); std::fclose(out); }
    if (log) std::fclose(log);
    for (int i = 0; i < 120; i++)
        gspWaitForVBlank();
    gfxExit();
    return 0;
}
