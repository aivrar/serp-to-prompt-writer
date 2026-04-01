#include "threadpool.h"
#include "utils.h"
#include "app_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Worker thread function ───────────────────────────────────── */

#ifdef _WIN32

static DWORD WINAPI worker(LPVOID arg) {
    ThreadPool *tp = (ThreadPool *)arg;

    for (;;) {
        EnterCriticalSection(&tp->lock);

        while (tp->queue_count == 0 && !tp->stop)
            SleepConditionVariableCS(&tp->cond_work, &tp->lock, INFINITE);

        if (tp->stop && tp->queue_count == 0) {
            LeaveCriticalSection(&tp->lock);
            http_thread_cleanup();
            return 0;
        }

        /* Dequeue */
        TPWork work = tp->queue[tp->queue_head];
        tp->queue_head = (tp->queue_head + 1) % tp->queue_cap;
        tp->queue_count--;
        tp->active_count++;
        LeaveCriticalSection(&tp->lock);

        work.fn(work.arg);

        EnterCriticalSection(&tp->lock);
        tp->active_count--;
        tp->total_completed++;
        if (tp->queue_count == 0 && tp->active_count == 0)
            WakeAllConditionVariable(&tp->cond_done);
        LeaveCriticalSection(&tp->lock);
    }
}

int tp_create(ThreadPool *tp, int num_threads, int queue_cap) {
    memset(tp, 0, sizeof(ThreadPool));

    if (queue_cap <= 0) queue_cap = TP_DEFAULT_QUEUE;
    tp->queue_cap = queue_cap;
    tp->queue = (TPWork *)calloc((size_t)queue_cap, sizeof(TPWork));
    if (!tp->queue) return -1;

    tp->thread_count = num_threads;

    InitializeCriticalSection(&tp->lock);
    InitializeConditionVariable(&tp->cond_work);
    InitializeConditionVariable(&tp->cond_done);

    tp->threads = (HANDLE *)calloc((size_t)num_threads, sizeof(HANDLE));
    if (!tp->threads) { free(tp->queue); return -1; }

    for (int i = 0; i < num_threads; i++) {
        tp->threads[i] = CreateThread(NULL, 0, worker, tp, 0, NULL);
        if (!tp->threads[i]) {
            tp->thread_count = i;  /* only i threads were created */
            tp_destroy(tp);
            return -1;
        }
    }
    return 0;
}

int tp_submit(ThreadPool *tp, tp_work_fn fn, void *arg) {
    EnterCriticalSection(&tp->lock);

    if (tp->queue_count >= tp->queue_cap) {
        app_log(LOG_WARN, "ThreadPool: queue full (%d/%d), dropping work item", tp->queue_count, tp->queue_cap);
        LeaveCriticalSection(&tp->lock);
        return -1; /* queue full */
    }

    tp->queue[tp->queue_tail].fn = fn;
    tp->queue[tp->queue_tail].arg = arg;
    tp->queue_tail = (tp->queue_tail + 1) % tp->queue_cap;
    tp->queue_count++;
    tp->total_submitted++;
    WakeConditionVariable(&tp->cond_work);
    LeaveCriticalSection(&tp->lock);
    return 0;
}

void tp_wait(ThreadPool *tp) {
    EnterCriticalSection(&tp->lock);
    while (tp->queue_count > 0 || tp->active_count > 0)
        SleepConditionVariableCS(&tp->cond_done, &tp->lock, INFINITE);
    LeaveCriticalSection(&tp->lock);
}

int tp_pending(ThreadPool *tp) {
    EnterCriticalSection(&tp->lock);
    int n = tp->queue_count + tp->active_count;
    LeaveCriticalSection(&tp->lock);
    return n;
}

void tp_drain(ThreadPool *tp) {
    EnterCriticalSection(&tp->lock);
    int dropped = tp->queue_count;
    tp->queue_count = 0;
    tp->queue_head = tp->queue_tail = 0;
    LeaveCriticalSection(&tp->lock);
    if (dropped > 0)
        app_log(LOG_WARN, "ThreadPool: drained %d pending work items", dropped);
}

void tp_destroy(ThreadPool *tp) {
    if (!tp->threads) return;

    EnterCriticalSection(&tp->lock);
    tp->stop = 1;
    WakeAllConditionVariable(&tp->cond_work);
    LeaveCriticalSection(&tp->lock);

    DWORD wait_result = WaitForMultipleObjects(tp->thread_count, tp->threads,
                                               TRUE, 30000);
    for (int i = 0; i < tp->thread_count; i++) {
        if (wait_result == WAIT_TIMEOUT) {
            DWORD exit_code = 0;
            GetExitCodeThread(tp->threads[i], &exit_code);
            if (exit_code == STILL_ACTIVE) {
                app_log(LOG_ERROR, "ThreadPool: thread %d still active after 30s, forcing termination", i);
                TerminateThread(tp->threads[i], 1);
            }
        }
        CloseHandle(tp->threads[i]);
    }

    free(tp->threads);
    tp->threads = NULL;
    free(tp->queue);
    tp->queue = NULL;
    DeleteCriticalSection(&tp->lock);
}

void tp_destroy_fast(ThreadPool *tp) {
    if (!tp->threads) return;

    /* Drain queue so no new work starts */
    EnterCriticalSection(&tp->lock);
    int dropped = tp->queue_count;
    tp->queue_count = 0;
    tp->queue_head = tp->queue_tail = 0;
    tp->stop = 1;
    WakeAllConditionVariable(&tp->cond_work);
    LeaveCriticalSection(&tp->lock);

    if (dropped > 0)
        app_log(LOG_WARN, "ThreadPool: fast-destroy drained %d items", dropped);

    /* Short wait -- active workers should finish fast since inference
       servers are already dead (connection reset on their HTTP calls) */
    DWORD wait_result = WaitForMultipleObjects(tp->thread_count, tp->threads,
                                               TRUE, 2000);
    int any_leaked = 0;
    for (int i = 0; i < tp->thread_count; i++) {
        if (wait_result == WAIT_TIMEOUT) {
            DWORD exit_code = 0;
            GetExitCodeThread(tp->threads[i], &exit_code);
            if (exit_code == STILL_ACTIVE) {
                app_log(LOG_WARN, "ThreadPool: thread %d still active after fast-destroy timeout, leaking handle (safer than TerminateThread)", i);
                any_leaked = 1;
                continue;  /* leak the thread handle -- process exit will clean up */
            }
        }
        CloseHandle(tp->threads[i]);
    }

    if (any_leaked) {
        /* Leaked threads may still touch queue/CS -- don't free them.
           Process exit reclaims all.  Set threads=NULL so tp_destroy
           (called later in engine_shutdown) is a no-op. */
        app_log(LOG_WARN, "ThreadPool: leaked threads, skipping queue/CS cleanup");
        tp->threads = NULL;
        return;
    }

    free(tp->threads);
    tp->threads = NULL;
    free(tp->queue);
    tp->queue = NULL;
    DeleteCriticalSection(&tp->lock);
}

/* Non-Win32 implementation removed -- this app is Win32-only */

#endif
