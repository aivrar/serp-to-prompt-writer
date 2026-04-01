#include "proxy.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

void proxy_init(ProxyPool *pp) {
    memset(pp, 0, sizeof(ProxyPool));
    pp->timeout_sec = PROXY_TIMEOUT_SEC;
    pp->max_retries = PROXY_MAX_RETRIES;
    pp->enabled = 0;
#ifdef _WIN32
    InitializeCriticalSection(&pp->pool_lock);
#endif
}

void proxy_shutdown(ProxyPool *pp) {
#ifdef _WIN32
    DeleteCriticalSection(&pp->pool_lock);
#endif
}

int proxy_add(ProxyPool *pp, const char *addr, ProxyProtocol proto) {
    if (!addr || !addr[0]) return -1;
#ifdef _WIN32
    EnterCriticalSection(&pp->pool_lock);
#endif
    if (pp->pool_count >= PROXY_MAX_POOL) {
#ifdef _WIN32
        LeaveCriticalSection(&pp->pool_lock);
#endif
        return -1;
    }
    /* Dedup */
    for (int i = 0; i < pp->pool_count; i++) {
        if (strcmp(pp->pool[i].addr, addr) == 0) {
#ifdef _WIN32
            LeaveCriticalSection(&pp->pool_lock);
#endif
            return 0;
        }
    }
    ProxyEntry *e = &pp->pool[pp->pool_count];
    snprintf(e->addr, sizeof(e->addr), "%s", addr);
    e->protocol = proto;
    e->success_count = 0;
    e->fail_count = 0;
    e->active = 1;
    pp->pool_count++;
#ifdef _WIN32
    LeaveCriticalSection(&pp->pool_lock);
#endif
    return 0;
}

int proxy_add_list(ProxyPool *pp, const char *text, ProxyProtocol proto) {
    if (!text) return 0;
    int added = 0;
    char *copy = str_duplicate(text);
    if (!copy) return 0;
    char *saveptr = NULL;
    char *line = strtok_s(copy, "\n\r", &saveptr);
    while (line) {
        str_trim(line);
        if (line[0] && line[0] != '#') {
            if (proxy_add(pp, line, proto) == 0)
                added++;
        }
        line = strtok_s(NULL, "\n\r", &saveptr);
    }
    free(copy);
    if (added > 0)
        app_log(LOG_INFO, "Added %d proxies (total: %d)", added, pp->pool_count);
    return added;
}

int proxy_get_next(ProxyPool *pp, char *out_url, int url_max) {
    if (!pp->enabled || pp->pool_count == 0) return -1;

#ifdef _WIN32
    long idx = InterlockedIncrement(&pp->pool_next) - 1;
#else
    long idx = __sync_fetch_and_add(&pp->pool_next, 1);
#endif

    /* Find next active proxy */
    for (int tries = 0; tries < pp->pool_count; tries++) {
        int i = (int)((idx + tries) % pp->pool_count);
        if (!pp->pool[i].active) continue;

        const char *scheme;
        switch (pp->pool[i].protocol) {
            case PROXY_SOCKS5: scheme = "socks5://"; break;
            case PROXY_HTTPS:  scheme = "https://"; break;
            default:           scheme = "http://"; break;
        }

        if (pp->username[0] && pp->password[0])
            snprintf(out_url, url_max, "%s%s:%s@%s",
                     scheme, pp->username, pp->password, pp->pool[i].addr);
        else
            snprintf(out_url, url_max, "%s%s", scheme, pp->pool[i].addr);
        return i;
    }
    return -1;
}

void proxy_mark_success(ProxyPool *pp, int idx) {
    if (idx >= 0 && idx < pp->pool_count) {
        InterlockedIncrement((volatile long*)&pp->pool[idx].success_count);
    }
}

void proxy_mark_fail(ProxyPool *pp, int idx) {
    if (idx >= 0 && idx < pp->pool_count) {
        InterlockedIncrement((volatile long*)&pp->pool[idx].fail_count);
        /* Auto-disable after too many failures */
        int total = pp->pool[idx].success_count + pp->pool[idx].fail_count;
        if (total >= 10) {
            float rate = (float)pp->pool[idx].success_count / total;
            if (rate < 0.2f) {
                pp->pool[idx].active = 0;
                app_log(LOG_WARN, "Proxy %s disabled (%.0f%% success)",
                        pp->pool[idx].addr, rate * 100);
            }
        }
    }
}

int proxy_remove(ProxyPool *pp, int index) {
    if (index < 0 || index >= pp->pool_count) return -1;
#ifdef _WIN32
    EnterCriticalSection(&pp->pool_lock);
#endif
    for (int i = index; i < pp->pool_count - 1; i++)
        pp->pool[i] = pp->pool[i + 1];
    pp->pool_count--;
#ifdef _WIN32
    LeaveCriticalSection(&pp->pool_lock);
#endif
    return 0;
}

int proxy_count(ProxyPool *pp) { return pp->pool_count; }

void proxy_set_credentials(ProxyPool *pp, const char *user, const char *pass) {
    snprintf(pp->username, sizeof(pp->username), "%s", user ? user : "");
    snprintf(pp->password, sizeof(pp->password), "%s", pass ? pass : "");
}

int proxy_save(const ProxyPool *pp, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { app_log(LOG_ERROR, "Proxy: failed to write %s", path); return -1; }
    for (int i = 0; i < pp->pool_count; i++)
        fprintf(f, "%s\n", pp->pool[i].addr);
    fclose(f);
    app_log(LOG_INFO, "Proxy: saved %d entries to %s", pp->pool_count, path);
    return 0;
}

int proxy_load_file(ProxyPool *pp, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        if (line[0] && line[0] != '#')
            proxy_add(pp, line, PROXY_HTTP);
    }
    fclose(f);
    app_log(LOG_INFO, "Proxy: loaded %d entries from %s", pp->pool_count, path);
    return 0;
}


