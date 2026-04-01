#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define LOG_DEBUG   0
#define LOG_INFO    1
#define LOG_WARN    2
#define LOG_ERROR   3

/* Ring buffer for UI log display */
#define LOG_MAX_ENTRIES 2000
#define LOG_MAX_MSG     512

typedef struct {
    char               msg[LOG_MAX_MSG];
    int                level;
    time_t             timestamp;
    unsigned long long seq;
} LogEntry;

typedef struct {
    LogEntry entries[LOG_MAX_ENTRIES];
    int      head;
    int      count;
    unsigned long long next_seq;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#endif
} LogBuffer;

extern LogBuffer g_log;
extern int       g_log_min_level;

void log_init(void);
void log_shutdown(void);

void app_log(int level, const char *fmt, ...);

/* Get log entries for UI display */
int  log_get_entries(LogEntry *out, int max_count, int min_level);

#endif
