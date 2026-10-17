#ifndef D2_PLATFORM_CTR_PROBES_H
#define D2_PLATFORM_CTR_PROBES_H

typedef struct probe_tally {
    unsigned passed, failed, info;
} probe_tally;

// Memory budget figures and the process address-space map.
void memprobe_run(probe_tally *t);
// Fixed-address mappings at the Win32 image base, with alias and execute checks.
void mapprobe_run(probe_tally *t);
// M0 SD fixture: write, read back and compare; reports the previous run's build.
void sdfixture_run(probe_tally *t);

#endif
