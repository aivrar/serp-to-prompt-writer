#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

LogBuffer g_log;
int g_log_min_level = LOG_DEBUG;
static FILE *g_log_file = NULL;
static volatile int g_log_shutdown = 0;

static const char *level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};

void log_init(void) {
    memset(&g_log, 0, sizeof(g_log));
#ifdef _WIN32
    InitializeCriticalSection(&g_log.lock);
#endif

    /* Rotate log if > 5MB */
    {FILE *check = fopen("debug.log", "rb");
    if (check) {
        fseek(check, 0, SEEK_END);
        long sz = ftell(check);
        fclose(check);
        if (sz > 5 * 1024 * 1024) {
            remove("debug.log.old");
            rename("debug.log", "debug.log.old");
        }
    }}

    g_log_file = fopen("debug.log", "a");
    if (g_log_file) {
        fprintf(g_log_file, "\n========== SESSION START ==========\n");
        fflush(g_log_file);
    }
}

void log_shutdown(void) {
    g_log_shutdown = 1;
#ifdef _WIN32
    Sleep(10);  /* grace period for threads between flag check and CS enter */
#endif
    if (g_log_file) {
        fprintf(g_log_file, "========== SESSION END ==========\n");
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
#ifdef _WIN32
    DeleteCriticalSection(&g_log.lock);
#endif
}

void app_log(int level, const char *fmt, ...) {
    if (g_log_shutdown) return;
    if (level < g_log_min_level) return;

    char msg[LOG_MAX_MSG];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Console output (stderr is thread-safe on MSVC CRT) */
    time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    struct tm *tp = localtime(&now);
    if (tp) t = *tp; else memset(&t, 0, sizeof(t));
#endif
    fprintf(stderr, "[%02d:%02d:%02d] %s: %s\n",
            t.tm_hour, t.tm_min, t.tm_sec,
            level_names[(level >= 0 && level < 4) ? level : 3], msg);

    /* All shared state (log file + ring buffer) under lock */
#ifdef _WIN32
    EnterCriticalSection(&g_log.lock);
#endif

    /* Persistent log file — flush on WARN+ and periodically (every 50 writes)
       to prevent losing important lines like batch completion stats */
    static int flush_counter = 0;
    if (g_log_file) {
        fprintf(g_log_file, "[%02d:%02d:%02d] %s: %s\n",
                t.tm_hour, t.tm_min, t.tm_sec,
                level_names[(level >= 0 && level < 4) ? level : 3], msg);
        flush_counter++;
        if (level >= LOG_WARN || flush_counter >= 50) {
            fflush(g_log_file);
            flush_counter = 0;
        }
    }

    /* Ring buffer for UI */
    int idx = (g_log.head + g_log.count) % LOG_MAX_ENTRIES;
    if (g_log.count >= LOG_MAX_ENTRIES) {
        g_log.head = (g_log.head + 1) % LOG_MAX_ENTRIES;
    } else {
        g_log.count++;
    }
    g_log.entries[idx].seq = g_log.next_seq++;
    snprintf(g_log.entries[idx].msg, LOG_MAX_MSG, "%s", msg);
    g_log.entries[idx].level = level;
    g_log.entries[idx].timestamp = now;

#ifdef _WIN32
    LeaveCriticalSection(&g_log.lock);
#endif
}

int log_get_entries(LogEntry *out, int max_count, int min_level) {
    int copied = 0;
#ifdef _WIN32
    EnterCriticalSection(&g_log.lock);
#endif

    for (int i = 0; i < g_log.count && copied < max_count; i++) {
        int idx = (g_log.head + i) % LOG_MAX_ENTRIES;
        if (g_log.entries[idx].level >= min_level) {
            out[copied++] = g_log.entries[idx];
        }
    }

#ifdef _WIN32
    LeaveCriticalSection(&g_log.lock);
#endif
    return copied;
}
