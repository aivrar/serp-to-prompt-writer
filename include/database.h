#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    sqlite3 *db;
    sqlite3 *settings_db;             /* separate DB for settings (survives purge) */
#ifdef _WIN32
    CRITICAL_SECTION write_lock;
#endif
    /* Cached prepared statements for hot-path functions */
    sqlite3_stmt *stmt_update_url;    /* cached for db_update_serp_url_full */
    sqlite3_stmt *stmt_save_heading;  /* cached for db_save_serp_headings inner loop */
    sqlite3_stmt *stmt_save_keyword;  /* cached for db_save_serp_page_keywords inner loop */
} Database;

int  db_open(Database *db, const char *path);
void db_close(Database *db);

/* Analysis session persistence — saves EVERYTHING per keyword */
int  db_save_session(Database *db, const char *keyword, const char *json_data);
int  db_load_session(Database *db, const char *keyword, char **json_data);
int  db_list_sessions(Database *db, char sessions[][256], int max_count);
int  db_list_sessions_rich(Database *db, char keywords[][256], char dates[][32],
                           char **json_out, int max_count);
int  db_delete_session(Database *db, const char *keyword);
int  db_delete_all_sessions(Database *db);

/* Keyword position tracking over time (content decay detection) */
int  db_save_position(Database *db, const char *keyword, const char *domain,
                      int position, int word_count, int heading_count);

/* Outbound link storage -- filtered OBL URLs saved per scraped page */
int  db_save_obl_links(Database *db, const char *source_url, const char *keyword,
                       const char **urls, const char **anchors, int count);
int  db_get_obl_links(Database *db, const char *source_url,
                      char urls[][2048], char anchors[][256], int max_count);

/* Settings persistence */
int  db_save_setting(Database *db, const char *key, const char *value);
int  db_load_setting(Database *db, const char *key, char *value, int max_len);

/* SERP URL queue -- crash-safe, tracks scrape status */
int  db_save_serp_url(Database *db, const char *url, const char *keyword,
                      const char *domain, const char *title, const char *snippet,
                      int position, int page);
int  db_get_pending_urls(Database *db, char urls[][2048], char keywords[][256],
                         char domains[][256], int max_count);
int  db_get_pending_urls_rich(Database *db, char urls[][2048], char keywords[][256],
                              char domains[][256], char titles[][512],
                              char snippets[][512], int *positions, int max_count);
int  db_count_pending_urls(Database *db);
int  db_delete_pending_url(Database *db, const char *url, const char *keyword);
int  db_delete_all_pending_urls(Database *db);

/* ── DB-backed Results (persistent scraped page data) ────────── */

typedef struct {
    char   url[2048];
    char   keyword[256];
    char   domain[256];
    char   title[512];
    int    serp_position;
    int    word_count;
    int    heading_count;
    int    outbound_count;
    int    internal_count;
    double scrape_time_secs;
    int    js_fallback;
    int    is_crawled;
    char   source_url[2048];
    char   status[16];
} DbResultRow;

/* Get scraped results. keyword=NULL for all. Ordered by keyword, position. */
int  db_get_results(Database *db, const char *keyword, DbResultRow *rows, int max_count);
int  db_delete_result(Database *db, const char *url, const char *keyword);

/* Extended scrape status update (saves all metadata) */
int  db_update_serp_url_full(Database *db, const char *url, const char *keyword,
                             const char *status, int word_count, int heading_count,
                             int outbound_count, int internal_count,
                             double scrape_time_secs, int js_fallback, int is_crawled,
                             const char *source_url);

/* Per-page headings storage */
int  db_save_serp_headings(Database *db, const char *url, const char *keyword,
                           const char (*tags)[8], const char (*texts)[512],
                           const int *levels, int count);
int  db_get_serp_headings(Database *db, const char *url, const char *keyword,
                          char tags[][8], char texts[][512], int *levels, int max_count);

/* Per-page keywords storage */
int  db_save_serp_page_keywords(Database *db, const char *url, const char *keyword,
                                const char (*kws)[64], int count);
int  db_get_serp_page_keywords(Database *db, const char *url, const char *keyword,
                               char kws[][64], int max_count);

/* PAA + Related searches (from Serper API, stored per keyword) */
int  db_save_paa(Database *db, const char *keyword,
                 const char (*questions)[512], const char (*snippets)[1024],
                 const char (*links)[2048], int count);
int  db_load_paa(Database *db, const char *keyword,
                 char questions[][512], char snippets[][1024], char links[][2048],
                 int max_count);
int  db_save_related(Database *db, const char *keyword,
                     const char (*queries)[256], int count);
int  db_load_related(Database *db, const char *keyword,
                     char queries[][256], int max_count);

/* Tuning parameters (set from config) */
extern int g_db_cache_mb;
extern int g_db_mmap_mb;
extern int g_db_busy_timeout;

#endif
