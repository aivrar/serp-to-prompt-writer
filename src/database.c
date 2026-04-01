#include "database.h"
#include "app_log.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int g_db_cache_mb = 32;
int g_db_mmap_mb = 256;
int g_db_busy_timeout = 5000;

int db_open(Database *db, const char *path) {
    memset(db, 0, sizeof(Database));
#ifdef _WIN32
    InitializeCriticalSection(&db->write_lock);
#endif

    /* Verify SQLite was compiled with thread safety */
    int ts = sqlite3_threadsafe();
    if (ts < 1)
        app_log(LOG_WARN, "DB: SQLite compiled with THREADSAFE=%d -- concurrent access unsafe", ts);

    int rc = sqlite3_open(path, &db->db);
    if (rc != SQLITE_OK) {
        app_log(LOG_ERROR, "DB: failed to open %s: %s", path, sqlite3_errmsg(db->db));
        sqlite3_close(db->db);
        db->db = NULL;
        return -1;
    }

    /* Tuning */
    char pragma[128];
    snprintf(pragma, sizeof(pragma), "PRAGMA cache_size=-%d;", g_db_cache_mb * 1024);
    sqlite3_exec(db->db, pragma, NULL, NULL, NULL);
    snprintf(pragma, sizeof(pragma), "PRAGMA mmap_size=%lld;", (long long)g_db_mmap_mb * 1024 * 1024);
    sqlite3_exec(db->db, pragma, NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(db->db, g_db_busy_timeout);

    /* Create tables */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  keyword TEXT PRIMARY KEY,"
        "  json_data TEXT,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");", NULL, NULL, NULL);

    /* Settings in separate DB (survives purge) */
    {char settings_path[512];
    snprintf(settings_path, sizeof(settings_path), "%.*s",
             (int)(strrchr(path, '/') ? strrchr(path, '/') - path : strlen(path)), path);
    snprintf(settings_path + strlen(settings_path),
             sizeof(settings_path) - strlen(settings_path), "/settings.db");
    rc = sqlite3_open(settings_path, &db->settings_db);
    if (rc != SQLITE_OK) {
        app_log(LOG_WARN, "DB: failed to open settings DB, using main DB");
        db->settings_db = NULL;
    } else {
        sqlite3_exec(db->settings_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
        sqlite3_exec(db->settings_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
        sqlite3_exec(db->settings_db,
            "CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT"
            ");", NULL, NULL, NULL);
        /* Migrate settings from main DB if they exist there */
        {sqlite3_stmt *mig; int migrated = 0;
        if (sqlite3_prepare_v2(db->db, "SELECT key, value FROM settings;", -1, &mig, NULL) == SQLITE_OK) {
            while (sqlite3_step(mig) == SQLITE_ROW) {
                const char *k = (const char *)sqlite3_column_text(mig, 0);
                const char *v = (const char *)sqlite3_column_text(mig, 1);
                if (k && v) {
                    sqlite3_stmt *ins;
                    if (sqlite3_prepare_v2(db->settings_db,
                        "INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?);",
                        -1, &ins, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(ins, 1, k, -1, SQLITE_STATIC);
                        sqlite3_bind_text(ins, 2, v, -1, SQLITE_STATIC);
                        sqlite3_step(ins);
                        sqlite3_finalize(ins);
                        migrated++;
                    }
                }
            }
            sqlite3_finalize(mig);
            /* Only drop source table if rows were actually copied */
            if (migrated > 0) {
                sqlite3_exec(db->db, "DROP TABLE IF EXISTS settings;", NULL, NULL, NULL);
                app_log(LOG_INFO, "DB: migrated %d settings to separate settings.db", migrated);
            }
        }}
    }}
    /* Keep settings table in main DB as fallback if settings_db failed */
    if (!db->settings_db) {
        sqlite3_exec(db->db,
            "CREATE TABLE IF NOT EXISTS settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT"
            ");", NULL, NULL, NULL);
    }

    /* Position tracking for content decay detection */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS positions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  keyword TEXT NOT NULL,"
        "  domain TEXT NOT NULL,"
        "  position INTEGER,"
        "  word_count INTEGER,"
        "  heading_count INTEGER,"
        "  checked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->db,
        "CREATE INDEX IF NOT EXISTS idx_positions_keyword ON positions(keyword, checked_at);",
        NULL, NULL, NULL);

    /* SERP URL queue -- survives crashes, tracks scrape status */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_urls ("
        "  url TEXT NOT NULL,"
        "  keyword TEXT NOT NULL,"
        "  domain TEXT,"
        "  title TEXT,"
        "  snippet TEXT,"
        "  serp_position INTEGER,"
        "  serp_page INTEGER,"
        "  status TEXT DEFAULT 'pending',"
        "  word_count INTEGER DEFAULT 0,"
        "  heading_count INTEGER DEFAULT 0,"
        "  scraped_at TIMESTAMP,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY(url, keyword)"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->db,
        "CREATE INDEX IF NOT EXISTS idx_serp_urls_status ON serp_urls(status);",
        NULL, NULL, NULL);

    /* SERP position/metadata change history */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  url TEXT NOT NULL,"
        "  keyword TEXT NOT NULL,"
        "  serp_position INTEGER,"
        "  title TEXT,"
        "  recorded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ");", NULL, NULL, NULL);

    /* Outbound link storage -- filtered OBL URLs saved during every scrape */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS obl_links ("
        "  source_url TEXT NOT NULL,"
        "  keyword TEXT NOT NULL,"
        "  obl_url TEXT NOT NULL,"
        "  anchor_text TEXT,"
        "  created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "  PRIMARY KEY(source_url, obl_url)"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->db,
        "CREATE INDEX IF NOT EXISTS idx_obl_keyword ON obl_links(keyword);",
        NULL, NULL, NULL);

    /* PAA + Related searches per keyword (from Serper API) */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_paa ("
        "  keyword TEXT NOT NULL,"
        "  question TEXT NOT NULL,"
        "  snippet TEXT DEFAULT '',"
        "  link TEXT DEFAULT '',"
        "  ordinal INTEGER NOT NULL,"
        "  PRIMARY KEY(keyword, ordinal)"
        ");", NULL, NULL, NULL);

    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_related ("
        "  keyword TEXT NOT NULL,"
        "  query TEXT NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  PRIMARY KEY(keyword, ordinal)"
        ");", NULL, NULL, NULL);

    /* ── Schema migration: add columns to serp_urls if missing ── */
    /* Use PRAGMA user_version to skip migrations that have already run */
    #define SCHEMA_VERSION 1
    {
        int cur_ver = 0;
        sqlite3_stmt *ver_stmt;
        if (sqlite3_prepare_v2(db->db, "PRAGMA user_version;", -1, &ver_stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(ver_stmt) == SQLITE_ROW)
                cur_ver = sqlite3_column_int(ver_stmt, 0);
            sqlite3_finalize(ver_stmt);
        }
        if (cur_ver < SCHEMA_VERSION) {
            /* v0 -> v1: extra columns on serp_urls */
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN outbound_count INTEGER DEFAULT 0;", NULL, NULL, NULL);
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN internal_count INTEGER DEFAULT 0;", NULL, NULL, NULL);
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN scrape_time_secs REAL DEFAULT 0;", NULL, NULL, NULL);
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN used_js_fallback INTEGER DEFAULT 0;", NULL, NULL, NULL);
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN is_crawled INTEGER DEFAULT 0;", NULL, NULL, NULL);
            sqlite3_exec(db->db, "ALTER TABLE serp_urls ADD COLUMN source_url TEXT DEFAULT '';", NULL, NULL, NULL);

            char ver_pragma[64];
            snprintf(ver_pragma, sizeof(ver_pragma), "PRAGMA user_version=%d;", SCHEMA_VERSION);
            sqlite3_exec(db->db, ver_pragma, NULL, NULL, NULL);
        }
    }

    /* Per-page headings storage */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_headings ("
        "  url TEXT NOT NULL,"
        "  keyword TEXT NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  tag TEXT NOT NULL,"
        "  text TEXT NOT NULL,"
        "  level INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(url, keyword, ordinal)"
        ");", NULL, NULL, NULL);

    /* Per-page extracted keywords */
    sqlite3_exec(db->db,
        "CREATE TABLE IF NOT EXISTS serp_page_keywords ("
        "  url TEXT NOT NULL,"
        "  keyword TEXT NOT NULL,"
        "  page_keyword TEXT NOT NULL,"
        "  ordinal INTEGER NOT NULL,"
        "  PRIMARY KEY(url, keyword, ordinal)"
        ");", NULL, NULL, NULL);

    app_log(LOG_INFO, "DB: opened %s (cache=%dMB, mmap=%dMB)",
            path, g_db_cache_mb, g_db_mmap_mb);
    return 0;
}

void db_close(Database *db) {
    /* Finalize cached prepared statements before closing */
    if (db->stmt_update_url)   { sqlite3_finalize(db->stmt_update_url);   db->stmt_update_url = NULL; }
    if (db->stmt_save_heading) { sqlite3_finalize(db->stmt_save_heading); db->stmt_save_heading = NULL; }
    if (db->stmt_save_keyword) { sqlite3_finalize(db->stmt_save_keyword); db->stmt_save_keyword = NULL; }

    if (db->settings_db) {
        sqlite3_close(db->settings_db);
        db->settings_db = NULL;
    }
    if (db->db) {
        sqlite3_close(db->db);
        db->db = NULL;
    }
#ifdef _WIN32
    /* Only delete CS if no thread holds it -- a late scrape worker
       could still be mid-write during shutdown.  Accept the leak;
       process exit reclaims all OS resources. */
    if (TryEnterCriticalSection(&db->write_lock)) {
        LeaveCriticalSection(&db->write_lock);
        DeleteCriticalSection(&db->write_lock);
    } else {
        app_log(LOG_WARN, "db_close: write_lock held, skipping CS delete");
    }
#endif
}

int db_save_session(Database *db, const char *keyword, const char *json_data) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT OR REPLACE INTO sessions (keyword, json_data, updated_at) "
        "VALUES (?, ?, CURRENT_TIMESTAMP);", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, json_data, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_load_session(Database *db, const char *keyword, char **json_data) {
    if (!db->db) return -1;
    *json_data = NULL;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT json_data FROM sessions WHERE keyword=?;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(stmt, 0);
            if (text) *json_data = str_duplicate(text);
        }
        sqlite3_finalize(stmt);
    }
    return *json_data ? 0 : -1;
}

