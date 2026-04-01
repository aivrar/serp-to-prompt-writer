#ifndef RESMON_H
#define RESMON_H

#include "sysinfo.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define RESMON_HISTORY  120
#define RESMON_MAX_GPUS 4

typedef struct {
    float  process_cpu_pct;
    float  system_cpu_pct;
    size_t process_ram_bytes;
    size_t available_ram_bytes;
    size_t total_ram_bytes;
    float  process_ram_pct;
    float  system_ram_pct;
    int    gpu_count;
    float  gpu_util_pct[RESMON_MAX_GPUS];
    size_t gpu_vram_used[RESMON_MAX_GPUS];
    size_t gpu_vram_total[RESMON_MAX_GPUS];
    float  gpu_vram_pct[RESMON_MAX_GPUS];
    int    active_connections;
    int    active_threads;
    int    queued_work;
    double timestamp;
} ResourceSnapshot;

typedef struct {
#ifdef _WIN32
    HANDLE              thread;
    CRITICAL_SECTION    lock;
    LARGE_INTEGER       perf_freq;
    LARGE_INTEGER       start_counter;
    FILETIME            prev_sys_idle, prev_sys_kernel, prev_sys_user;
    FILETIME            prev_proc_kernel, prev_proc_user;
    LARGE_INTEGER       prev_sample_counter;
#else
    pthread_t           thread;
    pthread_mutex_t     lock;
    double              start_time;
    long long           prev_proc_utime, prev_proc_stime;
    double              prev_sample_time;
#endif
    volatile int        stop;

    ResourceSnapshot    current;
    ResourceSnapshot    history[RESMON_HISTORY];
    int                 history_count;
    int                 history_idx;

    int    sample_interval_ms;

    int    cpu_cores;
    int    cpu_threads;
    int    total_ram_mb;

    /* NVML dynamic loading */
    void  *nvml_handle;
    int    nvml_ok;
    int    nvml_gpu_count;
    void  *nvml_devices[RESMON_MAX_GPUS];
    void  *fn_nvmlInit;
    void  *fn_nvmlShutdown;
    void  *fn_nvmlDeviceGetCount;
    void  *fn_nvmlDeviceGetHandleByIndex;
    void  *fn_nvmlDeviceGetUtilizationRates;
    void  *fn_nvmlDeviceGetMemoryInfo;
} ResourceMonitor;

int  resmon_init(ResourceMonitor *mon, const SystemInfo *sys);
void resmon_shutdown(ResourceMonitor *mon);
void resmon_get(ResourceMonitor *mon, ResourceSnapshot *out);

#endif
