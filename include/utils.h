#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/* ── String utilities ────────────────────────────────────────── */
void str_trim(char *s);
void str_to_lower(char *s);
int  str_starts_with(const char *s, const char *prefix);
int  str_ends_with(const char *s, const char *suffix);
int  str_contains(const char *haystack, const char *needle);
int  str_contains_i(const char *haystack, const char *needle); /* case-insensitive */
char *str_duplicate(const char *s);  /* caller frees */

/* ── URL utilities ───────────────────────────────────────────── */
void url_extract_domain(const char *url, char *domain, int max_len);
void url_strip_www(char *domain);
void url_resolve_relative(const char *base, const char *relative,
                          char *resolved, int max_len);
int  url_is_valid(const char *url);
void url_strip_tracking_params(const char *url, char *clean, int max_len);

/* ── HTTP client (libcurl, thread-local) ─────────────────────── */
typedef struct {
    char   *data;
    size_t  size;
} HttpBuffer;

int  http_get(const char *url, HttpBuffer *buf);
int  http_get_proxy(const char *url, const char *proxy_url, HttpBuffer *buf);
int  http_post_json(const char *url, const char *json_body,
                    const char *api_key, HttpBuffer *buf);
int  http_post_json_proxy(const char *url, const char *json_body,
                          const char *api_key, const char *proxy_url, HttpBuffer *buf);
void http_buffer_free(HttpBuffer *buf);

/* Retry with exponential backoff */
int  http_get_retry(const char *url, HttpBuffer *buf, int max_attempts);
int  http_get_proxy_retry(const char *url, const char *proxy_url, HttpBuffer *buf, int max_attempts);

/* Thread-local cleanup (call before thread exit) */
void http_thread_cleanup(void);

/* Configurable HTTP settings */
extern int  g_http_get_timeout;
extern int  g_http_post_timeout;
extern int  g_http_max_redirects;
extern int  g_http_retry_count;
extern int  g_http_retry_base_ms;
extern char g_http_user_agent[512];
extern int  g_http_proxy_ssl_verify;

/* User-Agent rotation — loads from data/user_agents.txt */
int  http_load_user_agents(const char *path);
const char *http_random_ua(void);
int  http_ua_count(void);

/* Array shuffle (Fisher-Yates) for URL randomization */
void shuffle_ints(int *array, int count);

/* ── JSON utilities ──────────────────────────────────────────── */
void json_escape(const char *src, char *dst, int max);

/* ── File utilities ──────────────────────────────────────────── */
int  file_exists(const char *path);
int  ensure_directory(const char *path);
void set_exe_directory(void);

#endif
