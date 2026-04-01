#ifndef THREADPOOL_H
#define THREADPOOL_H

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#define TP_DEFAULT_QUEUE 8192

typedef void (*tp_work_fn)(void *arg);

typedef struct {
    tp_work_fn fn;
    void      *arg;
} TPWork;

typedef struct {
#ifdef _WIN32
    HANDLE             *threads;
    CRITICAL_SECTION    lock;
    CONDITION_VARIABLE  cond_work;
    CONDITION_VARIABLE  cond_done;
#else
    pthread_t          *threads;
    pthread_mutex_t     lock;
    pthread_cond_t      cond_work;
    pthread_cond_t      cond_done;
#endif
    int                 thread_count;

    TPWork             *queue;
    int                 queue_cap;
    int                 queue_head;
    int                 queue_tail;
    int                 queue_count;

    int                 active_count;
    volatile int        stop;

    long long           total_submitted;
    long long           total_completed;

} ThreadPool;

int  tp_create(ThreadPool *tp, int num_threads, int queue_cap);
int  tp_submit(ThreadPool *tp, tp_work_fn fn, void *arg);
void tp_wait(ThreadPool *tp);
int  tp_pending(ThreadPool *tp);
void tp_drain(ThreadPool *tp);
void tp_destroy(ThreadPool *tp);
void tp_destroy_fast(ThreadPool *tp);

#endif
