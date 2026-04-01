#ifndef PROXY_H
#define PROXY_H

#ifdef _WIN32
#include <windows.h>
#endif
#include "utils.h"

#define PROXY_MAX_POOL      4096
#define PROXY_TIMEOUT_SEC   5
#define PROXY_MAX_RETRIES   10

typedef enum {
    PROXY_HTTP = 0,
    PROXY_HTTPS,
    PROXY_SOCKS5
} ProxyProtocol;

typedef struct {
    char            addr[64];       /* "IP:PORT" */
    ProxyProtocol   protocol;
    int             success_count;
    int             fail_count;
    int             active;
} ProxyEntry;

typedef struct {
    ProxyEntry      pool[PROXY_MAX_POOL];
    int             pool_count;
    volatile long   pool_next;
#ifdef _WIN32
    CRITICAL_SECTION pool_lock;
#endif

    /* Universal credentials */
    char            username[128];
    char            password[128];

    int             enabled;
    int             timeout_sec;
    int             max_retries;
} ProxyPool;

void proxy_init(ProxyPool *pp);
void proxy_shutdown(ProxyPool *pp);

int  proxy_add(ProxyPool *pp, const char *addr, ProxyProtocol proto);
int  proxy_remove(ProxyPool *pp, int index);
int  proxy_add_list(ProxyPool *pp, const char *text, ProxyProtocol proto); /* newline-separated */
int  proxy_get_next(ProxyPool *pp, char *out_url, int url_max);
void proxy_mark_success(ProxyPool *pp, int idx);
void proxy_mark_fail(ProxyPool *pp, int idx);
int  proxy_count(ProxyPool *pp);

void proxy_set_credentials(ProxyPool *pp, const char *user, const char *pass);
int  proxy_save(const ProxyPool *pp, const char *path);
int  proxy_load_file(ProxyPool *pp, const char *path);


#endif
