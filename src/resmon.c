#include "resmon.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

/* ── NVML dynamic loading ────────────────────────────────────── */

typedef int (*nvmlInit_fn)(void);
typedef int (*nvmlShutdown_fn)(void);
typedef int (*nvmlDeviceGetCount_fn)(unsigned int *);
typedef int (*nvmlDeviceGetHandleByIndex_fn)(unsigned int, void **);
typedef struct { unsigned int gpu; unsigned int memory; } nvmlUtilization_t;
typedef int (*nvmlDeviceGetUtilizationRates_fn)(void *, nvmlUtilization_t *);
typedef struct { unsigned long long total; unsigned long long free; unsigned long long used; } nvmlMemory_t;
typedef int (*nvmlDeviceGetMemoryInfo_fn)(void *, nvmlMemory_t *);

static void nvml_load(ResourceMonitor *mon) {
    HMODULE lib = LoadLibraryA("nvml.dll");
    if (!lib) {
        char path[512];
        DWORD len = GetEnvironmentVariableA("CUDA_PATH", path, 400);
        if (len > 0) {
            strcat(path, "\\bin\\nvml.dll");
            lib = LoadLibraryA(path);
        }
    }
    if (!lib) lib = LoadLibraryA("C:\\Windows\\System32\\nvml.dll");
    if (!lib) { mon->nvml_ok = 0; return; }

    mon->nvml_handle = lib;
    mon->fn_nvmlInit = (void *)GetProcAddress(lib, "nvmlInit_v2");
    mon->fn_nvmlShutdown = (void *)GetProcAddress(lib, "nvmlShutdown");
    mon->fn_nvmlDeviceGetCount = (void *)GetProcAddress(lib, "nvmlDeviceGetCount_v2");
    mon->fn_nvmlDeviceGetHandleByIndex = (void *)GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
    mon->fn_nvmlDeviceGetUtilizationRates = (void *)GetProcAddress(lib, "nvmlDeviceGetUtilizationRates");
    mon->fn_nvmlDeviceGetMemoryInfo = (void *)GetProcAddress(lib, "nvmlDeviceGetMemoryInfo");

    if (!mon->fn_nvmlInit || !mon->fn_nvmlDeviceGetCount || !mon->fn_nvmlDeviceGetHandleByIndex) {
        FreeLibrary(lib); mon->nvml_handle = NULL; mon->nvml_ok = 0; return;
    }

    if (((nvmlInit_fn)mon->fn_nvmlInit)() != 0) {
        FreeLibrary(lib); mon->nvml_handle = NULL; mon->nvml_ok = 0; return;
    }

    unsigned int count = 0;
    ((nvmlDeviceGetCount_fn)mon->fn_nvmlDeviceGetCount)(&count);
    mon->nvml_gpu_count = (int)(count < RESMON_MAX_GPUS ? count : RESMON_MAX_GPUS);

    for (int i = 0; i < mon->nvml_gpu_count; i++)
        ((nvmlDeviceGetHandleByIndex_fn)mon->fn_nvmlDeviceGetHandleByIndex)(i, &mon->nvml_devices[i]);

    mon->nvml_ok = 1;
    app_log(LOG_INFO, "NVML: %d GPU(s) detected", mon->nvml_gpu_count);
}

/* ── Sampling ────────────────────────────────────────────────── */

static void sample_cpu(ResourceMonitor *mon, ResourceSnapshot *snap) {
    /* System CPU via GetSystemTimes delta */
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);

    ULONGLONG si = *(ULONGLONG*)&idle - *(ULONGLONG*)&mon->prev_sys_idle;
    ULONGLONG sk = *(ULONGLONG*)&kernel - *(ULONGLONG*)&mon->prev_sys_kernel;
    ULONGLONG su = *(ULONGLONG*)&user - *(ULONGLONG*)&mon->prev_sys_user;
    ULONGLONG total = sk + su;
    snap->system_cpu_pct = total > 0 ? (float)(total - si) / total * 100.0f : 0;

    mon->prev_sys_idle = idle;
    mon->prev_sys_kernel = kernel;
    mon->prev_sys_user = user;

    /* Process CPU */
    FILETIME create, exit, pk, pu;
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &pk, &pu);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - mon->prev_sample_counter.QuadPart) / mon->perf_freq.QuadPart;

    ULONGLONG proc_total = (*(ULONGLONG*)&pk - *(ULONGLONG*)&mon->prev_proc_kernel)
                         + (*(ULONGLONG*)&pu - *(ULONGLONG*)&mon->prev_proc_user);
    double proc_sec = proc_total / 10000000.0;
    snap->process_cpu_pct = elapsed > 0 ? (float)(proc_sec / elapsed / mon->cpu_threads * 100.0) : 0;

    mon->prev_proc_kernel = pk;
    mon->prev_proc_user = pu;
    mon->prev_sample_counter = now;
}

