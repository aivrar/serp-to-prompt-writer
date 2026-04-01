#include "config.h"
#include "app_log.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_key_lock;
static int g_key_lock_init = 0;
#endif

/* ── Base config (ported from E:\searcher) ───────────────────── */

int config_load(Config *cfg, const char *env_path) {
    memset(cfg, 0, sizeof(Config));
    cfg->daily_limit = 100;  /* Serper free tier default */

#ifdef _WIN32
    if (!g_key_lock_init) {
        InitializeCriticalSection(&g_key_lock);
        g_key_lock_init = 1;
    }
#endif

    FILE *f = fopen(env_path, "r");
    if (!f) { app_log(LOG_WARN, "Config: %s not found, using defaults", env_path); return -1; }

    char line[2200];
    while (fgets(line, sizeof(line), f) && cfg->count < MAX_ENV_ENTRIES) {
        str_trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        str_trim(key);
        str_trim(val);

        /* Strip quotes */
        size_t vlen = strlen(val);
        if (vlen >= 2 && ((val[0] == '"' && val[vlen-1] == '"') ||
                          (val[0] == '\'' && val[vlen-1] == '\''))) {
            val[vlen-1] = '\0';
            val++;
        }

        EnvEntry *e = &cfg->entries[cfg->count];
        snprintf(e->key, MAX_KEY_LEN, "%s", key);
        snprintf(e->value, MAX_VAL_LEN, "%s", val);
        cfg->count++;
    }
    fclose(f);

    /* Set convenience pointers */
    cfg->serper_key = config_get(cfg, "SERPER_API");

    /* Load all API keys: SERPER_API (primary) + SERPER_API_N (additional) */
    if (cfg->serper_key && cfg->serper_key[0])
        config_add_api_key(cfg, cfg->serper_key);

    for (int n = 2; n <= MAX_API_KEYS; n++) {
        char keyname[32];
        snprintf(keyname, sizeof(keyname), "SERPER_API_%d", n);
        const char *val = config_get(cfg, keyname);
        if (!val || !val[0]) break;
        config_add_api_key(cfg, val);
    }

    app_log(LOG_INFO, "Config: loaded %d entries, %d API keys from %s",
            cfg->count, cfg->api_key_count, env_path);
    return 0;
}

const char *config_get(const Config *cfg, const char *key) {
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].value;
    }
    return NULL;
}

void config_set(Config *cfg, const char *key, const char *value) {
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0) {
            snprintf(cfg->entries[i].value, MAX_VAL_LEN, "%s", value);
            return;
        }
    }
    if (cfg->count < MAX_ENV_ENTRIES) {
        EnvEntry *e = &cfg->entries[cfg->count];
        snprintf(e->key, MAX_KEY_LEN, "%s", key);
        snprintf(e->value, MAX_VAL_LEN, "%s", value);
        cfg->count++;
    }
}

int config_save(const Config *cfg, const char *env_path) {
    FILE *f = fopen(env_path, "w");
    if (!f) { app_log(LOG_ERROR, "Config: failed to write %s", env_path); return -1; }

    /* Write all entries EXCEPT SERPER_API / SERPER_API_N (we'll write fresh ones) */
    for (int i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, "SERPER_API") == 0) continue;
        if (strncmp(cfg->entries[i].key, "SERPER_API_", 11) == 0) continue;
        fprintf(f, "%s=\"%s\"\n", cfg->entries[i].key, cfg->entries[i].value);
    }

    /* Write current api_keys[] as SERPER_API (first) + SERPER_API_N (rest) */
    for (int i = 0; i < cfg->api_key_count; i++) {
        if (i == 0)
            fprintf(f, "SERPER_API=\"%s\"\n", cfg->api_keys[i].key);
        else
            fprintf(f, "SERPER_API_%d=\"%s\"\n", i + 1, cfg->api_keys[i].key);
    }

    fclose(f);
    return 0;
}

/* ── Multi-key management (new for this app) ─────────────────── */

int config_add_api_key(Config *cfg, const char *key) {
    if (!key || !key[0]) return -1;
    if (cfg->api_key_count >= MAX_API_KEYS) return -1;

    /* Dedup */
    for (int i = 0; i < cfg->api_key_count; i++)
        if (strcmp(cfg->api_keys[i].key, key) == 0) return 0;

    ApiKeyEntry *e = &cfg->api_keys[cfg->api_key_count];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->requests_today = 0;
    e->daily_limit = cfg->daily_limit;
    e->active = 1;
    e->mapped_proxy = -1;
    cfg->api_key_count++;
    return 0;
}

int config_remove_api_key(Config *cfg, int index) {
    if (index < 0 || index >= cfg->api_key_count) return -1;
    for (int i = index; i < cfg->api_key_count - 1; i++)
        cfg->api_keys[i] = cfg->api_keys[i + 1];
    cfg->api_key_count--;
    return 0;
}

const char *config_next_api_key(Config *cfg) {
    if (cfg->api_key_count == 0) return NULL;

#ifdef _WIN32
    if (g_key_lock_init) EnterCriticalSection(&g_key_lock);
#endif

    const char *result = NULL;
    /* Round-robin, skip exhausted/disabled keys */
    for (int tries = 0; tries < cfg->api_key_count; tries++) {
        int idx = cfg->api_key_next % cfg->api_key_count;
        cfg->api_key_next = (cfg->api_key_next + 1) % cfg->api_key_count;
        ApiKeyEntry *e = &cfg->api_keys[idx];
        if (e->active && e->requests_today < e->daily_limit) {
            e->requests_today++;
            result = e->key;
            break;
        }
    }

#ifdef _WIN32
    if (g_key_lock_init) LeaveCriticalSection(&g_key_lock);
#endif
    return result;
}

int config_total_remaining(Config *cfg) {
    int total = 0;
    for (int i = 0; i < cfg->api_key_count; i++) {
        if (cfg->api_keys[i].active) {
            int rem = cfg->api_keys[i].daily_limit - cfg->api_keys[i].requests_today;
            if (rem > 0) total += rem;
        }
    }
    return total;
}

void config_auto_map_proxies(Config *cfg, int proxy_count) {
    if (cfg->api_key_count == 0 || proxy_count == 0) return;
    for (int i = 0; i < cfg->api_key_count; i++)
        cfg->api_keys[i].mapped_proxy = i % proxy_count;
    app_log(LOG_INFO, "Auto-mapped %d keys to %d proxies", cfg->api_key_count, proxy_count);
}

void config_shutdown(void) {
#ifdef _WIN32
    if (g_key_lock_init) {
        DeleteCriticalSection(&g_key_lock);
        g_key_lock_init = 0;
    }
#endif
}