int db_list_sessions(Database *db, char sessions[][256], int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT keyword FROM sessions ORDER BY updated_at DESC;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *kw = (const char *)sqlite3_column_text(stmt, 0);
            if (kw) { snprintf(sessions[count], 256, "%s", kw); count++; }
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_list_sessions_rich(Database *db, char keywords[][256], char dates[][32],
                          char **json_out, int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT keyword, strftime('%Y-%m-%d %H:%M', updated_at), json_data "
        "FROM sessions ORDER BY updated_at DESC;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *kw = (const char *)sqlite3_column_text(stmt, 0);
            const char *dt = (const char *)sqlite3_column_text(stmt, 1);
            const char *js = (const char *)sqlite3_column_text(stmt, 2);
            if (kw) snprintf(keywords[count], 256, "%s", kw);
            if (dt) snprintf(dates[count], 32, "%s", dt);
            else    snprintf(dates[count], 32, "?");
            json_out[count] = js ? str_duplicate(js) : NULL;
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_delete_session(Database *db, const char *keyword) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "DELETE FROM sessions WHERE keyword=?;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_all_sessions(Database *db) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    int rc = sqlite3_exec(db->db, "DELETE FROM sessions;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_OK) ? 0 : -1;
}

int db_save_setting(Database *db, const char *key, const char *value) {
    sqlite3 *sdb = db->settings_db ? db->settings_db : db->db;
    if (!sdb) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sdb,
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_load_setting(Database *db, const char *key, char *value, int max_len) {
    sqlite3 *sdb = db->settings_db ? db->settings_db : db->db;
    if (!sdb) return -1;
    value[0] = '\0';
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(sdb,
        "SELECT value FROM settings WHERE key=?;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *text = (const char *)sqlite3_column_text(stmt, 0);
            if (text) snprintf(value, max_len, "%s", text);
        }
        sqlite3_finalize(stmt);
    }
    return value[0] ? 0 : -1;
}