static void sample_ram(ResourceMonitor *mon, ResourceSnapshot *snap) {
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);

    snap->total_ram_bytes = mem.ullTotalPhys;
    snap->available_ram_bytes = mem.ullAvailPhys;
    snap->system_ram_pct = (float)mem.dwMemoryLoad;

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        snap->process_ram_bytes = pmc.WorkingSetSize;
        snap->process_ram_pct = snap->total_ram_bytes > 0
            ? (float)pmc.WorkingSetSize / snap->total_ram_bytes * 100.0f : 0;
    }
}

static void sample_gpu(ResourceMonitor *mon, ResourceSnapshot *snap) {
    if (!mon->nvml_ok) { snap->gpu_count = 0; return; }
    snap->gpu_count = mon->nvml_gpu_count;

    for (int i = 0; i < mon->nvml_gpu_count; i++) {
        if (mon->fn_nvmlDeviceGetUtilizationRates) {
            nvmlUtilization_t util;
            if (((nvmlDeviceGetUtilizationRates_fn)mon->fn_nvmlDeviceGetUtilizationRates)(
                    mon->nvml_devices[i], &util) == 0)
                snap->gpu_util_pct[i] = (float)util.gpu;
        }
        if (mon->fn_nvmlDeviceGetMemoryInfo) {
            nvmlMemory_t mem;
            if (((nvmlDeviceGetMemoryInfo_fn)mon->fn_nvmlDeviceGetMemoryInfo)(
                    mon->nvml_devices[i], &mem) == 0) {
                snap->gpu_vram_used[i] = mem.used;
                snap->gpu_vram_total[i] = mem.total;
                snap->gpu_vram_pct[i] = mem.total > 0
                    ? (float)mem.used / mem.total * 100.0f : 0;
            }
        }
    }
}

static DWORD WINAPI resmon_thread(LPVOID arg) {
    ResourceMonitor *mon = (ResourceMonitor *)arg;

    /* Prime CPU counters */
    GetSystemTimes(&mon->prev_sys_idle, &mon->prev_sys_kernel, &mon->prev_sys_user);
    FILETIME c, e;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &mon->prev_proc_kernel, &mon->prev_proc_user);
    QueryPerformanceCounter(&mon->prev_sample_counter);
    Sleep(100);

    while (!mon->stop) {
        ResourceSnapshot snap;
        memset(&snap, 0, sizeof(snap));

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        snap.timestamp = (double)(now.QuadPart - mon->start_counter.QuadPart) / mon->perf_freq.QuadPart;

        sample_cpu(mon, &snap);
        sample_ram(mon, &snap);
        sample_gpu(mon, &snap);
        snap.active_connections = mon->current.active_connections;
        snap.active_threads = mon->current.active_threads;
        snap.queued_work = mon->current.queued_work;

        EnterCriticalSection(&mon->lock);
        mon->current = snap;
        mon->history[mon->history_idx] = snap;
        mon->history_idx = (mon->history_idx + 1) % RESMON_HISTORY;
        if (mon->history_count < RESMON_HISTORY) mon->history_count++;
        LeaveCriticalSection(&mon->lock);

        Sleep(mon->sample_interval_ms);
    }
    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

int resmon_init(ResourceMonitor *mon, const SystemInfo *sys) {
    memset(mon, 0, sizeof(ResourceMonitor));

    mon->cpu_cores = sys->cpu_cores;
    mon->cpu_threads = sys->cpu_threads;
    mon->total_ram_mb = sys->total_ram_mb;

    mon->sample_interval_ms = 1000;

    InitializeCriticalSection(&mon->lock);
    QueryPerformanceFrequency(&mon->perf_freq);
    QueryPerformanceCounter(&mon->start_counter);

    nvml_load(mon);

    mon->thread = CreateThread(NULL, 0, resmon_thread, mon, 0, NULL);
    if (!mon->thread) return -1;

    app_log(LOG_INFO, "ResMon: started (interval=%dms)", mon->sample_interval_ms);
    return 0;
}

void resmon_shutdown(ResourceMonitor *mon) {
    mon->stop = 1;
    if (mon->thread) {
        DWORD wait = WaitForSingleObject(mon->thread, 5000);
        if (wait == WAIT_TIMEOUT)
            app_log(LOG_WARN, "ResMon: thread did not exit within 5s, forcing shutdown");
        CloseHandle(mon->thread);
        mon->thread = NULL;
    }
    if (mon->nvml_ok && mon->fn_nvmlShutdown)
        ((nvmlShutdown_fn)mon->fn_nvmlShutdown)();
    if (mon->nvml_handle)
        FreeLibrary(mon->nvml_handle);
    DeleteCriticalSection(&mon->lock);
    app_log(LOG_INFO, "ResMon: stopped");
}

void resmon_get(ResourceMonitor *mon, ResourceSnapshot *out) {
    EnterCriticalSection(&mon->lock);
    *out = mon->current;
    LeaveCriticalSection(&mon->lock);
}

#else
/* Minimal stubs for non-Windows */
int  resmon_init(ResourceMonitor *mon, const SystemInfo *sys) { (void)mon; (void)sys; return 0; }
void resmon_shutdown(ResourceMonitor *mon) { (void)mon; }
void resmon_get(ResourceMonitor *mon, ResourceSnapshot *out) { memset(out, 0, sizeof(*out)); (void)mon; }
#endif
