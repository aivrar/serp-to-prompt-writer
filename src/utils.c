#include "utils.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ── Configurable HTTP settings ──────────────────────────────── */

int  g_http_get_timeout   = 10;
int  g_http_post_timeout  = 30;
int  g_http_max_redirects = 5;
int  g_http_retry_count   = 2;
int  g_http_retry_base_ms = 1000;
int  g_http_proxy_ssl_verify = 0;  /* 0=off (default for proxy compat), 1=on */
char g_http_user_agent[512] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

/* ── String utilities ────────────────────────────────────────── */

void str_trim(char *s) {
    if (!s) return;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
}

void str_to_lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

int str_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s), sfxlen = strlen(suffix);
    if (sfxlen > slen) return 0;
    return strcmp(s + slen - sfxlen, suffix) == 0;
}

int str_contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

int str_contains_i(const char *haystack, const char *needle) {
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen > hlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i+j]) != tolower((unsigned char)needle[j])) {
                match = 0; break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

char *str_duplicate(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *dup = (char *)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/* ── JSON utilities ──────────────────────────────────────────── */

void json_escape(const char *src, char *dst, int max) {
    int j = 0;
    for (int i = 0; src[i] && j < max - 6; i++) {
        switch (src[i]) {
            case '"':  dst[j++]='\\'; dst[j++]='"'; break;
            case '\\': dst[j++]='\\'; dst[j++]='\\'; break;
            case '\n': dst[j++]='\\'; dst[j++]='n'; break;
            case '\r': dst[j++]='\\'; dst[j++]='r'; break;
            case '\t': dst[j++]='\\'; dst[j++]='t'; break;
            case '\b': dst[j++]='\\'; dst[j++]='b'; break;
            case '\f': dst[j++]='\\'; dst[j++]='f'; break;
            default:
                if ((unsigned char)src[i] < 0x20) { /* other control chars */
                    j += snprintf(dst+j, max-j, "\\u%04x", (unsigned char)src[i]);
                } else {
                    dst[j++] = src[i];
                }
        }
    }
    dst[j] = '\0';
}

/* ── URL utilities ───────────────────────────────────────────── */

void url_extract_domain(const char *url, char *domain, int max_len) {
    domain[0] = '\0';
    const char *p = strstr(url, "://");
    if (p) p += 3; else p = url;
    const char *end = strchr(p, '/');
    int len = end ? (int)(end - p) : (int)strlen(p);
    if (len >= max_len) len = max_len - 1;
    memcpy(domain, p, len);
    domain[len] = '\0';
    url_strip_www(domain);
}

void url_strip_www(char *domain) {
    if (strncmp(domain, "www.", 4) == 0)
        memmove(domain, domain + 4, strlen(domain + 4) + 1);
}

void url_resolve_relative(const char *base, const char *relative,
                          char *resolved, int max_len) {
    if (strncmp(relative, "http://", 7) == 0 || strncmp(relative, "https://", 8) == 0) {
        snprintf(resolved, max_len, "%s", relative);
        return;
    }
    if (relative[0] == '/') {
        /* Absolute path -- combine with base scheme+host */
        const char *p = strstr(base, "://");
        if (p) {
            p += 3;
            const char *slash = strchr(p, '/');
            int host_len = slash ? (int)(slash - base) : (int)strlen(base);
            snprintf(resolved, max_len, "%.*s%s", host_len, base, relative);
        } else {
            snprintf(resolved, max_len, "%s", relative);
        }
    } else {
        /* Relative path */
        char base_dir[2048];
        snprintf(base_dir, sizeof(base_dir), "%s", base);
        char *last_slash = strrchr(base_dir, '/');
        if (last_slash) *(last_slash + 1) = '\0';
        snprintf(resolved, max_len, "%s%s", base_dir, relative);
    }
}

int url_is_valid(const char *url) {
    return (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

void url_strip_tracking_params(const char *url, char *clean, int max_len) {
    static const char *tracking[] = {
        "utm_source", "utm_medium", "utm_campaign", "utm_term", "utm_content",
        "fbclid", "gclid", "gclsrc", "dclid", "msclkid",
        "mc_cid", "mc_eid", "_ga", "_gl", NULL
    };

    const char *q = strchr(url, '?');
    if (!q) { snprintf(clean, max_len, "%s", url); return; }

    /* Copy base URL */
    int base_len = (int)(q - url);
    if (base_len >= max_len) base_len = max_len - 1;
    memcpy(clean, url, base_len);

    /* Parse and filter query params (with overflow tracking) */
    char params[2048];
    snprintf(params, sizeof(params), "%s", q + 1);
    char filtered[2048];
    int fpos = 0;
    filtered[0] = '\0';
    char *saveptr = NULL;
    char *tok = strtok_s(params, "&", &saveptr);
    while (tok) {
        int skip = 0;
        for (int i = 0; tracking[i]; i++) {
            size_t tlen = strlen(tracking[i]);
            if (strncmp(tok, tracking[i], tlen) == 0 && tok[tlen] == '=') {
                skip = 1; break;
            }
        }
        if (!skip) {
            int remaining = (int)sizeof(filtered) - fpos;
            if (remaining <= 1) break;
            if (fpos > 0) {
                fpos += snprintf(filtered + fpos, remaining, "&%s", tok);
            } else {
                fpos += snprintf(filtered + fpos, remaining, "%s", tok);
            }
        }
        tok = strtok_s(NULL, "&", &saveptr);
    }

    if (filtered[0])
        snprintf(clean + base_len, max_len - base_len, "?%s", filtered);
    else
        clean[base_len] = '\0';
}

/* ── HTTP client (libcurl, fresh handle per request) ────────── */

/* Debug callback: logs curl verbose output through app_log */
static int curl_debug_cb(CURL *handle, curl_infotype type, char *data, size_t size, void *userp) {
    (void)handle; (void)userp;
    if (type == CURLINFO_TEXT && size > 0) {
        char line[512];
        int len = (int)size;
        if (len > 500) len = 500;
        memcpy(line, data, len);
        line[len] = '\0';
        /* strip trailing newline */
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > 0) app_log(LOG_DEBUG, "CURL: %s", line);
    }
    return 0;
}

static int g_curl_verbose = 0;  /* set to 1 for curl wire diagnosis */

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    HttpBuffer *buf = (HttpBuffer *)userp;
    char *ptr = (char *)realloc(buf->data, buf->size + total + 1);
    if (!ptr) return 0;
    buf->data = ptr;
    memcpy(&buf->data[buf->size], contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

int http_get(const char *url, HttpBuffer *buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    buf->data = NULL;
    buf->size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_http_get_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, http_random_ua());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)g_http_max_redirects);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  /* enable in-memory cookie engine */

    /* Browser-like headers to reduce bot detection */
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua: \"Chromium\";v=\"134\", \"Not:A-Brand\";v=\"24\", \"Google Chrome\";v=\"134\"");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua-mobile: ?0");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua-platform: \"Windows\"");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (g_curl_verbose) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        app_log(LOG_DEBUG, "HTTP GET failed: %s -- %s", url, curl_easy_strerror(res));
        http_buffer_free(buf);
        return -1;
    }
    return 0;
}