int db_save_position(Database *db, const char *keyword, const char *domain,
                     int position, int word_count, int heading_count) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO positions (keyword, domain, position, word_count, heading_count) "
        "VALUES (?, ?, ?, ?, ?);", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, domain, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, position);
        sqlite3_bind_int(stmt, 4, word_count);
        sqlite3_bind_int(stmt, 5, heading_count);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── SERP URL queue ─────────────────────────────────────────── */

int db_save_serp_url(Database *db, const char *url, const char *keyword,
                     const char *domain, const char *title, const char *snippet,
                     int position, int page) {
    if (!db->db) return -1;

    /* History check is read-only -- safe outside write_lock under WAL mode.
       This avoids holding the lock during the SELECT, reducing contention
       when many scrape_workers call this concurrently. */
    int need_history = 0;
    int old_pos = 0;
    char old_title_buf[512] = "";
    {sqlite3_stmt *check;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT serp_position, title FROM serp_urls WHERE url=? AND keyword=?;",
        -1, &check, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(check, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(check, 2, keyword, -1, SQLITE_STATIC);
        if (sqlite3_step(check) == SQLITE_ROW) {
            old_pos = sqlite3_column_int(check, 0);
            const char *ot = (const char *)sqlite3_column_text(check, 1);
            if (ot) snprintf(old_title_buf, sizeof(old_title_buf), "%s", ot);
            if (old_pos != position || (ot && title && strcmp(ot, title) != 0))
                need_history = 1;
        }
        sqlite3_finalize(check);
    }}

#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif

    /* Write history record if position/title changed */
    if (need_history) {
        sqlite3_stmt *hist;
        if (sqlite3_prepare_v2(db->db,
            "INSERT INTO serp_history (url, keyword, serp_position, title) VALUES (?, ?, ?, ?);",
            -1, &hist, NULL) == SQLITE_OK) {
            sqlite3_bind_text(hist, 1, url, -1, SQLITE_STATIC);
            sqlite3_bind_text(hist, 2, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_int(hist, 3, old_pos);
            sqlite3_bind_text(hist, 4, old_title_buf, -1, SQLITE_STATIC);
            sqlite3_step(hist);
            sqlite3_finalize(hist);
        }
    }

    /* Upsert: insert new or update metadata (preserve status and scraped_at) */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT INTO serp_urls (url, keyword, domain, title, snippet, serp_position, serp_page) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(url, keyword) DO UPDATE SET "
        "  domain=excluded.domain, title=excluded.title, snippet=excluded.snippet, "
        "  serp_position=excluded.serp_position, serp_page=excluded.serp_page;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, domain, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, title, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, snippet, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 6, position);
        sqlite3_bind_int(stmt, 7, page);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_pending_urls(Database *db, char urls[][2048], char keywords[][256],
                        char domains[][256], int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT url, keyword, domain FROM serp_urls WHERE status='pending' "
        "ORDER BY created_at ASC LIMIT ?;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, max_count);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *u = (const char *)sqlite3_column_text(stmt, 0);
            const char *k = (const char *)sqlite3_column_text(stmt, 1);
            const char *d = (const char *)sqlite3_column_text(stmt, 2);
            if (u) snprintf(urls[count], 2048, "%s", u);
            if (k) snprintf(keywords[count], 256, "%s", k);
            if (d) snprintf(domains[count], 256, "%s", d);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_get_pending_urls_rich(Database *db, char urls[][2048], char keywords[][256],
                             char domains[][256], char titles[][512],
                             char snippets[][512], int *positions, int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT url, keyword, domain, COALESCE(title,''), COALESCE(snippet,''), COALESCE(serp_position,0) "
        "FROM serp_urls WHERE status='pending' "
        "ORDER BY keyword, serp_position ASC LIMIT ?;", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, max_count);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *u = (const char *)sqlite3_column_text(stmt, 0);
            const char *k = (const char *)sqlite3_column_text(stmt, 1);
            const char *d = (const char *)sqlite3_column_text(stmt, 2);
            const char *t = (const char *)sqlite3_column_text(stmt, 3);
            const char *s = (const char *)sqlite3_column_text(stmt, 4);
            int p = sqlite3_column_int(stmt, 5);
            if (u) snprintf(urls[count], 2048, "%s", u);
            if (k) snprintf(keywords[count], 256, "%s", k);
            if (d) snprintf(domains[count], 256, "%s", d);
            if (t) snprintf(titles[count], 512, "%s", t);
            if (s) snprintf(snippets[count], 512, "%s", s);
            positions[count] = p;
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_count_pending_urls(Database *db) {
    if (!db->db) return 0;
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM serp_urls WHERE status='pending';",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_delete_pending_url(Database *db, const char *url, const char *keyword) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "DELETE FROM serp_urls WHERE url=? AND keyword=? AND status='pending';", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_delete_all_pending_urls(Database *db) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    int rc = sqlite3_exec(db->db, "DELETE FROM serp_urls WHERE status='pending';", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* ── OBL link storage ──────────────────────────────────────────── */

int db_save_obl_links(Database *db, const char *source_url, const char *keyword,
                      const char **urls, const char **anchors, int count) {
    if (!db->db || count == 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "INSERT OR IGNORE INTO obl_links (source_url, keyword, obl_url, anchor_text) "
        "VALUES (?, ?, ?, ?);", -1, &stmt, NULL);
    int saved = 0;
    if (rc == SQLITE_OK) {
        for (int i = 0; i < count; i++) {
            sqlite3_bind_text(stmt, 1, source_url, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, urls[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, anchors ? anchors[i] : "", -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_DONE) saved++;
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return saved;
}

int db_get_obl_links(Database *db, const char *source_url,
                     char urls[][2048], char anchors[][256], int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db,
        "SELECT obl_url, anchor_text FROM obl_links WHERE source_url=? ORDER BY rowid;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, source_url, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *u = (const char *)sqlite3_column_text(stmt, 0);
            const char *a = (const char *)sqlite3_column_text(stmt, 1);
            if (u) snprintf(urls[count], 2048, "%s", u);
            if (a) snprintf(anchors[count], 256, "%s", a);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

/* ── DB-backed Results functions ────────────────────────────── */

int db_update_serp_url_full(Database *db, const char *url, const char *keyword,
                            const char *status, int word_count, int heading_count,
                            int outbound_count, int internal_count,
                            double scrape_time_secs, int js_fallback, int is_crawled,
                            const char *source_url) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    /* Use cached prepared statement if available, otherwise prepare and cache */
    static const char *sql_update_url =
        "UPDATE serp_urls SET status=?, word_count=?, heading_count=?,"
        "  outbound_count=?, internal_count=?, scrape_time_secs=?,"
        "  used_js_fallback=?,"
        "  is_crawled = CASE WHEN ?!=0 THEN ? ELSE is_crawled END,"
        "  source_url = CASE WHEN ?!='' THEN ? ELSE source_url END,"
        "  scraped_at=CURRENT_TIMESTAMP"
        " WHERE url=? AND keyword=?;";
    int rc = SQLITE_OK;
    if (!db->stmt_update_url) {
        rc = sqlite3_prepare_v2(db->db, sql_update_url, -1, &db->stmt_update_url, NULL);
    } else {
        sqlite3_reset(db->stmt_update_url);
        sqlite3_clear_bindings(db->stmt_update_url);
    }
    int step_rc = SQLITE_ERROR;
    if (rc == SQLITE_OK && db->stmt_update_url) {
        sqlite3_stmt *stmt = db->stmt_update_url;
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, word_count);
        sqlite3_bind_int(stmt, 3, heading_count);
        sqlite3_bind_int(stmt, 4, outbound_count);
        sqlite3_bind_int(stmt, 5, internal_count);
        sqlite3_bind_double(stmt, 6, scrape_time_secs);
        sqlite3_bind_int(stmt, 7, js_fallback);
        /* is_crawled: only set if non-zero (preserves existing value for SERP re-scrapes) */
        sqlite3_bind_int(stmt, 8, is_crawled);
        sqlite3_bind_int(stmt, 9, is_crawled);
        /* source_url: only set if non-empty (preserves existing value) */
        sqlite3_bind_text(stmt, 10, source_url ? source_url : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 11, source_url ? source_url : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 12, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 13, keyword, -1, SQLITE_STATIC);
        step_rc = sqlite3_step(stmt);
    }
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return (rc == SQLITE_OK && step_rc == SQLITE_DONE) ? 0 : -1;
}

int db_get_results(Database *db, const char *keyword, DbResultRow *rows, int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    const char *sql = keyword
        ? "SELECT url, keyword, domain, COALESCE(title,''), serp_position,"
          "  word_count, heading_count, COALESCE(outbound_count,0), COALESCE(internal_count,0),"
          "  COALESCE(scrape_time_secs,0), COALESCE(used_js_fallback,0),"
          "  COALESCE(is_crawled,0), COALESCE(source_url,''), status"
          " FROM serp_urls WHERE keyword=? AND status IN ('scraped','failed')"
          " ORDER BY is_crawled ASC, serp_position ASC;"
        : "SELECT url, keyword, domain, COALESCE(title,''), serp_position,"
          "  word_count, heading_count, COALESCE(outbound_count,0), COALESCE(internal_count,0),"
          "  COALESCE(scrape_time_secs,0), COALESCE(used_js_fallback,0),"
          "  COALESCE(is_crawled,0), COALESCE(source_url,''), status"
          " FROM serp_urls WHERE status IN ('scraped','failed')"
          " ORDER BY keyword ASC, is_crawled ASC, serp_position ASC;";
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (keyword) sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            DbResultRow *r = &rows[count];
            memset(r, 0, sizeof(DbResultRow));
            const char *v;
            if ((v = (const char *)sqlite3_column_text(stmt, 0))) snprintf(r->url, sizeof(r->url), "%s", v);
            if ((v = (const char *)sqlite3_column_text(stmt, 1))) snprintf(r->keyword, sizeof(r->keyword), "%s", v);
            if ((v = (const char *)sqlite3_column_text(stmt, 2))) snprintf(r->domain, sizeof(r->domain), "%s", v);
            if ((v = (const char *)sqlite3_column_text(stmt, 3))) snprintf(r->title, sizeof(r->title), "%s", v);
            r->serp_position = sqlite3_column_int(stmt, 4);
            r->word_count = sqlite3_column_int(stmt, 5);
            r->heading_count = sqlite3_column_int(stmt, 6);
            r->outbound_count = sqlite3_column_int(stmt, 7);
            r->internal_count = sqlite3_column_int(stmt, 8);
            r->scrape_time_secs = sqlite3_column_double(stmt, 9);
            r->js_fallback = sqlite3_column_int(stmt, 10);
            r->is_crawled = sqlite3_column_int(stmt, 11);
            if ((v = (const char *)sqlite3_column_text(stmt, 12))) snprintf(r->source_url, sizeof(r->source_url), "%s", v);
            if ((v = (const char *)sqlite3_column_text(stmt, 13))) snprintf(r->status, sizeof(r->status), "%s", v);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_delete_result(Database *db, const char *url, const char *keyword) {
    if (!db->db) return -1;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    sqlite3_stmt *stmt;
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    /* Delete from serp_urls */
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_urls WHERE url=? AND keyword=?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    /* Delete associated headings */
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_headings WHERE url=? AND keyword=?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    /* Delete associated page keywords */
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_page_keywords WHERE url=? AND keyword=?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    /* Delete associated OBL links */
    if (sqlite3_prepare_v2(db->db, "DELETE FROM obl_links WHERE source_url=? AND keyword=?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_finalize(stmt);
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return 0;
}

int db_save_serp_headings(Database *db, const char *url, const char *keyword,
                          const char (*tags)[8], const char (*texts)[512],
                          const int *levels, int count) {
    if (!db->db || count == 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    /* DELETE + INSERTs must be in the same transaction for crash safety */
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    {sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_headings WHERE url=? AND keyword=?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(del, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(del); sqlite3_finalize(del);
    }}
    /* Use cached prepared statement for the INSERT loop */
    static const char *sql_insert_heading =
        "INSERT INTO serp_headings (url, keyword, ordinal, tag, text, level) VALUES (?,?,?,?,?,?);";
    int saved = 0;
    int need_prepare = (!db->stmt_save_heading);
    if (need_prepare) {
        if (sqlite3_prepare_v2(db->db, sql_insert_heading, -1, &db->stmt_save_heading, NULL) != SQLITE_OK)
            db->stmt_save_heading = NULL;
    }
    if (db->stmt_save_heading) {
        sqlite3_stmt *stmt = db->stmt_save_heading;
        for (int i = 0; i < count && i < 200; i++) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, i);
            sqlite3_bind_text(stmt, 4, tags[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 5, texts[i], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 6, levels[i]);
            if (sqlite3_step(stmt) == SQLITE_DONE) saved++;
        }
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return saved;
}

int db_get_serp_headings(Database *db, const char *url, const char *keyword,
                         char tags[][8], char texts[][512], int *levels, int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
        "SELECT tag, text, level FROM serp_headings WHERE url=? AND keyword=? ORDER BY ordinal;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            const char *x = (const char *)sqlite3_column_text(stmt, 1);
            if (t) snprintf(tags[count], 8, "%s", t);
            if (x) snprintf(texts[count], 512, "%s", x);
            levels[count] = sqlite3_column_int(stmt, 2);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_save_serp_page_keywords(Database *db, const char *url, const char *keyword,
                               const char (*kws)[64], int count) {
    if (!db->db || count == 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    /* DELETE + INSERTs must be in the same transaction for crash safety */
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    {sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_page_keywords WHERE url=? AND keyword=?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(del, 2, keyword, -1, SQLITE_STATIC);
        sqlite3_step(del); sqlite3_finalize(del);
    }}
    /* Use cached prepared statement for the INSERT loop */
    static const char *sql_insert_kw =
        "INSERT INTO serp_page_keywords (url, keyword, page_keyword, ordinal) VALUES (?,?,?,?);";
    int saved = 0;
    int need_prepare = (!db->stmt_save_keyword);
    if (need_prepare) {
        if (sqlite3_prepare_v2(db->db, sql_insert_kw, -1, &db->stmt_save_keyword, NULL) != SQLITE_OK)
            db->stmt_save_keyword = NULL;
    }
    if (db->stmt_save_keyword) {
        sqlite3_stmt *stmt = db->stmt_save_keyword;
        for (int i = 0; i < count && i < 30; i++) {
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, kws[i], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, i);
            if (sqlite3_step(stmt) == SQLITE_DONE) saved++;
        }
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return saved;
}

int db_get_serp_page_keywords(Database *db, const char *url, const char *keyword,
                              char kws[][64], int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
        "SELECT page_keyword FROM serp_page_keywords WHERE url=? AND keyword=? ORDER BY ordinal;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, keyword, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *k = (const char *)sqlite3_column_text(stmt, 0);
            if (k) { snprintf(kws[count], 64, "%s", k); count++; }
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

/* ── PAA + Related searches persistence ─────────────────────── */

int db_save_paa(Database *db, const char *keyword,
                const char (*questions)[512], const char (*snippets)[1024],
                const char (*links)[2048], int count) {
    if (!db->db || count == 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    /* DELETE + INSERTs must be in the same transaction for crash safety */
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    {sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_paa WHERE keyword=?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, keyword, -1, SQLITE_STATIC);
        sqlite3_step(del); sqlite3_finalize(del);
    }}
    sqlite3_stmt *stmt;
    int saved = 0;
    if (sqlite3_prepare_v2(db->db,
        "INSERT INTO serp_paa (keyword, question, snippet, link, ordinal) VALUES (?,?,?,?,?);",
        -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < count && i < 10; i++) {
            sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, questions[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, snippets ? snippets[i] : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, links ? links[i] : "", -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 5, i);
            if (sqlite3_step(stmt) == SQLITE_DONE) saved++;
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return saved;
}

int db_load_paa(Database *db, const char *keyword,
                char questions[][512], char snippets[][1024], char links[][2048],
                int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
        "SELECT question, snippet, link FROM serp_paa WHERE keyword=? ORDER BY ordinal;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *q = (const char *)sqlite3_column_text(stmt, 0);
            const char *s = (const char *)sqlite3_column_text(stmt, 1);
            const char *l = (const char *)sqlite3_column_text(stmt, 2);
            if (q) snprintf(questions[count], 512, "%s", q);
            if (s && snippets) snprintf(snippets[count], 1024, "%s", s);
            if (l && links) snprintf(links[count], 2048, "%s", l);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

int db_save_related(Database *db, const char *keyword,
                    const char (*queries)[256], int count) {
    if (!db->db || count == 0) return 0;
#ifdef _WIN32
    EnterCriticalSection(&db->write_lock);
#endif
    /* DELETE + INSERTs must be in the same transaction for crash safety */
    sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
    {sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db->db, "DELETE FROM serp_related WHERE keyword=?;", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_text(del, 1, keyword, -1, SQLITE_STATIC);
        sqlite3_step(del); sqlite3_finalize(del);
    }}
    sqlite3_stmt *stmt;
    int saved = 0;
    if (sqlite3_prepare_v2(db->db,
        "INSERT INTO serp_related (keyword, query, ordinal) VALUES (?,?,?);",
        -1, &stmt, NULL) == SQLITE_OK) {
        for (int i = 0; i < count && i < 20; i++) {
            sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, queries[i], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, i);
            if (sqlite3_step(stmt) == SQLITE_DONE) saved++;
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
#ifdef _WIN32
    LeaveCriticalSection(&db->write_lock);
#endif
    return saved;
}

int db_load_related(Database *db, const char *keyword,
                    char queries[][256], int max_count) {
    if (!db->db) return 0;
    int count = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
        "SELECT query FROM serp_related WHERE keyword=? ORDER BY ordinal;",
        -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, keyword, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
            const char *q = (const char *)sqlite3_column_text(stmt, 0);
            if (q) snprintf(queries[count], 256, "%s", q);
            count++;
        }
        sqlite3_finalize(stmt);
    }
    return count;
}
