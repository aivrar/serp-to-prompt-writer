#include "js_render.h"
#include "app_log.h"
#include "utils.h"
#include "cJSON.h"

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "winhttp.lib")

/* ── Browser discovery ──────────────────────────────────────── */

static const char *find_browser(void) {
    static const char *paths[] = {
        "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
        "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
        "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
        NULL
    };
    for (int i = 0; paths[i]; i++)
        if (file_exists(paths[i])) return paths[i];
    return NULL;
}

/* ── Edge process management ────────────────────────────────── */

typedef struct {
    HANDLE hProcess;
    HANDLE hJob;
    int    port;
    char   profile_dir[MAX_PATH];
} BrowserProc;

static int browser_launch(BrowserProc *bp, const char *exe_path, int slot_index) {
    bp->port = 9300 + slot_index;
    snprintf(bp->profile_dir, MAX_PATH, "data\\_cdp_%d", slot_index);
    CreateDirectoryA("data", NULL);
    CreateDirectoryA(bp->profile_dir, NULL);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" --headless --disable-gpu --no-sandbox --no-first-run "
        "--disable-extensions --disable-background-networking "
        "--disable-default-apps --disable-sync --disable-translate "
        "--disable-component-extensions-with-background-pages "
        "--disable-features=msEdgeSync "
        "--disable-blink-features=AutomationControlled "
        "--mute-audio "
        "--remote-debugging-port=%d --user-data-dir=\"%s\" about:blank",
        exe_path, bp->port, bp->profile_dir);

    /* Job Object: kills entire process tree when handle is closed.
       This ensures Edge subprocesses don't leak on crash or restart. */
    bp->hJob = CreateJobObjectA(NULL, NULL);
    if (bp->hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        memset(&jeli, 0, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(bp->hJob, JobObjectExtendedLimitInformation,
                                &jeli, sizeof(jeli));
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        app_log(LOG_ERROR, "CDP: CreateProcess failed (err=%lu)", GetLastError());
        if (bp->hJob) CloseHandle(bp->hJob);
        return -1;
    }

    if (bp->hJob) AssignProcessToJobObject(bp->hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    bp->hProcess = pi.hProcess;
    return 0;
}

static void browser_kill(BrowserProc *bp) {
    if (bp->hJob)     { TerminateJobObject(bp->hJob, 0); CloseHandle(bp->hJob); bp->hJob = NULL; }
    if (bp->hProcess) { WaitForSingleObject(bp->hProcess, 2000); CloseHandle(bp->hProcess); bp->hProcess = NULL; }
}

/* ── WinHTTP: simple HTTP GET to localhost ───────────────────── */

static char *local_http_get(int port, const wchar_t *path, int timeout_ms) {
    HINTERNET ses = WinHttpOpen(L"CDP/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!ses) return NULL;
    WinHttpSetTimeouts(ses, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET con = WinHttpConnect(ses, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!con) { WinHttpCloseHandle(ses); return NULL; }

    HINTERNET req = WinHttpOpenRequest(con, L"GET", path, NULL, NULL, NULL, 0);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return NULL; }

    if (!WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return NULL;
    }

    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    DWORD nr;
    while (WinHttpReadData(req, buf + len, (DWORD)(cap - len - 1), &nr) && nr > 0) {
        len += nr;
        if (len + 2048 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
    }
    buf[len] = '\0';

    WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
    return buf;
}

/* ── WinHTTP WebSocket ──────────────────────────────────────── */

/* WinHttpWebSocketReceive blocks with no timeout (MSDN confirms:
   WINHTTP_OPTION_RECEIVE_TIMEOUT does NOT apply after WebSocket upgrade).
   Fix: dedicated receive thread signals an event when data arrives.
   ws_recv_timed() does WaitForSingleObject with a real timeout. */

typedef struct {
    HINTERNET ses;
    HINTERNET con;
    HINTERNET ws;
    /* Receive thread state */
    HANDLE    hThread;
    HANDLE    hDataReady;    /* auto-reset event: signaled when a message is ready */
    HANDLE    hRecvRequest;  /* auto-reset event: signaled to request next receive */
    char     *recv_buf;
    size_t    recv_len;
    DWORD     recv_err;      /* 0 = ok, nonzero = error */
    volatile LONG recv_shutdown;
} WsConn;

static DWORD WINAPI ws_recv_thread(LPVOID param) {
    WsConn *c = (WsConn *)param;
    size_t cap = 131072;
    char *buf = (char *)malloc(cap);
    if (!buf) { c->recv_err = 1; SetEvent(c->hDataReady); return 1; }

    while (!c->recv_shutdown) {
        /* Wait for consumer to request next message */
        if (WaitForSingleObject(c->hRecvRequest, 500) != WAIT_OBJECT_0) continue;
        if (c->recv_shutdown) break;

        /* Read one complete WebSocket message (blocking) */
        size_t len = 0;
        DWORD err = 0;
        for (;;) {
            DWORD nr = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE btype;
            DWORD rc = WinHttpWebSocketReceive(c->ws, buf + len, (DWORD)(cap - len - 1), &nr, &btype);
            if (rc != NO_ERROR) { err = rc; break; }
            len += nr;
            if (btype == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                btype == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                break;
            if (len + 65536 > cap) {
                cap *= 2;
                char *tmp = (char *)realloc(buf, cap);
                if (!tmp) { err = 1; break; }
                buf = tmp;
            }
        }

        if (c->recv_shutdown) break;

        /* Hand message to consumer */
        buf[len] = '\0';
        c->recv_buf = (char *)malloc(len + 1);
        if (c->recv_buf) { memcpy(c->recv_buf, buf, len + 1); c->recv_len = len; }
        c->recv_err = err;
        SetEvent(c->hDataReady);
    }
    free(buf);
    return 0;
}

static int ws_connect(WsConn *c, int port, const char *path) {
    memset(c, 0, sizeof(*c));

    c->ses = WinHttpOpen(L"CDP/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!c->ses) return -1;
    WinHttpSetTimeouts(c->ses, 15000, 15000, 15000, 15000);

    c->con = WinHttpConnect(c->ses, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!c->con) goto fail;

    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);

    HINTERNET req = WinHttpOpenRequest(c->con, L"GET", wpath, NULL, NULL, NULL, 0);
    if (!req) goto fail;

    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        WinHttpCloseHandle(req); goto fail;
    }

    if (!WinHttpSendRequest(req, NULL, 0, NULL, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        WinHttpCloseHandle(req); goto fail;
    }

    c->ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!c->ws) goto fail;

    /* Start receive thread */
    c->hDataReady   = CreateEventA(NULL, FALSE, FALSE, NULL);  /* auto-reset */
    c->hRecvRequest = CreateEventA(NULL, FALSE, FALSE, NULL);  /* auto-reset */
    c->recv_shutdown = 0;
    c->hThread = CreateThread(NULL, 0, ws_recv_thread, c, 0, NULL);
    if (!c->hThread) goto fail;

    return 0;

fail:
    if (c->hDataReady)   CloseHandle(c->hDataReady);
    if (c->hRecvRequest) CloseHandle(c->hRecvRequest);
    if (c->ws)  WinHttpCloseHandle(c->ws);
    if (c->con) WinHttpCloseHandle(c->con);
    if (c->ses) WinHttpCloseHandle(c->ses);
    memset(c, 0, sizeof(*c));
    return -1;
}

static void ws_close(WsConn *c) {
    /* Signal receive thread to stop */
    if (c->hThread) {
        InterlockedExchange(&c->recv_shutdown, 1);
        SetEvent(c->hRecvRequest);  /* unblock if waiting for request */
        /* Close WebSocket to unblock WinHttpWebSocketReceive */
        if (c->ws) {
            WinHttpWebSocketClose(c->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle(c->ws); c->ws = NULL;
        }
        WaitForSingleObject(c->hThread, 3000);
        CloseHandle(c->hThread); c->hThread = NULL;
    } else {
        if (c->ws) {
            WinHttpWebSocketClose(c->ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
            WinHttpCloseHandle(c->ws); c->ws = NULL;
        }
    }
    if (c->hDataReady)   { CloseHandle(c->hDataReady);   c->hDataReady = NULL; }
    if (c->hRecvRequest) { CloseHandle(c->hRecvRequest); c->hRecvRequest = NULL; }
    free(c->recv_buf); c->recv_buf = NULL;
    if (c->con) WinHttpCloseHandle(c->con);
    if (c->ses) WinHttpCloseHandle(c->ses);
    memset(c, 0, sizeof(*c));
}

static int ws_send(WsConn *c, const char *msg) {
    DWORD len = (DWORD)strlen(msg);
    return (WinHttpWebSocketSend(c->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void *)msg, len) == NO_ERROR) ? 0 : -1;
}

/* Receive one complete WebSocket message with a timeout.
   Returns malloc'd string on success, empty malloc'd string on timeout, NULL on error.
   Caller must free non-NULL returns. */
static char *ws_recv_timed(WsConn *c, int timeout_ms) {
    /* Ask the receive thread to get the next message */
    SetEvent(c->hRecvRequest);

    DWORD wr = WaitForSingleObject(c->hDataReady, (DWORD)timeout_ms);
    if (wr == WAIT_TIMEOUT) {
        /* No data within timeout — return empty string (not NULL = not error) */
        char *empty = (char *)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    if (wr != WAIT_OBJECT_0) return NULL;

    /* Receive thread delivered a message */
    if (c->recv_err) { free(c->recv_buf); c->recv_buf = NULL; return NULL; }
    char *msg = c->recv_buf;
    c->recv_buf = NULL;
    return msg;
}

/* Blocking receive (for cdp_call where we know data is coming soon). */
static char *ws_recv(WsConn *c) {
    return ws_recv_timed(c, 30000);  /* 30s hard ceiling */
}

/* ── CDP helpers ────────────────────────────────────────────── */

/* Send a CDP command and wait for response with matching id.
   Discards any interleaved events.  Returns malloc'd JSON string or NULL. */
static char *cdp_call(WsConn *c, int id, const char *msg, int timeout_ms) {
    if (ws_send(c, msg) != 0) return NULL;

    DWORD t0 = GetTickCount();
    while ((GetTickCount() - t0) < (DWORD)timeout_ms) {
        char *resp = ws_recv(c);
        if (!resp) return NULL;

        cJSON *j = cJSON_Parse(resp);
        if (j) {
            cJSON *jid = cJSON_GetObjectItem(j, "id");
            if (jid && jid->valueint == id) {
                cJSON_Delete(j);
                return resp;  /* caller frees */
            }
            cJSON_Delete(j);
        }
        free(resp);
    }
    return NULL;
}

/* Wait for a specific CDP event (e.g. "Page.loadEventFired").
   Returns 0 on success, -1 on timeout. */
static int cdp_wait_event(WsConn *c, const char *event_name, int timeout_ms) {
    DWORD t0 = GetTickCount();
    while ((GetTickCount() - t0) < (DWORD)timeout_ms) {
        int remaining = timeout_ms - (int)(GetTickCount() - t0);
        if (remaining <= 0) break;
        if (remaining > 2000) remaining = 2000;  /* poll in 2s chunks */

        char *msg = ws_recv_timed(c, remaining);
        if (!msg) return -1;                          /* error */
        if (msg[0] == '\0') { free(msg); continue; }  /* timeout, keep waiting */

        cJSON *j = cJSON_Parse(msg);
        int found = 0;
        if (j) {
            cJSON *m = cJSON_GetObjectItem(j, "method");
            if (m && m->valuestring && strcmp(m->valuestring, event_name) == 0)
                found = 1;
            cJSON_Delete(j);
        }
        free(msg);
        if (found) return 0;
    }
    return -1;
}

/* ── CDP browser pool ───────────────────────────────────────── */

/* Pool of persistent Edge instances, dynamically sized based on CPU cores
   and available RAM.  Threads grab a slot via semaphore, use that instance,
   and release.  Instances auto-restart on crash.  No startup cost after
   first use.

   Sizing formula (from Puppeteer/Browserless production benchmarks):
     - 1 physical CPU core per headless browser instance
     - ~512 MB RAM per instance, reserve 2 GB for OS + app
     - Clamped to [2, 10]  */

#define CDP_POOL_MAX 10

typedef struct {
    BrowserProc bp;
    WsConn      ws;
    int         ready;
    int         msg_id;
    int         render_count;
    CRITICAL_SECTION lock;  /* per-instance lock */
} CdpSlot;

static CdpSlot  g_cdp_pool[CDP_POOL_MAX];
static int      g_cdp_pool_size = 0;
static HANDLE   g_cdp_sem    = NULL;
static HANDLE   g_cdp_cancel = NULL;   /* manual-reset event for shutdown */
static LONG     g_cdp_init   = 0;

/* System info stored by js_render_init(), used by cdp_pool_init() */
static int g_cdp_cpu_cores    = 4;
static int g_cdp_total_ram_mb = 8192;

void js_render_init(int cpu_cores, int total_ram_mb) {
    g_cdp_cpu_cores    = cpu_cores  > 0 ? cpu_cores  : 4;
    g_cdp_total_ram_mb = total_ram_mb > 0 ? total_ram_mb : 8192;
}

static void cdp_pool_init(void) {
    if (InterlockedCompareExchange(&g_cdp_init, 1, 0) == 0) {
        int by_cpu = g_cdp_cpu_cores;
        int by_ram = (g_cdp_total_ram_mb - 2048) / 512;
        int n = by_cpu < by_ram ? by_cpu : by_ram;
        if (n < 2)  n = 2;
        if (n > CDP_POOL_MAX) n = CDP_POOL_MAX;
        g_cdp_pool_size = n;

        g_cdp_sem    = CreateSemaphoreA(NULL, g_cdp_pool_size, g_cdp_pool_size, NULL);
        g_cdp_cancel = CreateEventA(NULL, TRUE, FALSE, NULL);  /* manual reset, non-signaled */

        for (int i = 0; i < g_cdp_pool_size; i++) {
            memset(&g_cdp_pool[i], 0, sizeof(CdpSlot));
            InitializeCriticalSection(&g_cdp_pool[i].lock);
            g_cdp_pool[i].msg_id = 10;
        }

        app_log(LOG_INFO, "CDP: pool initialized (%d slots, %d cores, %d MB RAM)",
                g_cdp_pool_size, g_cdp_cpu_cores, g_cdp_total_ram_mb);
    }
    while (!g_cdp_sem) Sleep(1);
}

/* Launch/restart a single pool slot. Must be called under slot->lock. */
static int cdp_slot_ensure(CdpSlot *slot, int slot_index) {
    if (slot->ready) {
        if (WaitForSingleObject(slot->bp.hProcess, 0) != WAIT_TIMEOUT) {
            app_log(LOG_DEBUG, "CDP: slot %d Edge exited after %d renders, restarting",
                    slot_index, slot->render_count);
            ws_close(&slot->ws);
            browser_kill(&slot->bp);
            slot->ready = 0;
            slot->render_count = 0;
        } else if (slot->render_count >= 50) {
            app_log(LOG_DEBUG, "CDP: slot %d proactive restart after %d renders",
                    slot_index, slot->render_count);
            ws_close(&slot->ws);
            browser_kill(&slot->bp);
            slot->ready = 0;
            slot->render_count = 0;
        } else {
            return 1;
        }
    }

    const char *browser = find_browser();
    if (!browser) return 0;

    memset(&slot->bp, 0, sizeof(slot->bp));
    if (browser_launch(&slot->bp, browser, slot_index) != 0) return 0;

    /* Poll /json */
    char *json_resp = NULL;
    DWORD t0 = GetTickCount();
    while ((GetTickCount() - t0) < 10000) {
        json_resp = local_http_get(slot->bp.port, L"/json", 1500);
        if (json_resp && json_resp[0] == '[') break;
        free(json_resp); json_resp = NULL;
        Sleep(150);
    }
    if (!json_resp) {
        app_log(LOG_DEBUG, "CDP: slot %d browser didn't start (port %d)", slot_index, slot->bp.port);
        browser_kill(&slot->bp);
        return 0;
    }

    /* Parse WebSocket URL */
    char ws_path[512] = "";
    {
        cJSON *tabs = cJSON_Parse(json_resp);
        free(json_resp);
        if (tabs && cJSON_IsArray(tabs)) {
            int n = cJSON_GetArraySize(tabs);
            for (int i = 0; i < n; i++) {
                cJSON *tab = cJSON_GetArrayItem(tabs, i);
                cJSON *type = cJSON_GetObjectItem(tab, "type");
                if (!type || !type->valuestring || strcmp(type->valuestring, "page") != 0)
                    continue;
                cJSON *wsurl = cJSON_GetObjectItem(tab, "webSocketDebuggerUrl");
                if (wsurl && wsurl->valuestring) {
                    const char *p = strstr(wsurl->valuestring, "/devtools/");
                    if (p) snprintf(ws_path, sizeof(ws_path), "%s", p);
                }
                break;
            }
        }
        cJSON_Delete(tabs);
    }
    if (!ws_path[0]) { browser_kill(&slot->bp); return 0; }

    if (ws_connect(&slot->ws, slot->bp.port, ws_path) != 0) { browser_kill(&slot->bp); return 0; }

    char *r = cdp_call(&slot->ws, 1, "{\"id\":1,\"method\":\"Page.enable\"}", 5000);
    if (!r) { ws_close(&slot->ws); browser_kill(&slot->bp); return 0; }
    free(r);

    slot->ready = 1;
    slot->msg_id = 10;
    app_log(LOG_INFO, "CDP: slot %d ready (port %d)", slot_index, slot->bp.port);
    return 1;
}

/* ── Public API ─────────────────────────────────────────────── */

char *js_render_page(const char *url, int timeout_ms) {
    cdp_pool_init();

    if (!g_cdp_sem || !g_cdp_cancel) {
        app_log(LOG_ERROR, "CDP: pool not initialized");
        return NULL;
    }

    /* Wait for any free slot, with cancel support.
       120s timeout handles deep queues in batch mode (e.g. 50 JS pages / 6 slots). */
    HANDLE waits[2] = { g_cdp_sem, g_cdp_cancel };
    DWORD wr = WaitForMultipleObjects(2, waits, FALSE, 120000);
    if (wr != WAIT_OBJECT_0) {
        if (wr == WAIT_OBJECT_0 + 1)
            app_log(LOG_DEBUG, "CDP: cancelled for %s", url);
        else
            app_log(LOG_DEBUG, "CDP: queue timeout (120s) for %s", url);
        return NULL;
    }

    /* Find a free slot */
    CdpSlot *slot = NULL;
    int slot_index = 0;
    for (int i = 0; i < g_cdp_pool_size; i++) {
        if (TryEnterCriticalSection(&g_cdp_pool[i].lock)) {
            slot = &g_cdp_pool[i];
            slot_index = i;
            break;
        }
    }
    if (!slot) {
        /* All slots locked (shouldn't happen with semaphore) -- wait for first */
        EnterCriticalSection(&g_cdp_pool[0].lock);
        slot = &g_cdp_pool[0];
        slot_index = 0;
    }

    char *html = NULL;

    if (!cdp_slot_ensure(slot, slot_index)) {
        LeaveCriticalSection(&slot->lock);
        ReleaseSemaphore(g_cdp_sem, 1, NULL);
        return NULL;
    }

    /* Navigate */
    int nav_id = slot->msg_id++;
    {
        char nav[4096];
        char escaped_url[2048];
        json_escape(url, escaped_url, sizeof(escaped_url));
        snprintf(nav, sizeof(nav),
            "{\"id\":%d,\"method\":\"Page.navigate\",\"params\":{\"url\":\"%s\"}}",
            nav_id, escaped_url);
        char *r = cdp_call(&slot->ws, nav_id, nav, 5000);
        if (!r) { slot->ready = 0; goto done; }
        free(r);
    }

    /* Wait for page load, then extra sleep for JS hydration (React/Next.js) */
    if (cdp_wait_event(&slot->ws, "Page.loadEventFired", timeout_ms - 2000) != 0)
        app_log(LOG_DEBUG, "CDP: load timeout for %s -- extracting anyway", url);

    Sleep(2000);  /* post-load grace for JS hydration (was 800ms, too short for SPAs) */

    /* Extract DOM */
    {
        int eval_id = slot->msg_id++;
        char eval_msg[256];
        snprintf(eval_msg, sizeof(eval_msg),
            "{\"id\":%d,\"method\":\"Runtime.evaluate\","
            "\"params\":{\"expression\":\"document.documentElement.outerHTML\","
            "\"returnByValue\":true}}", eval_id);
        char *r = cdp_call(&slot->ws, eval_id, eval_msg, 10000);
        if (r) {
            cJSON *j = cJSON_Parse(r);
            if (j) {
                cJSON *res   = cJSON_GetObjectItem(j, "result");
                cJSON *inner = res ? cJSON_GetObjectItem(res, "result") : NULL;
                cJSON *val   = inner ? cJSON_GetObjectItem(inner, "value") : NULL;
                if (val && val->valuestring && val->valuestring[0])
                    html = str_duplicate(val->valuestring);
                cJSON_Delete(j);
            }
            free(r);
        } else {
            slot->ready = 0;
        }
    }

    slot->render_count++;

    /* Navigate back to about:blank to keep Edge alive.
       Without this, Edge headless exits after the page finishes loading. */
    if (slot->ready) {
        int blank_id = slot->msg_id++;
        char blank_nav[128];
        snprintf(blank_nav, sizeof(blank_nav),
            "{\"id\":%d,\"method\":\"Page.navigate\",\"params\":{\"url\":\"about:blank\"}}",
            blank_id);
        char *br = cdp_call(&slot->ws, blank_id, blank_nav, 3000);
        free(br);  /* NULL-safe */
    }

done:
    LeaveCriticalSection(&slot->lock);
    ReleaseSemaphore(g_cdp_sem, 1, NULL);

    if (html)
        app_log(LOG_DEBUG, "CDP: rendered %zu bytes for %s", strlen(html), url);
    else
        app_log(LOG_DEBUG, "CDP: failed to render %s", url);
    return html;
}

void js_render_cancel(void) {
    if (g_cdp_cancel)
        SetEvent(g_cdp_cancel);
}

void js_render_shutdown(void) {
    if (!g_cdp_pool_size) return;

    /* Signal all waiting threads to give up */
    if (g_cdp_cancel) SetEvent(g_cdp_cancel);
    Sleep(500);

    /* Force-close all slots */
    int total_renders = 0;
    for (int i = 0; i < g_cdp_pool_size; i++) {
        EnterCriticalSection(&g_cdp_pool[i].lock);
        total_renders += g_cdp_pool[i].render_count;
        if (g_cdp_pool[i].ready) {
            ws_close(&g_cdp_pool[i].ws);
            browser_kill(&g_cdp_pool[i].bp);
            g_cdp_pool[i].ready = 0;
        }
        LeaveCriticalSection(&g_cdp_pool[i].lock);
        DeleteCriticalSection(&g_cdp_pool[i].lock);
    }

    if (g_cdp_sem)    { CloseHandle(g_cdp_sem);    g_cdp_sem    = NULL; }
    if (g_cdp_cancel) { CloseHandle(g_cdp_cancel); g_cdp_cancel = NULL; }

    app_log(LOG_INFO, "CDP: pool shutdown (%d slots, %d total renders)",
            g_cdp_pool_size, total_renders);

    g_cdp_pool_size = 0;
    InterlockedExchange(&g_cdp_init, 0);
}
