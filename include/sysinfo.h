#ifndef SYSINFO_H
#define SYSINFO_H

typedef struct {
    int  cpu_cores;
    int  cpu_threads;
    int  total_ram_mb;
    int  gpu_count;
    char gpu_names[4][256];
    int  gpu_vram_mbs[4];
    char os_name[128];
    char cpu_name[256];
} SystemInfo;

void sysinfo_detect(SystemInfo *info);

/* Returns warning string if system is weak, NULL otherwise */
const char *sysinfo_capability_warning(const SystemInfo *info);

#endif