int http_get_proxy(const char *url, const char *proxy_url, HttpBuffer *buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    buf->data = NULL;
    buf->size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_http_get_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, http_random_ua());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)g_http_max_redirects);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, (long)g_http_proxy_ssl_verify);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8");
    hdrs = curl_slist_append(hdrs, "Accept-Language: en-US,en;q=0.9");
    hdrs = curl_slist_append(hdrs, "Upgrade-Insecure-Requests: 1");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua: \"Chromium\";v=\"134\", \"Not:A-Brand\";v=\"24\", \"Google Chrome\";v=\"134\"");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua-mobile: ?0");
    hdrs = curl_slist_append(hdrs, "sec-ch-ua-platform: \"Windows\"");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Dest: document");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Mode: navigate");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-Site: none");
    hdrs = curl_slist_append(hdrs, "Sec-Fetch-User: ?1");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    if (proxy_url && proxy_url[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        http_buffer_free(buf);
        return -1;
    }
    return 0;
}

int http_post_json(const char *url, const char *json_body,
                   const char *api_key, HttpBuffer *buf) {
    return http_post_json_proxy(url, json_body, api_key, NULL, buf);
}

int http_post_json_proxy(const char *url, const char *json_body,
                         const char *api_key, const char *proxy_url,
                         HttpBuffer *buf) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    buf->data = NULL;
    buf->size = 0;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (api_key && api_key[0]) {
        char auth[512];
        snprintf(auth, sizeof(auth), "X-API-KEY: %s", api_key);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_http_post_timeout);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, http_random_ua());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");

    if (proxy_url && proxy_url[0])
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        app_log(LOG_DEBUG, "HTTP POST failed: %s -- %s", url, curl_easy_strerror(res));
        http_buffer_free(buf);
        return -1;
    }
    return 0;
}

