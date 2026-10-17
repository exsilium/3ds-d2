#ifndef D2_PROBE_JITPROBE_H
#define D2_PROBE_JITPROBE_H

#include <stddef.h>

typedef struct jitprobe_config {
    size_t pool_size;      // bytes of executable memory to allocate
    unsigned stress_rounds;
    unsigned yield_count;
} jitprobe_config;

typedef struct jitprobe_result {
    unsigned passed, failed, info;
} jitprobe_result;

// Runs the M1 write/execute/rewrite checks and logs one RESULT line per check.
void jitprobe_run(const jitprobe_config *cfg, jitprobe_result *out);

#endif
