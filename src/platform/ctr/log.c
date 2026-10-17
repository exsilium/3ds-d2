// plat_log for the 3DS: every line goes to the console and to the SD log. The
// file is flushed per line so a process killed by the kernel (for example on an
// unpermitted SVC) still leaves the last CHECKPOINT on the card.
#include "platform/ctr/log.h"
#include "runtime/jitmem.h"

#include <3ds.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static FILE *g_file;
static char g_path[128];
static char g_pending[8192];
static size_t g_pending_len;

const char *log_path(void) { return g_path; }

bool log_open(const char *name)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(D2_SD_ROOT, 0777);
    mkdir(D2_SD_ROOT "/logs", 0777);
    snprintf(g_path, sizeof g_path, D2_SD_ROOT "/logs/%s.log", name);
    g_file = fopen(g_path, "w");
    if (!g_file)
        return false;
    fwrite(g_pending, 1, g_pending_len, g_file);
    fflush(g_file);
    g_pending_len = 0;
    return true;
}

void log_close(void)
{
    if (g_file)
        fclose(g_file);
    g_file = 0;
}

void plat_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;

    fputs(line, stdout);
    if (g_file) {
        fwrite(line, 1, (size_t)n, g_file);
        fflush(g_file);
    } else if (g_pending_len + (size_t)n < sizeof g_pending) {
        memcpy(g_pending + g_pending_len, line, (size_t)n);
        g_pending_len += (size_t)n;
    }
}
