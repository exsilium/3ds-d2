/* Symbols the CPU-only WinVita build references from layers the lane does not
 * link (the Vita console log, the bridge, the schedulers). Each is a no-op or a
 * neutral value; the link step names anything new. */
#include <stdint.h>
#include <stdio.h>

/* The backend's console progress lines (arena setup, fatal errors) go to
 * stderr, which the 3DS apps redirect to a file on the SD card. */
__attribute__((weak)) void d2_ctr_progress_hook(const char *msg) { (void)msg; }   /* an app may show it */
void wx86_vita_progress_c(const char *msg)
{
    fprintf(stderr, "[wx86] %s\n", msg);
    fflush(stderr);
    d2_ctr_progress_hook(msg);
}
void wx86_vita_progress_flush_c(void) { fflush(stderr); }

/* Defined by sched_native.cpp, which the 3DS build leaves out (cooperative
 * scheduler only); the bridge reads and arms it. */
int d2rt_wakeprof = 0;
