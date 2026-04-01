#ifndef CONFIG_H
#define CONFIG_H

#define MAX_KEY_LEN     64
#define MAX_VAL_LEN     2100
#define MAX_ENV_ENTRIES  128

/* Multiple API keys support */
#define MAX_API_KEYS    64

typedef struct {
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
} EnvEntry;

typedef struct {
    char    key[128];
    int     requests_today;
    int     daily_limit;
    int     active;
    int     mapped_proxy;       /* index into proxy pool, -1 = none */
} ApiKeyEntry;

typedef struct {
    EnvEntry entries[MAX_ENV_ENTRIES];
    int count;

    /* Serper API keys (multiple) */
    ApiKeyEntry api_keys[MAX_API_KEYS];
    int         api_key_count;
    int         api_key_next;   /* round-robin index */
    int         daily_limit;    /* per-key limit, default 100 */

    /* Convenience pointers */
    const char *serper_key;     /* first key, for compat */
} Config;

int  config_load(Config *cfg, const char *env_path);
const char *config_get(const Config *cfg, const char *key);
void config_set(Config *cfg, const char *key, const char *value);
int  config_save(const Config *cfg, const char *env_path);

/* Multi-key management */
int  config_add_api_key(Config *cfg, const char *key);
int  config_remove_api_key(Config *cfg, int index);
const char *config_next_api_key(Config *cfg);
int  config_total_remaining(Config *cfg);
void config_auto_map_proxies(Config *cfg, int proxy_count);
void config_shutdown(void);

#endif
