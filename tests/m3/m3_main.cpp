// M3 lane host (Linux, qemu-arm): runs a PE32 through 3ds-d2's Win32 runtime
// on the ARMv6 Box86 backend and passes the guest's output through.
//
//   m3_host <program.exe> [files-root]
//
// Exit status: the guest's exit code (the M3 fixture exits with its failure count).
#include "runtime/rt_host.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <utility>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <program.exe> [files-root]\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> exe;
    if (FILE *f = std::fopen(argv[1], "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            exe.insert(exe.end(), buf, buf + n);
        std::fclose(f);
    }
    if (exe.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    rt::RunOptions opt;
    opt.data_root = argc > 2 ? argv[2] : ".";
    opt.out = [](const char *s, size_t n) { std::fwrite(s, 1, n, stdout); std::fflush(stdout); };
    opt.log = [](const char *s) { std::fprintf(stderr, "[rt] %s\n", s); };
    if (std::getenv("M3_REAL_CLOCK")) {   // the console's mode: real time, idling by sleeping
        opt.real_clock = true;
        opt.sleep_ms = [](uint32_t ms) { usleep(ms * 1000); };
    }
    rt::RunResult r = rt::run_exe(std::move(exe), opt);
    if (!r.loaded) {
        std::fprintf(stderr, "[rt] FATAL %s\n", r.error.c_str());
        return 3;
    }
    std::fprintf(stderr, "[rt] exited=%d code=%u\n", r.exited, r.exit_code);
    std::fflush(stdout);
    std::_Exit(r.exited ? (int)r.exit_code : 4);
}