void http_buffer_free(HttpBuffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
}

int http_get_retry(const char *url, HttpBuffer *buf, int max_attempts) {
    if (max_attempts <= 0) max_attempts = g_http_retry_count;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (http_get(url, buf) == 0) return 0;
        if (attempt < max_attempts - 1) {
            int wait_ms = g_http_retry_base_ms * (1 << attempt);
            if (wait_ms > 8000) wait_ms = 8000;
            app_log(LOG_WARN, "HTTP retry %d/%d in %ds: %s",
                    attempt+1, max_attempts, wait_ms/1000, url);
            Sleep(wait_ms);
        }
    }
    return -1;
}

int http_get_proxy_retry(const char *url, const char *proxy_url, HttpBuffer *buf, int max_attempts) {
    if (max_attempts <= 0) max_attempts = g_http_retry_count;
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        if (http_get_proxy(url, proxy_url, buf) == 0) return 0;
        if (attempt < max_attempts - 1) {
            int wait_ms = g_http_retry_base_ms * (1 << attempt);
            if (wait_ms > 8000) wait_ms = 8000;
            app_log(LOG_WARN, "HTTP proxy retry %d/%d in %ds: %s",
                    attempt+1, max_attempts, wait_ms/1000, url);
            Sleep(wait_ms);
        }
    }
    return -1;
}

void http_thread_cleanup(void) {
    /* no-op: handles are per-request now */
}

/* ── File utilities ──────────────────────────────────────────── */

int file_exists(const char *path) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES);
#else
    return access(path, F_OK) == 0;
#endif
}

int ensure_directory(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

void set_exe_directory(void) {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    SetCurrentDirectoryA(path);
#endif
}

/* ── User-Agent rotation ─────────────────────────────────────── */

#define MAX_USER_AGENTS 100

static char g_ua_pool[MAX_USER_AGENTS][512];
static int  g_ua_pool_count = 0;

int http_load_user_agents(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        app_log(LOG_WARN, "UA file not found: %s -- using default UA", path);
        return 0;
    }
    char line[512];
    g_ua_pool_count = 0;
    while (fgets(line, sizeof(line), f) && g_ua_pool_count < MAX_USER_AGENTS) {
        str_trim(line);
        if (line[0] && line[0] != '#') {
            snprintf(g_ua_pool[g_ua_pool_count], 512, "%s", line);
            g_ua_pool_count++;
        }
    }
    fclose(f);
    app_log(LOG_INFO, "Loaded %d user agents from %s", g_ua_pool_count, path);
    return g_ua_pool_count;
}

/* Thread-safe random number (uses rand_s on Windows, rand elsewhere) */
static unsigned int safe_rand(void) {
#ifdef _WIN32
    unsigned int val;
    if (rand_s(&val) == 0) return val;
#endif
    return (unsigned int)rand();
}

const char *http_random_ua(void) {
    if (g_ua_pool_count <= 0) return g_http_user_agent;  /* fallback to default */
    return g_ua_pool[safe_rand() % g_ua_pool_count];
}

int http_ua_count(void) { return g_ua_pool_count; }

/* ── Shuffle (Fisher-Yates) ──────────────────────────────────── */

void shuffle_ints(int *array, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = safe_rand() % (i + 1);
        int tmp = array[i];
        array[i] = array[j];
        array[j] = tmp;
    }
}
