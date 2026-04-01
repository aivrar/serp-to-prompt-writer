#include "sysinfo.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void detect_cpu(SystemInfo *info) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    info->cpu_threads = (int)si.dwNumberOfProcessors;

    /* Count physical cores via GetLogicalProcessorInformation */
    info->cpu_cores = 0;
    {DWORD buf_len = 0;
    GetLogicalProcessorInformation(NULL, &buf_len);
    if (buf_len > 0) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf =
            (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(buf_len);
        if (buf && GetLogicalProcessorInformation(buf, &buf_len)) {
            DWORD count = buf_len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (DWORD i = 0; i < count; i++) {
                if (buf[i].Relationship == RelationProcessorCore)
                    info->cpu_cores++;
            }
        }
        free(buf);
    }}
    if (info->cpu_cores < 1) info->cpu_cores = info->cpu_threads / 2;
    if (info->cpu_cores < 1) info->cpu_cores = 1;

    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(info->cpu_name);
        RegQueryValueExA(hKey, "ProcessorNameString", NULL, NULL,
                        (LPBYTE)info->cpu_name, &size);
        RegCloseKey(hKey);
    }

    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    info->total_ram_mb = (int)(mem.ullTotalPhys / (1024 * 1024));
}

static void detect_gpu(SystemInfo *info) {
    info->gpu_count = 0;
    __try {
        HKEY hEnum;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}",
                0, KEY_READ, &hEnum) == ERROR_SUCCESS) {
            char subname[16];
            DWORD idx = 0;
            int misses = 0;
            while (info->gpu_count < 4 && misses < 20) {
                snprintf(subname, sizeof(subname), "%04d", (int)idx);
                HKEY hDev;
                if (RegOpenKeyExA(hEnum, subname, 0, KEY_READ, &hDev) != ERROR_SUCCESS) {
                    idx++; misses++; continue;
                }
                misses = 0;
                char name[256] = {0};
                DWORD sz = sizeof(name);
                DWORD type = 0;
                RegQueryValueExA(hDev, "DriverDesc", NULL, &type, (LPBYTE)name, &sz);
                long long vram = 0;
                DWORD vram32 = 0;
                sz = sizeof(vram);
                if (RegQueryValueExA(hDev, "HardwareInformation.qwMemorySize",
                                     NULL, &type, (LPBYTE)&vram, &sz) != ERROR_SUCCESS) {
                    sz = sizeof(vram32);
                    if (RegQueryValueExA(hDev, "HardwareInformation.MemorySize",
                                         NULL, &type, (LPBYTE)&vram32, &sz) == ERROR_SUCCESS)
                        vram = vram32;
                }
                RegCloseKey(hDev);
                if (name[0] && !strstr(name, "Basic Display")) {
                    int gi = info->gpu_count;
                    snprintf(info->gpu_names[gi], sizeof(info->gpu_names[gi]), "%s", name);
                    info->gpu_vram_mbs[gi] = (int)(vram / (1024 * 1024));
                    info->gpu_count++;
                }
                idx++;
            }
            RegCloseKey(hEnum);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        app_log(LOG_WARN, "GPU detection failed (registry exception)");
    }
}

#else
static void detect_cpu(SystemInfo *info) {
    info->cpu_cores = 4;
    info->cpu_threads = 8;
    info->total_ram_mb = 8192;
    snprintf(info->cpu_name, sizeof(info->cpu_name), "Unknown");
}
static void detect_gpu(SystemInfo *info) { info->gpu_count = 0; }
#endif

void sysinfo_detect(SystemInfo *info) {
    memset(info, 0, sizeof(SystemInfo));
    detect_cpu(info);
    detect_gpu(info);

#ifdef _WIN32
    snprintf(info->os_name, sizeof(info->os_name), "Windows");
#elif __APPLE__
    snprintf(info->os_name, sizeof(info->os_name), "macOS");
#else
    snprintf(info->os_name, sizeof(info->os_name), "Linux");
#endif

    app_log(LOG_INFO, "System: %s, %d cores/%d threads, %d MB RAM",
            info->cpu_name, info->cpu_cores, info->cpu_threads, info->total_ram_mb);
    for (int i = 0; i < info->gpu_count; i++)
        app_log(LOG_INFO, "GPU %d: %s (%d MB VRAM)", i+1, info->gpu_names[i], info->gpu_vram_mbs[i]);
    if (info->gpu_count == 0)
        app_log(LOG_INFO, "GPU: none detected");
}

const char *sysinfo_capability_warning(const SystemInfo *info) {
    static char warning[512];
    warning[0] = '\0';

    int wpos = 0;
    if (info->cpu_threads < 4)
        wpos += snprintf(warning + wpos, sizeof(warning) - wpos, "Low CPU (%d threads). Heavy scraping may be slow.", info->cpu_threads);
    if (info->total_ram_mb < 4096) {
        if (wpos > 0 && wpos < (int)sizeof(warning) - 1)
            wpos += snprintf(warning + wpos, sizeof(warning) - wpos, " ");
        snprintf(warning + wpos, sizeof(warning) - wpos, "Low RAM (%d MB).", info->total_ram_mb);
    }
    return warning[0] ? warning : NULL;
}
