#ifndef D2_PLATFORM_CTR_LOG_H
#define D2_PLATFORM_CTR_LOG_H

#include <stdbool.h>

#define D2_SD_ROOT "sdmc:/3ds/3ds-d2"

// Opens D2_SD_ROOT/logs/<name>.log for writing (truncating). Lines logged
// before this call are buffered and written on open.
bool log_open(const char *name);
void log_close(void);
const char *log_path(void);

#endif
