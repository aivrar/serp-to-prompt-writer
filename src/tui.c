#include "tui.h"
#include "utils.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <uxtheme.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* ── Theme ────────────────────────────────────────────────────── */

static COLORREF BG      = RGB(18, 18, 24);
static COLORREF BG2     = RGB(28, 28, 38);
static COLORREF BG3     = RGB(38, 38, 52);
static COLORREF BORDER  = RGB(55, 55, 75);
static COLORREF TXT     = RGB(210, 210, 225);
static COLORREF DIM     = RGB(155, 155, 175);
static COLORREF CYAN    = RGB(0, 210, 235);
static COLORREF GREEN   = RGB(0, 220, 120);
static COLORREF YELLOW  = RGB(255, 210, 60);
static COLORREF RED     = RGB(255, 80, 80);
static int g_font_base_size = 16;

/* Cached result of onnx_nlp_models_present() -- avoids filesystem checks during WM_PAINT.
   -1 = not yet checked, 0 = not present, 1 = present.  Set to -1 to invalidate. */
static int g_models_cached = -1;

/* ── DPI scaling ──────────────────────────────────────────────── */

static int g_dpi = 96;
static int dpi_scale(int val) { return MulDiv(val, g_dpi, 96); }

/* ── Layout ───────────────────────────────────────────────────── */

#define D(x)      dpi_scale(x)
#define M         D(15)
#define TABH      D(32)
#define TABY      D(36)
#define CY        D(72)
#define BH        D(26)
#define EH        D(22)
#define SH        D(24)
#define PAD       D(16)
#define HPAD      D(10)
#define SECGAP    D(28)

/* ── IDs ──────────────────────────────────────────────────────── */

enum {
    /* tabs */
    ID_TAB0=1000, ID_TAB1, ID_TAB2, ID_TAB3, ID_TAB4,
    /* Search tab */
    ID_KEYWORD=2000, ID_SEARCH, ID_STOP,
    ID_PAGES_COMBO, ID_DEPTH_COMBO, ID_CHK_PROXY,
    ID_CHK_AUTO_SCRAPE, ID_CHK_AUTO_NLP, ID_CHK_AUTO_OBL, ID_CHK_AUTO_OBL_NLP,
    /* ID_BATCH removed - batch handled by multiline hKeyword */
    /* Session history (Search tab) */
    ID_HISTORY_LIST=2100,
    /* Context menu IDs */
    ID_CTX_HIST_DELETE=2300, ID_CTX_HIST_DELETE_ALL,
    ID_CTX_PEND_PROCESS_SEL, ID_CTX_PEND_PROCESS_ALL,
    ID_CTX_PEND_SCRAPE_SEL, ID_CTX_PEND_SCRAPE_ALL,
    ID_CTX_PEND_DELETE_SEL, ID_CTX_PEND_DELETE_ALL,
    ID_CTX_NLP_COPY_RAW, ID_CTX_NLP_COPY_FULL, ID_CTX_NLP_SELECT_ALL,
    ID_CTX_PROMPT_COPY_RAW, ID_CTX_PROMPT_COPY_FULL, ID_CTX_PROMPT_SELECT_ALL,
    ID_CTX_RESULT_OBL, ID_CTX_RESULT_NLP, ID_CTX_RESULT_OBL_NLP, ID_CTX_RESULT_SELECT_ALL, ID_CTX_RESULT_DELETE,
    ID_CTX_PEND_VIEW_DETAIL,
    ID_PROMPT_LIST=4400,
    /* Results tab */
    ID_RESULTS_LIST=3000, ID_DETAIL_TEXT,
    /* Analysis tab */
    ID_PROMPT_TEXT=4000, ID_COPY_BTN, ID_SAVE_TXT, ID_SAVE_JSON,
    ID_TEMPLATE_COMBO, ID_NLP_INFO,
    /* Content gap (Analysis tab) */
    ID_GAP_INPUT=4100, ID_GAP_ANALYZE, ID_GAP_RESULT,
    /* Prompt options (Analysis tab) */
    ID_CHK_KEYWORDS=4300, ID_CHK_ENTITIES, ID_CHK_OUTLINE,
    ID_CHK_STATS, ID_CHK_PAA, ID_CHK_RELATED,
    ID_MAX_KW_EDIT, ID_MAX_ENT_EDIT,
    /* Export */
    ID_SAVE_MD=4200,
    /* Settings tab */
    ID_THREADS_COMBO=5000,
    ID_KEYS_LIST=5002, ID_KEY_INPUT, ID_KEY_ADD, ID_KEY_REMOVE,
    ID_CTX_KEY_CHECK=5050, ID_CTX_KEY_DELETE,
    ID_PROXY_LIST, ID_PROXY_INPUT, ID_PROXY_ADD, ID_PROXY_REMOVE,
    ID_PROXY_USER, ID_PROXY_PASS, ID_AUTOMAP_BTN,
    ID_DOWNLOAD_MODELS, ID_PURGE_DB,
    ID_SYSINFO_LABEL,
    /* Full settings */
    ID_GET_TIMEOUT=5100, ID_POST_TIMEOUT, ID_MAX_REDIRECTS,
    ID_RETRY_COUNT, ID_RETRY_BASE,
    ID_BLOCKLIST_EDIT, ID_BLOCKLIST_ADD, ID_BLOCKLIST_LIST, ID_BLOCKLIST_DEL,
    ID_KEY_IMPORT, ID_KEY_PASTE, ID_PROXY_IMPORT, ID_PROXY_PASTE,
    ID_BLOCKLIST_IMPORT, ID_BLOCKLIST_PASTE,
    ID_SETTINGS_SUB_CONN=5200, ID_SETTINGS_SUB_DATA,
    /* Log tab */
    ID_LOG_LIST=6000, ID_LOG_CLEAR,
    ID_LOG_FILTER_DBG, ID_LOG_FILTER_INFO, ID_LOG_FILTER_WARN, ID_LOG_FILTER_ERR,
    /* Global */
    ID_NUKE=9000, ID_PROGRESS_TIMER=9001, ID_LOG_TIMER=9002,
    ID_STATUS=9010
};

static const char *INTENT_NAMES[] = {"Informational","Commercial","Transactional","Navigational"};
static const char *INTENT_NAMES_LC[] = {"informational","commercial","transactional","navigational"};

/* ── Globals ──────────────────────────────────────────────────── */

static HWND hw;
static HWND tabs[TAB_COUNT];

/* Search tab */
static HWND hKeyword, hSearch, hStop;
static HWND hPagesCombo, hDepthCombo, hChkProxy;
static HWND hChkAutoScrape, hChkAutoNlp, hChkAutoObl, hChkAutoOblNlp;
static HWND hProgress;
/* Search tab labels (STATIC controls, not painted) */
static HWND hLblPages, hLblDepth;
static HWND hLblInstr[10];  /* up to 10 instruction lines */
static HWND hLblProgress;
static HWND hLblHistory;
static HWND hResmonCpu, hResmonRam, hResmonGpu;
#define INSTR_LINES 2

/* Session history */
static HWND hHistoryList;

/* Pending scrape queue */
static HWND hPendingList, hLblPending;

/* Results tab */
static HWND hResultsList, hDetailText;

/* Analysis tab */
static HWND hPromptText, hPromptList, hCopyBtn, hSaveTxt, hSaveJson;
static HWND hTemplateCombo, hNlpInfo;
static HWND hChkKw, hChkEnt, hChkOutline, hChkStats, hChkPaa, hChkRelated;
static int  g_chk_state[6] = {1,1,1,1,1,1};  /* owner-draw checkbox states (all checked) */
static HWND hMaxKwEdit, hMaxEntEdit, hLblMaxKw, hLblMaxEnt;

/* Content gap */
static HWND hGapInput, hGapAnalyze, hGapResult;

/* Export */
static HWND hSaveMd;

/* Settings tab */
static HWND hThreadsCombo;
static HWND hKeysList, hKeyInput, hKeyAdd, hKeyRemove;
static HWND hProxyList, hProxyInput, hProxyAdd, hProxyRemove;
static HWND hProxyUser, hProxyPass, hAutoMap;
static HWND hSysInfoLabel;
static HWND hDownloadModels;

/* Full settings */
static HWND hGetTimeout, hPostTimeout, hMaxRedirects, hRetryCount, hRetryBase;
static HWND hBlocklistEdit, hBlocklistAdd, hBlocklistList, hBlocklistDel;

/* Download thread */
static HANDLE g_download_thread = NULL;

/* Log tab */
static HWND hLogList, hLogClear;
static HWND hLogFilterDbg, hLogFilterInfo, hLogFilterWarn, hLogFilterErr;
static int g_log_show_dbg = 0, g_log_show_info = 1, g_log_show_warn = 1, g_log_show_err = 1;

/* Global controls */
static HWND hNuke;
static HWND hStatus;

/* State */
static HFONT fNorm, fBold, fTitle, fMono;
static HBRUSH brBg, brBg2, brBg3;
static AppState *ST;
static int curTab;
static int g_controls_ready;
static SystemInfo SI;

/* Settings sub-tabs */
static HWND hSettingsSubConn, hSettingsSubData;
static int  settingsSubTab;  /* 0=Connection, 1=Data & Tools */

/* Log tracking */
static unsigned long long g_last_log_seq;

/* Deferred auto-save flags (issue #6: avoid heavy I/O in timer) */
static int g_save_pending;       /* single-keyword save deferred to WM_APP+13 */
static int g_batch_save_pending; /* batch save deferred to WM_APP+14 */

/* History list: store raw keywords for DB lookup (display text is enriched) */
#define HISTORY_MAX 50
static char g_history_keywords[HISTORY_MAX][256];
static int  g_history_count;

/* Pending queue: store full Serper data for detail popup */
#define PENDING_MAX 500
static char  g_pend_urls[PENDING_MAX][2048];
static char  g_pend_titles[PENDING_MAX][512];
static char  g_pend_snippets[PENDING_MAX][512];
static char  g_pend_keywords[PENDING_MAX][256];
static char  g_pend_domains[PENDING_MAX][256];
static int   g_pend_positions[PENDING_MAX];
static int   g_pend_count;
/* Map listbox row → data index (-1 for header/separator rows) */
#define PEND_MAP_MAX 1024
static int   g_pend_map[PEND_MAP_MAX];
static int   g_pend_map_count;

/* Results list — DB-backed persistent data */
#define RESULT_DB_MAX 2048
static DbResultRow g_result_rows[RESULT_DB_MAX];
static int g_result_row_count;

/* Results list → g_result_rows index map (tree order differs from DB order) */
#define RESULT_MAP_MAX 2048
static int g_result_page_map[RESULT_MAP_MAX];  /* listbox row → g_result_rows index */
static int g_result_map_count;

/* ── Forward declarations ─────────────────────────────────────── */
static void relayout(void);
static void setStatus(const char *fmt, ...);
static void switchTab(int t);
static void on_download_models(void);

/* ── History list helper: populate with rich session info ─────── */
static void populate_history_list(void) {
    SendMessageA(hHistoryList, LB_RESETCONTENT, 0, 0);
    g_history_count = 0;
    /* Header row (index 0 — data rows start at 1) */
    /* Header and data use identical column widths (no suffixes in data) */
    {char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "%-36s  %4s  %6s  %4s  %4s  %6s    %-12s  %-5s  %-14s  %3s  %3s  %s",
        "KEYWORD", "PG", "KW", "ENT", "HD", "AVG W",
        "TYPE", "INTNT", "TONE", "PAA", "REL", "DATE");
    SendMessageA(hHistoryList, LB_ADDSTRING, 0, (LPARAM)hdr);}
    if (!ST || !ST->db) return;

    char keywords[50][256];
    char dates[50][32];
    char *jsons[50];
    memset(jsons, 0, sizeof(jsons));
    int sc = db_list_sessions_rich(ST->db, keywords, dates, jsons, 50);

    for (int i = 0; i < sc; i++) {
        char line[512];
        /* Extract summary from JSON if available */
        int pages = 0, kws = 0, ents = 0, heads = 0;
        int avg_words = 0, paa = 0, related = 0;
        const char *intent = "";
        const char *ctype = "";
        const char *tone = "";
        if (jsons[i]) {
            cJSON *root = cJSON_Parse(jsons[i]);
            if (root) {
                cJSON *v;
                if ((v = cJSON_GetObjectItem(root, "keyword_count"))) kws = v->valueint;
                else { v = cJSON_GetObjectItem(root, "keywords"); if (v && cJSON_IsArray(v)) kws = cJSON_GetArraySize(v); }
                if ((v = cJSON_GetObjectItem(root, "entity_count"))) ents = v->valueint;
                else { v = cJSON_GetObjectItem(root, "entities"); if (v && cJSON_IsArray(v)) ents = cJSON_GetArraySize(v); }
                if ((v = cJSON_GetObjectItem(root, "heading_count"))) heads = v->valueint;
                if ((v = cJSON_GetObjectItem(root, "intent"))) intent = v->valuestring;
                if ((v = cJSON_GetObjectItem(root, "content_type"))) ctype = v->valuestring;
                if ((v = cJSON_GetObjectItem(root, "tone"))) tone = v->valuestring;
                v = cJSON_GetObjectItem(root, "people_also_ask");
                if (v && cJSON_IsArray(v)) paa = cJSON_GetArraySize(v);
                v = cJSON_GetObjectItem(root, "related_searches");
                if (v && cJSON_IsArray(v)) related = cJSON_GetArraySize(v);
                {cJSON *st = cJSON_GetObjectItem(root, "stats");
                if (st) { v = cJSON_GetObjectItem(st, "avg_words"); if (v) avg_words = (int)v->valuedouble; }}
                v = cJSON_GetObjectItem(root, "pages");
                if (v && cJSON_IsArray(v)) {
                    int pc = cJSON_GetArraySize(v);
                    for (int j = 0; j < pc; j++) {
                        cJSON *pg = cJSON_GetArrayItem(v, j);
                        cJSON *cr = cJSON_GetObjectItem(pg, "is_crawled");
                        if (!cr || !cr->valueint) pages++;
                    }
                }
                /* Shorten date to just month+day */
                char short_date[16] = "";
                if (dates[i][0] && strlen(dates[i]) >= 10) {
                    int mon = 0, day = 0;
                    sscanf(dates[i] + 5, "%d-%d", &mon, &day);
                    static const char *months[] = {"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                    if (mon >= 1 && mon <= 12)
                        snprintf(short_date, sizeof(short_date), "%s %d", months[mon], day);
                }
                /* Truncate keyword to fit column, pad to fixed width */
                char kw_col[37];
                snprintf(kw_col, sizeof(kw_col), "%s", keywords[i]);
                int klen = (int)strlen(kw_col);
                while (klen < 36) kw_col[klen++] = ' ';
                kw_col[36] = '\0';

                /* Abbreviate intent: info/comm/tran/nav */
                const char *iabbr = "";
                if (intent[0] == 'i') iabbr = "info";
                else if (intent[0] == 'c') iabbr = "comm";
                else if (intent[0] == 't') iabbr = "tran";
                else if (intent[0] == 'n') iabbr = "nav";

                /* Tone column (14 chars max) */
                char tone_col[16] = "";
                if (tone[0]) snprintf(tone_col, sizeof(tone_col), "%s", tone);

                snprintf(line, sizeof(line),
                    "%s  %4d  %6d  %4d  %4d  %6d    %-12s  %-5s  %-14s  %3d  %3d  %s",
                    kw_col, pages, kws, ents, heads, avg_words,
                    ctype[0] ? ctype : "",
                    iabbr,
                    tone_col,
                    paa, related,
                    short_date);
                cJSON_Delete(root);
            } else {
                snprintf(line, sizeof(line), "%s", keywords[i]);
            }
            free(jsons[i]);
        } else {
            snprintf(line, sizeof(line), "%s", keywords[i]);
        }
        /* Store raw keyword for DB lookup */
        if (g_history_count < HISTORY_MAX)
            snprintf(g_history_keywords[g_history_count++], 256, "%s", keywords[i]);
        SendMessageA(hHistoryList, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

/* Helper: get raw keyword from history list selection (row 0 is header) */
static const char *history_get_keyword(int sel) {
    sel -= 1;  /* offset for header row */
    if (sel >= 0 && sel < g_history_count) return g_history_keywords[sel];
    return NULL;
}

/* ── Domain truncation for results list ───────────────────────── */
static void trunc_domain(const char *domain, char *out, int max_w) {
    int len = (int)strlen(domain);
    if (len <= max_w) {
        snprintf(out, max_w + 1, "%-*s", max_w, domain);
    } else {
        /* Truncate with .. */
        snprintf(out, max_w + 1, "%.*s..", max_w - 2, domain);
    }
}

/* ── NLP info listbox helper ──────────────────────────────────── */

static void nlp_info_add(const char *fmt, ...) {
    char line[512];
    va_list a; va_start(a, fmt); vsnprintf(line, sizeof(line), fmt, a); va_end(a);
    SendMessageA(hNlpInfo, LB_ADDSTRING, 0, (LPARAM)line);
}

static void populate_nlp_info(const NLPResult *nlp) {
    SendMessageA(hNlpInfo, LB_RESETCONTENT, 0, 0);
    if (!nlp || nlp->keyword_count == 0) return;

    nlp_info_add("Keyword: %s", nlp->keyword);
    nlp_info_add("Intent: %s (%.0f%%)", INTENT_NAMES[nlp->intent.primary_intent],
        nlp->intent.confidence[nlp->intent.primary_intent]*100);
    nlp_info_add("Type: %s  |  Tone: %s", nlp->intent.content_type, nlp->intent.tone);
    nlp_info_add("Target: ~%d words, %d+ headings",
        nlp->intent.recommended_words, nlp->intent.recommended_headings);
    nlp_info_add("");
    nlp_info_add("Competitors: avg %.0f words (%d-%d)",
        nlp->stats.avg_word_count, nlp->stats.min_word_count, nlp->stats.max_word_count);
    nlp_info_add("Avg H2: %.1f  |  Avg H3: %.1f",
        nlp->stats.avg_h2_count, nlp->stats.avg_h3_count);
    nlp_info_add("");
    nlp_info_add("Keywords: %d  |  Headings: %d  |  Entities: %d",
        nlp->keyword_count, nlp->heading_count, nlp->entity_count);
    nlp_info_add("");

    if (nlp->keyword_count > 0) {
        nlp_info_add("=== KEYWORDS (%d) ===", nlp->keyword_count);
        for (int i = 0; i < nlp->keyword_count; i++)
            nlp_info_add("  %-32s  %.0f%%  x%d  %s",
                nlp->keywords[i].text,
                nlp->keywords[i].score * 100,
                nlp->keywords[i].frequency,
                nlp->keywords[i].source);
    }
    if (nlp->entity_count > 0) {
        nlp_info_add("");
        nlp_info_add("=== ENTITIES (%d) ===", nlp->entity_count);
        for (int i = 0; i < nlp->entity_count; i++)
            nlp_info_add("  %-32s  %-8s  x%d",
                nlp->entities[i].text, nlp->entities[i].label, nlp->entities[i].frequency);
    }
    if (nlp->heading_count > 0) {
        nlp_info_add("");
        nlp_info_add("=== HEADINGS (%d) ===", nlp->heading_count);
        for (int i = 0; i < nlp->heading_count; i++)
            nlp_info_add("  [%s] %-40s  %d pg",
                nlp->headings[i].tag, nlp->headings[i].text, nlp->headings[i].count);
    }
    if (nlp->paa_count > 0) {
        nlp_info_add("");
        nlp_info_add("=== PAA (%d) ===", nlp->paa_count);
        for (int i = 0; i < nlp->paa_count; i++)
            nlp_info_add("  %s", nlp->paa[i]);
    }
    if (nlp->related_count > 0) {
        nlp_info_add("");
        nlp_info_add("=== RELATED (%d) ===", nlp->related_count);
        for (int i = 0; i < nlp->related_count; i++)
            nlp_info_add("  %s", nlp->related[i]);
    }
}

/* ── Brush rebuild ────────────────────────────────────────────── */
static void rebuild_brushes(void) {
    if(brBg) DeleteObject(brBg); if(brBg2) DeleteObject(brBg2); if(brBg3) DeleteObject(brBg3);
    brBg=CreateSolidBrush(BG); brBg2=CreateSolidBrush(BG2); brBg3=CreateSolidBrush(BG3);
}

/* ── Font creation ────────────────────────────────────────────── */
static void mkfonts(void) {
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if(fNorm){DeleteObject(fNorm);DeleteObject(fBold);DeleteObject(fTitle);DeleteObject(fMono);}
    int fh = dpi_scale(g_font_base_size);
    int fh_title = dpi_scale(g_font_base_size + 6);
    int fh_mono = dpi_scale(g_font_base_size - 2);
    fNorm  = CreateFontA(fh,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");
    fBold  = CreateFontA(fh,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");
    fTitle = CreateFontA(fh_title,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,"Segoe UI");
    fMono  = CreateFontA(fh_mono,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
}

/* ── Draw helpers ─────────────────────────────────────────────── */

static void drawBtn(DRAWITEMSTRUCT *d, COLORREF fg, COLORREF bg) {
    HBRUSH b=CreateSolidBrush(bg); FillRect(d->hDC,&d->rcItem,b); DeleteObject(b);
    HPEN p=CreatePen(PS_SOLID,1,BORDER); HPEN oldP=(HPEN)SelectObject(d->hDC,p);
    SelectObject(d->hDC,GetStockObject(NULL_BRUSH));
    RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,D(5),D(5));
    SelectObject(d->hDC,oldP); DeleteObject(p);
    if(d->itemState&ODS_SELECTED){b=CreateSolidBrush(RGB(60,60,80));FillRect(d->hDC,&d->rcItem,b);DeleteObject(b);}
    SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,fg); SelectObject(d->hDC,fBold);
    char t[128]; GetWindowTextA(d->hwndItem,t,128);
    DrawTextA(d->hDC,t,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

static void drawTab(DRAWITEMSTRUCT *d, int idx) {
    int active=(idx==curTab);
    HBRUSH b=CreateSolidBrush(active?BG2:BG); FillRect(d->hDC,&d->rcItem,b); DeleteObject(b);
    if(active){RECT a=d->rcItem;a.top=a.bottom-D(3);b=CreateSolidBrush(CYAN);FillRect(d->hDC,&a,b);DeleteObject(b);}
    SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,active?CYAN:TXT);
    SelectObject(d->hDC,active?fBold:fNorm);
    char t[64]; GetWindowTextA(d->hwndItem,t,64);
    DrawTextA(d->hDC,t,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}

/* ── Show/Hide ────────────────────────────────────────────────── */

#define VIS(h,show) do{if(h)ShowWindow(h,(show)?SW_SHOW:SW_HIDE);}while(0)

/* ── Themed message dialog (replaces MessageBoxA) ────────────── */

#define DLGW D(420)
#define DLGH D(220)

static int g_dlg_result;
static const char *g_dlg_title;
static const char *g_dlg_text;
static int g_dlg_buttons; /* 0=OK only, 1=Yes/No */

static LRESULT CALLBACK dlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        /* Yes / OK button */
        CreateWindowA("BUTTON", g_dlg_buttons ? "Yes" : "OK",
            WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            DLGW/2 - (g_dlg_buttons ? D(90) : D(50)), DLGH-D(50),
            D(80), BH, h, (HMENU)1, NULL, NULL);
        /* No button (if yes/no) */
        if(g_dlg_buttons) {
            CreateWindowA("BUTTON", "No",
                WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
                DLGW/2 + D(10), DLGH-D(50), D(80), BH, h, (HMENU)2, NULL, NULL);
        }
        /* Set fonts on buttons */
        HWND btn;
        btn = GetDlgItem(h, 1); if(btn) SendMessage(btn, WM_SETFONT, (WPARAM)fBold, TRUE);
        btn = GetDlgItem(h, 2); if(btn) SendMessage(btn, WM_SETFONT, (WPARAM)fBold, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==1) { g_dlg_result = 1; DestroyWindow(h); return 0; }
        if(LOWORD(wp)==2) { g_dlg_result = 0; DestroyWindow(h); return 0; }
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if(d->CtlID == 1) drawBtn(d, BG, CYAN);
        else drawBtn(d, TXT, BG3);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, TXT);
        return (LRESULT)brBg;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)wp, &r, brBg);
        /* Border */
        HPEN pen = CreatePen(PS_SOLID, D(2), CYAN);
        HPEN oldPen = (HPEN)SelectObject((HDC)wp, pen);
        SelectObject((HDC)wp, GetStockObject(NULL_BRUSH));
        Rectangle((HDC)wp, 0, 0, r.right, r.bottom);
        SelectObject((HDC)wp, oldPen);
        DeleteObject(pen);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        /* Title */
        SelectObject(dc, fBold); SetTextColor(dc, CYAN);
        TextOutA(dc, D(20), D(15), g_dlg_title, (int)strlen(g_dlg_title));
        /* Separator */
        HPEN pen = CreatePen(PS_SOLID, 1, BORDER);
        HPEN oldPen = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, D(15), D(38), NULL); LineTo(dc, rc.right-D(15), D(38));
        SelectObject(dc, oldPen); DeleteObject(pen);
        /* Body text */
        SelectObject(dc, fNorm); SetTextColor(dc, TXT);
        RECT tr = {D(20), D(48), rc.right - D(20), DLGH - D(60)};
        DrawTextA(dc, g_dlg_text, -1, &tr, DT_LEFT|DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if(wp == VK_RETURN) { g_dlg_result = 1; DestroyWindow(h); return 0; }
        if(wp == VK_ESCAPE) { g_dlg_result = 0; DestroyWindow(h); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* Show a themed dialog. Returns 1 for OK/Yes, 0 for No/Cancel. */
static int themedDialog(const char *title, const char *text, int yesno) {
    g_dlg_title = title;
    g_dlg_text = text;
    g_dlg_buttons = yesno;
    g_dlg_result = 0;

    static int cls_registered = 0;
    if(!cls_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = dlgProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "SerpDlg";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        cls_registered = 1;
    }

    /* Center on parent */
    RECT pr; GetWindowRect(hw, &pr);
    int px = pr.left + (pr.right-pr.left-DLGW)/2;
    int py = pr.top + (pr.bottom-pr.top-DLGH)/2;

    HWND dlg = CreateWindowExA(WS_EX_TOPMOST, "SerpDlg", "",
        WS_POPUP|WS_VISIBLE, px, py, DLGW, DLGH,
        hw, NULL, GetModuleHandle(NULL), NULL);
    if (!dlg) return 0;

    EnableWindow(hw, FALSE); /* modal */
    SetFocus(dlg);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    EnableWindow(hw, TRUE);
    SetForegroundWindow(hw);
    return g_dlg_result;
}

/* ── Large detail popup (scrollable, for Serper details etc.) ─── */

static const char *g_detail_title;
static const char *g_detail_text;

static LRESULT CALLBACK detailDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        int w, ht;
        {RECT rc; GetClientRect(h, &rc); w = rc.right; ht = rc.bottom;}
        /* Scrollable read-only edit for body */
        HWND hEdit = CreateWindowExA(0, "EDIT", g_detail_text,
            WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            D(15), D(42), w-D(30), ht-D(100), h, (HMENU)10, NULL, NULL);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)fMono, TRUE);
        SetWindowTheme(hEdit, L"DarkMode_Explorer", NULL);  /* dark scrollbar */
        /* OK button */
        CreateWindowA("BUTTON", "OK", WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            w/2-D(50), ht-D(48), D(100), BH, h, (HMENU)1, NULL, NULL);
        {HWND btn = GetDlgItem(h, 1); if(btn) SendMessage(btn, WM_SETFONT, (WPARAM)fBold, TRUE);}
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wp)==1) { DestroyWindow(h); return 0; }
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        drawBtn(d, BG, CYAN);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, TXT);
        SetBkColor((HDC)wp, BG2);
        return (LRESULT)brBg2;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)wp, &r, brBg);
        HPEN pen = CreatePen(PS_SOLID, D(2), CYAN);
        HPEN oldPen = (HPEN)SelectObject((HDC)wp, pen);
        SelectObject((HDC)wp, GetStockObject(NULL_BRUSH));
        Rectangle((HDC)wp, 0, 0, r.right, r.bottom);
        SelectObject((HDC)wp, oldPen);
        DeleteObject(pen);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, fBold); SetTextColor(dc, CYAN);
        TextOutA(dc, D(20), D(12), g_detail_title, (int)strlen(g_detail_title));
        HPEN pen = CreatePen(PS_SOLID, 1, BORDER);
        HPEN oldPen = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, D(15), D(36), NULL); LineTo(dc, rc.right-D(15), D(36));
        SelectObject(dc, oldPen); DeleteObject(pen);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if(wp == VK_RETURN || wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void detailPopup(const char *title, const char *text) {
    g_detail_title = title;
    g_detail_text = text;

    static int cls_registered = 0;
    if(!cls_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = detailDlgProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "SerpDetail";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        cls_registered = 1;
    }

    int dw = D(550), dh = D(420);
    RECT pr; GetWindowRect(hw, &pr);
    int px = pr.left + (pr.right-pr.left-dw)/2;
    int py = pr.top + (pr.bottom-pr.top-dh)/2;

    HWND dlg = CreateWindowExA(WS_EX_TOPMOST, "SerpDetail", "",
        WS_POPUP|WS_VISIBLE, px, py, dw, dh,
        hw, NULL, GetModuleHandle(NULL), NULL);
    if (!dlg) return;

    EnableWindow(hw, FALSE);
    SetFocus(dlg);

    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    EnableWindow(hw, TRUE);
    SetForegroundWindow(hw);
}

static void switchSettingsSubTab(int sub) {
    settingsSubTab = sub;
    if (curTab != TAB_SETTINGS) return;
    int c = (sub == 0);  /* Connection */
    int d = (sub == 1);  /* Data & Tools */

    /* Connection controls */
    VIS(hKeysList,c); VIS(hKeyInput,c); VIS(hKeyAdd,c); VIS(hKeyRemove,c);
    {HWND h1=GetDlgItem(hw,ID_KEY_IMPORT); if(h1) VIS(h1,c);}
    {HWND h1p=GetDlgItem(hw,ID_KEY_PASTE); if(h1p) VIS(h1p,c);}
    VIS(hProxyList,c); VIS(hProxyInput,c); VIS(hProxyAdd,c); VIS(hProxyRemove,c);
    {HWND h2=GetDlgItem(hw,ID_PROXY_IMPORT); if(h2) VIS(h2,c);}
    {HWND h2p=GetDlgItem(hw,ID_PROXY_PASTE); if(h2p) VIS(h2p,c);}
    VIS(hProxyUser,c); VIS(hProxyPass,c); VIS(hAutoMap,c);
    VIS(hThreadsCombo,c);
    VIS(hGetTimeout,c); VIS(hPostTimeout,c); VIS(hMaxRedirects,c);
    VIS(hRetryCount,c); VIS(hRetryBase,c);

    /* Data & Tools controls */
    VIS(hBlocklistEdit,d); VIS(hBlocklistAdd,d); VIS(hBlocklistList,d); VIS(hBlocklistDel,d);
    {HWND h3=GetDlgItem(hw,ID_BLOCKLIST_IMPORT); if(h3) VIS(h3,d);}
    {HWND h3p=GetDlgItem(hw,ID_BLOCKLIST_PASTE); if(h3p) VIS(h3p,d);}
    VIS(hSysInfoLabel,d); VIS(hDownloadModels,d);
    {HWND hPurge=GetDlgItem(hw,ID_PURGE_DB); if(hPurge) VIS(hPurge,d);}

    InvalidateRect(hSettingsSubConn, NULL, FALSE);
    InvalidateRect(hSettingsSubData, NULL, FALSE);
    InvalidateRect(hw, NULL, TRUE);
}

static void switchTab(int t) {
    curTab=t;
    if(!g_controls_ready) return;
    int s=(t==0), r=(t==1), a=(t==2), se=(t==3), lo=(t==4);

    /* Search tab */
    VIS(hKeyword,s); VIS(hSearch,s); VIS(hStop,s);
    VIS(hLblPages,s);
    VIS(hLblProgress,s);
    VIS(hLblHistory,s);
    VIS(hPagesCombo,s); VIS(hChkProxy,s);
    VIS(hChkAutoScrape,s); VIS(hChkAutoNlp,s);
    VIS(hProgress,s);
    VIS(hHistoryList,s);
    VIS(hPendingList,s); VIS(hLblPending,s);

    /* Results tab */
    VIS(hResultsList,r); VIS(hDetailText,r);

    /* Analysis tab */
    VIS(hCopyBtn,a);
    VIS(hTemplateCombo,a); VIS(hNlpInfo,a);
    /* Prompt text vs keyword list: show the right one based on template */
    if(a) {
        int tmpl = (int)SendMessageA(hTemplateCombo, CB_GETCURSEL, 0, 0);
        int is_kw = (tmpl == 1); /* PROMPT_KEYWORDS_ONLY */
        ShowWindow(hPromptList, is_kw ? SW_SHOW : SW_HIDE);
        ShowWindow(hPromptText, is_kw ? SW_HIDE : SW_SHOW);
    } else {
        VIS(hPromptText,0); VIS(hPromptList,0);
    }
    VIS(hChkKw,a); VIS(hChkEnt,a); VIS(hChkOutline,a);
    VIS(hChkStats,a); VIS(hChkPaa,a); VIS(hChkRelated,a);
    VIS(hMaxKwEdit,a); VIS(hMaxEntEdit,a); VIS(hLblMaxKw,a); VIS(hLblMaxEnt,a);
    VIS(hGapInput,a); VIS(hGapAnalyze,a); VIS(hGapResult,a);

    /* Settings tab — sub-tab buttons always visible when on settings */
    VIS(hSettingsSubConn,se); VIS(hSettingsSubData,se);
    if(se) {
        /* Prime the model-presence cache when entering Settings */
        if (g_models_cached < 0)
            g_models_cached = onnx_nlp_models_present("models");
        switchSettingsSubTab(settingsSubTab);
    } else {
        /* Hide all settings controls */
        VIS(hKeysList,0); VIS(hKeyInput,0); VIS(hKeyAdd,0); VIS(hKeyRemove,0);
        {HWND h1=GetDlgItem(hw,ID_KEY_IMPORT); if(h1) VIS(h1,0);}
        {HWND h1p=GetDlgItem(hw,ID_KEY_PASTE); if(h1p) VIS(h1p,0);}
        VIS(hProxyList,0); VIS(hProxyInput,0); VIS(hProxyAdd,0); VIS(hProxyRemove,0);
        {HWND h2=GetDlgItem(hw,ID_PROXY_IMPORT); if(h2) VIS(h2,0);}
        {HWND h2p=GetDlgItem(hw,ID_PROXY_PASTE); if(h2p) VIS(h2p,0);}
        VIS(hProxyUser,0); VIS(hProxyPass,0); VIS(hAutoMap,0);
        VIS(hThreadsCombo,0);
        VIS(hGetTimeout,0); VIS(hPostTimeout,0); VIS(hMaxRedirects,0);
        VIS(hRetryCount,0); VIS(hRetryBase,0);
        VIS(hBlocklistEdit,0); VIS(hBlocklistAdd,0); VIS(hBlocklistList,0); VIS(hBlocklistDel,0);
        {HWND h3=GetDlgItem(hw,ID_BLOCKLIST_IMPORT); if(h3) VIS(h3,0);}
        {HWND h3p=GetDlgItem(hw,ID_BLOCKLIST_PASTE); if(h3p) VIS(h3p,0);}
        VIS(hSysInfoLabel,0); VIS(hDownloadModels,0);
        {HWND hPurge=GetDlgItem(hw,ID_PURGE_DB); if(hPurge) VIS(hPurge,0);}
    }

    /* Log tab */
    VIS(hLogList,lo); VIS(hLogClear,lo);
    VIS(hLogFilterDbg,lo); VIS(hLogFilterInfo,lo);
    VIS(hLogFilterWarn,lo); VIS(hLogFilterErr,lo);

    /* Always visible */
    VIS(hNuke,1);

    /* Redraw tabs + main window */
    for(int i=0;i<TAB_COUNT;i++) InvalidateRect(tabs[i],NULL,FALSE);
    InvalidateRect(hw,NULL,TRUE);
}

/* ── Status helper ────────────────────────────────────────────── */

static void setStatus(const char *fmt, ...) {
    va_list a; va_start(a,fmt);
    vsnprintf(ST->status_msg,sizeof(ST->status_msg),fmt,a);
    va_end(a);
    if(hStatus) SetWindowTextA(hStatus,ST->status_msg);
}

/* ── Set text on EDIT control (converts \n → \r\n for Win32) ──── */
static void setEditText(HWND hEdit, const char *text) {
    if (!text || !hEdit) { if (hEdit) SetWindowTextA(hEdit, ""); return; }
    /* Count \n that aren't preceded by \r */
    int extra = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n' && (p == text || *(p-1) != '\r')) extra++;
    if (extra == 0) { SetWindowTextA(hEdit, text); return; }
    /* Convert */
    size_t len = strlen(text);
    char *crlf = (char *)malloc(len + extra + 1);
    if (!crlf) { SetWindowTextA(hEdit, text); return; }
    char *dst = crlf;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && (p == text || *(p-1) != '\r'))
            *dst++ = '\r';
        *dst++ = *p;
    }
    *dst = '\0';
    SetWindowTextA(hEdit, crlf);
    free(crlf);
}
#define setPromptText(text) setEditText(hPromptText, (text))

/* ── Background credit check (avoids blocking UI thread) ─────── */

static volatile int g_credit_result = -1;
static volatile int g_credit_checking = 0;
static int  g_credit_key_index = -1;  /* which key was checked (-1 = first/default) */
static char g_credit_key_buf[128];    /* key string for per-key check */
static char g_credit_proxy_buf[256];  /* proxy URL for credit check (empty = no proxy) */

static DWORD WINAPI credit_check_thread(LPVOID param) {
    (void)param;
    g_credit_result = serper_check_credits(g_credit_key_buf,
        g_credit_proxy_buf[0] ? g_credit_proxy_buf : NULL);
    g_credit_checking = 0;
    PostMessage(hw, WM_APP+12, 0, 0);
    return 0;
}
static void credit_check_fill_proxy(AppState *st) {
    g_credit_proxy_buf[0] = '\0';
    if (st->proxy && st->proxy->enabled && st->proxy->pool_count > 0)
        proxy_get_next(st->proxy, g_credit_proxy_buf, sizeof(g_credit_proxy_buf));
}
static void start_credit_check(AppState *st) {
    if (g_credit_checking) return;
    if (st->config->api_key_count <= 0) return;
    g_credit_checking = 1;
    g_credit_key_index = -1;  /* default = first key */
    snprintf(g_credit_key_buf, sizeof(g_credit_key_buf), "%s",
             st->config->api_keys[0].key);
    credit_check_fill_proxy(st);
    HANDLE h = CreateThread(NULL, 0, credit_check_thread, st, 0, NULL);
    if (h) CloseHandle(h);
}
static void start_credit_check_key(AppState *st, int key_index) {
    if (g_credit_checking) return;
    if (key_index < 0 || key_index >= st->config->api_key_count) return;
    g_credit_checking = 1;
    g_credit_key_index = key_index;
    snprintf(g_credit_key_buf, sizeof(g_credit_key_buf), "%s",
             st->config->api_keys[key_index].key);
    credit_check_fill_proxy(st);
    HANDLE h = CreateThread(NULL, 0, credit_check_thread, st, 0, NULL);
    if (h) CloseHandle(h);
}

/* ── Engine callbacks (called from background thread) ─────────── */

static volatile long g_progress_update_flag = 0;
static char g_progress_msg[512];

static void on_engine_progress(const char *phase, float pct,
                               const char *message, void *userdata)
{
    (void)userdata; (void)phase;
    /* Do NOT write to ST->status_msg from background thread.
       Instead, post a message and let the main thread handle it. */
    if(hProgress && pct >= 0.0f) {
        int pos = (int)(pct * 100.0f);
        if(pos > 100) pos = 100;
        PostMessageA(hw, WM_APP+1, (WPARAM)pos, 0);
    }
    /* Copy into a thread-safe buffer, signal the main thread via WM_APP+10 */
    snprintf(g_progress_msg, sizeof(g_progress_msg), "%s", message);
    InterlockedExchange(&g_progress_update_flag, 1);
    PostMessageA(hw, WM_APP+10, 0, 0);
}

static void on_engine_page(const ScrapedPage *page, int rank, void *userdata)
{
    (void)userdata;
    /* We can't safely call SendMessage from background thread to add to listbox;
       store a formatted string and post a message to the UI thread. */
    char *text = (char *)malloc(512);
    if(!text) return;
    {char dom[29]; trunc_domain(page->domain, dom, 28);
    if(page->word_count > 0)
        snprintf(text, 512, "#%-3d  %s  %5d words  %3d hdg  %5.1fs  %s",
                 rank, dom, page->word_count, page->heading_count,
                 page->scrape_time, page->page_title);
    else
        snprintf(text, 512, "#%-3d  %s  FAILED: %s",
                 rank, dom, page->error);}
    PostMessageA(hw, WM_APP+2, 0, (LPARAM)text);
}

/* ── update_stat ──────────────────────────────────────────────── */

/* update_stat_val removed - stats updated via SetWindowTextA directly */

/* ── Paint helpers (section labels in WM_PAINT) ───────────────── */

static void paintSearch(HDC dc, RECT *rc) {
    (void)rc;
    /* Draw placeholder hint in keyword box when empty */
    if(hKeyword && GetWindowTextLengthA(hKeyword) == 0 && GetFocus() != hKeyword) {
        RECT kr; GetWindowRect(hKeyword, &kr);
        POINT kp = {kr.left, kr.top}; ScreenToClient(hw, &kp);
        SetTextColor(dc, RGB(100,100,120));
        SelectObject(dc, fNorm);
        TextOutA(dc, kp.x + D(4), kp.y + D(3), "Keywords (one per line) -- Ctrl+Enter to analyze", 48);
    }
}

static void paintResults(HDC dc, RECT *rc) {
    (void)rc;
    int y = CY + PAD;
    SelectObject(dc, fBold); SetTextColor(dc, CYAN);
    TextOutA(dc, M, y, "SCRAPED PAGES", 13);
    y += D(20);

    SetTextColor(dc, DIM); SelectObject(dc, fNorm);
    TextOutA(dc, M, y, "Each result shows: rank, domain, word count, headings, and scrape time.", 70);
    y += D(16);
    TextOutA(dc, M, y, "Click a result to see its headings and metadata below.", 53);
}

static void paintAnalysis(HDC dc, RECT *rc) {
    int y = CY + PAD;
    int cw = rc->right - M*2;
    SelectObject(dc, fBold); SetTextColor(dc, CYAN);
    TextOutA(dc, M, y, "GENERATED PROMPT", 16);
    y += D(20);

    SetTextColor(dc, DIM); SelectObject(dc, fNorm);
    TextOutA(dc, M, y, "AI prompt built from keywords, headings, and structure that top pages use.", 72);

    /* Template label (positioned relative to combo) */
    if(hTemplateCombo){RECT cr;GetWindowRect(hTemplateCombo,&cr);POINT p={cr.left,cr.top};ScreenToClient(hw,&p);
        SetTextColor(dc, DIM); SelectObject(dc, fNorm);
        TextOutA(dc,p.x-D(70),p.y+D(3),"Template:",9);}

    /* NLP info header */
    int info_x = M + cw*72/100 + PAD;
    SetTextColor(dc, CYAN); SelectObject(dc, fBold);
    TextOutA(dc, info_x, CY + PAD, "ANALYSIS SUMMARY", 16);

    /* Content gap section header */
    if(hGapInput) {
        RECT gr; GetWindowRect(hGapInput,&gr); POINT gp={gr.left,gr.top}; ScreenToClient(hw,&gp);
        int gy = gp.y - D(18);
        SetTextColor(dc, CYAN); SelectObject(dc, fBold);
        TextOutA(dc, M, gy, "CONTENT GAP ANALYZER", 20);
    }
}

static void paintSettings(HDC dc, RECT *rc) {
    int W = rc->right;
    int col_gap = D(30);
    int col_w = (W - M*2 - col_gap) / 2;
    int rx = M + col_w + col_gap;  /* right column x */

    /* Helper: get control position */
    #define CPOS(ctrl, px, py) { \
        RECT _r; GetWindowRect(ctrl,&_r); POINT _pt={_r.left,_r.top}; ScreenToClient(hw,&_pt); \
        px=_pt.x; py=_pt.y; }

    if (settingsSubTab == 0) {
        /* ── Connection: Left = Keys+Proxies, Right = Performance+HTTP ── */

        /* LEFT: API KEYS header + desc */
        if(hKeyInput && IsWindowVisible(hKeyInput)) {
            int cx,cy; CPOS(hKeyInput,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, M, cy - D(22) - D(16), "API KEYS", 8);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            TextOutA(dc, M, cy - D(16), "Right-click key to check balance", 32);
        }

        /* LEFT: PROXIES header + desc */
        if(hProxyInput && IsWindowVisible(hProxyInput)) {
            int cx,cy; CPOS(hProxyInput,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, M, cy - D(22) - D(16), "PRIVATE PROXIES", 15);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            TextOutA(dc, M, cy - D(16), "Optional. IP:PORT format.", 25);
        }

        /* LEFT: Proxy credential labels */
        if(hProxyUser && IsWindowVisible(hProxyUser)) {
            int cx,cy; CPOS(hProxyUser,cx,cy);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            TextOutA(dc, cx, cy - D(16), "Username", 8);
        }
        if(hProxyPass && IsWindowVisible(hProxyPass)) {
            int cx,cy; CPOS(hProxyPass,cx,cy);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            TextOutA(dc, cx, cy - D(16), "Password", 8);
        }

        /* RIGHT: PERFORMANCE */
        if(hThreadsCombo && IsWindowVisible(hThreadsCombo)) {
            int cx,cy; CPOS(hThreadsCombo,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, rx, cy - D(22) - D(6), "PERFORMANCE", 11);
            SetTextColor(dc, TXT); SelectObject(dc, fNorm);
            TextOutA(dc, rx, cy + D(3), "Threads:", 8);
        }
        /* RIGHT: HTTP SETTINGS */
        if(hGetTimeout && IsWindowVisible(hGetTimeout)) {
            int cx,cy; CPOS(hGetTimeout,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, rx, cy - D(22) - D(6), "HTTP SETTINGS", 13);
            SetTextColor(dc, TXT); SelectObject(dc, fNorm);
            TextOutA(dc, rx, cy + D(3), "GET (sec):", 10);
        }
        if(hPostTimeout && IsWindowVisible(hPostTimeout)) {
            int cx,cy; CPOS(hPostTimeout,cx,cy);
            SetTextColor(dc, TXT); SelectObject(dc, fNorm);
            TextOutA(dc, rx, cy + D(3), "POST (sec):", 11);
        }
        if(hMaxRedirects && IsWindowVisible(hMaxRedirects)) {
            int cx,cy; CPOS(hMaxRedirects,cx,cy);
            SetTextColor(dc, TXT); SelectObject(dc, fNorm);
            TextOutA(dc, rx, cy + D(3), "Redirects:", 10);
        }
        if(hRetryCount && IsWindowVisible(hRetryCount)) {
            int cx,cy; CPOS(hRetryCount,cx,cy);
            TextOutA(dc, rx, cy + D(3), "Retries:", 8);
        }
        if(hRetryBase && IsWindowVisible(hRetryBase)) {
            int cx,cy; CPOS(hRetryBase,cx,cy);
            TextOutA(dc, rx, cy + D(3), "Base (sec):", 11);
        }

    } else {
        /* ── Data & Tools: Left = Blocklist, Right = Resources+Tools+System ── */

        /* LEFT: BLOCKLIST */
        if(hBlocklistEdit && IsWindowVisible(hBlocklistEdit)) {
            int cx,cy; CPOS(hBlocklistEdit,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, M, cy - D(22) - D(16), "DOMAIN BLOCKLIST", 16);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            TextOutA(dc, M, cy - D(16), "Filtered from SERP results", 26);
        }


        /* RIGHT: TOOLS */
        if(hDownloadModels && IsWindowVisible(hDownloadModels)) {
            int cx,cy; CPOS(hDownloadModels,cx,cy);
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, rx, cy - D(22) - D(6), "TOOLS", 5);

            int dx = cx + D(210);
            SetTextColor(dc, DIM); SelectObject(dc, fNorm);
            /* Use cached value to avoid filesystem checks during WM_PAINT */
            if (g_models_cached < 0)
                g_models_cached = onnx_nlp_models_present("models");
            if(g_models_cached) {
                SetTextColor(dc, GREEN);
                TextOutA(dc, dx, cy + D(4), "Models installed", 16);
            } else {
                SetTextColor(dc, YELLOW);
                TextOutA(dc, dx, cy + D(4), "Not installed (~522MB)", 22);
            }

            {HWND hPurge = GetDlgItem(hw, ID_PURGE_DB);
            if(hPurge && IsWindowVisible(hPurge)) {
                int px,py; CPOS(hPurge,px,py);
                SetTextColor(dc, DIM); SelectObject(dc, fNorm);
                TextOutA(dc, px + D(210), py + D(4), "Clears all data", 15);
            }}
        }

        /* RIGHT: SYSTEM — live meters (STATIC is hidden, we paint directly) */
        if(hSysInfoLabel) {
            int cx,cy; CPOS(hSysInfoLabel,cx,cy);
            int sy = cy;
            SetTextColor(dc, CYAN); SelectObject(dc, fBold);
            TextOutA(dc, rx, sy - D(20), "SYSTEM", 6);

            /* Row 1: CPU + RAM meters side by side */
            int bar_w = (col_w - D(90)) / 2;  /* half column each */
            int bar_h = D(14);
            int bar_max = col_w - M;  /* right margin for meter bars */
            ResourceSnapshot snap = {0};
            if(ST->resmon) resmon_get(ST->resmon, &snap);

            /* CPU */
            {float pct = snap.system_cpu_pct;
            COLORREF c = pct > 90 ? RED : pct > 70 ? YELLOW : GREEN;
            char lbl[64]; snprintf(lbl, sizeof(lbl), "CPU %3.0f%%  %s", pct,
                SI.cpu_name[0] ? SI.cpu_name : "");
            SetTextColor(dc, TXT); SelectObject(dc, fNorm);
            TextOutA(dc, rx, sy, lbl, (int)strlen(lbl));
            sy += D(16);
            RECT bg1 = {rx, sy, rx + bar_max, sy + bar_h};
            FillRect(dc, &bg1, brBg3);
            int fw1 = (int)(bar_max * pct / 100.0f);
            if(fw1 > 0) { RECT fg1 = {rx, sy, rx + fw1, sy + bar_h};
            HBRUSH fb = CreateSolidBrush(c); FillRect(dc, &fg1, fb); DeleteObject(fb); }
            sy += bar_h + D(8);}

            /* RAM */
            {float pct = snap.system_ram_pct;
            COLORREF c = pct > 90 ? RED : pct > 70 ? YELLOW : GREEN;
            char lbl[64]; snprintf(lbl, sizeof(lbl), "RAM %3.0f%%  %d MB total", pct, SI.total_ram_mb);
            SetTextColor(dc, TXT);
            TextOutA(dc, rx, sy, lbl, (int)strlen(lbl));
            sy += D(16);
            RECT bg2 = {rx, sy, rx + bar_max, sy + bar_h};
            FillRect(dc, &bg2, brBg3);
            int fw2 = (int)(bar_max * pct / 100.0f);
            if(fw2 > 0) { RECT fg2 = {rx, sy, rx + fw2, sy + bar_h};
            HBRUSH fb = CreateSolidBrush(c); FillRect(dc, &fg2, fb); DeleteObject(fb); }
            sy += bar_h + D(8);}

            /* Row 2+: GPU meters — one per detected GPU, full width */
            for(int gi = 0; gi < SI.gpu_count; gi++) {
                float pct = (gi < snap.gpu_count) ? snap.gpu_util_pct[gi] : 0;
                COLORREF c = pct > 90 ? RED : pct > 70 ? YELLOW : GREEN;
                char lbl[128]; snprintf(lbl, sizeof(lbl), "GPU%d %3.0f%%  %s (%dMB)",
                    gi+1, pct, SI.gpu_names[gi], SI.gpu_vram_mbs[gi]);
                SetTextColor(dc, TXT);
                TextOutA(dc, rx, sy, lbl, (int)strlen(lbl));
                sy += D(16);
                RECT bgG = {rx, sy, rx + bar_max, sy + bar_h};
                FillRect(dc, &bgG, brBg3);
                int fwG = (int)(bar_max * pct / 100.0f);
                if(fwG > 0) { RECT fgG = {rx, sy, rx + fwG, sy + bar_h};
                HBRUSH fb = CreateSolidBrush(c); FillRect(dc, &fgG, fb); DeleteObject(fb); }
                sy += bar_h + D(8);
            }
            if(SI.gpu_count == 0) {
                SetTextColor(dc, YELLOW); SelectObject(dc, fNorm);
                TextOutA(dc, rx, sy, "No GPU detected - CPU inference only", 36);
            }
        }
    }
    #undef CPOS
}

static void paintLog(HDC dc, RECT *rc) {
    (void)rc;
    SelectObject(dc, fBold); SetTextColor(dc, CYAN);
    TextOutA(dc, M, CY + D(8), "APPLICATION LOG", 15);
    /* Filter buttons sit at CY + D(32), listbox starts at CY + D(60) */
}

/* ── Relayout ─────────────────────────────────────────────────── */

static void relayout(void) {
    if(!g_controls_ready) return;
    RECT rc; GetClientRect(hw,&rc);
    int W=rc.right, H=rc.bottom, cw=W-M*2;
    int statusH = D(40);
    int botY = H - statusH;
    int y;

    /* Tabs */
    {int tw=W/TAB_COUNT;
    for(int i=0;i<TAB_COUNT;i++) MoveWindow(tabs[i],i*tw,TABY,tw,TABH,TRUE);}

    /* ── Search tab: two-column layout ──────────────────────── */
    y = CY + PAD;

    /* Column dimensions */
    {int col_gap = D(12);
    int left_w = cw * 42 / 100;
    int right_x = M + left_w + col_gap;
    int right_w = cw - left_w - col_gap;

    /* RIGHT COLUMN: checkboxes + pages, then buttons + progress */
    int ry = y;

    /* Row 1: Checkboxes + Pages dropdown */
    {int cx = right_x;
    MoveWindow(hChkProxy, cx, ry, D(90), D(18), TRUE); cx += D(94);
    MoveWindow(hChkAutoScrape, cx, ry, D(85), D(18), TRUE); cx += D(89);
    MoveWindow(hChkAutoNlp, cx, ry, D(72), D(18), TRUE); cx += D(80);
    MoveWindow(hPagesCombo, cx, ry-D(1), D(38), 200, TRUE); cx += D(42);
    MoveWindow(hLblPages, cx, ry+D(1), D(40), EH, TRUE);}
    ry += D(18) + D(16);

    /* Row 2: Go + Stop buttons */
    {int btn_w = (right_w - HPAD) / 2;
    MoveWindow(hSearch, right_x, ry, btn_w, BH, TRUE);
    MoveWindow(hStop, right_x + btn_w + HPAD, ry, btn_w, BH, TRUE);}
    ry += BH + D(12);

    /* Row 3: Progress */
    MoveWindow(hLblProgress, right_x, ry, D(60), D(14), TRUE);
    MoveWindow(hProgress, right_x + D(60), ry + D(4), right_w - D(60), D(5), TRUE);
    ry += D(18);

    /* LEFT COLUMN: Keyword box fills full height */
    MoveWindow(hKeyword, M, y, left_w, ry - y, TRUE);

    y = ry + D(4);
    }

    /* History + Pending -- fill remaining space, history gets 2/3 */
    {int avail = botY - y;
    int lbl_h = D(16);
    int gap = D(4);
    int usable = avail - lbl_h*2 - gap*3;
    int hist_h = usable * 2 / 3;
    int pend_h = usable - hist_h;
    if(hist_h < D(40)) hist_h = D(40);
    if(pend_h < D(25)) pend_h = D(25);

    MoveWindow(hLblHistory, M, y, cw, lbl_h, TRUE);
    y += lbl_h + gap;
    MoveWindow(hHistoryList, M, y, cw, hist_h, TRUE);
    y += hist_h + gap;

    MoveWindow(hLblPending, M, y, cw, lbl_h, TRUE);
    y += lbl_h + gap;
    MoveWindow(hPendingList, M, y, cw, pend_h, TRUE);}

    /* Results tab - stacked layout (list on top, detail below) */
    {int list_top = CY + D(60) + PAD;
    int list_bot = botY - PAD;
    int split = list_top + (list_bot - list_top) * 55 / 100;
    MoveWindow(hResultsList, M, list_top, cw, split - list_top, TRUE);
    MoveWindow(hDetailText, M, split + HPAD, cw, list_bot - split - HPAD, TRUE);}

    /* ── Analysis tab ──────────────────── */
    {int ay = CY + PAD + D(22) + PAD + D(18) + PAD;  /* after header + description */
    /* Template row */
    MoveWindow(hTemplateCombo,M+D(75),ay,D(170),200,TRUE);
    int bx = M + D(260);
    MoveWindow(hCopyBtn,bx,ay,D(70),BH,TRUE);

    ay += BH + D(10);

    /* Prompt option checkboxes row */
    {int cx = M;
    MoveWindow(hChkKw,cx,ay,D(80),D(18),TRUE); cx+=D(84);
    MoveWindow(hChkEnt,cx,ay,D(72),D(18),TRUE); cx+=D(76);
    MoveWindow(hChkOutline,cx,ay,D(68),D(18),TRUE); cx+=D(72);
    MoveWindow(hChkStats,cx,ay,D(56),D(18),TRUE); cx+=D(60);
    MoveWindow(hChkPaa,cx,ay,D(48),D(18),TRUE); cx+=D(52);
    MoveWindow(hChkRelated,cx,ay,D(68),D(18),TRUE); cx+=D(80);
    MoveWindow(hLblMaxKw,cx,ay+D(1),D(22),D(16),TRUE); cx+=D(24);
    MoveWindow(hMaxKwEdit,cx,ay,D(32),D(18),TRUE); cx+=D(38);
    MoveWindow(hLblMaxEnt,cx,ay+D(1),D(26),D(16),TRUE); cx+=D(28);
    MoveWindow(hMaxEntEdit,cx,ay,D(32),D(18),TRUE);}
    ay += D(26);

    /* Prompt text + NLP info split */
    int prompt_w = cw*72/100;
    int info_w = cw - prompt_w - PAD;
    int anal_bot = botY - PAD;  /* gap above shutdown */
    /* Split: prompt gets 60%, gap area gets 40% of available vertical space */
    int total_avail = anal_bot - ay - SECGAP - D(20); /* minus section gap + label */
    int gap_total = total_avail * 40 / 100;
    if(gap_total < D(140)) gap_total = D(140);
    if(gap_total > D(300)) gap_total = D(300);
    int prompt_h = total_avail - gap_total;
    if(prompt_h < D(60)) prompt_h = D(60);
    MoveWindow(hPromptText,M,ay,prompt_w,prompt_h,TRUE);
    MoveWindow(hPromptList,M,ay,prompt_w,prompt_h,TRUE);
    MoveWindow(hNlpInfo,M+prompt_w+PAD,ay,info_w,anal_bot-ay,TRUE);

    /* Content gap area — input gets 40% of gap space, result gets 60% */
    int gy = ay + prompt_h + SECGAP + D(20);
    int gap_avail = anal_bot - gy - HPAD - BH; /* minus button height and padding */
    int gap_input_h = gap_avail * 40 / 100;
    if(gap_input_h < D(50)) gap_input_h = D(50);
    int gap_btn_x = M + prompt_w - D(92);
    MoveWindow(hGapInput,M,gy,prompt_w-D(100),gap_input_h,TRUE);
    MoveWindow(hGapAnalyze,gap_btn_x,gy,D(92),BH,TRUE);
    int gr_y = gy + gap_input_h + HPAD;
    int gr_h = anal_bot - gr_y; if(gr_h<D(16)) gr_h=D(16);
    MoveWindow(hGapResult,M,gr_y,prompt_w,gr_h,TRUE);}

    /* ── Settings tab — sub-tabs, two-column layout per sub-tab ── */
    {int stab_y = CY + D(6);
    int stab_w = D(130);
    MoveWindow(hSettingsSubConn, M, stab_y, stab_w, D(26), TRUE);
    MoveWindow(hSettingsSubData, M + stab_w + D(8), stab_y, stab_w, D(26), TRUE);

    int top = stab_y + D(44);       /* content starts below sub-tab buttons */
    int col_gap = D(30);             /* gap between columns */
    int col_w = (cw - col_gap) / 2;  /* each column width */
    int lx = M;                      /* left column x */
    int rx = M + col_w + col_gap;    /* right column x */
    int lbl_w = D(90);               /* label width for right-column rows */

    if (settingsSubTab == 0) {
        /* ── Connection: Left = API Keys + Proxies, Right = Performance + HTTP ── */
        /* Compute list heights to fit: total avail split between keys list + proxy list */
        int avail = botY - top;
        /* Fixed overhead per section: header(24)+desc(16)+gap(8)+input(BH)+gap(4)+import_row(BH)+gap(4) */
        int fixed_per = D(24) + D(16) + D(8) + BH + D(4) + BH + D(4);
        /* Extra for proxies: cred labels(18)+creds(EH)+gap(8)+automap(BH) */
        int proxy_extra = D(18) + EH + D(8) + BH;
        int gap_between = D(18);
        int list_avail = avail - fixed_per * 2 - proxy_extra - gap_between;
        int list_h = list_avail / 2;
        if (list_h < D(35)) list_h = D(35);
        if (list_h > D(120)) list_h = D(120);

        int ly = top;
        int inp_w = col_w - D(70);

        /* LEFT: API KEYS */
        ly += D(24) + D(16) + D(8);  /* header + desc */
        MoveWindow(hKeyInput, lx, ly, inp_w, EH, TRUE);
        MoveWindow(hKeyAdd, lx + inp_w + D(6), ly, D(28), BH, TRUE);
        MoveWindow(hKeyRemove, lx + inp_w + D(38), ly, D(28), BH, TRUE);
        ly += BH + D(4);
        MoveWindow(hKeysList, lx, ly, col_w, list_h, TRUE);
        ly += list_h + D(4);
        {HWND hi=GetDlgItem(hw,ID_KEY_IMPORT);
        HWND hp=GetDlgItem(hw,ID_KEY_PASTE);
        if(hi) MoveWindow(hi, lx, ly, D(80), BH, TRUE);
        if(hp) MoveWindow(hp, lx + D(86), ly, D(80), BH, TRUE);}
        ly += BH + gap_between;

        /* LEFT: PROXIES */
        ly += D(24) + D(16) + D(8);
        MoveWindow(hProxyInput, lx, ly, inp_w, EH, TRUE);
        MoveWindow(hProxyAdd, lx + inp_w + D(6), ly, D(28), BH, TRUE);
        MoveWindow(hProxyRemove, lx + inp_w + D(38), ly, D(28), BH, TRUE);
        ly += BH + D(4);
        MoveWindow(hProxyList, lx, ly, col_w, list_h, TRUE);
        ly += list_h + D(4);
        {HWND hi=GetDlgItem(hw,ID_PROXY_IMPORT);
        HWND hp=GetDlgItem(hw,ID_PROXY_PASTE);
        if(hi) MoveWindow(hi, lx, ly, D(80), BH, TRUE);
        if(hp) MoveWindow(hp, lx + D(86), ly, D(80), BH, TRUE);}
        ly += BH + D(4);
        ly += D(18);  /* cred labels */
        {int cw2 = (col_w - D(10)) / 2;
        MoveWindow(hProxyUser, lx, ly, cw2, EH, TRUE);
        MoveWindow(hProxyPass, lx + cw2 + D(10), ly, cw2, EH, TRUE);}
        ly += EH + D(8);
        MoveWindow(hAutoMap, lx, ly, D(200), BH, TRUE);

        /* RIGHT: PERFORMANCE */
        int ry2 = top;
        int rinp = rx + lbl_w;
        ry2 += D(24) + D(10);
        MoveWindow(hThreadsCombo, rinp, ry2, D(60), 200, TRUE);
        ry2 += EH + D(10);
        ry2 += EH + D(20);

        /* RIGHT: HTTP SETTINGS */
        ry2 += D(24) + D(10);
        {int iw = D(50);
        MoveWindow(hGetTimeout, rinp, ry2, iw, EH, TRUE);
        ry2 += EH + D(10);
        MoveWindow(hPostTimeout, rinp, ry2, iw, EH, TRUE);
        ry2 += EH + D(10);
        MoveWindow(hMaxRedirects, rinp, ry2, iw, EH, TRUE);
        ry2 += EH + D(10);
        MoveWindow(hRetryCount, rinp, ry2, iw, EH, TRUE);
        ry2 += EH + D(10);
        MoveWindow(hRetryBase, rinp, ry2, iw, EH, TRUE);}

    } else {
        /* ── Data & Tools: Left = Blocklist, Right = Resources + Tools + System ── */
        int ly = top;
        int bl_inp_w = col_w - D(70);

        /* LEFT: DOMAIN BLOCKLIST */
        ly += D(26) + D(8) + D(16) + D(10);
        MoveWindow(hBlocklistEdit, lx, ly, bl_inp_w, EH, TRUE);
        MoveWindow(hBlocklistAdd, lx + bl_inp_w + D(6), ly, D(28), BH, TRUE);
        MoveWindow(hBlocklistDel, lx + bl_inp_w + D(38), ly, D(28), BH, TRUE);
        ly += BH + D(6);
        int list_h = botY - ly - BH - D(14);
        if (list_h < D(60)) list_h = D(60);
        MoveWindow(hBlocklistList, lx, ly, col_w, list_h, TRUE);
        ly += list_h + D(4);
        /* Import + Paste below list */
        {HWND hi=GetDlgItem(hw,ID_BLOCKLIST_IMPORT);
        HWND hp=GetDlgItem(hw,ID_BLOCKLIST_PASTE);
        if(hi) MoveWindow(hi, lx, ly, D(80), BH, TRUE);
        if(hp) MoveWindow(hp, lx + D(86), ly, D(80), BH, TRUE);}

        int ry2 = top;
        int rinp = rx + lbl_w;

        /* RIGHT: TOOLS */
        ry2 += D(26) + D(10);
        MoveWindow(hDownloadModels, rx, ry2, D(200), BH, TRUE);
        ry2 += BH + D(12);
        {HWND hPurge = GetDlgItem(hw, ID_PURGE_DB);
        if(hPurge) MoveWindow(hPurge, rx, ry2, D(200), BH, TRUE);}
        ry2 += BH + SECGAP;

        /* RIGHT: SYSTEM — hide the STATIC, meters are GDI-painted */
        MoveWindow(hSysInfoLabel, rx, ry2, col_w, botY - ry2 - D(4), TRUE);
        ShowWindow(hSysInfoLabel, SW_HIDE);  /* meters painted directly */
    }}

    /* ── Log tab ───────────────────────── */
    {int log_top = CY + D(22) + PAD + D(18) + PAD;
    int log_bot = botY - PAD;  /* gap above shutdown */
    {int fx = M; int fbw = D(64); int fgap = D(8);
    int log_btn_y = CY + D(32);
    int log_top2 = CY + D(60);
    MoveWindow(hLogFilterDbg,fx,log_btn_y,fbw,BH,TRUE); fx+=fbw+fgap;
    MoveWindow(hLogFilterInfo,fx,log_btn_y,fbw,BH,TRUE); fx+=fbw+fgap;
    MoveWindow(hLogFilterWarn,fx,log_btn_y,fbw,BH,TRUE); fx+=fbw+fgap;
    MoveWindow(hLogFilterErr,fx,log_btn_y,fbw,BH,TRUE);
    MoveWindow(hLogClear,W-M-D(60),log_btn_y,D(60),BH,TRUE);
    MoveWindow(hLogList,M,log_top2,cw,log_bot-log_top2,TRUE);}}

    /* ── Footer bar: resmon labels + status + shutdown ── */
    {int fy = H - statusH;
    int pad = D(10);  /* top/bottom padding */
    int eh = statusH - pad*2;  /* element height */
    int resmon_w = D(110);
    int gap = D(10);
    MoveWindow(hResmonCpu, M, fy+pad, resmon_w, eh, TRUE);
    MoveWindow(hResmonRam, M+resmon_w+gap, fy+pad, resmon_w, eh, TRUE);
    int gpu_x = M+resmon_w*2+gap*2;
    int nuke_w = D(85);
    int nuke_x = W-M-nuke_w;
    /* GPU label gets generous width for multi-GPU */
    int gpu_w = D(260);
    MoveWindow(hResmonGpu, gpu_x, fy+pad, gpu_w, eh, TRUE);
    MoveWindow(hNuke, nuke_x, fy+pad, nuke_w, eh, TRUE);
    /* Status text between GPU and shutdown */
    int status_x = gpu_x + gpu_w + gap;
    MoveWindow(hStatus, status_x, fy+pad, nuke_x-status_x-gap, eh, TRUE);}
}

/* ── Results list refresh from DB ─────────────────────────────── */

static void refresh_results_list(const char *keyword) {
    if (!ST || !ST->db) return;
    /* Save scroll position and selection */
    int saved_top = (int)SendMessageA(hResultsList, LB_GETTOPINDEX, 0, 0);
    int saved_caret = (int)SendMessageA(hResultsList, LB_GETCURSEL, 0, 0);

    SendMessageA(hResultsList, WM_SETREDRAW, FALSE, 0);
    SendMessageA(hResultsList, LB_RESETCONTENT, 0, 0);
    g_result_map_count = 0;

    g_result_row_count = db_get_results((Database *)ST->db, keyword, g_result_rows, RESULT_DB_MAX);

    if (g_result_row_count == 0) {
        SendMessageA(hResultsList, LB_ADDSTRING, 0, (LPARAM)"(no scraped results)");
        SendMessageA(hResultsList, WM_SETREDRAW, TRUE, 0);
        return;
    }

    /* Header */
    SendMessageA(hResultsList, LB_ADDSTRING, 0,
        (LPARAM)" #    Domain                        Words   Hdg   Time   Keyword");
    if (g_result_map_count < RESULT_MAP_MAX) g_result_page_map[g_result_map_count++] = -1;
    SendMessageA(hResultsList, LB_ADDSTRING, 0,
        (LPARAM)"---   ----------------------------  -----   ---   -----  -------");
    if (g_result_map_count < RESULT_MAP_MAX) g_result_page_map[g_result_map_count++] = -1;

    /* Build parent-child index: hash source_url -> list of child indices.
       Replaces the O(n^2) nested loop with O(n) build + O(1) lookup. */
    #define OBL_HT_BUCKETS 521  /* prime */
    typedef struct OblNode { int idx; struct OblNode *next; } OblNode;
    OblNode *obl_ht[OBL_HT_BUCKETS];
    memset(obl_ht, 0, sizeof(obl_ht));

    /* Pre-allocate node storage -- at most g_result_row_count OBL children */
    OblNode *obl_nodes = (OblNode *)malloc(g_result_row_count * sizeof(OblNode));
    int obl_node_count = 0;

    if (obl_nodes) {
        for (int j = 0; j < g_result_row_count; j++) {
            DbResultRow *c = &g_result_rows[j];
            if (!c->is_crawled || !c->source_url[0]) continue;
            /* DJB2 hash of source_url */
            unsigned long h = 5381;
            for (const char *p = c->source_url; *p; p++)
                h = ((h << 5) + h) + (unsigned char)*p;
            int bucket = (int)(h % OBL_HT_BUCKETS);
            OblNode *n = &obl_nodes[obl_node_count++];
            n->idx = j;
            n->next = obl_ht[bucket];
            obl_ht[bucket] = n;
        }
    }

    /* SERP pages first, then OBL children via hash lookup */
    for (int i = 0; i < g_result_row_count; i++) {
        DbResultRow *r = &g_result_rows[i];
        if (r->is_crawled) continue;  /* show SERP pages first */
        char line[320], dom[29];
        trunc_domain(r->domain, dom, 28);
        const char *fail = (strcmp(r->status, "failed") == 0) ? " FAIL" : "";
        snprintf(line, sizeof(line), "#%-3d  %s  %5d   %3d   %5.1fs%s%s %s",
            r->serp_position, dom, r->word_count, r->heading_count,
            r->scrape_time_secs, r->js_fallback ? " JS" : "   ", fail, r->keyword);
        SendMessageA(hResultsList, LB_ADDSTRING, 0, (LPARAM)line);
        if (g_result_map_count < RESULT_MAP_MAX) g_result_page_map[g_result_map_count++] = i;

        /* OBL children under this SERP page -- O(1) hash lookup */
        if (obl_nodes) {
            unsigned long h = 5381;
            for (const char *p = r->url; *p; p++)
                h = ((h << 5) + h) + (unsigned char)*p;
            int bucket = (int)(h % OBL_HT_BUCKETS);
            for (OblNode *n = obl_ht[bucket]; n; n = n->next) {
                DbResultRow *c = &g_result_rows[n->idx];
                if (strcmp(c->source_url, r->url) != 0) continue;
                char cline[320], cdom[29];
                trunc_domain(c->domain, cdom, 28);
                const char *cfail = (strcmp(c->status, "failed") == 0) ? " FAIL" : "";
                snprintf(cline, sizeof(cline), "  -- %s  %5d   %3d   %5.1fs%s",
                    cdom, c->word_count, c->heading_count, c->scrape_time_secs, cfail);
                SendMessageA(hResultsList, LB_ADDSTRING, 0, (LPARAM)cline);
                if (g_result_map_count < RESULT_MAP_MAX) g_result_page_map[g_result_map_count++] = n->idx;
            }
        }
    }

    free(obl_nodes);
    #undef OBL_HT_BUCKETS

    /* Set horizontal scroll width for long lines */
    /* No horizontal scroll — vertical only */

    /* Restore scroll position */
    int total_rows = (int)SendMessageA(hResultsList, LB_GETCOUNT, 0, 0);
    if (saved_top >= 0 && saved_top < total_rows)
        SendMessageA(hResultsList, LB_SETTOPINDEX, saved_top, 0);
    if (saved_caret >= 0 && saved_caret < total_rows)
        SendMessageA(hResultsList, LB_SETCURSEL, saved_caret, 0);

    SendMessageA(hResultsList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hResultsList, NULL, TRUE);
    {int serp_n=0, obl_n=0;
    for(int i=0;i<g_result_row_count;i++) g_result_rows[i].is_crawled ? obl_n++ : serp_n++;
    app_log(LOG_INFO, "Results list refreshed: %d DB rows (%d SERP + %d OBL), %d listbox rows",
            g_result_row_count, serp_n, obl_n, g_result_map_count);}
}

/* ── Pending queue refresh ───────────────────────────────────── */

static void refresh_pending_list(void) {
    if (!ST || !ST->db || !hPendingList) return;
    int pending = db_count_pending_urls(ST->db);

    /* Only refresh if count changed (avoid flicker) */
    if (pending == g_pend_count) return;

    /* Suppress painting during bulk update */
    SendMessageA(hPendingList, WM_SETREDRAW, FALSE, 0);
    SendMessageA(hPendingList, LB_RESETCONTENT, 0, 0);
    g_pend_count = 0;

    {char lbl[64]; snprintf(lbl, sizeof(lbl), "Pending scrape queue: %d", pending);
    SetWindowTextA(hLblPending, lbl);}

    g_pend_map_count = 0;
    if (pending == 0) {
        SendMessageA(hPendingList, LB_ADDSTRING, 0, (LPARAM)"(no pending scrape jobs)");
    } else {
        int show = pending < PENDING_MAX ? pending : PENDING_MAX;
        g_pend_count = db_get_pending_urls_rich(ST->db,
            g_pend_urls, g_pend_keywords, g_pend_domains,
            g_pend_titles, g_pend_snippets, g_pend_positions, show);

        /* Column header */
        SendMessageA(hPendingList, LB_ADDSTRING, 0,
            (LPARAM)"Rank  Domain                        Keyword");
        if (g_pend_map_count < PEND_MAP_MAX) g_pend_map[g_pend_map_count++] = -1;
        SendMessageA(hPendingList, LB_ADDSTRING, 0,
            (LPARAM)"----  ----------------------------  -------");
        if (g_pend_map_count < PEND_MAP_MAX) g_pend_map[g_pend_map_count++] = -1;

        /* Sorted by keyword then rank (DB ORDER BY keyword, serp_position) */
        for (int i = 0; i < g_pend_count; i++) {
            char line[320], dom[29];
            trunc_domain(g_pend_domains[i], dom, 28);
            snprintf(line, sizeof(line), "#%-3d  %s  %s",
                g_pend_positions[i], dom, g_pend_keywords[i]);
            SendMessageA(hPendingList, LB_ADDSTRING, 0, (LPARAM)line);
            if (g_pend_map_count < PEND_MAP_MAX) g_pend_map[g_pend_map_count++] = i;
        }
    }

    SendMessageA(hPendingList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hPendingList, NULL, TRUE);
}

/* ── Scrape pending ──────────────────────────────────────────── */

/* Thread arg for scraping pending URLs (used by both "Scrape All" and "Scrape Selected") */
typedef struct {
    char **urls;
    char **keywords;
    int count;
    int do_nlp;  /* 0 = scrape only, 1 = scrape + corpus NLP + save session */
} ScrapeSelectedArg;

static DWORD WINAPI scrape_selected_thread(LPVOID arg) {
    ScrapeSelectedArg *sa = (ScrapeSelectedArg *)arg;
    SearchEngine *eng = ST->engine;

    /* Create scrape pool if needed */
    if (!eng->scrape_pool_created) {
        int threads = eng->scrape_threads;
        if (threads < 4) threads = 4;
        if (threads > 128) threads = 128;
        tp_create(&eng->scrape_pool, threads, 0);
        eng->scrape_pool_created = 1;

    }

    /* Free old engine pages */
    if (eng->pages) {
        for (int i = 0; i < eng->page_count; i++) scraper_free(&eng->pages[i]);
        free(eng->pages);
    }
    eng->pages = (ScrapedPage *)calloc(sa->count, sizeof(ScrapedPage));
    if (!eng->pages) {
        app_log(LOG_ERROR, "Scrape: failed to allocate %d pages (%d MB)",
                sa->count, (int)(sa->count * sizeof(ScrapedPage) / (1024*1024)));
        eng->page_count = 0;
        eng->running = 0;
        goto cleanup;
    }
    eng->page_count = sa->count;
    app_log(LOG_INFO, "Scrape: starting %d URLs (%d MB allocated)",
            sa->count, (int)(sa->count * sizeof(ScrapedPage) / (1024*1024)));

    /* Submit scrape jobs */
    for (int i = 0; i < sa->count && !eng->stop; i++) {
        ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
        if (!job) continue;
        job->eng = eng;
        snprintf(job->url, sizeof(job->url), "%s", sa->urls[i]);
        snprintf(job->keyword, sizeof(job->keyword), "%s", sa->keywords[i]);
        job->rank = i + 1;
        job->result = &eng->pages[i];
        tp_submit(&eng->scrape_pool, scrape_worker, job);
    }
    tp_wait(&eng->scrape_pool);

    /* Run per-page keywords */
    int ok = 0;
    for (int i = 0; i < sa->count; i++) {
        if (eng->pages[i].word_count > 50) {
            ok++;
            nlp_extract_page_keywords(&eng->pages[i]);
            /* Save page keywords to DB */
            if (ST->db && eng->pages[i].page_keyword_count > 0)
                db_save_serp_page_keywords((Database *)ST->db, eng->pages[i].page_url,
                    eng->pages[i].keyword, (const char(*)[64])eng->pages[i].page_keywords,
                    eng->pages[i].page_keyword_count);
        }
    }
    app_log(LOG_INFO, "Scrape complete: %d/%d successful", ok, sa->count);

    /* Group by keyword and run corpus NLP + save session for each (if requested) */
    if (!sa->do_nlp) goto skip_nlp;
    {char done_kws[50][256]; int done_count = 0;
    for (int i = 0; i < sa->count && done_count < 50; i++) {
        /* Skip if already processed this keyword */
        int already = 0;
        for (int d = 0; d < done_count; d++)
            if (strcmp(done_kws[d], eng->pages[i].keyword) == 0) { already = 1; break; }
        if (already || !eng->pages[i].keyword[0]) continue;
        snprintf(done_kws[done_count++], 256, "%s", eng->pages[i].keyword);

        const char *kw = eng->pages[i].keyword;

        /* Collect pages for this keyword */
        ScrapedPage *kw_pages = (ScrapedPage *)malloc(sa->count * sizeof(ScrapedPage));
        int kw_count = 0;
        if (kw_pages) {
            for (int j = 0; j < sa->count; j++) {
                if (strcmp(eng->pages[j].keyword, kw) == 0)
                    kw_pages[kw_count++] = eng->pages[j];  /* shallow copy for NLP */
            }

            /* Run corpus NLP */
            NLPResult *nlp = eng->nlp_result;
            if (nlp && kw_count > 0) {
                nlp_analyze(kw, kw_pages, kw_count, nlp, eng->onnx, eng->nli);

                /* ONNX NER entity extraction */
                if (eng->onnx && onnx_nlp_available(eng->onnx)) {
                    const char **ner_texts = (const char **)malloc(kw_count * sizeof(char *));
                    const char **ner_doms = (const char **)malloc(kw_count * sizeof(char *));
                    int ner_valid = 0;
                    if (ner_texts && ner_doms) {
                        for (int ni = 0; ni < kw_count; ni++) {
                            if (kw_pages[ni].page_text && kw_pages[ni].word_count > 50) {
                                ner_texts[ner_valid] = kw_pages[ni].page_text;
                                ner_doms[ner_valid] = kw_pages[ni].domain;
                                ner_valid++;
                            }
                        }
                        if (ner_valid > 0) {
                            __try {
                                nlp->entity_count = onnx_nlp_extract_entities_batch(
                                    eng->onnx, ner_texts, ner_doms, ner_valid,
                                    nlp->entities, NLP_MAX_ENTITIES);
                            } __except(EXCEPTION_EXECUTE_HANDLER) {
                                nlp->entity_count = 0;
                            }
                            app_log(LOG_INFO, "NER: %d entities from %d pages for '%s'",
                                    nlp->entity_count, ner_valid, kw);
                        }
                    }
                    free(ner_texts); free(ner_doms);
                }

                /* Generate prompt */
                {char *prompt_buf = (char *)malloc(PROMPT_MAX_LEN);
                if (prompt_buf) {
                    PromptOptions opts = {1,1,1,1,1,1, PROMPT_FULL, 0, 0};
                    prompt_build(nlp, &opts, prompt_buf, PROMPT_MAX_LEN);
                    free(eng->generated_prompt);
                    eng->generated_prompt = prompt_buf;
                }}

                /* Save session to DB */
                if (ST->db) {
                    char *nlp_json = (char *)malloc(PROMPT_MAX_LEN);
                    if (nlp_json) {
                        prompt_export_json(nlp, nlp_json, PROMPT_MAX_LEN);
                        cJSON *root_j = cJSON_CreateObject();
                        cJSON *nlp_obj = cJSON_Parse(nlp_json);
                        if (nlp_obj && root_j) {
                            cJSON *child = nlp_obj->child;
                            while(child) { cJSON *next = child->next; cJSON_DetachItemViaPointer(nlp_obj, child); cJSON_AddItemToObject(root_j, child->string, child); child = next; }
                            cJSON_Delete(nlp_obj);

                            /* Pages array */
                            cJSON *pages_j = cJSON_CreateArray();
                            for (int p = 0; p < kw_count; p++) {
                                ScrapedPage *pg = &kw_pages[p];
                                cJSON *pj = cJSON_CreateObject();
                                cJSON_AddStringToObject(pj, "domain", pg->domain);
                                cJSON_AddStringToObject(pj, "url", pg->page_url);
                                cJSON_AddStringToObject(pj, "title", pg->page_title);
                                cJSON_AddStringToObject(pj, "meta_desc", pg->meta_description);
                                cJSON_AddNumberToObject(pj, "words", pg->word_count);
                                cJSON_AddNumberToObject(pj, "headings", pg->heading_count);
                                cJSON_AddNumberToObject(pj, "outbound", pg->outbound_count);
                                cJSON_AddNumberToObject(pj, "internal", pg->internal_count);
                                cJSON_AddNumberToObject(pj, "scrape_time", pg->scrape_time);
                                cJSON_AddNumberToObject(pj, "serp_position", pg->serp_position);
                                cJSON_AddNumberToObject(pj, "is_crawled", pg->is_crawled);
                                cJSON_AddNumberToObject(pj, "source_idx", pg->source_page_idx);
                                cJSON_AddNumberToObject(pj, "js_fallback", pg->used_js_fallback);
                                if (pg->keyword[0]) cJSON_AddStringToObject(pj, "keyword", pg->keyword);
                                /* Per-page headings */
                                if (pg->heading_count > 0) {
                                    cJSON *hdgs = cJSON_CreateArray();
                                    for (int h = 0; h < pg->heading_count && h < 50; h++) {
                                        cJSON *hj = cJSON_CreateObject();
                                        cJSON_AddStringToObject(hj, "tag", pg->headings[h].tag);
                                        cJSON_AddStringToObject(hj, "text", pg->headings[h].text);
                                        cJSON_AddItemToArray(hdgs, hj);
                                    }
                                    cJSON_AddItemToObject(pj, "page_headings", hdgs);
                                }
                                /* Per-page keywords */
                                if (pg->page_keyword_count > 0) {
                                    cJSON *pkw = cJSON_CreateArray();
                                    for (int k = 0; k < pg->page_keyword_count; k++)
                                        cJSON_AddItemToArray(pkw, cJSON_CreateString(pg->page_keywords[k]));
                                    cJSON_AddItemToObject(pj, "page_keywords", pkw);
                                }
                                cJSON_AddItemToArray(pages_j, pj);
                            }
                            cJSON_AddItemToObject(root_j, "pages", pages_j);

                            char *final_json = cJSON_Print(root_j);
                            if (final_json) {
                                db_save_session((Database *)ST->db, kw, final_json);
                                app_log(LOG_INFO, "Session saved: '%s' (%d pages, %d kw, %d bytes)",
                                        kw, kw_count, nlp->keyword_count, (int)strlen(final_json));
                                cJSON_free(final_json);
                            }
                        }
                        if (root_j) cJSON_Delete(root_j);
                        free(nlp_json);
                    }
                }

                app_log(LOG_INFO, "NLP complete for '%s': %d pages, %d keywords, %d headings",
                        kw, kw_count, nlp->keyword_count, nlp->heading_count);
            }
            free(kw_pages);
        }
    }}

skip_nlp:
    eng->running = 0;
    http_thread_cleanup();
    PostMessageA(hw, WM_APP+3, sa->do_nlp ? (1|2) : 0, (LPARAM)sa->count);

cleanup:
    for (int i = 0; i < sa->count; i++) { free(sa->urls[i]); free(sa->keywords[i]); }
    free(sa->urls);
    free(sa->keywords);
    free(sa);
    return 0;
}

static void on_scrape_pending_ex(int do_nlp) {
    if (engine_is_running(ST->engine)) {
        themedDialog("Engine Running", "Wait for the current operation to finish.", 0);
        return;
    }
    if (g_pend_count == 0) {
        themedDialog("No Pending URLs", "There are no pending URLs to scrape.", 0);
        return;
    }

    ScrapeSelectedArg *sa = (ScrapeSelectedArg *)calloc(1, sizeof(ScrapeSelectedArg));
    if (!sa) return;
    sa->urls = (char **)calloc(g_pend_count, sizeof(char *));
    sa->keywords = (char **)calloc(g_pend_count, sizeof(char *));
    if (!sa->urls || !sa->keywords) { free(sa->urls); free(sa->keywords); free(sa); return; }

    for (int i = 0; i < g_pend_count; i++) {
        sa->urls[i] = str_duplicate(g_pend_urls[i]);
        sa->keywords[i] = str_duplicate(g_pend_keywords[i]);
    }
    sa->count = g_pend_count;
    sa->do_nlp = do_nlp;

    ST->engine->stop = 0;
    EnableWindow(hSearch, FALSE);
    setStatus("%s %d URLs...", do_nlp ? "Scraping + NLP for" : "Scraping", g_pend_count);

    if (ST->engine->pipeline_handle) {
        CloseHandle(ST->engine->pipeline_handle);
        ST->engine->pipeline_handle = NULL;
    }
    ST->engine->pipeline_handle = CreateThread(NULL, 0, scrape_selected_thread, sa, 0, NULL);
    ST->engine->running = 1;  /* set AFTER handle stored so engine_nuke can find it */
}

static void on_scrape_pending_selected_ex(int do_nlp) {
    if (engine_is_running(ST->engine)) {
        themedDialog("Engine Running", "Wait for the current operation to finish.", 0);
        return;
    }

    int sel_count = (int)SendMessageA(hPendingList, LB_GETSELCOUNT, 0, 0);
    if (sel_count <= 0) {
        themedDialog("No Selection", "Select one or more pending URLs first.", 0);
        return;
    }

    /* Collect selected URLs via the map */
    int *rows = (int *)malloc(sel_count * sizeof(int));
    if (!rows) return;
    SendMessageA(hPendingList, LB_GETSELITEMS, sel_count, (LPARAM)rows);

    ScrapeSelectedArg *sa = (ScrapeSelectedArg *)calloc(1, sizeof(ScrapeSelectedArg));
    if (!sa) { free(rows); return; }
    sa->urls = (char **)calloc(sel_count, sizeof(char *));
    sa->keywords = (char **)calloc(sel_count, sizeof(char *));
    if (!sa->urls || !sa->keywords) { free(sa->urls); free(sa->keywords); free(sa); free(rows); return; }

    int actual = 0;
    for (int i = 0; i < sel_count; i++) {
        int row = rows[i];
        int di = (row >= 0 && row < g_pend_map_count) ? g_pend_map[row] : -1;
        if (di >= 0 && di < g_pend_count) {
            sa->urls[actual] = str_duplicate(g_pend_urls[di]);
            sa->keywords[actual] = str_duplicate(g_pend_keywords[di]);
            actual++;
        }
    }
    free(rows);

    if (actual == 0) {
        free(sa->urls); free(sa->keywords); free(sa);
        themedDialog("No Valid URLs", "Selected rows don't contain valid pending URLs.", 0);
        return;
    }
    sa->count = actual;
    sa->do_nlp = do_nlp;

    ST->engine->running = 1;
    ST->engine->stop = 0;
    EnableWindow(hSearch, FALSE);
    setStatus("%s %d selected URLs...", do_nlp ? "Scraping + NLP for" : "Scraping", actual);

    if (ST->engine->pipeline_handle) {
        CloseHandle(ST->engine->pipeline_handle);
        ST->engine->pipeline_handle = NULL;
    }
    ST->engine->pipeline_handle = CreateThread(NULL, 0, scrape_selected_thread, sa, 0, NULL);
}

static void on_pending_delete_selected(void) {
    if (!ST || !ST->db || !hPendingList) return;
    int sel_count = (int)SendMessageA(hPendingList, LB_GETSELCOUNT, 0, 0);
    if (sel_count <= 0) return;

    int *rows = (int *)malloc(sel_count * sizeof(int));
    if (!rows) return;
    SendMessageA(hPendingList, LB_GETSELITEMS, sel_count, (LPARAM)rows);

    /* Use the map to find data indices, delete from DB */
    int deleted = 0;
    for (int i = sel_count - 1; i >= 0; i--) {
        int row = rows[i];
        int di = (row >= 0 && row < g_pend_map_count) ? g_pend_map[row] : -1;
        if (di >= 0 && di < g_pend_count) {
            db_delete_pending_url(ST->db, g_pend_urls[di], g_pend_keywords[di]);
            deleted++;
        }
    }
    free(rows);
    refresh_pending_list();
    setStatus("Deleted %d pending URLs", deleted);
}

static void on_pending_delete_all(void) {
    int count = db_count_pending_urls(ST->db);
    if (count <= 0) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete all %d pending URLs? This cannot be undone.", count);
    if (!themedDialog("Delete All Pending", msg, 1)) return;
    db_delete_all_pending_urls(ST->db);
    refresh_pending_list();
    setStatus("Deleted all %d pending URLs", count);
}

/* ── Results context: OBL crawl + per-page NLP ───────────────── */

/* Get selected page indices from results list (skips headers and -1 map entries) */
static int get_selected_page_indices(int *out_indices, int max_count) {
    int sel_count = (int)SendMessageA(hResultsList, LB_GETSELCOUNT, 0, 0);
    if (sel_count <= 0) return 0;
    int *sel_items = (int *)malloc(sel_count * sizeof(int));
    if (!sel_items) return 0;
    SendMessageA(hResultsList, LB_GETSELITEMS, sel_count, (LPARAM)sel_items);
    int count = 0;
    for (int i = 0; i < sel_count && count < max_count; i++) {
        int row = sel_items[i];
        if (row >= 0 && row < g_result_map_count && g_result_page_map[row] >= 0)
            out_indices[count++] = g_result_page_map[row];
    }
    free(sel_items);
    return count;
}

/* Thread arg for OBL + NLP operations */
/* Action types for result_action_thread */
#define ACTION_BUILD_PROMPT  1  /* corpus NLP → prompt → history */
#define ACTION_CRAWL_OBL     2  /* fetch outbound links, add to results */

typedef struct {
    char **urls;
    char **keywords;
    int count;
    int action;           /* ACTION_BUILD_PROMPT or ACTION_CRAWL_OBL */
    int needs_scrape;     /* 1 = scrape URLs first (from queue), 0 = use existing DB data (from results) */
    char group_name[256]; /* keyword/name for the session in History */
} ResultActionArg;

static DWORD WINAPI result_action_thread(LPVOID arg) {
    ResultActionArg *ra = (ResultActionArg *)arg;
    SearchEngine *eng = ST->engine;
    app_log(LOG_INFO, "Result action: action=%d count=%d scrape=%d name='%s'",
            ra->action, ra->count, ra->needs_scrape, ra->group_name);

    /* Ensure scrape pool exists */
    if (!eng->scrape_pool_created) {
        int threads = eng->scrape_threads;
        if (threads < 4) threads = 4;
        if (threads > 128) threads = 128;
        tp_create(&eng->scrape_pool, threads, 0);
        eng->scrape_pool_created = 1;

    }

    int result_flags = 0;  /* for WM_APP+3: 1=has session, 2=obl added */

    if (ra->action == ACTION_BUILD_PROMPT) {
        /* ── Build Prompt: scrape (if needed) → corpus NLP → prompt → History ── */
        ScrapedPage *pages = (ScrapedPage *)calloc(ra->count, sizeof(ScrapedPage));
        if (!pages) { app_log(LOG_ERROR, "Build prompt: alloc failed"); goto done; }

        if (ra->needs_scrape) {
            /* Scrape all URLs via thread pool */
            for (int i = 0; i < ra->count && !eng->stop; i++) {
                ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                if (!job) continue;
                job->eng = eng;
                snprintf(job->url, sizeof(job->url), "%s", ra->urls[i]);
                snprintf(job->keyword, sizeof(job->keyword), "%s", ra->keywords[i]);
                job->rank = i + 1;
                job->result = &pages[i];
                tp_submit(&eng->scrape_pool, scrape_worker, job);
            }
            tp_wait(&eng->scrape_pool);
            app_log(LOG_INFO, "Build prompt: scrape complete for %d URLs", ra->count);
        } else {
            /* Re-scrape from Results tab (pages already in DB, need fresh text for NLP) */
            for (int i = 0; i < ra->count && !eng->stop; i++) {
                app_log(LOG_INFO, "Build prompt [%d/%d]: fetching %s", i+1, ra->count, ra->urls[i]);
                scraper_fetch_page(ra->urls[i], NULL, &pages[i]);
                snprintf(pages[i].keyword, sizeof(pages[i].keyword), "%s", ra->keywords[i]);
                pages[i].serp_position = i + 1;
            }
            app_log(LOG_INFO, "Build prompt: fetched %d pages for NLP", ra->count);
        }

        /* Per-page keywords */
        int ok = 0;
        for (int i = 0; i < ra->count; i++) {
            if (pages[i].word_count > 50) {
                nlp_extract_page_keywords(&pages[i]);
                if (ST->db && pages[i].page_keyword_count > 0)
                    db_save_serp_page_keywords((Database *)ST->db, pages[i].page_url,
                        pages[i].keyword, (const char(*)[64])pages[i].page_keywords,
                        pages[i].page_keyword_count);
                ok++;
            }
        }
        app_log(LOG_INFO, "Build prompt: %d/%d pages have content", ok, ra->count);

        /* Corpus NLP — group by keyword, build separate prompts for each */
        NLPResult *nlp = eng->nlp_result;
        if (nlp && ok > 0) {
        char done_kws[50][256]; int done_count = 0;
        for (int ki = 0; ki < ra->count && done_count < 50; ki++) {
            if (!pages[ki].keyword[0]) continue;
            /* Skip if already processed this keyword */
            int already = 0;
            for (int d = 0; d < done_count; d++)
                if (strcmp(done_kws[d], pages[ki].keyword) == 0) { already = 1; break; }
            if (already) continue;
            snprintf(done_kws[done_count++], 256, "%s", pages[ki].keyword);
            const char *kw = pages[ki].keyword;

            /* Collect pages for this keyword into a temp array */
            ScrapedPage *kw_pages = (ScrapedPage *)malloc(ra->count * sizeof(ScrapedPage));
            int kw_count = 0;
            if (!kw_pages) continue;
            for (int j = 0; j < ra->count; j++) {
                if (strcmp(pages[j].keyword, kw) == 0 && pages[j].word_count > 50)
                    kw_pages[kw_count++] = pages[j];  /* shallow copy for NLP */
            }
            if (kw_count == 0) { free(kw_pages); continue; }

            /* Load PAA + Related from DB */
            if (ST->db) {
                nlp->paa_count = db_load_paa((Database *)ST->db, kw,
                    nlp->paa, nlp->paa_snippets, nlp->paa_links, 10);
                nlp->related_count = db_load_related((Database *)ST->db, kw,
                    nlp->related, 20);
                app_log(LOG_INFO, "Build prompt [%s]: loaded %d PAA + %d related",
                        kw, nlp->paa_count, nlp->related_count);
            }

            nlp_analyze(kw, kw_pages, kw_count, nlp, eng->onnx, eng->nli);

            /* ONNX NER entity extraction */
            if (eng->onnx && onnx_nlp_available(eng->onnx)) {
                const char **ner_texts = (const char **)malloc(kw_count * sizeof(char *));
                const char **ner_doms = (const char **)malloc(kw_count * sizeof(char *));
                int ner_valid = 0;
                if (ner_texts && ner_doms) {
                    for (int ni = 0; ni < kw_count; ni++) {
                        if (kw_pages[ni].page_text && kw_pages[ni].word_count > 50) {
                            ner_texts[ner_valid] = kw_pages[ni].page_text;
                            ner_doms[ner_valid] = kw_pages[ni].domain;
                            ner_valid++;
                        }
                    }
                    if (ner_valid > 0) {
                        __try {
                            nlp->entity_count = onnx_nlp_extract_entities_batch(
                                eng->onnx, ner_texts, ner_doms, ner_valid,
                                nlp->entities, NLP_MAX_ENTITIES);
                        } __except(EXCEPTION_EXECUTE_HANDLER) {
                            nlp->entity_count = 0;
                        }
                        app_log(LOG_INFO, "NER: %d entities from %d pages for '%s'",
                                nlp->entity_count, ner_valid, kw);
                    }
                }
                free(ner_texts); free(ner_doms);
            }

            /* Generate prompt */
            char *prompt_buf = (char *)malloc(PROMPT_MAX_LEN);
            if (prompt_buf) {
                PromptOptions opts = {1,1,1,1,1,1, PROMPT_FULL, 0, 0};
                prompt_build(nlp, &opts, prompt_buf, PROMPT_MAX_LEN);
                free(eng->generated_prompt);
                eng->generated_prompt = prompt_buf;
            }

            /* Save session to DB */
            if (ST->db) {
                char *nlp_json = (char *)malloc(PROMPT_MAX_LEN);
                if (nlp_json) {
                    prompt_export_json(nlp, nlp_json, PROMPT_MAX_LEN);
                    cJSON *root_j = cJSON_CreateObject();
                    cJSON *nlp_obj = cJSON_Parse(nlp_json);
                    if (nlp_obj && root_j) {
                        cJSON *child = nlp_obj->child;
                        while(child) { cJSON *next = child->next; cJSON_DetachItemViaPointer(nlp_obj, child); cJSON_AddItemToObject(root_j, child->string, child); child = next; }
                        cJSON_Delete(nlp_obj);
                        /* Pages array */
                        cJSON *pages_j = cJSON_CreateArray();
                        for (int p = 0; p < kw_count; p++) {
                            ScrapedPage *pg = &kw_pages[p];
                            cJSON *pj = cJSON_CreateObject();
                            cJSON_AddStringToObject(pj, "domain", pg->domain);
                            cJSON_AddStringToObject(pj, "url", pg->page_url);
                            cJSON_AddStringToObject(pj, "title", pg->page_title);
                            cJSON_AddStringToObject(pj, "meta_desc", pg->meta_description);
                            cJSON_AddNumberToObject(pj, "words", pg->word_count);
                            cJSON_AddNumberToObject(pj, "headings", pg->heading_count);
                            cJSON_AddNumberToObject(pj, "scrape_time", pg->scrape_time);
                            cJSON_AddNumberToObject(pj, "serp_position", pg->serp_position);
                            if (pg->keyword[0]) cJSON_AddStringToObject(pj, "keyword", pg->keyword);
                            cJSON_AddItemToArray(pages_j, pj);
                        }
                        cJSON_AddItemToObject(root_j, "pages", pages_j);
                        char *final_json = cJSON_Print(root_j);
                        if (final_json) {
                            db_save_session((Database *)ST->db, kw, final_json);
                            app_log(LOG_INFO, "Session saved: '%s' (%d pages, %d kw, %d bytes)",
                                kw, kw_count, nlp->keyword_count, (int)strlen(final_json));
                            cJSON_free(final_json);
                        }
                    }
                    if (root_j) cJSON_Delete(root_j);
                    free(nlp_json);
                }
            }
            app_log(LOG_INFO, "Build prompt complete: '%s' - %d keywords, %d headings",
                    kw, nlp->keyword_count, nlp->heading_count);
            result_flags = 1;
            free(kw_pages);
        } /* end keyword loop */
        }
        for (int i = 0; i < ra->count; i++) scraper_free(&pages[i]);
        free(pages);

    } else if (ra->action == ACTION_CRAWL_OBL) {
        /* ── Crawl OBL: get links from DB, scrape, save as children ── */
        int crawl_cap = 30 * ra->count;
        if (crawl_cap > 200) crawl_cap = 200;
        char **crawl_urls = (char **)calloc(crawl_cap, sizeof(char *));
        char **crawl_kws = (char **)calloc(crawl_cap, sizeof(char *));
        char **crawl_sources = (char **)calloc(crawl_cap, sizeof(char *));
        int crawl_total = 0;

        if (crawl_urls && crawl_kws && crawl_sources && ST->db) {
            for (int s = 0; s < ra->count && crawl_total < crawl_cap; s++) {
                typedef struct { char urls[100][2048]; char anchors[100][256]; } OblBufT;
                OblBufT *ob = (OblBufT *)malloc(sizeof(OblBufT));
                int oc = ob ? db_get_obl_links((Database *)ST->db, ra->urls[s], ob->urls, ob->anchors, 100) : 0;
                for (int o = 0; o < oc && crawl_total < crawl_cap; o++) {
                    int dup = 0;
                    for (int d = 0; d < crawl_total && !dup; d++)
                        if (strcmp(crawl_urls[d], ob->urls[o]) == 0) dup = 1;
                    if (dup) continue;
                    crawl_urls[crawl_total] = str_duplicate(ob->urls[o]);
                    crawl_kws[crawl_total] = str_duplicate(ra->keywords[s]);
                    crawl_sources[crawl_total] = str_duplicate(ra->urls[s]);
                    crawl_total++;
                }
                free(ob);
            }
            app_log(LOG_INFO, "OBL crawl: %d URLs from DB", crawl_total);
            if (crawl_total > 0) {
                ScrapedPage *obl_pages = (ScrapedPage *)calloc(crawl_total, sizeof(ScrapedPage));
                if (obl_pages) {
                    for (int i = 0; i < crawl_total && !eng->stop; i++) {
                        ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                        if (!job) continue;
                        job->eng = eng;
                        snprintf(job->url, sizeof(job->url), "%s", crawl_urls[i]);
                        snprintf(job->keyword, sizeof(job->keyword), "%s", crawl_kws[i]);
                        job->rank = i + 1;
                        job->result = &obl_pages[i];
                        tp_submit(&eng->scrape_pool, scrape_worker, job);
                    }
                    tp_wait(&eng->scrape_pool);
                    for (int i = 0; i < crawl_total; i++) {
                        if (obl_pages[i].word_count > 0 && ST->db) {
                            db_save_serp_url((Database *)ST->db, crawl_urls[i], crawl_kws[i],
                                obl_pages[i].domain, obl_pages[i].page_title, "", 0, 0);
                            db_update_serp_url_full((Database *)ST->db, crawl_urls[i], crawl_kws[i],
                                "scraped", obl_pages[i].word_count, obl_pages[i].heading_count,
                                obl_pages[i].outbound_count, obl_pages[i].internal_count,
                                obl_pages[i].scrape_time, obl_pages[i].used_js_fallback,
                                1, crawl_sources[i]);
                        }
                        scraper_free(&obl_pages[i]);
                    }
                    free(obl_pages);
                    app_log(LOG_INFO, "OBL crawl: scraped %d pages", crawl_total);
                }
            }
            for (int i = 0; i < crawl_total; i++) { free(crawl_urls[i]); free(crawl_kws[i]); free(crawl_sources[i]); }
        }
        free(crawl_urls); free(crawl_kws); free(crawl_sources);
        result_flags = 2;  /* signal: OBL added, rebuild results */
    }

done:
    {int action_count = ra->count;
    for (int i = 0; i < ra->count; i++) { free(ra->urls[i]); free(ra->keywords[i]); }
    free(ra->urls); free(ra->keywords); free(ra);
    eng->running = 0;
    http_thread_cleanup();
    PostMessageA(hw, WM_APP+3, result_flags, (LPARAM)action_count);}
    return 0;
}

static void on_result_action(int action) {
    if (engine_is_running(ST->engine)) {
        themedDialog("Engine Running", "Wait for the current operation to finish.", 0);
        return;
    }
    int indices[512];
    int count = get_selected_page_indices(indices, 512);
    if (count == 0) {
        themedDialog("No Pages Selected", "Select one or more SERP result rows first.", 0);
        return;
    }

    /* Build URL/keyword arrays from g_result_rows */
    ResultActionArg *ra = (ResultActionArg *)calloc(1, sizeof(ResultActionArg));
    if (!ra) return;
    ra->urls = (char **)calloc(count, sizeof(char *));
    ra->keywords = (char **)calloc(count, sizeof(char *));
    if (!ra->urls || !ra->keywords) { free(ra->urls); free(ra->keywords); free(ra); return; }
    int actual = 0;
    for (int i = 0; i < count; i++) {
        int ri = indices[i];
        if (ri >= 0 && ri < g_result_row_count) {
            ra->urls[actual] = str_duplicate(g_result_rows[ri].url);
            ra->keywords[actual] = str_duplicate(g_result_rows[ri].keyword);
            actual++;
        }
    }
    if (actual == 0) { free(ra->urls); free(ra->keywords); free(ra); return; }
    ra->count = actual;
    ra->action = action;
    ra->needs_scrape = 0;  /* Results tab: pages already scraped, just need fresh text for NLP */
    /* Build group name from the first keyword */
    snprintf(ra->group_name, sizeof(ra->group_name), "%s",
        (actual > 0 && ra->keywords[0]) ? ra->keywords[0] : "custom group");

    ST->engine->running = 1;
    ST->engine->stop = 0;
    EnableWindow(hSearch, FALSE);

    setStatus("%s %d pages...",
        action == ACTION_BUILD_PROMPT ? "Building prompt from" : "Crawling OBL for", actual);

    if (ST->engine->pipeline_handle) {
        CloseHandle(ST->engine->pipeline_handle);
        ST->engine->pipeline_handle = NULL;
    }
    ST->engine->pipeline_handle = CreateThread(NULL, 0, result_action_thread, ra, 0, NULL);
}

/* ── Command handlers ─────────────────────────────────────────── */

static void on_search(void) {
    /* Guard: reject clicks while engine is already running */
    if(engine_is_running(ST->engine)) {
        app_log(LOG_DEBUG, "Search click ignored -- engine already running");
        return;
    }

    /* Get all text from the keyword box */
    int tlen = GetWindowTextLengthA(hKeyword);
    if(tlen <= 0) {
        themedDialog("Keyword Required",
            "Enter one or more keywords to analyze.\n\n"
            "One keyword per line. If you enter multiple,\n"
            "they'll be processed as a batch.", 0);
        SetFocus(hKeyword);
        return;
    }
    char *all_text = (char *)malloc(tlen + 1);
    GetWindowTextA(hKeyword, all_text, tlen + 1);
    app_log(LOG_DEBUG, "Keyword box text (%d chars): [%.200s%s]",
            tlen, all_text, tlen > 200 ? "..." : "");

    /* Count lines */
    int line_count = 0;
    {char *p = all_text;
    while(*p) {
        while(*p && (*p=='\r'||*p=='\n'||*p==' '||*p=='\t')) p++;
        if(*p) { line_count++; while(*p && *p!='\r' && *p!='\n') p++; }
    }}

    /* Multiple keywords = concurrent batch */
    if(line_count > 1) {
        /* Parse into keyword array */
        char **kw_list = (char **)malloc(256 * sizeof(char *));
        int kw_count = 0;
        char *copy = str_duplicate(all_text);
        char *ctx = NULL;
        char *line = strtok_s(copy, "\r\n", &ctx);
        while(line && kw_count < 256) {
            str_trim(line);
            if(line[0]) kw_list[kw_count++] = str_duplicate(line);
            line = strtok_s(NULL, "\r\n", &ctx);
        }
        free(copy);
        free(all_text);

        if(kw_count > 1) {
            app_log(LOG_INFO, "Concurrent batch: %d keywords", kw_count);

            /* Read combo values here since we're about to launch */
            char buf2[32];
            GetWindowTextA(hPagesCombo, buf2, sizeof(buf2));
            int pages2 = atoi(buf2); if(pages2 < 1) pages2 = 1;
            GetWindowTextA(hDepthCombo, buf2, sizeof(buf2));
            int depth2 = atoi(buf2);
            /* Clear results */
            SendMessageA(hResultsList, LB_RESETCONTENT, 0, 0);
            SetWindowTextA(hPromptText, "");
            SendMessageA(hNlpInfo, LB_RESETCONTENT, 0, 0);
            if(hProgress) SendMessageA(hProgress, PBM_SETPOS, 0, 0);

            engine_set_callbacks(ST->engine, on_engine_progress, on_engine_page, NULL);
            ST->engine->auto_scrape = (SendMessageA(hChkAutoScrape, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ST->engine->auto_nlp = (SendMessageA(hChkAutoNlp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ST->engine->auto_obl = (SendMessageA(hChkAutoObl, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ST->engine->auto_obl_nlp = (SendMessageA(hChkAutoOblNlp, BM_GETCHECK, 0, 0) == BST_CHECKED);

            setStatus("Batch: %d keywords launching concurrently...", kw_count);
            EnableWindow(hSearch, FALSE);
            EnableWindow(hStop, TRUE);
            SetWindowTextA(hSearch, "RUNNING...");
            InvalidateRect(hSearch, NULL, TRUE);

            engine_run_batch(ST->engine, (const char **)kw_list, kw_count,
                            10, pages2, depth2);

            for(int i = 0; i < kw_count; i++) free(kw_list[i]);
            free(kw_list);
            return;
        } else {
            all_text = str_duplicate(kw_list[0]);
            for(int i = 0; i < kw_count; i++) free(kw_list[i]);
            free(kw_list);
        }
    }

    char keyword[256];
    snprintf(keyword, sizeof(keyword), "%s", all_text);
    str_trim(keyword);
    free(all_text);

    if(!keyword[0]) {
        themedDialog("Keyword Required", "Enter a keyword to analyze.", 0);
        SetFocus(hKeyword);
        return;
    }

    if(ST->config->api_key_count == 0) {
        themedDialog("No API Keys",
            "You need at least one Serper API key.\n\n"
            "1. Go to serper.dev and sign up (free: 100/day)\n"
            "2. Copy your API key\n"
            "3. Paste it in Settings under 'API Keys'", 0);
        switchTab(TAB_SETTINGS);
        SetFocus(hKeyInput);
        return;
    }

    /* Credit check happens via Serper API, not local tracking */

    /* Offer model download if not present (one-time prompt) */
    {static int models_offered = 0;
    if (!models_offered && !onnx_nlp_models_present("models")) {
        models_offered = 1;
        if (themedDialog("Download NLP Models?",
                "NLP models are not installed. Without them, entity\n"
                "extraction (people, companies, products) won't work.\n\n"
                "The analysis still works - keywords, headings, intent,\n"
                "and prompts all run without models.\n\n"
                "Download now? (~522MB) Or do it later from Settings.", 1)) {
            on_download_models();
        }
    }}

    /* Clear results */
    SendMessageA(hResultsList, LB_RESETCONTENT, 0, 0);
    SetWindowTextA(hPromptText, "");
    SendMessageA(hNlpInfo, LB_RESETCONTENT, 0, 0);
    if(hProgress) SendMessageA(hProgress, PBM_SETPOS, 0, 0);

    /* Read combo values */
    char buf[32];
    int num = 10; /* Serper returns ~10 organic results per page */
    GetWindowTextA(hPagesCombo, buf, sizeof(buf));
    int pages = atoi(buf); if(pages < 1) pages = 1;
    GetWindowTextA(hDepthCombo, buf, sizeof(buf));
    int depth = atoi(buf);

    /* Apply thread settings */
    GetWindowTextA(hThreadsCombo, buf, sizeof(buf));
    int threads = atoi(buf); if(threads < 2) threads = 16;
    ST->engine->scrape_threads = threads;

    /* Set up engine */
    engine_set_callbacks(ST->engine, on_engine_progress, on_engine_page, NULL);
    ST->engine->use_proxies = (SendMessageA(hChkProxy, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ST->engine->auto_scrape = (SendMessageA(hChkAutoScrape, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ST->engine->auto_nlp = (SendMessageA(hChkAutoNlp, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ST->engine->auto_obl = (SendMessageA(hChkAutoObl, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ST->engine->auto_obl_nlp = (SendMessageA(hChkAutoOblNlp, BM_GETCHECK, 0, 0) == BST_CHECKED);

    /* Wire link depth */
    if(depth > 0) ST->engine->link_depth = depth;
    else ST->engine->link_depth = 0;

    snprintf(ST->keyword, sizeof(ST->keyword), "%s", keyword);
    ST->num_results = num;
    ST->pages_depth = pages;
    ST->link_depth = depth;

    setStatus("Analyzing: %s", keyword);
    app_log(LOG_INFO, "Search started: keyword='%s', results=%d, pages=%d, depth=%d",
            keyword, num, pages, depth);

    EnableWindow(hSearch, FALSE);
    EnableWindow(hStop, TRUE);
    SetWindowTextA(hSearch, "RUNNING...");
    InvalidateRect(hSearch, NULL, TRUE);

    engine_run(ST->engine, keyword, num, pages, depth);
}

static void on_stop(void) {
    engine_cancel(ST->engine);
    setStatus("Stopped by user");
    app_log(LOG_WARN, "Analysis stopped by user");
    EnableWindow(hSearch, TRUE);
    EnableWindow(hStop, FALSE);
    SetWindowTextA(hSearch, "Go");
    InvalidateRect(hSearch, NULL, TRUE);
}

static void cancel_download(void) {
    if (g_download_thread) {
        g_download_cancel = 1;
        WaitForSingleObject(g_download_thread, 5000);
        CloseHandle(g_download_thread);
        g_download_thread = NULL;
    }
}

static void on_shutdown(void) {
    if(themedDialog("Shut Down",
            "This will stop all running tasks, save your data, "
            "and close the application.\n\nContinue?", 1)) {
        app_log(LOG_INFO, "Shutdown initiated by user");
        cancel_download();
        engine_nuke(ST->engine);
        DestroyWindow(hw);
    }
}

static void on_copy_prompt(void) {
    int len = GetWindowTextLengthA(hPromptText);
    if(len <= 0) {
        themedDialog("Nothing to Copy", "No prompt to copy. Run an analysis first.", 0);
        return;
    }
    char *text = (char *)malloc(len + 1);
    GetWindowTextA(hPromptText, text, len + 1);
    if(OpenClipboard(hw)) {
        EmptyClipboard();
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if(hg) { memcpy(GlobalLock(hg), text, len + 1); GlobalUnlock(hg); SetClipboardData(CF_TEXT, hg); }
        CloseClipboard();
        setStatus("Prompt copied to clipboard (%d chars)", len);
        app_log(LOG_INFO, "Prompt copied (%d chars)", len);
    }
    free(text);
}

static void on_save_txt(void) {
    int len = GetWindowTextLengthA(hPromptText);
    if(len <= 0) return;
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hw;
    ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = "output"; ofn.lpstrDefExt = "txt"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if(GetSaveFileNameA(&ofn)) {
        char *text = (char *)malloc(len + 1);
        GetWindowTextA(hPromptText, text, len + 1);
        FILE *f = fopen(path, "w");
        if(f) { fputs(text, f); fclose(f); setStatus("Saved: %s", path); app_log(LOG_INFO, "Saved to: %s", path); }
        else { app_log(LOG_ERROR, "Failed to save: %s", path); }
        free(text);
    }
}

static void on_save_json(void) {
    NLPResult *nlp = ST->engine->nlp_result;
    if(nlp->keyword_count == 0 && nlp->heading_count == 0) return;
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hw;
    ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = "output"; ofn.lpstrDefExt = "json"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if(GetSaveFileNameA(&ofn)) {
        char *json = (char *)malloc(PROMPT_MAX_LEN);
        if(json) {
            prompt_export_json(nlp, json, PROMPT_MAX_LEN);
            FILE *f = fopen(path, "w");
            if(f) { fputs(json, f); fclose(f); setStatus("JSON saved: %s", path); app_log(LOG_INFO, "Saved to: %s", path); }
            else { app_log(LOG_ERROR, "Failed to save JSON: %s", path); }
            free(json);
        }
    }
}

static void on_save_md(void) {
    NLPResult *nlp = ST->engine->nlp_result;
    if(nlp->keyword_count == 0 && nlp->heading_count == 0) return;
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hw;
    ofn.lpstrFilter = "Markdown Files\0*.md\0All Files\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = "output"; ofn.lpstrDefExt = "md"; ofn.Flags = OFN_OVERWRITEPROMPT;
    if(GetSaveFileNameA(&ofn)) {
        char *md = (char *)malloc(PROMPT_MAX_LEN);
        if(md) {
            prompt_export_markdown(nlp, md, PROMPT_MAX_LEN);
            FILE *f = fopen(path, "w");
            if(f) { fputs(md, f); fclose(f); setStatus("Markdown saved: %s", path); app_log(LOG_INFO, "Saved to: %s", path); }
            else { app_log(LOG_ERROR, "Failed to save markdown: %s", path); }
            free(md);
        }
    }
}

static void read_prompt_opts(PromptOptions *opts) {
    int sel = (int)SendMessageA(hTemplateCombo, CB_GETCURSEL, 0, 0);
    opts->template_type = sel >= 0 ? sel : PROMPT_FULL;
    opts->include_keywords = g_chk_state[0];
    opts->include_entities = g_chk_state[1];
    opts->include_outline  = g_chk_state[2];
    opts->include_stats    = g_chk_state[3];
    opts->include_paa      = g_chk_state[4];
    opts->include_related  = g_chk_state[5];
    char tmp[16];
    GetWindowTextA(hMaxKwEdit, tmp, sizeof(tmp));
    opts->max_keywords = atoi(tmp);  /* 0 = all/smart default */
    GetWindowTextA(hMaxEntEdit, tmp, sizeof(tmp));
    opts->max_entities = atoi(tmp);
}

/* Populate the keyword listbox for Keywords Only template */
static void populate_prompt_keyword_list(const NLPResult *nlp) {
    SendMessageA(hPromptList, LB_RESETCONTENT, 0, 0);
    if(!nlp) return;

    char line[256];
    snprintf(line, sizeof(line), "TARGET: %s", nlp->keyword);
    SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
    SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)"");

    /* Primary keywords */
    int pri = 0;
    for (int i = 0; i < nlp->keyword_count; i++)
        if (nlp->keywords[i].score > 0.40f) pri++;
    if (pri > 0) {
        snprintf(line, sizeof(line), "--- PRIMARY KEYWORDS (%d) ---", pri);
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        for (int i = 0; i < nlp->keyword_count; i++) {
            if (nlp->keywords[i].score <= 0.40f) continue;
            snprintf(line, sizeof(line), "  %-34s %3.0f%%  x%-3d %s",
                nlp->keywords[i].text,
                nlp->keywords[i].score * 100,
                nlp->keywords[i].frequency,
                nlp->keywords[i].source);
            SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        }
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)"");
    }

    /* Secondary keywords */
    int sec = 0;
    for (int i = 0; i < nlp->keyword_count; i++)
        if (nlp->keywords[i].score > 0.25f && nlp->keywords[i].score <= 0.40f) sec++;
    if (sec > 0) {
        snprintf(line, sizeof(line), "--- SECONDARY KEYWORDS (%d) ---", sec);
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        for (int i = 0; i < nlp->keyword_count; i++) {
            if (nlp->keywords[i].score <= 0.25f || nlp->keywords[i].score > 0.40f) continue;
            snprintf(line, sizeof(line), "  %-34s %3.0f%%  x%-3d %s",
                nlp->keywords[i].text,
                nlp->keywords[i].score * 100,
                nlp->keywords[i].frequency,
                nlp->keywords[i].source);
            SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        }
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)"");
    }

    /* Entities */
    if (nlp->entity_count > 0) {
        snprintf(line, sizeof(line), "--- ENTITIES (%d) ---", nlp->entity_count);
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        for (int i = 0; i < nlp->entity_count; i++) {
            snprintf(line, sizeof(line), "  %-34s %-8s x%d",
                nlp->entities[i].text, nlp->entities[i].label, nlp->entities[i].frequency);
            SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        }
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)"");
    }

    /* Related searches */
    if (nlp->related_count > 0) {
        snprintf(line, sizeof(line), "--- RELATED SEARCHES (%d) ---", nlp->related_count);
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        for (int i = 0; i < nlp->related_count; i++) {
            snprintf(line, sizeof(line), "  %s", nlp->related[i]);
            SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        }
    }

    /* PAA */
    if (nlp->paa_count > 0) {
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)"");
        snprintf(line, sizeof(line), "--- PEOPLE ALSO ASK (%d) ---", nlp->paa_count);
        SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        for (int i = 0; i < nlp->paa_count; i++) {
            snprintf(line, sizeof(line), "  %s", nlp->paa[i]);
            SendMessageA(hPromptList, LB_ADDSTRING, 0, (LPARAM)line);
        }
    }
}

static void on_template_change(void) {
    NLPResult *nlp = ST->engine->nlp_result;
    if(!nlp || (nlp->keyword_count == 0 && nlp->heading_count == 0)) return;

    PromptOptions opts;
    memset(&opts, 0, sizeof(opts));
    read_prompt_opts(&opts);

    int is_kw = (opts.template_type == PROMPT_KEYWORDS_ONLY);
    ShowWindow(hPromptList, is_kw ? SW_SHOW : SW_HIDE);
    ShowWindow(hPromptText, is_kw ? SW_HIDE : SW_SHOW);

    if (is_kw) {
        populate_prompt_keyword_list(nlp);
    } else {
        static char *s_prompt_buf = NULL;
        if (!s_prompt_buf) s_prompt_buf = (char *)malloc(PROMPT_MAX_LEN);
        if(s_prompt_buf) {
            prompt_build(nlp, &opts, s_prompt_buf, PROMPT_MAX_LEN);
            setPromptText(s_prompt_buf);
        }
    }
}

static void on_gap_analyze(void) {
    int len = GetWindowTextLengthA(hGapInput);
    if(len <= 0) {
        themedDialog("No Content", "Paste your article content into the box first.", 0);
        return;
    }
    char *content = (char *)malloc(len + 1);
    GetWindowTextA(hGapInput, content, len + 1);
    nlp_content_gap(ST->engine->nlp_result, content);
    free(content);

    /* Show gap report */
    char *report = (char *)malloc(PROMPT_MAX_LEN);
    if(report) {
        prompt_build_gap_report(ST->engine->nlp_result, report, PROMPT_MAX_LEN);
        setEditText(hGapResult, report);
        app_log(LOG_INFO, "Gap analysis: coverage=%.0f%%, missing %d keywords",
                ST->engine->nlp_result->gap.keyword_coverage,
                ST->engine->nlp_result->gap.missing_keyword_count);
        free(report);
    }
}

/* ── Import list: file open dialog, reads lines, calls callback per line ── */
typedef void (*import_line_fn)(const char *line);

static void import_from_file(const char *title, const char *filter_str, import_line_fn callback) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hw;
    ofn.lpstrFilter = filter_str;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) return;

    FILE *f = fopen(path, "r");
    if (!f) { themedDialog("Error", "Could not open file.", 0); return; }

    char line[2048];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline and trim */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';
        char *start = line;
        while (*start == ' ') start++;
        if (!*start || *start == '#') continue;  /* skip empty/comment lines */
        callback(start);
        count++;
    }
    fclose(f);

    char msg[128];
    snprintf(msg, sizeof(msg), "Imported %d items from %s", count, path);
    setStatus("%s", msg);
    app_log(LOG_INFO, "%s", msg);
}

/* Import callbacks */
static void import_key_line(const char *line) {
    if (strlen(line) < 10) return;
    config_add_api_key(ST->config, line);
    SendMessageA(hKeysList, LB_ADDSTRING, 0, (LPARAM)line);
}

static void import_proxy_line(const char *line) {
    /* Parse common proxy formats:
       host:port
       host:port:user:pass
       http://user:pass@host:port
       socks5://user:pass@host:port */
    char addr[256] = "", user[128] = "", pass[128] = "";

    if (strstr(line, "://")) {
        /* URL format: protocol://user:pass@host:port */
        const char *at = strchr(line, '@');
        if (at) {
            const char *creds = strstr(line, "://") + 3;
            int clen = (int)(at - creds);
            char cred_buf[256];
            snprintf(cred_buf, sizeof(cred_buf), "%.*s", clen, creds);
            char *colon = strchr(cred_buf, ':');
            if (colon) { *colon = '\0'; snprintf(user, sizeof(user), "%s", cred_buf); snprintf(pass, sizeof(pass), "%s", colon+1); }
            snprintf(addr, sizeof(addr), "%s", at + 1);
        } else {
            const char *hp = strstr(line, "://") + 3;
            snprintf(addr, sizeof(addr), "%s", hp);
        }
    } else {
        /* Check for host:port:user:pass (4 parts) */
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", line);
        char *parts[4]; int pc = 0;
        char *tok = strtok(tmp, ":");
        while (tok && pc < 4) { parts[pc++] = tok; tok = strtok(NULL, ":"); }
        if (pc == 4) {
            snprintf(addr, sizeof(addr), "%s:%s", parts[0], parts[1]);
            snprintf(user, sizeof(user), "%s", parts[2]);
            snprintf(pass, sizeof(pass), "%s", parts[3]);
        } else {
            snprintf(addr, sizeof(addr), "%s", line);
        }
    }

    if (addr[0]) {
        proxy_add(ST->proxy, addr, PROXY_HTTP);
        char display[512];
        if (user[0])
            snprintf(display, sizeof(display), "%s (%s)", addr, user);
        else
            snprintf(display, sizeof(display), "%s", addr);
        SendMessageA(hProxyList, LB_ADDSTRING, 0, (LPARAM)display);
    }
}

static void import_blocklist_line(const char *line) {
    if (ST->blocklist) {
        filter_add_domain(ST->blocklist, line);
        SendMessageA(hBlocklistList, LB_ADDSTRING, 0, (LPARAM)line);
    }
}

static void paste_from_clipboard(import_line_fn callback) {
    if (!OpenClipboard(hw)) return;
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (!hData) { CloseClipboard(); return; }
    const char *text = (const char *)GlobalLock(hData);
    if (!text) { CloseClipboard(); return; }

    /* Split by newlines */
    char line[2048];
    int count = 0;
    const char *p = text;
    while (*p) {
        int len = 0;
        while (*p && *p != '\n' && *p != '\r' && len < 2046)
            line[len++] = *p++;
        line[len] = '\0';
        while (*p == '\n' || *p == '\r') p++;
        /* Trim */
        char *start = line;
        while (*start == ' ') start++;
        int slen = (int)strlen(start);
        while (slen > 0 && start[slen-1] == ' ') start[--slen] = '\0';
        if (*start && *start != '#') { callback(start); count++; }
    }
    GlobalUnlock(hData);
    CloseClipboard();

    char msg[64];
    snprintf(msg, sizeof(msg), "Pasted %d items from clipboard", count);
    setStatus("%s", msg);
    app_log(LOG_INFO, "%s", msg);
}

static void on_key_add(void) {
    char key[256];
    GetWindowTextA(hKeyInput, key, sizeof(key));
    str_trim(key);
    if(!key[0]) return;
    if(strlen(key) < 10) {
        themedDialog("Invalid Key", "That doesn't look like a valid API key.", 0);
        app_log(LOG_ERROR, "Invalid API key (too short)");
        return;
    }
    config_add_api_key(ST->config, key);
    SendMessageA(hKeysList, LB_ADDSTRING, 0, (LPARAM)key);
    SetWindowTextA(hKeyInput, "");
    config_save(ST->config, ".env");
    setStatus("API key added. %d total keys configured.", ST->config->api_key_count);
    app_log(LOG_INFO, "API key added (total: %d)", ST->config->api_key_count);
}

static void on_key_remove(void) {
    int sel = (int)SendMessageA(hKeysList, LB_GETCURSEL, 0, 0);
    if(sel >= 0) {
        config_remove_api_key(ST->config, sel);
        SendMessageA(hKeysList, LB_DELETESTRING, sel, 0);
        config_save(ST->config, ".env");
        setStatus("API key removed. %d remaining.", ST->config->api_key_count);
        app_log(LOG_INFO, "API key removed (total: %d)", ST->config->api_key_count);
    }
}

static void on_key_check(void) {
    int sel = (int)SendMessageA(hKeysList, LB_GETCURSEL, 0, 0);
    if(sel < 0 || sel >= ST->config->api_key_count) {
        themedDialog("No Key Selected", "Right-click a key in the list to check its credits.", 0);
        return;
    }
    setStatus("Checking credits for key %d...", sel + 1);
    start_credit_check_key(ST, sel);
}

static void on_proxy_add(void) {
    char addr[128];
    GetWindowTextA(hProxyInput, addr, sizeof(addr));
    str_trim(addr);
    if(!addr[0]) return;
    proxy_add(ST->proxy, addr, PROXY_HTTP);
    SendMessageA(hProxyList, LB_ADDSTRING, 0, (LPARAM)addr);
    SetWindowTextA(hProxyInput, "");
    proxy_save(ST->proxy, "data/proxies.txt");
    setStatus("Proxy added: %s (%d total)", addr, proxy_count(ST->proxy));
    app_log(LOG_INFO, "Proxy added: %s (total: %d)", addr, proxy_count(ST->proxy));
}

static void on_proxy_remove(void) {
    int sel = (int)SendMessageA(hProxyList, LB_GETCURSEL, 0, 0);
    if(sel >= 0) {
        proxy_remove(ST->proxy, sel);
        SendMessageA(hProxyList, LB_DELETESTRING, sel, 0);
        proxy_save(ST->proxy, "data/proxies.txt");
        setStatus("Proxy removed. %d remaining.", proxy_count(ST->proxy));
        app_log(LOG_INFO, "Proxy removed (total: %d)", proxy_count(ST->proxy));
    }
}

static void on_automap(void) {
    char user[128], pass[128];
    GetWindowTextA(hProxyUser, user, sizeof(user));
    GetWindowTextA(hProxyPass, pass, sizeof(pass));
    proxy_set_credentials(ST->proxy, user, pass);
    ST->proxy->enabled = 1;
    config_auto_map_proxies(ST->config, proxy_count(ST->proxy));
    /* Persist credentials to .env */
    config_set(ST->config, "PROXY_USER", user);
    config_set(ST->config, "PROXY_PASS", pass);
    config_save(ST->config, ".env");
    setStatus("Mapped %d keys to %d proxies", ST->config->api_key_count, proxy_count(ST->proxy));
    app_log(LOG_INFO, "Auto-mapped %d keys to %d proxies", ST->config->api_key_count, proxy_count(ST->proxy));
}

static DWORD WINAPI download_models_thread(LPVOID arg) {
    (void)arg;
    /* NOTE: do NOT call themedDialog() from this thread -- it creates
       windows on a non-UI thread which causes deadlocks. Use PostMessageA. */
    app_log(LOG_INFO, "Starting NLP model download...");
    g_download_cancel = 0;
    int rc = onnx_nlp_download_models("models", NULL, NULL);
    if (g_download_cancel) {
        app_log(LOG_WARN, "NLP model download cancelled");
    } else if (rc == 0) {
        app_log(LOG_INFO, "NLP models downloaded. Restart app to activate.");
    } else {
        app_log(LOG_ERROR, "NLP model download failed");
    }
    /* Post result to main thread; WM_APP+11 handler shows the dialog */
    if (!g_download_cancel)
        PostMessageA(hw, WM_APP+11, (WPARAM)(rc == 0 ? 1 : 0), 0);
    g_download_thread = NULL;
    return 0;
}

static void on_purge_db(void) {
    if (!themedDialog("Purge Database",
        "Delete ALL data?\n\n"
        "This removes all sessions, SERP URLs, positions,\n"
        "OBL links, and history. Settings are kept.\n\n"
        "The database will be recreated fresh.", 1)) return;

    if (ST->db) {
        Database *db = (Database *)ST->db;
#ifdef _WIN32
        EnterCriticalSection(&db->write_lock);
#endif
        sqlite3_exec(db->db, "BEGIN;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM sessions;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_urls;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM positions;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM obl_links;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_history;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_headings;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_page_keywords;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_paa;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "DELETE FROM serp_related;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "COMMIT;", NULL, NULL, NULL);
        sqlite3_exec(db->db, "VACUUM;", NULL, NULL, NULL);
#ifdef _WIN32
        LeaveCriticalSection(&db->write_lock);
#endif
        app_log(LOG_INFO, "Database purged: all data tables cleared");
    }

    /* Clear UI */
    SendMessageA(hHistoryList, LB_RESETCONTENT, 0, 0);
    g_history_count = 0;
    refresh_pending_list();
    SendMessageA(hResultsList, LB_RESETCONTENT, 0, 0);
    g_result_map_count = 0;
    setStatus("Database purged -- all data cleared");
}

static void on_download_models(void) {
    if (onnx_nlp_models_present("models")) {
        themedDialog("Models Present",
                    "NLP models are already installed.\n\n"
                    "models/ner.onnx - Entity Recognition\n"
                    "models/embed.onnx - Sentence Embeddings\n"
                    "models/vocab.txt - NER Tokenizer\n"
                    "models/nli.onnx - Content Classification\n"
                    "models/nli_vocab.json - NLI Tokenizer\n"
                    "models/nli_merges.txt - NLI Byte-Pair Merges", 0);
        return;
    }

    if (themedDialog("Download NLP Models",
            "Downloads ~1.5GB total:\n\n"
            "- NER model (bert-base-NER, ~400MB) - entity recognition\n"
            "- Embedding model (MiniLM, ~80MB) - semantic similarity\n"
            "- NLI model (distilbart-mnli, ~978MB) - content classification\n"
            "- Tokenizer files (~2MB) - NER + NLI vocabularies\n\n"
            "Saves to the 'models' directory. Download now?", 1)) {
        EnableWindow(hDownloadModels, FALSE);
        g_download_thread = CreateThread(NULL, 0, download_models_thread, NULL, 0, NULL);
    }
}

static void on_blocklist_add(void) {
    char domain[256];
    GetWindowTextA(hBlocklistEdit, domain, sizeof(domain));
    str_trim(domain);
    if(!domain[0]) return;
    if(ST->blocklist) {
        /* Add to blocklist via filter API */
        filter_add_domain(ST->blocklist, domain);
        SendMessageA(hBlocklistList, LB_ADDSTRING, 0, (LPARAM)domain);
        SetWindowTextA(hBlocklistEdit, "");
        filter_save_blocklist(ST->blocklist, "data/blocklist.txt");
        app_log(LOG_INFO, "Blocklist: added '%s'", domain);
    }
}

static void on_blocklist_del(void) {
    int sel = (int)SendMessageA(hBlocklistList, LB_GETCURSEL, 0, 0);
    if(sel >= 0) {
        char domain[256];
        SendMessageA(hBlocklistList, LB_GETTEXT, sel, (LPARAM)domain);
        if(ST->blocklist) filter_remove_domain(ST->blocklist, domain);
        SendMessageA(hBlocklistList, LB_DELETESTRING, sel, 0);
        filter_save_blocklist(ST->blocklist, "data/blocklist.txt");
        app_log(LOG_INFO, "Blocklist: removed '%s'", domain);
    }
}

/* ── Settings persistence ─────────────────────────────────────── */

static void save_settings(void) {
    if(!ST->db) return;
    char buf[64];
    GetWindowTextA(hThreadsCombo, buf, sizeof(buf));  db_save_setting(ST->db, "scrape_threads", buf);
    GetWindowTextA(hGetTimeout, buf, sizeof(buf));    db_save_setting(ST->db, "get_timeout", buf);
    GetWindowTextA(hPostTimeout, buf, sizeof(buf));   db_save_setting(ST->db, "post_timeout", buf);
    GetWindowTextA(hMaxRedirects, buf, sizeof(buf));  db_save_setting(ST->db, "max_redirects", buf);
    GetWindowTextA(hRetryCount, buf, sizeof(buf));    db_save_setting(ST->db, "retry_count", buf);
    GetWindowTextA(hRetryBase, buf, sizeof(buf));     db_save_setting(ST->db, "retry_base", buf);

    /* Apply HTTP settings to globals */
    {extern int g_http_get_timeout, g_http_post_timeout, g_http_max_redirects;
    extern int g_http_retry_count, g_http_retry_base_ms;
    GetWindowTextA(hGetTimeout, buf, sizeof(buf));    g_http_get_timeout = atoi(buf);
    GetWindowTextA(hPostTimeout, buf, sizeof(buf));   g_http_post_timeout = atoi(buf);
    GetWindowTextA(hMaxRedirects, buf, sizeof(buf));  g_http_max_redirects = atoi(buf);
    GetWindowTextA(hRetryCount, buf, sizeof(buf));    g_http_retry_count = atoi(buf);
    GetWindowTextA(hRetryBase, buf, sizeof(buf));     g_http_retry_base_ms = atoi(buf) * 1000;
    if (g_http_retry_count < 1) g_http_retry_count = 1;
    if (g_http_retry_count > 10) g_http_retry_count = 10;}

    app_log(LOG_INFO, "Settings saved to database");
}

static void load_settings(void) {
    if(!ST->db) return;
    char val[256];
    if(db_load_setting(ST->db, "scrape_threads", val, sizeof(val)) == 0) {
        int idx = (int)SendMessageA(hThreadsCombo, CB_FINDSTRINGEXACT, -1, (LPARAM)val);
        if(idx >= 0) SendMessageA(hThreadsCombo, CB_SETCURSEL, idx, 0);
    }
    if(db_load_setting(ST->db, "get_timeout", val, sizeof(val)) == 0) SetWindowTextA(hGetTimeout, val);
    if(db_load_setting(ST->db, "post_timeout", val, sizeof(val)) == 0) SetWindowTextA(hPostTimeout, val);
    if(db_load_setting(ST->db, "max_redirects", val, sizeof(val)) == 0) SetWindowTextA(hMaxRedirects, val);
    if(db_load_setting(ST->db, "retry_count", val, sizeof(val)) == 0) SetWindowTextA(hRetryCount, val);
    if(db_load_setting(ST->db, "retry_base", val, sizeof(val)) == 0) SetWindowTextA(hRetryBase, val);

    /* Apply HTTP settings to globals */
    {extern int g_http_get_timeout, g_http_post_timeout, g_http_max_redirects;
    extern int g_http_retry_count, g_http_retry_base_ms;
    char buf[64];
    GetWindowTextA(hGetTimeout, buf, sizeof(buf));    if(buf[0]) g_http_get_timeout = atoi(buf);
    GetWindowTextA(hPostTimeout, buf, sizeof(buf));   if(buf[0]) g_http_post_timeout = atoi(buf);
    GetWindowTextA(hMaxRedirects, buf, sizeof(buf));  if(buf[0]) g_http_max_redirects = atoi(buf);
    GetWindowTextA(hRetryCount, buf, sizeof(buf));    if(buf[0]) g_http_retry_count = atoi(buf);
    GetWindowTextA(hRetryBase, buf, sizeof(buf));     if(buf[0]) g_http_retry_base_ms = atoi(buf) * 1000;
    if (g_http_retry_count < 1) g_http_retry_count = 1;
    if (g_http_retry_count > 10) g_http_retry_count = 10;}

    /* Apply thread count to engine so non-Analyze paths use the saved value */
    {char tbuf[16];
    GetWindowTextA(hThreadsCombo, tbuf, sizeof(tbuf));
    int t = atoi(tbuf); if (t >= 2) ST->engine->scrape_threads = t;}

    app_log(LOG_INFO, "Settings loaded from database");
}

static void on_history_load(void) {
    if (engine_is_running(ST->engine)) return;
    int sel = (int)SendMessageA(hHistoryList, LB_GETCURSEL, 0, 0);
    if(sel < 0) return;
    const char *kw_ptr = history_get_keyword(sel);
    if(!kw_ptr || !kw_ptr[0]) return;
    char keyword[256];
    snprintf(keyword, sizeof(keyword), "%s", kw_ptr);

    char *json = NULL;
    if(db_load_session(ST->db, keyword, &json) == 0 && json) {
        /* Deserialize JSON back into NLPResult */
        NLPResult *nlp = ST->engine->nlp_result;
        if(nlp_from_json(json, nlp) == 0) {
            SetWindowTextA(hKeyword, keyword);

            /* Regenerate prompt using current checkbox/count options */
            {char *prompt = (char *)malloc(PROMPT_MAX_LEN);
            if(prompt) {
                PromptOptions opts;
                memset(&opts, 0, sizeof(opts));
                read_prompt_opts(&opts);
                prompt_build(nlp, &opts, prompt, PROMPT_MAX_LEN);
                setPromptText(prompt);
                free(ST->engine->generated_prompt);
                ST->engine->generated_prompt = prompt;
            }}

            SetWindowTextA(hDetailText, "");

            /* Update NLP info panel -- listbox with all data (#18/#19) */
            populate_nlp_info(nlp);

            setStatus("Restored: %s (%d kw, %d headings, %d entities)",
                     keyword, nlp->keyword_count, nlp->heading_count, nlp->entity_count);
        } else {
            /* JSON parse failed - show raw */
            setPromptText(json);
            setStatus("Loaded raw session: %s (parse failed, showing JSON)", keyword);
            switchTab(TAB_ANALYSIS);
        }
        free(json);
    } else {
        app_log(LOG_ERROR, "Failed to load session: %s", keyword);
        themedDialog("Load Failed", "Could not load this session from the database.", 0);
    }
}

static void on_history_delete(void) {
    int sel = (int)SendMessageA(hHistoryList, LB_GETCURSEL, 0, 0);
    if(sel < 0) return;
    const char *kw_ptr = history_get_keyword(sel);
    if(!kw_ptr || !kw_ptr[0]) return;
    char keyword[256];
    snprintf(keyword, sizeof(keyword), "%s", kw_ptr);

    db_delete_session(ST->db, keyword);
    setStatus("Deleted session: %s", keyword);
    app_log(LOG_INFO, "Deleted session: %s", keyword);

    /* Rebuild from DB to keep listbox and g_history_keywords[] in sync */
    populate_history_list();
    int remaining = g_history_count;
    if(remaining > 0) {
        if(sel >= remaining) sel = remaining - 1;
        SendMessageA(hHistoryList, LB_SETCURSEL, sel, 0);
        on_history_load();
    }
}

static void on_history_delete_all(void) {
    int count = (int)SendMessageA(hHistoryList, LB_GETCOUNT, 0, 0);
    if (count <= 0) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Delete all %d saved sessions? This cannot be undone.", count);
    if (!themedDialog("Delete All Sessions", msg, 1)) return;

    db_delete_all_sessions(ST->db);
    SendMessageA(hHistoryList, LB_RESETCONTENT, 0, 0);
    setStatus("Deleted all %d sessions", count);
    app_log(LOG_INFO, "Deleted all %d sessions", count);
}

/* ── Timer handler ────────────────────────────────────────────── */

static void on_timer(void) {
    if(!ST || !ST->engine) return;

    /* Check if engine just finished */
    if(!engine_is_running(ST->engine) && !IsWindowEnabled(hSearch)) {
        app_log(LOG_INFO, "CHECKPOINT: timer detected engine finished (page_count=%d)", ST->engine->page_count);
        EnableWindow(hSearch, TRUE);
        EnableWindow(hStop, FALSE);
        SetWindowTextA(hSearch, "Go");
        InvalidateRect(hSearch, NULL, TRUE);

        app_log(LOG_INFO, "CHECKPOINT: rebuilding results list from DB");
        refresh_results_list(NULL);

        NLPResult *nlp = ST->engine->nlp_result;
        if(!nlp) return;
        app_log(LOG_INFO, "CHECKPOINT: nlp result check (kw=%d, hdg=%d, nlp_ptr=%p)",
                nlp->keyword_count, nlp->heading_count, (void*)nlp);
        if(nlp->keyword_count > 0 || nlp->heading_count > 0) {
            app_log(LOG_INFO, "CHECKPOINT: populating analysis tab");
            /* Populate analysis tab */
            if(ST->engine->generated_prompt)
                setPromptText(ST->engine->generated_prompt);

            /* NLP info listbox -- show ALL keywords and entities (#18/#19) */
            populate_nlp_info(nlp);

            int successful = 0;
            for(int i = 0; i < ST->engine->page_count; i++)
                if(ST->engine->pages[i].word_count > 50) successful++;

            setStatus("Done: %d pages scraped, %d keywords, %d headings -- prompt ready!",
                     successful, nlp->keyword_count, nlp->heading_count);
            app_log(LOG_INFO, "Analysis complete: %d pages, %d keywords, %d headings, %.1fs",
                    successful, nlp->keyword_count, nlp->heading_count, nlp->analysis_time);

            if(hProgress) SendMessageA(hProgress, PBM_SETPOS, 100, 0);

            /* Defer heavy auto-save (JSON/DB/file I/O) to next message loop
               iteration so the timer doesn't block the UI paint cycle (#6) */
            if(!g_save_pending) {
                g_save_pending = 1;
                PostMessage(hw, WM_APP+13, 0, 0);
            }

            /* Lightweight post-analysis tasks stay on the timer */
            save_settings();
            start_credit_check(ST);
            InvalidateRect(hw, NULL, FALSE);
        }

        /* Handle concurrent batch completion -- defer heavy save work (#6) */
        if(ST->engine->batch && ST->engine->batch->phase >= 4) {
            if(!g_batch_save_pending) {
                g_batch_save_pending = 1;
                PostMessage(hw, WM_APP+14, 0, 0);
            }
        }
    }

    /* Update resmon footer labels */
    if(ST->resmon) {
        ResourceSnapshot snap;
        resmon_get(ST->resmon, &snap);

        /* CPU label */
        {char buf[64];
        snprintf(buf, sizeof(buf), "CPU: %.0f%%", snap.system_cpu_pct);
        SetWindowTextA(hResmonCpu, buf);}

        /* RAM label */
        {char buf[64];
        snprintf(buf, sizeof(buf), "RAM: %.0f%%", snap.system_ram_pct);
        SetWindowTextA(hResmonRam, buf);}

        /* GPU label (all detected GPUs) */
        if(snap.gpu_count > 0) {
            char buf[128]; int pos = 0;
            for(int gi = 0; gi < snap.gpu_count && pos < 120; gi++) {
                if(gi > 0) pos += snprintf(buf+pos, sizeof(buf)-pos, " ");
                pos += snprintf(buf+pos, sizeof(buf)-pos, "GPU%d: %.0f%%", gi+1, snap.gpu_util_pct[gi]);
            }
            SetWindowTextA(hResmonGpu, buf);
        }

        /* Repaint system meters on Settings tab */
        if(curTab == TAB_SETTINGS && settingsSubTab == 1 && hSysInfoLabel) {
            RECT sr; GetWindowRect(hSysInfoLabel,&sr);
            POINT sp={sr.left,sr.top}; ScreenToClient(hw,&sp);
            RECT inv = {sp.x, sp.y - D(20), sr.right - sr.left + sp.x, sr.bottom - sr.top + sp.y};
            InvalidateRect(hw, &inv, TRUE);
        }
    }

}

/* ── Log timer ────────────────────────────────────────────────── */

static void on_log_timer(void) {
    LogEntry entries[100];
    int count = log_get_entries(entries, 100, LOG_DEBUG);
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].seq <= g_last_log_seq) continue;
        g_last_log_seq = entries[i].seq;
        /* Apply level filter */
        if (entries[i].level == LOG_DEBUG && !g_log_show_dbg) continue;
        if (entries[i].level == LOG_INFO  && !g_log_show_info) continue;
        if (entries[i].level == LOG_WARN  && !g_log_show_warn) continue;
        if (entries[i].level == LOG_ERROR && !g_log_show_err) continue;
        char line[600];
        struct tm *t = localtime(&entries[i].timestamp);
        const char *lvl = entries[i].level == LOG_DEBUG ? "DBG " :
                          entries[i].level == LOG_WARN  ? "WARN" :
                          entries[i].level == LOG_ERROR ? "ERR " : "INFO";
        if(t)
            snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s  %s",
                     t->tm_hour, t->tm_min, t->tm_sec, lvl, entries[i].msg);
        else
            snprintf(line, sizeof(line), "[??:??:??] %s  %s", lvl, entries[i].msg);
        SendMessageA(hLogList, LB_ADDSTRING, 0, (LPARAM)line);
        added++;
    }
    if (added) {
        int total = (int)SendMessageA(hLogList, LB_GETCOUNT, 0, 0);
        if(total > 0) SendMessageA(hLogList, LB_SETTOPINDEX, total - 1, 0);
    }

    /* Refresh pending queue only while engine is running (avoid wiping user's selection) */
    if (ST && ST->engine && engine_is_running(ST->engine))
        refresh_pending_list();
}

/* ── Create all controls ──────────────────────────────────────── */

static void mkcontrols(HWND h) {
    RECT rc; GetClientRect(h,&rc);
    int W=rc.right, H=rc.bottom, cw=W-M*2;
    int botY=H-SH-M-BH-D(4);
    int y;

    /* Tab buttons */
    const char *tn[]={"SEARCH","RESULTS","ANALYSIS","SETTINGS","LOG"};
    int tw=W/TAB_COUNT;
    for(int i=0;i<TAB_COUNT;i++)
        tabs[i]=CreateWindowA("BUTTON",tn[i],WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
            i*tw,TABY,tw,TABH,h,(HMENU)(UINT_PTR)(ID_TAB0+i),NULL,NULL);

    /* ── Search tab (all labels as STATIC controls) ──────── */
    y=CY+D(6);

    /* Keyword input (no label, placeholder hint instead) */
    /* hLblKeyword removed -- placeholder hint replaces it */
    hKeyword=CreateWindowExA(0,"EDIT","",
        WS_CHILD|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,
        M,y,D(280),D(65),h,(HMENU)ID_KEYWORD,NULL,NULL);
    SendMessage(hKeyword,WM_SETFONT,(WPARAM)fNorm,1);
    /* Hint painted by paintSearch when empty (EM_SETCUEBANNER doesn't work on multiline) */

    /* Analyze button */
    hSearch=CreateWindowA("BUTTON","Go",WS_CHILD|BS_OWNERDRAW,
        W-M-D(100),y,D(100),BH,h,(HMENU)ID_SEARCH,NULL,NULL);

    /* Stop button */
    hStop=CreateWindowA("BUTTON","Stop",WS_CHILD|BS_OWNERDRAW,
        W-M-D(100),y+BH+HPAD,D(100),BH,h,(HMENU)ID_STOP,NULL,NULL);
    EnableWindow(hStop, FALSE);

    y+=D(55)+PAD;

    /* Combo labels */
    /* Results combo removed -- Serper always returns ~10 per page */

    hLblPages=CreateWindowA("STATIC","Pages:",WS_CHILD|SS_RIGHT,M,y+D(2),D(50),EH,h,NULL,NULL,NULL);
    SendMessage(hLblPages,WM_SETFONT,(WPARAM)fNorm,1);
    hPagesCombo=CreateWindowA("COMBOBOX",NULL,WS_CHILD|CBS_DROPDOWNLIST,
        M+D(173),y,D(45),200,h,(HMENU)ID_PAGES_COMBO,NULL,NULL);
    SendMessage(hPagesCombo,WM_SETFONT,(WPARAM)fNorm,1);
    {const char *pv[]={"1","2","3","4","5"};
    for(int i=0;i<5;i++) SendMessageA(hPagesCombo,CB_ADDSTRING,0,(LPARAM)pv[i]);}
    SendMessageA(hPagesCombo,CB_SETCURSEL,0,0);

    hLblDepth=CreateWindowA("STATIC","OBL Depth:",WS_CHILD|SS_RIGHT,M+D(210),y+D(2),D(70),EH,h,NULL,NULL,NULL);
    SendMessage(hLblDepth,WM_SETFONT,(WPARAM)fNorm,1);
    hDepthCombo=CreateWindowA("COMBOBOX",NULL,WS_CHILD|CBS_DROPDOWNLIST,
        M+D(280),y,D(60),200,h,(HMENU)ID_DEPTH_COMBO,NULL,NULL);
    SendMessage(hDepthCombo,WM_SETFONT,(WPARAM)fNorm,1);
    {const char *dv[]={"0","1","2","3"};
    for(int i=0;i<4;i++) SendMessageA(hDepthCombo,CB_ADDSTRING,0,(LPARAM)dv[i]);}
    SendMessageA(hDepthCombo,CB_SETCURSEL,0,0);

    hChkProxy=CreateWindowA("BUTTON","Use proxies",WS_CHILD|BS_AUTOCHECKBOX,
        M+D(355),y,D(130),D(20),h,(HMENU)ID_CHK_PROXY,NULL,NULL);
    SendMessage(hChkProxy,WM_SETFONT,(WPARAM)fNorm,1);
    SetWindowTheme(hChkProxy,L"",L"");

    y+=EH+PAD;

    /* Instruction labels */
    {const char *instr[] = {
        "Each Page = ~10 Google results (1 API credit). OBL Depth = follow outbound links from scraped pages.",
        "Free Serper API key from serper.dev. Add it in Settings.",
    };
    for(int ii=0; ii<INSTR_LINES; ii++) {
        hLblInstr[ii]=CreateWindowA("STATIC", instr[ii],
            WS_CHILD|SS_LEFT, M, y, cw, D(16), h, NULL, NULL, NULL);
        SendMessage(hLblInstr[ii],WM_SETFONT,(WPARAM)fNorm,1);
        y += D(15);
    }}
    y += D(6);

    /* Pipeline checkboxes */
    hChkAutoScrape=CreateWindowA("BUTTON","Auto Scrape",WS_CHILD|BS_AUTOCHECKBOX,
        M,y,D(100),D(20),h,(HMENU)ID_CHK_AUTO_SCRAPE,NULL,NULL);
    SendMessage(hChkAutoScrape,WM_SETFONT,(WPARAM)fNorm,1);
    SetWindowTheme(hChkAutoScrape,L"",L"");
    SendMessageA(hChkAutoScrape,BM_SETCHECK,BST_UNCHECKED,0);

    hChkAutoNlp=CreateWindowA("BUTTON","Auto NLP",WS_CHILD|BS_AUTOCHECKBOX,
        M+D(108),y,D(85),D(20),h,(HMENU)ID_CHK_AUTO_NLP,NULL,NULL);
    SendMessage(hChkAutoNlp,WM_SETFONT,(WPARAM)fNorm,1);
    SetWindowTheme(hChkAutoNlp,L"",L"");
    SendMessageA(hChkAutoNlp,BM_SETCHECK,BST_UNCHECKED,0);

    hChkAutoObl=CreateWindowA("BUTTON","Auto OBL",WS_CHILD|BS_AUTOCHECKBOX,
        M+D(201),y,D(85),D(20),h,(HMENU)ID_CHK_AUTO_OBL,NULL,NULL);
    SendMessage(hChkAutoObl,WM_SETFONT,(WPARAM)fNorm,1);
    SetWindowTheme(hChkAutoObl,L"",L"");

    hChkAutoOblNlp=CreateWindowA("BUTTON","Auto OBL NLP",WS_CHILD|BS_AUTOCHECKBOX,
        M+D(294),y,D(110),D(20),h,(HMENU)ID_CHK_AUTO_OBL_NLP,NULL,NULL);
    SendMessage(hChkAutoOblNlp,WM_SETFONT,(WPARAM)fNorm,1);
    SetWindowTheme(hChkAutoOblNlp,L"",L"");
    EnableWindow(hChkAutoOblNlp, FALSE);  /* disabled until Auto OBL is checked */

    y += D(22) + HPAD;

    /* Progress */
    hLblProgress=CreateWindowA("STATIC","Progress",WS_CHILD|SS_LEFT,M,y,D(80),D(16),h,NULL,NULL,NULL);
    SendMessage(hLblProgress,WM_SETFONT,(WPARAM)fBold,1);
    y+=D(20);
    hProgress=CreateWindowA(PROGRESS_CLASSA,NULL,WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
        M,y,cw,D(6),h,NULL,NULL,NULL);
    SendMessage(hProgress,PBM_SETRANGE,0,MAKELPARAM(0,100));
    SendMessage(hProgress,PBM_SETBARCOLOR,0,CYAN);
    SendMessage(hProgress,PBM_SETBKCOLOR,0,BG3);

    y+=D(6)+PAD;

    y+=D(6)+PAD;

    /* History listbox */
    hLblHistory=CreateWindowA("STATIC","Session History (right-click to manage)",
        WS_CHILD|SS_LEFT,M,y,D(300),D(16),h,NULL,NULL,NULL);
    SendMessage(hLblHistory,WM_SETFONT,(WPARAM)fNorm,1);
    y += D(18);
    hHistoryList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT
        |LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,
        M,y,cw,D(80),h,(HMENU)ID_HISTORY_LIST,NULL,NULL);
    SendMessage(hHistoryList,WM_SETFONT,(WPARAM)fMono,1);
    SendMessage(hHistoryList,LB_SETITEMHEIGHT,0,D(16));
    y += D(84);

    /* Pending scrape queue (multi-select, right-click for process/delete) */
    hLblPending=CreateWindowA("STATIC","Pending scrape queue:",WS_CHILD|SS_LEFT,
        M,y,D(200),D(16),h,NULL,NULL,NULL);
    SendMessage(hLblPending,WM_SETFONT,(WPARAM)fNorm,1);
    y += D(18);
    hPendingList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT,
        M,y,cw,D(80),h,NULL,NULL,NULL);
    SendMessage(hPendingList,WM_SETFONT,(WPARAM)fMono,1);

    /* ── Results tab (stacked: list on top, detail below) ────── */
    {int list_top=CY+D(56);
    int split=list_top+(botY-list_top)*55/100;
    hResultsList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_EXTENDEDSEL|LBS_NOTIFY
        |LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,
        M,list_top,cw,split-list_top,h,(HMENU)ID_RESULTS_LIST,NULL,NULL);
    SendMessage(hResultsList,WM_SETFONT,(WPARAM)fMono,1);
    SendMessage(hResultsList,LB_SETITEMHEIGHT,0,D(18));

    hDetailText=CreateWindowExA(0,"EDIT","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        M,split+HPAD,cw,botY-split-HPAD,h,(HMENU)ID_DETAIL_TEXT,NULL,NULL);
    SendMessage(hDetailText,WM_SETFONT,(WPARAM)fMono,1);}

    /* ── Analysis tab ────────────────────── */
    {int ctrl_y=CY+D(36);
    hTemplateCombo=CreateWindowA("COMBOBOX",NULL,WS_CHILD|CBS_DROPDOWNLIST,
        M+D(65),ctrl_y,D(175),200,h,(HMENU)ID_TEMPLATE_COMBO,NULL,NULL);
    SendMessage(hTemplateCombo,WM_SETFONT,(WPARAM)fNorm,1);
    {const char *tv[]={"Full System Prompt","Keywords Only","Outline Only","Competitive Brief"};
    for(int i=0;i<4;i++) SendMessageA(hTemplateCombo,CB_ADDSTRING,0,(LPARAM)tv[i]);}
    SendMessageA(hTemplateCombo,CB_SETCURSEL,0,0);

    hCopyBtn=CreateWindowA("BUTTON","Copy",WS_CHILD|BS_OWNERDRAW,
        M+D(260),ctrl_y-D(1),D(120),BH,h,(HMENU)ID_COPY_BTN,NULL,NULL);
    /* Save buttons removed — all 3 formats auto-save to output/ on every run */
    ctrl_y+=BH+D(4);

    /* Prompt content checkboxes (#20) -- owner-drawn for dark theme (#24) */
    {int cx = M;
    hChkKw=CreateWindowA("BUTTON","Keywords",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(80),D(18),h,(HMENU)ID_CHK_KEYWORDS,NULL,NULL); cx+=D(84);
    hChkEnt=CreateWindowA("BUTTON","Entities",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(72),D(18),h,(HMENU)ID_CHK_ENTITIES,NULL,NULL); cx+=D(76);
    hChkOutline=CreateWindowA("BUTTON","Outline",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(68),D(18),h,(HMENU)ID_CHK_OUTLINE,NULL,NULL); cx+=D(72);
    hChkStats=CreateWindowA("BUTTON","Stats",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(56),D(18),h,(HMENU)ID_CHK_STATS,NULL,NULL); cx+=D(60);
    hChkPaa=CreateWindowA("BUTTON","PAA",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(48),D(18),h,(HMENU)ID_CHK_PAA,NULL,NULL); cx+=D(52);
    hChkRelated=CreateWindowA("BUTTON","Related",WS_CHILD|BS_OWNERDRAW,
        cx,ctrl_y,D(68),D(18),h,(HMENU)ID_CHK_RELATED,NULL,NULL); cx+=D(80);

    /* Max keyword/entity inputs */
    hLblMaxKw=CreateWindowA("STATIC","Kw:",WS_CHILD,
        cx,ctrl_y+D(1),D(22),D(16),h,NULL,NULL,NULL); cx+=D(24);
    hMaxKwEdit=CreateWindowA("EDIT","0",WS_CHILD|WS_BORDER|ES_NUMBER|ES_CENTER,
        cx,ctrl_y,D(32),D(18),h,(HMENU)ID_MAX_KW_EDIT,NULL,NULL); cx+=D(38);
    hLblMaxEnt=CreateWindowA("STATIC","Ent:",WS_CHILD,
        cx,ctrl_y+D(1),D(26),D(16),h,NULL,NULL,NULL); cx+=D(28);
    hMaxEntEdit=CreateWindowA("EDIT","0",WS_CHILD|WS_BORDER|ES_NUMBER|ES_CENTER,
        cx,ctrl_y,D(32),D(18),h,(HMENU)ID_MAX_ENT_EDIT,NULL,NULL);

    /* Set fonts (check state tracked in g_chk_state[], not via BM_SETCHECK) */
    HWND chks[] = {hChkKw,hChkEnt,hChkOutline,hChkStats,hChkPaa,hChkRelated};
    for(int i=0;i<6;i++) SendMessage(chks[i],WM_SETFONT,(WPARAM)fNorm,1);
    SendMessage(hMaxKwEdit,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessage(hMaxEntEdit,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessage(hLblMaxKw,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessage(hLblMaxEnt,WM_SETFONT,(WPARAM)fNorm,1);
    }
    ctrl_y+=D(22);

    int prompt_w=cw*72/100;
    int info_w=cw-prompt_w-D(10);
    int gap_h = D(140);
    int prompt_bot = botY - gap_h - D(28);
    if(prompt_bot < ctrl_y + D(100)) prompt_bot = ctrl_y + D(100);

    hPromptText=CreateWindowExA(0,"EDIT","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        M,ctrl_y,prompt_w,prompt_bot-ctrl_y,h,(HMENU)ID_PROMPT_TEXT,NULL,NULL);
    SendMessage(hPromptText,WM_SETFONT,(WPARAM)fMono,1);
    SendMessageA(hPromptText,EM_SETLIMITTEXT,PROMPT_MAX_LEN,0);

    /* Keywords-only listbox -- overlaps prompt text, shown when template=Keywords Only */
    hPromptList=CreateWindowExA(0,"LISTBOX","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS,
        M,ctrl_y,prompt_w,prompt_bot-ctrl_y,h,(HMENU)ID_PROMPT_LIST,NULL,NULL);
    SendMessage(hPromptList,WM_SETFONT,(WPARAM)fMono,1);

    hNlpInfo=CreateWindowExA(0,"LISTBOX","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_EXTENDEDSEL|LBS_NOINTEGRALHEIGHT|LBS_HASSTRINGS,
        M+prompt_w+D(10),ctrl_y,info_w,botY-ctrl_y,h,(HMENU)ID_NLP_INFO,NULL,NULL);
    SendMessage(hNlpInfo,WM_SETFONT,(WPARAM)fMono,1);

    /* Content gap analyzer controls */
    int gap_y = prompt_bot + D(28);
    int gap_input_h = D(50);
    hGapInput=CreateWindowExA(0,"EDIT","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL,
        M,gap_y,prompt_w-D(90),gap_input_h,h,(HMENU)ID_GAP_INPUT,NULL,NULL);
    SendMessage(hGapInput,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hGapInput,EM_SETCUEBANNER,0,(LPARAM)L"Paste your existing article here to find content gaps...");

    hGapAnalyze=CreateWindowA("BUTTON","Analyze Gap",WS_CHILD|BS_OWNERDRAW,
        M+prompt_w-D(82),gap_y,D(82),BH,h,(HMENU)ID_GAP_ANALYZE,NULL,NULL);

    hGapResult=CreateWindowExA(0,"EDIT","",
        WS_CHILD|WS_VSCROLL|WS_BORDER|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        M,gap_y+gap_input_h+D(4),prompt_w,botY-gap_y-gap_input_h-D(4),h,(HMENU)ID_GAP_RESULT,NULL,NULL);
    SendMessage(hGapResult,WM_SETFONT,(WPARAM)fMono,1);}

    /* ── Settings tab ────────────────────── */
    /* Sub-tab buttons */
    hSettingsSubConn=CreateWindowA("BUTTON","Connection",WS_CHILD|BS_OWNERDRAW,
        M,CY+D(6),D(130),D(26),h,(HMENU)ID_SETTINGS_SUB_CONN,NULL,NULL);
    hSettingsSubData=CreateWindowA("BUTTON","Data && Tools",WS_CHILD|BS_OWNERDRAW,
        M+D(138),CY+D(6),D(130),D(26),h,(HMENU)ID_SETTINGS_SUB_DATA,NULL,NULL);

    {int sx=M, sy=CY+D(8)+D(40);
    int half_w=W/2-M-D(20);

    /* API key input */
    hKeyInput=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD,
        sx+D(35),sy,D(240),EH,h,(HMENU)ID_KEY_INPUT,NULL,NULL);
    SendMessage(hKeyInput,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hKeyInput,EM_SETCUEBANNER,0,(LPARAM)L"Paste Serper API key...");

    hKeyAdd=CreateWindowA("BUTTON","Add",WS_CHILD|BS_OWNERDRAW,
        sx+D(283),sy-D(1),D(45),BH,h,(HMENU)ID_KEY_ADD,NULL,NULL);
    hKeyRemove=CreateWindowA("BUTTON","Remove",WS_CHILD|BS_OWNERDRAW,
        sx+D(336),sy-D(1),D(60),BH,h,(HMENU)ID_KEY_REMOVE,NULL,NULL);
    CreateWindowA("BUTTON","Import",WS_CHILD|BS_OWNERDRAW,
        sx,sy+D(80),D(80),BH,h,(HMENU)ID_KEY_IMPORT,NULL,NULL);
    CreateWindowA("BUTTON","Paste",WS_CHILD|BS_OWNERDRAW,
        sx+D(86),sy+D(80),D(80),BH,h,(HMENU)ID_KEY_PASTE,NULL,NULL);
    sy+=BH+D(4);

    hKeysList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        sx,sy,half_w,D(80),h,(HMENU)ID_KEYS_LIST,NULL,NULL);
    SendMessage(hKeysList,WM_SETFONT,(WPARAM)fMono,1);
    sy+=D(80)+D(4)+D(24)+D(10)+D(20)+D(18);

    /* Proxy input */
    hProxyInput=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL,
        sx,sy,D(240),EH,h,(HMENU)ID_PROXY_INPUT,NULL,NULL);
    SendMessage(hProxyInput,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hProxyInput,EM_SETCUEBANNER,0,(LPARAM)L"IP:PORT...");

    hProxyAdd=CreateWindowA("BUTTON","Add",WS_CHILD|BS_OWNERDRAW,
        sx+D(248),sy-D(1),D(45),BH,h,(HMENU)ID_PROXY_ADD,NULL,NULL);
    hProxyRemove=CreateWindowA("BUTTON","Remove",WS_CHILD|BS_OWNERDRAW,
        sx+D(301),sy-D(1),D(60),BH,h,(HMENU)ID_PROXY_REMOVE,NULL,NULL);
    CreateWindowA("BUTTON","Import",WS_CHILD|BS_OWNERDRAW,
        sx,sy+D(60),D(80),BH,h,(HMENU)ID_PROXY_IMPORT,NULL,NULL);
    CreateWindowA("BUTTON","Paste",WS_CHILD|BS_OWNERDRAW,
        sx+D(86),sy+D(60),D(80),BH,h,(HMENU)ID_PROXY_PASTE,NULL,NULL);
    sy+=BH+D(4);

    hProxyList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        sx,sy,half_w,D(80),h,(HMENU)ID_PROXY_LIST,NULL,NULL);
    SendMessage(hProxyList,WM_SETFONT,(WPARAM)fMono,1);
    sy+=D(84);

    /* Proxy credentials */
    hProxyUser=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL,
        sx+D(45),sy,D(120),EH,h,(HMENU)ID_PROXY_USER,NULL,NULL);
    SendMessage(hProxyUser,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hProxyUser,EM_SETCUEBANNER,0,(LPARAM)L"username");

    hProxyPass=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_PASSWORD,
        sx+D(210),sy,D(105),EH,h,(HMENU)ID_PROXY_PASS,NULL,NULL);
    SendMessage(hProxyPass,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hProxyPass,EM_SETCUEBANNER,0,(LPARAM)L"password");
    sy+=EH+D(6);

    hAutoMap=CreateWindowA("BUTTON","Auto-Map Keys to Proxies",WS_CHILD|BS_OWNERDRAW,
        sx,sy,D(200),BH,h,(HMENU)ID_AUTOMAP_BTN,NULL,NULL);

    sy+=BH+SECGAP;
    hDownloadModels=CreateWindowA("BUTTON","Download NLP Models",WS_CHILD|BS_OWNERDRAW,
        sx,sy,D(200),BH,h,(HMENU)ID_DOWNLOAD_MODELS,NULL,NULL);
    sy+=BH+D(12);
    CreateWindowA("BUTTON","Purge Database",WS_CHILD|BS_OWNERDRAW,
        sx,sy,D(200),BH,h,(HMENU)ID_PURGE_DB,NULL,NULL);

    /* Right column: Performance combos */
    int rx=W/2+D(20);
    int ry=CY+D(8)+D(24);
    hThreadsCombo=CreateWindowA("COMBOBOX",NULL,WS_CHILD|CBS_DROPDOWNLIST,
        rx+D(120),ry,D(70),200,h,(HMENU)ID_THREADS_COMBO,NULL,NULL);
    SendMessage(hThreadsCombo,WM_SETFONT,(WPARAM)fNorm,1);
    {const char *tv[]={"2","4","8","16","32","64","128","256"};
    int tc[]={2,4,8,16,32,64,128,256};
    for(int i=0;i<8;i++) SendMessageA(hThreadsCombo,CB_ADDSTRING,0,(LPARAM)tv[i]);
    /* Auto-detect: cpu_threads * 2, pick closest combo value */
    int auto_threads = ST->resmon ? ST->resmon->cpu_threads * 2 : 16;
    int best_idx = 3; /* default 16 */
    int best_dist = 9999;
    for(int i=0;i<8;i++) { int d=abs(tc[i]-auto_threads); if(d<=best_dist){best_dist=d;best_idx=i;} }
    SendMessageA(hThreadsCombo,CB_SETCURSEL,best_idx,0);}

    ry+=D(26);
    /* HTTP settings */
    ry+=D(40);
    hGetTimeout=CreateWindowExA(0,"EDIT","30",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
        rx+D(100),ry,D(45),EH,h,(HMENU)ID_GET_TIMEOUT,NULL,NULL);
    SendMessage(hGetTimeout,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hGetTimeout,EM_SETCUEBANNER,0,(LPARAM)L"30");

    hPostTimeout=CreateWindowExA(0,"EDIT","30",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
        rx+D(270),ry,D(45),EH,h,(HMENU)ID_POST_TIMEOUT,NULL,NULL);
    SendMessage(hPostTimeout,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hPostTimeout,EM_SETCUEBANNER,0,(LPARAM)L"30");

    ry+=D(26);
    hMaxRedirects=CreateWindowExA(0,"EDIT","5",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
        rx+D(100),ry,D(40),EH,h,(HMENU)ID_MAX_REDIRECTS,NULL,NULL);
    SendMessage(hMaxRedirects,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hMaxRedirects,EM_SETCUEBANNER,0,(LPARAM)L"5");

    hRetryCount=CreateWindowExA(0,"EDIT","3",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
        rx+D(192),ry,D(30),EH,h,(HMENU)ID_RETRY_COUNT,NULL,NULL);
    SendMessage(hRetryCount,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hRetryCount,EM_SETCUEBANNER,0,(LPARAM)L"3");

    hRetryBase=CreateWindowExA(0,"EDIT","2",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL|ES_NUMBER,
        rx+D(310),ry,D(35),EH,h,(HMENU)ID_RETRY_BASE,NULL,NULL);
    SendMessage(hRetryBase,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hRetryBase,EM_SETCUEBANNER,0,(LPARAM)L"2");

    ry+=D(36);

    /* Domain blocklist */
    ry+=D(46);
    int bl_x = rx;
    int bl_w = W - rx - M;
    hBlocklistEdit=CreateWindowExA(0,"EDIT","",WS_CHILD|WS_BORDER|ES_AUTOHSCROLL,
        bl_x,ry,bl_w-D(130),EH,h,(HMENU)ID_BLOCKLIST_EDIT,NULL,NULL);
    SendMessage(hBlocklistEdit,WM_SETFONT,(WPARAM)fNorm,1);
    SendMessageW(hBlocklistEdit,EM_SETCUEBANNER,0,(LPARAM)L"example.com");

    hBlocklistAdd=CreateWindowA("BUTTON","Add",WS_CHILD|BS_OWNERDRAW,
        bl_x+bl_w-D(122),ry-D(1),D(45),BH,h,(HMENU)ID_BLOCKLIST_ADD,NULL,NULL);
    hBlocklistDel=CreateWindowA("BUTTON","Delete",WS_CHILD|BS_OWNERDRAW,
        bl_x+bl_w-D(69),ry-D(1),D(60),BH,h,(HMENU)ID_BLOCKLIST_DEL,NULL,NULL);
    CreateWindowA("BUTTON","Import",WS_CHILD|BS_OWNERDRAW,
        bl_x,ry+D(60),D(80),BH,h,(HMENU)ID_BLOCKLIST_IMPORT,NULL,NULL);
    CreateWindowA("BUTTON","Paste",WS_CHILD|BS_OWNERDRAW,
        bl_x+D(86),ry+D(60),D(80),BH,h,(HMENU)ID_BLOCKLIST_PASTE,NULL,NULL);
    ry+=BH+D(4);

    hBlocklistList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        bl_x,ry,bl_w,D(70),h,(HMENU)ID_BLOCKLIST_LIST,NULL,NULL);
    SendMessage(hBlocklistList,WM_SETFONT,(WPARAM)fMono,1);

    /* System info label (updated by timer with resmon data) */
    ry+=D(70)+D(18*4)+D(30);
    hSysInfoLabel=CreateWindowA("STATIC","",WS_CHILD|SS_LEFT,
        rx,ry,W/2-M-D(20),D(40),h,(HMENU)ID_SYSINFO_LABEL,NULL,NULL);
    SendMessage(hSysInfoLabel,WM_SETFONT,(WPARAM)fNorm,1);}

    /* ── Log tab ─────────────────────────── */
    {int log_btn_y = CY+D(32); int log_top = CY+D(60);
    {int fx = M; int fbw = D(64); int fgap = D(8);
    hLogFilterDbg=CreateWindowA("BUTTON","DEBUG",WS_CHILD|BS_OWNERDRAW,
        fx,log_btn_y,fbw,BH,h,(HMENU)ID_LOG_FILTER_DBG,NULL,NULL);
    fx += fbw + fgap;
    hLogFilterInfo=CreateWindowA("BUTTON","INFO",WS_CHILD|BS_OWNERDRAW,
        fx,log_btn_y,fbw,BH,h,(HMENU)ID_LOG_FILTER_INFO,NULL,NULL);
    fx += fbw + fgap;
    hLogFilterWarn=CreateWindowA("BUTTON","WARN",WS_CHILD|BS_OWNERDRAW,
        fx,log_btn_y,fbw,BH,h,(HMENU)ID_LOG_FILTER_WARN,NULL,NULL);
    fx += fbw + fgap;
    hLogFilterErr=CreateWindowA("BUTTON","ERROR",WS_CHILD|BS_OWNERDRAW,
        fx,log_btn_y,fbw,BH,h,(HMENU)ID_LOG_FILTER_ERR,NULL,NULL);
    SendMessage(hLogFilterDbg,WM_SETFONT,(WPARAM)fBold,1);
    SendMessage(hLogFilterInfo,WM_SETFONT,(WPARAM)fBold,1);
    SendMessage(hLogFilterWarn,WM_SETFONT,(WPARAM)fBold,1);
    SendMessage(hLogFilterErr,WM_SETFONT,(WPARAM)fBold,1);}

    hLogClear=CreateWindowA("BUTTON","Clear",WS_CHILD|BS_OWNERDRAW,
        W-M-D(60),log_btn_y,D(60),BH,h,(HMENU)ID_LOG_CLEAR,NULL,NULL);

    hLogList=CreateWindowExA(0,"LISTBOX",NULL,
        WS_CHILD|WS_VSCROLL|WS_BORDER|LBS_NOINTEGRALHEIGHT
        |LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,
        M,log_top,cw,botY-log_top,h,(HMENU)ID_LOG_LIST,NULL,NULL);
    SendMessage(hLogList,WM_SETFONT,(WPARAM)fMono,1);
    SendMessage(hLogList,LB_SETITEMHEIGHT,0,D(16));}

    /* ── Footer: resmon + status + shutdown ────── */
    {int fy = H - SH;
    hResmonCpu=CreateWindowA("STATIC","CPU: --",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        M,fy+D(2),D(120),SH-D(4),h,NULL,NULL,NULL);
    SendMessage(hResmonCpu,WM_SETFONT,(WPARAM)fBold,1);
    hResmonRam=CreateWindowA("STATIC","RAM: --",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        M+D(126),fy+D(2),D(120),SH-D(4),h,NULL,NULL,NULL);
    SendMessage(hResmonRam,WM_SETFONT,(WPARAM)fBold,1);
    hResmonGpu=CreateWindowA("STATIC","",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        M+D(252),fy+D(2),D(150),SH-D(4),h,NULL,NULL,NULL);
    SendMessage(hResmonGpu,WM_SETFONT,(WPARAM)fBold,1);

    hNuke=CreateWindowA("BUTTON","Shut Down",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,
        W-M-D(80),fy+D(2),D(80),SH-D(4),h,(HMENU)ID_NUKE,NULL,NULL);

    hStatus=CreateWindowA("STATIC","",
        WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE,
        M+D(408),fy,W-M*2-D(496),SH,h,(HMENU)ID_STATUS,NULL,NULL);
    SendMessage(hStatus,WM_SETFONT,(WPARAM)fBold,1);}

    /* ── Pre-populate API keys from config ── */
    if(ST->config->api_key_count > 0) {
        for(int i = 0; i < ST->config->api_key_count; i++)
            SendMessageA(hKeysList, LB_ADDSTRING, 0, (LPARAM)ST->config->api_keys[i].key);
    }

    /* ── Pre-populate proxies from proxy pool ── */
    if(ST->proxy && ST->proxy->pool_count > 0) {
        for(int i = 0; i < ST->proxy->pool_count; i++)
            SendMessageA(hProxyList, LB_ADDSTRING, 0, (LPARAM)ST->proxy->pool[i].addr);
        /* Pre-populate credentials */
        if(ST->proxy->username[0]) SetWindowTextA(hProxyUser, ST->proxy->username);
        if(ST->proxy->password[0]) SetWindowTextA(hProxyPass, ST->proxy->password);
    }

    /* ── Pre-populate blocklist from blocklist struct ── */
    if(ST->blocklist) {
        for(int i = 0; i < ST->blocklist->count; i++) {
            if(ST->blocklist->domains[i][0])
                SendMessageA(hBlocklistList, LB_ADDSTRING, 0, (LPARAM)ST->blocklist->domains[i]);
        }
    }

    /* ── Load persisted settings from config ── */
    {const char *v;
    if((v=config_get(ST->config,"SPIDER_THREADS"))) {
        int val=atoi(v);
        int tc[]={2,4,8,16,32,64,128,256};
        for(int i=0;i<8;i++) if(tc[i]==val){SendMessageA(hThreadsCombo,CB_SETCURSEL,i,0);break;}
    }
    }

    /* ── Load persisted settings from database ── */
    load_settings();

    /* ── Populate session history (don't auto-load into keyword box) ── */
    if(ST->db) {
        populate_history_list();
    }

    /* Populate results list from DB (persistent across restarts) */
    refresh_results_list(NULL);

    /* Populate pending scrape queue from DB (crash recovery) */
    refresh_pending_list();

    /* Apply dark scrollbar theme to all scrollable controls */
    {HWND *dark_ctrls[] = {
        &hKeyword, &hResultsList, &hDetailText, &hPromptText, &hPromptList,
        &hNlpInfo, &hGapInput, &hGapResult, &hHistoryList, &hPendingList,
        &hKeysList, &hProxyList, &hBlocklistList, &hLogList, &hSysInfoLabel,
        NULL
    };
    for (int i = 0; dark_ctrls[i]; i++)
        if (*dark_ctrls[i]) SetWindowTheme(*dark_ctrls[i], L"DarkMode_Explorer", NULL);}

    g_controls_ready=1;

    switchTab(TAB_SEARCH);
    relayout();
    SetFocus(hKeyword);

    {RECT dbg; GetClientRect(h,&dbg);
    app_log(LOG_INFO, "Window client: %dx%d, DPI=%d, SH=%d, botY=%d, status_y=%d",
            dbg.right, dbg.bottom, g_dpi, SH, dbg.bottom-SH-M-BH-D(4), dbg.bottom-SH);}
}

/* ── WndProc ──────────────────────────────────────────────────── */

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    __try {
    switch(msg) {

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi=(MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x=D(900);
        mmi->ptMinTrackSize.y=D(600);
        return 0;
    }

    case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT:
        if(!g_controls_ready) break;
        SetTextColor((HDC)wp,TXT); SetBkColor((HDC)wp,RGB(38,38,52));
        return (LRESULT)brBg3;
    case WM_CTLCOLORBTN: {
        /* Checkbox labels on dark theme (#24) */
        if(!g_controls_ready) break;
        HWND ctrl = (HWND)lp;
        if(ctrl==hChkKw||ctrl==hChkEnt||ctrl==hChkOutline||ctrl==hChkStats||ctrl==hChkPaa||ctrl==hChkRelated
           ||ctrl==hChkAutoScrape||ctrl==hChkAutoNlp||ctrl==hChkAutoObl||ctrl==hChkAutoOblNlp) {
            SetTextColor((HDC)wp,TXT);
            SetBkMode((HDC)wp,TRANSPARENT);
            return (LRESULT)brBg;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        if(!g_controls_ready) break;
        HWND ctrl = (HWND)lp;
        SetBkMode((HDC)wp,TRANSPARENT);
        /* Status bar */
        if(ctrl==hStatus) {
            SetTextColor((HDC)wp,DIM);
            SetBkColor((HDC)wp,BG2);
            return (LRESULT)brBg2;
        }
        /* Cyan bold labels (section headers) */
        if(ctrl==hLblProgress||ctrl==hLblHistory) {
            SetTextColor((HDC)wp,CYAN);
            return (LRESULT)brBg;
        }
        /* Resmon labels -- color-coded by usage */
        if(ctrl==hResmonCpu || ctrl==hResmonRam || ctrl==hResmonGpu) {
            float pct = 0;
            if(ST && ST->resmon) {
                ResourceSnapshot snap; resmon_get(ST->resmon, &snap);
                if(ctrl==hResmonCpu) pct = snap.system_cpu_pct;
                else if(ctrl==hResmonRam) pct = snap.system_ram_pct;
                else if(ctrl==hResmonGpu && snap.gpu_count > 0) pct = snap.gpu_util_pct[0];
            }
            COLORREF c = pct > 90 ? RGB(255,80,80) :
                         pct > 70 ? RGB(255,200,50) : RGB(80,220,100);
            SetTextColor((HDC)wp, c);
            SetBkColor((HDC)wp, BG2);
            return (LRESULT)brBg2;
        }
        /* Prompt option checkboxes and labels (#24) */
        if(ctrl==hChkKw||ctrl==hChkEnt||ctrl==hChkOutline||ctrl==hChkStats||ctrl==hChkPaa||ctrl==hChkRelated
           ||ctrl==hLblMaxKw||ctrl==hLblMaxEnt) {
            SetTextColor((HDC)wp,TXT);
            SetBkMode((HDC)wp,TRANSPARENT);
            return (LRESULT)brBg;
        }
        /* Dim instruction text */
        {int is_instr=0; for(int ii=0;ii<INSTR_LINES;ii++) if(ctrl==hLblInstr[ii]) is_instr=1;
        if(is_instr||ctrl==hLblPages||ctrl==hLblDepth) {
            SetTextColor((HDC)wp,DIM);
            return (LRESULT)brBg;
        }}
        SetTextColor((HDC)wp,TXT);
        return (LRESULT)brBg;
    }

    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h,&r);
        /* Main background */
        FillRect((HDC)wp,&r,brBg);
        /* Footer bar background */
        {RECT fb = {0, r.bottom - D(32), r.right, r.bottom};
        FillRect((HDC)wp, &fb, brBg2);}
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
        RECT rc; GetClientRect(h,&rc);
        int W=rc.right, H=rc.bottom;
        SetBkMode(dc,TRANSPARENT);

        /* Title bar */
        SelectObject(dc,fTitle); SetTextColor(dc,CYAN);
        TextOutA(dc,D(12),D(6),"SERP TO PROMPT WRITER",21);

        /* Separator lines */
        {HPEN pen=CreatePen(PS_SOLID,1,BORDER); HPEN oldP=(HPEN)SelectObject(dc,pen);
        MoveToEx(dc,0,TABY-D(2),NULL); LineTo(dc,rc.right,TABY-D(2));
        MoveToEx(dc,0,TABY+TABH,NULL); LineTo(dc,rc.right,TABY+TABH);
        SelectObject(dc,oldP); DeleteObject(pen);}

        /* (subtitle removed - guidance text is in each tab's paint function) */

        /* Tab-specific labels */
        if(g_controls_ready) {
            if(curTab==TAB_SEARCH)   paintSearch(dc,&rc);
            if(curTab==TAB_RESULTS)  paintResults(dc,&rc);
            if(curTab==TAB_ANALYSIS) paintAnalysis(dc,&rc);
            if(curTab==TAB_SETTINGS) paintSettings(dc,&rc);
            if(curTab==TAB_LOG)      paintLog(dc,&rc);
        }

        /* Footer bar background -- controls (hResmonCpu/Ram/Gpu, hStatus, hNuke) handle text */
        {int footH = D(40);
        RECT sb={0, H-footH, W, H};
        FillRect(dc, &sb, brBg2);
        HPEN fp=CreatePen(PS_SOLID,1,BORDER); HPEN oldFp=(HPEN)SelectObject(dc,fp);
        MoveToEx(dc, 0, H-footH, NULL); LineTo(dc, W, H-footH);
        SelectObject(dc,oldFp); DeleteObject(fp);}

        EndPaint(h,&ps); return 0;
    }

    case WM_SIZE:
        if(wp != SIZE_MINIMIZED) relayout();
        return 0;

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d=(DRAWITEMSTRUCT*)lp; UINT id=(UINT)wp;
        if(id>=ID_TAB0&&id<=ID_TAB4){drawTab(d,id-ID_TAB0);return TRUE;}
        /* NUKE button -- special red treatment */
        if(id==ID_NUKE) {
            COLORREF nbg = (d->itemState & ODS_SELECTED) ? RGB(120,20,20) : RGB(180,30,30);
            HBRUSH b=CreateSolidBrush(nbg); FillRect(d->hDC,&d->rcItem,b); DeleteObject(b);
            HPEN p=CreatePen(PS_SOLID,1,RGB(220,50,50)); SelectObject(d->hDC,p);
            SelectObject(d->hDC,GetStockObject(NULL_BRUSH));
            RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,D(5),D(5));
            DeleteObject(p);
            SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,RGB(255,255,255)); SelectObject(d->hDC,fBold);
            char t[128]; GetWindowTextA(d->hwndItem,t,128);
            DrawTextA(d->hDC,t,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            return TRUE;
        }
        /* Prompt option checkboxes -- owner-drawn for dark theme (#24) */
        if(id>=ID_CHK_KEYWORDS && id<=ID_CHK_RELATED) {
            int idx = id - ID_CHK_KEYWORDS;
            int checked = g_chk_state[idx];
            /* Fill background */
            FillRect(d->hDC, &d->rcItem, brBg);
            /* Draw checkbox square */
            int bsz = d->rcItem.bottom - d->rcItem.top - D(4);
            RECT box = {d->rcItem.left, d->rcItem.top + D(2), d->rcItem.left + bsz, d->rcItem.top + D(2) + bsz};
            HPEN pen = CreatePen(PS_SOLID, 1, checked ? CYAN : DIM);
            SelectObject(d->hDC, pen);
            {HBRUSH chkbr = checked ? CreateSolidBrush(RGB(0,40,50)) : NULL;
            SelectObject(d->hDC, chkbr ? chkbr : GetStockObject(NULL_BRUSH));
            Rectangle(d->hDC, box.left, box.top, box.right, box.bottom);
            if(chkbr) DeleteObject(chkbr);}
            if(checked) {
                /* Draw checkmark */
                HPEN cp = CreatePen(PS_SOLID, D(2), CYAN);
                SelectObject(d->hDC, cp);
                MoveToEx(d->hDC, box.left+D(2), box.top+bsz/2, NULL);
                LineTo(d->hDC, box.left+bsz/3, box.bottom-D(3));
                LineTo(d->hDC, box.right-D(2), box.top+D(2));
                DeleteObject(cp);
            }
            DeleteObject(pen);
            /* Draw label text */
            RECT tr = {box.right + D(4), d->rcItem.top, d->rcItem.right, d->rcItem.bottom};
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, checked ? TXT : DIM);
            SelectObject(d->hDC, fNorm);
            char t[64]; GetWindowTextA(d->hwndItem, t, 64);
            DrawTextA(d->hDC, t, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            return TRUE;
        }
        /* History list owner-draw: header row gets colored background */
        if(id==ID_HISTORY_LIST) {
            char text[512] = "";
            SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);
            int is_header = (d->itemID == 0);
            COLORREF bg_c = is_header ? RGB(20,50,60) : BG2;
            COLORREF fg = is_header ? CYAN : TXT;
            if (!is_header && (d->itemState & ODS_SELECTED))
                { bg_c = RGB(0,80,120); fg = RGB(255,255,255); }
            HBRUSH bb = CreateSolidBrush(bg_c);
            FillRect(d->hDC, &d->rcItem, bb); DeleteObject(bb);
            SetTextColor(d->hDC, fg);
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, fMono);
            RECT tr = d->rcItem; tr.left += D(2);
            DrawTextA(d->hDC, text, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            return TRUE;
        }
        /* Log list owner-draw: color-coded by level */
        if(id==ID_LOG_LIST) {
            char text[512] = "";
            SendMessageA(d->hwndItem, LB_GETTEXT, d->itemID, (LPARAM)text);
            /* Detect level from text: [HH:MM:SS] LEVEL  message */
            COLORREF fg = TXT;
            COLORREF bg_c = BG2;
            if (strstr(text, "ERROR")) { fg = RED; bg_c = RGB(40,15,15); }
            else if (strstr(text, "WARN")) { fg = YELLOW; bg_c = RGB(35,30,15); }
            else if (strstr(text, "INFO") && (strstr(text, "Session saved") || strstr(text, "complete") ||
                     strstr(text, "NLP:") || strstr(text, "Scrape complete") || strstr(text, "Build prompt")))
                { fg = GREEN; }
            else if (strstr(text, "DEBUG")) { fg = DIM; }

            HBRUSH bb = CreateSolidBrush((d->itemState & ODS_SELECTED) ? RGB(0,80,120) : bg_c);
            FillRect(d->hDC, &d->rcItem, bb); DeleteObject(bb);
            SetTextColor(d->hDC, (d->itemState & ODS_SELECTED) ? RGB(255,255,255) : fg);
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, fMono);
            RECT tr = d->rcItem; tr.left += D(4);
            DrawTextA(d->hDC, text, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            return TRUE;
        }
        /* Results list owner-draw: OBL rows get darker background */
        if(id==ID_RESULTS_LIST) {
            int row = d->itemID;
            int ri = (row >= 0 && row < g_result_map_count) ? g_result_page_map[row] : -1;
            int is_obl = (ri >= 0 && ri < g_result_row_count && g_result_rows[ri].is_crawled);
            int is_sel = (d->itemState & ODS_SELECTED);
            /* Background */
            COLORREF bg_c = is_sel ? RGB(0,80,120) : (is_obl ? RGB(24,24,35) : BG2);
            HBRUSH bg_b = CreateSolidBrush(bg_c);
            FillRect(d->hDC, &d->rcItem, bg_b);
            DeleteObject(bg_b);
            /* Text */
            COLORREF fg_c = is_sel ? RGB(255,255,255) : (is_obl ? DIM : TXT);
            SetTextColor(d->hDC, fg_c);
            SetBkMode(d->hDC, TRANSPARENT);
            SelectObject(d->hDC, fMono);
            char text[512] = "";
            SendMessageA(d->hwndItem, LB_GETTEXT, row, (LPARAM)text);
            RECT tr = d->rcItem;
            tr.left += D(4);
            DrawTextA(d->hDC, text, -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            /* Focus rect */
            if(d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
            return TRUE;
        }
        if(id==ID_SEARCH) { drawBtn(d,engine_is_running(ST->engine)?RED:CYAN,BG2); return TRUE; }
        if(id==ID_STOP) { drawBtn(d,YELLOW,BG2); return TRUE; }
        /* Copy button */
        if(id==ID_COPY_BTN) { drawBtn(d,CYAN,BG2); return TRUE; }
        /* Save buttons removed — auto-save handles all formats */
        /* Gap analyze button */
        if(id==ID_GAP_ANALYZE) { drawBtn(d,CYAN,BG2); return TRUE; }
        /* Settings buttons */
        if(id==ID_KEY_ADD||id==ID_PROXY_ADD||id==ID_BLOCKLIST_ADD) { drawBtn(d,CYAN,BG2); return TRUE; }
        if(id==ID_KEY_REMOVE||id==ID_PROXY_REMOVE||id==ID_BLOCKLIST_DEL) { drawBtn(d,RED,BG2); return TRUE; }
        /* ID_KEY_CHECK removed -- using right-click menu */
        if(id==ID_AUTOMAP_BTN) { drawBtn(d,YELLOW,BG2); return TRUE; }
        if(id==ID_DOWNLOAD_MODELS) { drawBtn(d,GREEN,BG2); return TRUE; }
        if(id==ID_PURGE_DB) { drawBtn(d,RED,BG2); return TRUE; }
        /* Settings sub-tab buttons */
        if(id==ID_SETTINGS_SUB_CONN || id==ID_SETTINGS_SUB_DATA) {
            int active = (id==ID_SETTINGS_SUB_CONN) ? (settingsSubTab==0) : (settingsSubTab==1);
            COLORREF bg = active ? BG2 : BG3;
            COLORREF fg = active ? CYAN : TXT;
            HBRUSH b=CreateSolidBrush(bg); FillRect(d->hDC,&d->rcItem,b); DeleteObject(b);
            /* Border around both tabs */
            {HPEN bp=CreatePen(PS_SOLID,1,active?CYAN:BORDER);
            SelectObject(d->hDC,bp); SelectObject(d->hDC,GetStockObject(NULL_BRUSH));
            RoundRect(d->hDC,d->rcItem.left,d->rcItem.top,d->rcItem.right,d->rcItem.bottom,D(4),D(4));
            DeleteObject(bp);}
            if(active) {
                /* Cyan underline */
                HPEN p=CreatePen(PS_SOLID,D(2),CYAN); SelectObject(d->hDC,p);
                MoveToEx(d->hDC,d->rcItem.left,d->rcItem.bottom-1,NULL);
                LineTo(d->hDC,d->rcItem.right,d->rcItem.bottom-1);
                DeleteObject(p);
            }
            SetBkMode(d->hDC,TRANSPARENT); SetTextColor(d->hDC,fg);
            SelectObject(d->hDC, active ? fBold : fNorm);
            char t[64]; GetWindowTextA(d->hwndItem,t,64);
            DrawTextA(d->hDC,t,-1,&d->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            return TRUE;
        }
        /* ID_SCRAPE_PENDING removed */
        /* History buttons */
        /* History buttons removed -- using right-click context menu */
        /* Log clear */
        if(id==ID_LOG_FILTER_DBG)  { drawBtn(d, g_log_show_dbg  ? BG : DIM, g_log_show_dbg  ? DIM  : BG2); return TRUE; }
        if(id==ID_LOG_FILTER_INFO) { drawBtn(d, g_log_show_info ? BG : DIM, g_log_show_info ? CYAN : BG2); return TRUE; }
        if(id==ID_LOG_FILTER_WARN) { drawBtn(d, g_log_show_warn ? BG : DIM, g_log_show_warn ? YELLOW : BG2); return TRUE; }
        if(id==ID_LOG_FILTER_ERR)  { drawBtn(d, g_log_show_err  ? BG : DIM, g_log_show_err  ? RED  : BG2); return TRUE; }
        if(id==ID_LOG_CLEAR) { drawBtn(d,YELLOW,BG2); return TRUE; }
        /* Default */
        drawBtn(d,TXT,BG2);
        return TRUE;
    }

    /* Engine progress update from worker thread */
    case WM_APP+1: {
        int pos = (int)wp;
        if(hProgress) SendMessage(hProgress,PBM_SETPOS,pos,0);
        return 0;
    }

    /* Engine page result from worker thread */
    case WM_APP+2: {
        char *text = (char *)lp;
        if(text) {
            SendMessageA(hResultsList, LB_ADDSTRING, 0, (LPARAM)text);
            free(text);
        }
        return 0;
    }

    /* Results action complete (OBL/NLP) -- rebuild results from DB */
    case WM_APP+3: {
        refresh_results_list(NULL);  /* reload all results from DB */
        EnableWindow(hSearch, TRUE);
        {int flags = (int)wp; int acted = (int)lp;
        if (flags & 1) {
            /* Session saved — refresh history and load analysis data */
            setStatus("Prompt built from %d pages -- saved to History", acted);
            populate_history_list();
            refresh_pending_list();
            if (g_history_count > 0) {
                SendMessageA(hHistoryList, LB_SETCURSEL, 0, 0);
                on_history_load();
                /* Restore full results view -- on_history_load filters to one keyword,
                   but user expects to see ALL scraped URLs on the Results tab */
                refresh_results_list(NULL);
            }
        } else if (flags & 2) {
            setStatus("OBL crawl complete -- %d pages added to Results", acted);
        } else {
            setStatus("Scrape complete -- %d pages on Results tab", acted);
        }}
        return 0;
    }

    /* Periodic timers */
    case WM_TIMER:
        if(wp==ID_PROGRESS_TIMER) on_timer();
        if(wp==ID_LOG_TIMER) on_log_timer();
        return 0;

    case WM_CONTEXTMENU: {
        HWND target = (HWND)wp;
        if (target == hKeysList) {
            int sel = (int)SendMessageA(hKeysList, LB_GETCURSEL, 0, 0);
            if (sel < 0) break;
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            AppendMenuA(menu, MF_STRING, ID_CTX_KEY_CHECK, "Check Credits");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, ID_CTX_KEY_DELETE, "Remove Key");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        /* History list right-click */
        if (target == hHistoryList) {
            int sel = (int)SendMessageA(hHistoryList, LB_GETCURSEL, 0, 0);
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            if (sel >= 0) AppendMenuA(menu, MF_STRING, ID_CTX_HIST_DELETE, "Delete");
            AppendMenuA(menu, MF_STRING, ID_CTX_HIST_DELETE_ALL, "Delete All");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        /* Results list right-click */
        if (target == hResultsList) {
            int sel_count = (int)SendMessageA(hResultsList, LB_GETSELCOUNT, 0, 0);
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            if (sel_count > 0) {
                AppendMenuA(menu, MF_STRING, ID_CTX_RESULT_NLP, "Build Prompt from Selected");
                AppendMenuA(menu, MF_STRING, ID_CTX_RESULT_OBL, "Crawl OBL for Selected");
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(menu, MF_STRING, ID_CTX_RESULT_DELETE, "Delete Selected");
                AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            }
            AppendMenuA(menu, MF_STRING, ID_CTX_RESULT_SELECT_ALL, "Select All");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        /* Prompt keyword list right-click */
        if (target == hPromptList) {
            int sel_count = (int)SendMessageA(hPromptList, LB_GETSELCOUNT, 0, 0);
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PROMPT_COPY_RAW, "Copy Selected (text only)");
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PROMPT_COPY_FULL, "Copy Selected (with scores)");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, ID_CTX_PROMPT_SELECT_ALL, "Select All");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        /* NLP info listbox right-click */
        if (target == hNlpInfo) {
            int sel_count = (int)SendMessageA(hNlpInfo, LB_GETSELCOUNT, 0, 0);
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_NLP_COPY_RAW, "Copy Selected (text only)");
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_NLP_COPY_FULL, "Copy Selected (with scores)");
            AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(menu, MF_STRING, ID_CTX_NLP_SELECT_ALL, "Select All");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        /* Pending list right-click */
        if (target == hPendingList) {
            int sel_count = (int)SendMessageA(hPendingList, LB_GETSELCOUNT, 0, 0);
            int total = (int)SendMessageA(hPendingList, LB_GETCOUNT, 0, 0);
            POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
            HMENU menu = CreatePopupMenu();
            if (sel_count == 1) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_VIEW_DETAIL, "View Serper Details");
            if (sel_count == 1) AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_SCRAPE_SEL, "Scrape Selected");
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_PROCESS_SEL, "Build Prompt from Selected");
            if (total > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_SCRAPE_ALL, "Scrape All");
            if (total > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_PROCESS_ALL, "Build Prompt from All");
            if (sel_count > 0 || total > 0) AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
            if (sel_count > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_DELETE_SEL, "Delete Selected");
            if (total > 0) AppendMenuA(menu, MF_STRING, ID_CTX_PEND_DELETE_ALL, "Delete All");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        WORD id=LOWORD(wp), note=HIWORD(wp);

        /* Auto-select-all when keyword box gains focus, so paste always replaces.
           Without this, on_history_load() puts old text in the box on startup,
           and a paste without Ctrl+A inserts into old text, concatenating keywords. */
        if(id==ID_KEYWORD && note==EN_SETFOCUS)
            PostMessageA(hKeyword, EM_SETSEL, 0, -1);

        /* Context menu commands */
        if(id==ID_CTX_KEY_CHECK) { on_key_check(); return 0; }
        if(id==ID_CTX_KEY_DELETE) { on_key_remove(); return 0; }

        /* Listbox copy commands (NLP info + prompt keyword list share same logic) */
        if(id==ID_CTX_PROMPT_COPY_RAW || id==ID_CTX_PROMPT_COPY_FULL) { id = (id==ID_CTX_PROMPT_COPY_RAW) ? ID_CTX_NLP_COPY_RAW : ID_CTX_NLP_COPY_FULL; }
        if(id==ID_CTX_NLP_COPY_RAW || id==ID_CTX_NLP_COPY_FULL) {
            /* Determine which listbox has the selection */
            HWND lb = hNlpInfo;
            if((int)SendMessageA(hPromptList, LB_GETSELCOUNT, 0, 0) > 0 &&
               (int)SendMessageA(hNlpInfo, LB_GETSELCOUNT, 0, 0) == 0)
                lb = hPromptList;
            int sel_count = (int)SendMessageA(lb, LB_GETSELCOUNT, 0, 0);
            if(sel_count > 0) {
                int *sel_items = (int *)malloc(sel_count * sizeof(int));
                if(sel_items) {
                    SendMessageA(lb, LB_GETSELITEMS, sel_count, (LPARAM)sel_items);
                    int cap = sel_count * 512;
                    char *buf = (char *)malloc(cap);
                    if(buf) {
                        int pos = 0;
                        for(int i = 0; i < sel_count && pos < cap-512; i++) {
                            char line[512];
                            SendMessageA(lb, LB_GETTEXT, sel_items[i], (LPARAM)line);
                            /* Skip section headers and blank lines */
                            if(line[0] == '=' || line[0] == '-' || line[0] == '\0') continue;
                            if(id == ID_CTX_NLP_COPY_RAW) {
                                /* Extract just the text: strip leading spaces, stop at first double-space */
                                char *start = line;
                                while(*start == ' ') start++;
                                /* Find end of text (before score columns) */
                                char *end = strstr(start, "  ");
                                if(end) *end = '\0';
                                if(start[0]) pos += snprintf(buf+pos, cap-pos, "%s\r\n", start);
                            } else {
                                /* Full line as-is */
                                char *start = line;
                                while(*start == ' ') start++;
                                if(start[0]) pos += snprintf(buf+pos, cap-pos, "%s\r\n", start);
                            }
                        }
                        /* Copy to clipboard */
                        if(pos > 0 && OpenClipboard(h)) {
                            EmptyClipboard();
                            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, pos+1);
                            if(hg) {
                                memcpy(GlobalLock(hg), buf, pos+1);
                                GlobalUnlock(hg);
                                SetClipboardData(CF_TEXT, hg);
                            }
                            CloseClipboard();
                            setStatus("Copied %d items to clipboard", sel_count);
                        }
                        free(buf);
                    }
                    free(sel_items);
                }
            }
            return 0;
        }
        if(id==ID_CTX_NLP_SELECT_ALL) {
            int count = (int)SendMessageA(hNlpInfo, LB_GETCOUNT, 0, 0);
            SendMessageA(hNlpInfo, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, count-1));
            return 0;
        }
        if(id==ID_CTX_PROMPT_SELECT_ALL) {
            int count = (int)SendMessageA(hPromptList, LB_GETCOUNT, 0, 0);
            SendMessageA(hPromptList, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, count-1));
            return 0;
        }

        /* Results list actions */
        if(id==ID_CTX_RESULT_NLP)     { on_result_action(ACTION_BUILD_PROMPT); return 0; }
        if(id==ID_CTX_RESULT_OBL)     { on_result_action(ACTION_CRAWL_OBL); return 0; }
        if(id==ID_CTX_RESULT_DELETE) {
            int sel_count = (int)SendMessageA(hResultsList, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *rows = (int *)malloc(sel_count * sizeof(int));
                if (rows) {
                    SendMessageA(hResultsList, LB_GETSELITEMS, sel_count, (LPARAM)rows);
                    int deleted = 0;
                    for (int i = sel_count - 1; i >= 0; i--) {
                        int ri = (rows[i] >= 0 && rows[i] < g_result_map_count) ? g_result_page_map[rows[i]] : -1;
                        if (ri >= 0 && ri < g_result_row_count) {
                            db_delete_result((Database *)ST->db, g_result_rows[ri].url, g_result_rows[ri].keyword);
                            deleted++;
                        }
                    }
                    free(rows);
                    refresh_results_list(NULL);
                    setStatus("Deleted %d results", deleted);
                }
            }
            return 0;
        }
        if(id==ID_CTX_RESULT_SELECT_ALL) {
            int count = (int)SendMessageA(hResultsList, LB_GETCOUNT, 0, 0);
            SendMessageA(hResultsList, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, count-1));
            return 0;
        }

        /* Tab buttons */
        if(id>=ID_TAB0&&id<=ID_TAB4){switchTab(id-ID_TAB0);return 0;}

        /* Search tab */
        if(id==ID_SEARCH)    { on_search(); return 0; }
        if(id==ID_STOP)      { on_stop(); return 0; }
        /* batch is now handled by on_search automatically */

        /* Session history */
        if(id==ID_HISTORY_LIST && note==LBN_SELCHANGE) { on_history_load(); return 0; }
        if(id==ID_HISTORY_LIST && note==LBN_DBLCLK) { on_history_load(); switchTab(TAB_ANALYSIS); return 0; }
        if(id==ID_CTX_HIST_DELETE) { on_history_delete(); return 0; }
        if(id==ID_CTX_HIST_DELETE_ALL) { on_history_delete_all(); return 0; }
        if(id==ID_CTX_PEND_VIEW_DETAIL) {
            /* Get single selected item index */
            int sel_idx = -1;
            {int sel_buf[1];
            if (SendMessageA(hPendingList, LB_GETSELITEMS, 1, (LPARAM)sel_buf) == 1) {
                int row = sel_buf[0];
                sel_idx = (row >= 0 && row < g_pend_map_count) ? g_pend_map[row] : -1;
            }}
            if (sel_idx >= 0 && sel_idx < g_pend_count) {
                char detail[4096];
                snprintf(detail, sizeof(detail),
                    "Keyword:    %s\r\n"
                    "Rank:       #%d\r\n"
                    "\r\n"
                    "Domain:     %s\r\n"
                    "URL:        %s\r\n"
                    "\r\n"
                    "Title:\r\n%s\r\n"
                    "\r\n"
                    "Snippet:\r\n%s",
                    g_pend_keywords[sel_idx],
                    g_pend_positions[sel_idx],
                    g_pend_domains[sel_idx],
                    g_pend_urls[sel_idx],
                    g_pend_titles[sel_idx][0] ? g_pend_titles[sel_idx] : "(no title saved)",
                    g_pend_snippets[sel_idx][0] ? g_pend_snippets[sel_idx] : "(no snippet saved)");
                detailPopup("Serper Details", detail);
            }
            return 0;
        }
        if(id==ID_CTX_PEND_SCRAPE_SEL)  { on_scrape_pending_selected_ex(0); return 0; }
        if(id==ID_CTX_PEND_PROCESS_SEL) {
            /* Build Prompt from selected pending URLs */
            if (engine_is_running(ST->engine)) { themedDialog("Engine Running", "Wait for the current operation to finish.", 0); return 0; }
            int sel_count = (int)SendMessageA(hPendingList, LB_GETSELCOUNT, 0, 0);
            if (sel_count <= 0) return 0;
            int *rows = (int *)malloc(sel_count * sizeof(int));
            if (!rows) return 0;
            SendMessageA(hPendingList, LB_GETSELITEMS, sel_count, (LPARAM)rows);
            ResultActionArg *ra = (ResultActionArg *)calloc(1, sizeof(ResultActionArg));
            if (!ra) { free(rows); return 0; }
            ra->urls = (char **)calloc(sel_count, sizeof(char *));
            ra->keywords = (char **)calloc(sel_count, sizeof(char *));
            if (!ra->urls || !ra->keywords) { free(ra->urls); free(ra->keywords); free(ra); free(rows); return 0; }
            int actual = 0;
            for (int i = 0; i < sel_count; i++) {
                int di = (rows[i] >= 0 && rows[i] < g_pend_map_count) ? g_pend_map[rows[i]] : -1;
                if (di >= 0 && di < g_pend_count) {
                    ra->urls[actual] = str_duplicate(g_pend_urls[di]);
                    ra->keywords[actual] = str_duplicate(g_pend_keywords[di]);
                    actual++;
                }
            }
            free(rows);
            if (actual == 0) { free(ra->urls); free(ra->keywords); free(ra); return 0; }
            ra->count = actual;
            ra->action = ACTION_BUILD_PROMPT;
            ra->needs_scrape = 1;
            snprintf(ra->group_name, sizeof(ra->group_name), "%s", ra->keywords[0]);
            ST->engine->stop = 0;
            EnableWindow(hSearch, FALSE);
            setStatus("Building prompt from %d URLs...", actual);
            if (ST->engine->pipeline_handle) { CloseHandle(ST->engine->pipeline_handle); ST->engine->pipeline_handle = NULL; }
            ST->engine->pipeline_handle = CreateThread(NULL, 0, result_action_thread, ra, 0, NULL);
            ST->engine->running = 1;
            return 0;
        }
        if(id==ID_CTX_PEND_SCRAPE_ALL)  { on_scrape_pending_ex(0); return 0; }
        if(id==ID_CTX_PEND_PROCESS_ALL) {
            /* Build Prompt from all pending URLs */
            if (engine_is_running(ST->engine)) { themedDialog("Engine Running", "Wait for the current operation to finish.", 0); return 0; }
            if (g_pend_count == 0) { themedDialog("No Pending", "No pending URLs.", 0); return 0; }
            {char conf[128]; snprintf(conf, sizeof(conf), "Build prompts from all %d pending URLs?\nThis will scrape and analyze them all.", g_pend_count);
            if (!themedDialog("Confirm", conf, 1)) return 0;}
            ResultActionArg *ra = (ResultActionArg *)calloc(1, sizeof(ResultActionArg));
            if (!ra) return 0;
            ra->urls = (char **)calloc(g_pend_count, sizeof(char *));
            ra->keywords = (char **)calloc(g_pend_count, sizeof(char *));
            if (!ra->urls || !ra->keywords) { free(ra->urls); free(ra->keywords); free(ra); return 0; }
            for (int i = 0; i < g_pend_count; i++) {
                ra->urls[i] = str_duplicate(g_pend_urls[i]);
                ra->keywords[i] = str_duplicate(g_pend_keywords[i]);
            }
            ra->count = g_pend_count;
            ra->action = ACTION_BUILD_PROMPT;
            ra->needs_scrape = 1;
            snprintf(ra->group_name, sizeof(ra->group_name), "%s", ra->keywords[0]);
            ST->engine->stop = 0;
            EnableWindow(hSearch, FALSE);
            setStatus("Building prompt from %d URLs...", g_pend_count);
            if (ST->engine->pipeline_handle) { CloseHandle(ST->engine->pipeline_handle); ST->engine->pipeline_handle = NULL; }
            ST->engine->pipeline_handle = CreateThread(NULL, 0, result_action_thread, ra, 0, NULL);
            ST->engine->running = 1;
            return 0;
        }
        if(id==ID_CTX_PEND_DELETE_SEL) { on_pending_delete_selected(); return 0; }
        if(id==ID_CTX_PEND_DELETE_ALL) { on_pending_delete_all(); return 0; }

        /* Analysis tab */
        if(id==ID_COPY_BTN)  { on_copy_prompt(); return 0; }
        /* Save buttons removed — auto-save handles all formats */
        if(id==ID_TEMPLATE_COMBO && note==CBN_SELCHANGE) { on_template_change(); return 0; }
        /* Prompt option checkboxes -- toggle state and regenerate (#20) */
        if(id>=ID_CHK_KEYWORDS && id<=ID_CHK_RELATED) {
            int idx = id - ID_CHK_KEYWORDS;
            g_chk_state[idx] = !g_chk_state[idx];
            InvalidateRect((HWND)lp, NULL, TRUE);  /* repaint the checkbox */
            on_template_change(); return 0;
        }
        /* Max keyword/entity edits -- regenerate live as user types (#25) */
        if((id==ID_MAX_KW_EDIT || id==ID_MAX_ENT_EDIT) && note==EN_CHANGE) {
            on_template_change(); return 0;
        }
        if(id==ID_GAP_ANALYZE) { on_gap_analyze(); return 0; }

        /* Settings tab */
        if(id==ID_KEY_ADD)      { on_key_add(); return 0; }
        if(id==ID_KEY_IMPORT)   { import_from_file("Import API Keys", "Text Files\0*.txt\0All Files\0*.*\0", import_key_line); config_save(ST->config, ".env"); return 0; }
        if(id==ID_KEY_PASTE)    { paste_from_clipboard(import_key_line); config_save(ST->config, ".env"); return 0; }
        if(id==ID_PROXY_IMPORT) { import_from_file("Import Proxies", "Text Files\0*.txt\0All Files\0*.*\0", import_proxy_line); proxy_save(ST->proxy, "data/proxies.txt"); return 0; }
        if(id==ID_PROXY_PASTE)  { paste_from_clipboard(import_proxy_line); proxy_save(ST->proxy, "data/proxies.txt"); return 0; }
        if(id==ID_BLOCKLIST_IMPORT) { import_from_file("Import Blocklist", "Text Files\0*.txt\0All Files\0*.*\0", import_blocklist_line); filter_save_blocklist(ST->blocklist, "data/blocklist.txt"); return 0; }
        if(id==ID_BLOCKLIST_PASTE)  { paste_from_clipboard(import_blocklist_line); filter_save_blocklist(ST->blocklist, "data/blocklist.txt"); return 0; }
        if(id==ID_KEY_REMOVE)   { on_key_remove(); return 0; }
        /* key check via right-click menu now */
        if(id==ID_PROXY_ADD)    { on_proxy_add(); return 0; }
        if(id==ID_PROXY_REMOVE) { on_proxy_remove(); return 0; }
        if(id==ID_AUTOMAP_BTN)     { on_automap(); return 0; }
        if(id==ID_DOWNLOAD_MODELS) { on_download_models(); return 0; }
        if(id==ID_PURGE_DB) { on_purge_db(); return 0; }
        if(id==ID_SETTINGS_SUB_CONN) { switchSettingsSubTab(0); relayout(); return 0; }
        if(id==ID_SETTINGS_SUB_DATA) { switchSettingsSubTab(1); relayout(); return 0; }
        if(id==ID_BLOCKLIST_ADD) { on_blocklist_add(); return 0; }
        if(id==ID_BLOCKLIST_DEL) { on_blocklist_del(); return 0; }
        /* scrape pending via right-click context menu now */

        /* Auto Scrape off → disable Auto NLP (and cascade to OBL) */
        if(id==ID_CHK_AUTO_SCRAPE) {
            int scrape_on = (int)SendMessageA(hChkAutoScrape, BM_GETCHECK, 0, 0);
            if(!scrape_on) {
                SendMessageA(hChkAutoNlp, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(hChkAutoNlp, FALSE);
                SendMessageA(hChkAutoObl, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(hChkAutoObl, FALSE);
                SendMessageA(hChkAutoOblNlp, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(hChkAutoOblNlp, FALSE);
            } else {
                EnableWindow(hChkAutoNlp, TRUE);
                EnableWindow(hChkAutoObl, TRUE);
                /* OBL NLP still depends on Auto OBL being checked */
                if(SendMessageA(hChkAutoObl, BM_GETCHECK, 0, 0))
                    EnableWindow(hChkAutoOblNlp, TRUE);
            }
            return 0;
        }
        /* Auto OBL off → disable Auto OBL NLP */
        if(id==ID_CHK_AUTO_OBL) {
            int obl_checked = (int)SendMessageA(hChkAutoObl, BM_GETCHECK, 0, 0);
            if(!obl_checked) {
                SendMessageA(hChkAutoOblNlp, BM_SETCHECK, BST_UNCHECKED, 0);
                EnableWindow(hChkAutoOblNlp, FALSE);
            } else {
                EnableWindow(hChkAutoOblNlp, TRUE);
            }
            return 0;
        }

        /* Settings combo changes -- auto-save */
        if(id==ID_THREADS_COMBO && note==CBN_SELCHANGE) {
            save_settings(); return 0;
        }
        /* Settings edit changes -- auto-save on killfocus */
        if((id==ID_GET_TIMEOUT || id==ID_POST_TIMEOUT || id==ID_MAX_REDIRECTS ||
            id==ID_RETRY_COUNT || id==ID_RETRY_BASE)
            && note==EN_KILLFOCUS) {
            save_settings(); return 0;
        }

        /* Log tab */
        if(id==ID_LOG_FILTER_DBG)  { g_log_show_dbg  = !g_log_show_dbg;  InvalidateRect(hLogFilterDbg,NULL,TRUE);  SendMessageA(hLogList,LB_RESETCONTENT,0,0); g_last_log_seq=0; return 0; }
        if(id==ID_LOG_FILTER_INFO) { g_log_show_info = !g_log_show_info; InvalidateRect(hLogFilterInfo,NULL,TRUE); SendMessageA(hLogList,LB_RESETCONTENT,0,0); g_last_log_seq=0; return 0; }
        if(id==ID_LOG_FILTER_WARN) { g_log_show_warn = !g_log_show_warn; InvalidateRect(hLogFilterWarn,NULL,TRUE); SendMessageA(hLogList,LB_RESETCONTENT,0,0); g_last_log_seq=0; return 0; }
        if(id==ID_LOG_FILTER_ERR)  { g_log_show_err  = !g_log_show_err;  InvalidateRect(hLogFilterErr,NULL,TRUE);  SendMessageA(hLogList,LB_RESETCONTENT,0,0); g_last_log_seq=0; return 0; }
        if(id==ID_LOG_CLEAR) { SendMessageA(hLogList, LB_RESETCONTENT, 0, 0); g_last_log_seq=0; return 0; }

        /* NUKE */
        if(id==ID_NUKE) { on_shutdown(); return 0; }

        /* Results list selection — DB-backed detail panel */
        if(id==ID_RESULTS_LIST && note==LBN_SELCHANGE) {
            int sel = (int)SendMessageA(hResultsList, LB_GETCURSEL, 0, 0);
            int ri = (sel >= 0 && sel < g_result_map_count) ? g_result_page_map[sel] : -1;
            if(ri < 0 || ri >= g_result_row_count) { SetWindowTextA(hDetailText, ""); break; }
            DbResultRow *r = &g_result_rows[ri];
            int cap = 32768;
            char *detail = (char *)malloc(cap);
            if(!detail) break;
            int n = snprintf(detail, cap,
                "URL: %s\r\nTitle: %s\r\nDomain: %s\r\n"
                "Words: %d  |  Headings: %d\r\n"
                "Outbound: %d  |  Internal: %d  |  Scrape: %.1fs\r\n"
                "Keyword: %s  |  Rank: #%d  |  Status: %s\r\n",
                r->url, r->title, r->domain,
                r->word_count, r->heading_count,
                r->outbound_count, r->internal_count, r->scrape_time_secs,
                r->keyword, r->serp_position, r->status);

            /* Headings from DB (heap-allocated -- ~102KB) */
            {typedef struct { char tags[200][8]; char texts[200][512]; int levels[200]; } HdgBuf;
            HdgBuf *hb = (HdgBuf *)malloc(sizeof(HdgBuf));
            if (hb) {
                int hc = db_get_serp_headings((Database *)ST->db, r->url, r->keyword,
                                              hb->tags, hb->texts, hb->levels, 200);
                if(hc > 0) {
                    n += snprintf(detail+n, cap-n, "\r\n--- HEADINGS (%d) ---\r\n", hc);
                    for(int i = 0; i < hc && n < cap-100; i++)
                        n += snprintf(detail+n, cap-n, "[%s] %s\r\n", hb->tags[i], hb->texts[i]);
                }
                free(hb);
            }}

            /* OBL links from DB (heap-allocated -- ~225KB) */
            {typedef struct { char urls[100][2048]; char anchors[100][256]; } OblBuf;
            OblBuf *ob = (OblBuf *)malloc(sizeof(OblBuf));
            if (ob) {
                int oc = db_get_obl_links((Database *)ST->db, r->url, ob->urls, ob->anchors, 100);
                if(oc > 0) {
                    n += snprintf(detail+n, cap-n, "\r\n--- OUTBOUND LINKS (%d filtered) ---\r\n", oc);
                    for(int i = 0; i < oc && n < cap-200; i++) {
                        n += snprintf(detail+n, cap-n, "  %s\r\n", ob->urls[i]);
                        if(ob->anchors[i][0]) n += snprintf(detail+n, cap-n, "    Text: %s\r\n", ob->anchors[i]);
                    }
                }
                free(ob);
            }}

            /* Per-page keywords from DB */
            {char pkws[30][64];
            int pkc = db_get_serp_page_keywords((Database *)ST->db, r->url, r->keyword, pkws, 30);
            if(pkc > 0) {
                n += snprintf(detail+n, cap-n, "\r\n--- PAGE KEYWORDS (%d) ---\r\n", pkc);
                for(int i = 0; i < pkc && n < cap-100; i++)
                    n += snprintf(detail+n, cap-n, "  %s\r\n", pkws[i]);
            }}

            SetWindowTextA(hDetailText, detail);
            free(detail);
            return 0;
        }
        break;
    }

    /* Status message update from background thread (Issue 4/6) */
    case WM_APP+10:
        if (InterlockedExchange(&g_progress_update_flag, 0)) {
            snprintf(ST->status_msg, sizeof(ST->status_msg), "%s", g_progress_msg);
            if (hStatus) SetWindowTextA(hStatus, ST->status_msg);
        }
        return 0;

    /* Download models result from background thread (Issue 5/7) */
    case WM_APP+11: {
        g_models_cached = -1;  /* invalidate -- re-check on next paint */
        EnableWindow(hDownloadModels, TRUE);
        if ((int)wp) {
            setStatus("NLP models downloaded successfully! Restart to enable entity extraction.");
            themedDialog("Download Complete",
                        "NLP models downloaded successfully!\n\n"
                        "Restart the app to enable entity extraction.\n"
                        "Models are in the 'models' directory.", 0);
        } else {
            setStatus("Model download failed. Check log for details.");
            themedDialog("Download Failed",
                        "Model download failed.\n\n"
                        "Check your internet connection and try again.\n"
                        "See the Log tab for details.", 0);
        }
        return 0;
    }

    /* Credit check result from background thread */
    case WM_APP+12: {
        int credits = g_credit_result;
        int key_idx = g_credit_key_index;
        if (key_idx >= 0) {
            /* Per-key check (right-click menu) */
            if (credits >= 0) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Key %d: %d credits remaining", key_idx + 1, credits);
                setStatus(msg);
                themedDialog("Credits", msg, 0);
            } else {
                setStatus("Could not check credits. See log.");
                themedDialog("Credit Check Failed",
                             "Could not check credits.\nSee the Log tab for details.", 0);
            }
        } else {
            /* Default check (startup / post-analysis / batch) */
            if (credits >= 0) {
                app_log(LOG_INFO, "Serper: %d credits remaining", credits);
            }
        }
        return 0;
    }

    /* ── Deferred auto-save: single keyword (#6) ── */
    case WM_APP+13: {
        g_save_pending = 0;
        if(!ST || !ST->engine) return 0;
        NLPResult *nlp = ST->engine->nlp_result;
        if(!nlp || (nlp->keyword_count == 0 && nlp->heading_count == 0)) return 0;

        /* Session save to DB now happens in pipeline_thread (engine.c) -- thread-safe
           via db write_lock.  Only lightweight UI refresh needed here. */
        if(ST->db) {
            /* Save positions for decay tracking (lightweight) */
            for(int i = 0; i < ST->engine->page_count; i++) {
                if(ST->engine->pages[i].word_count > 0)
                    db_save_position(ST->db, nlp->keyword, ST->engine->pages[i].domain,
                                    i+1, ST->engine->pages[i].word_count, ST->engine->pages[i].heading_count);
            }
            /* Refresh history */
            populate_history_list();
        }

        /* Auto-save all prompts to output/ in all 3 formats */
        {char safe_kw[128];
        snprintf(safe_kw, sizeof(safe_kw), "%s", nlp->keyword);
        /* Sanitize keyword for filename: replace spaces/special chars with underscores */
        for(char *p = safe_kw; *p; p++) {
            if(!isalnum((unsigned char)*p) && *p != '-') *p = '_';
        }
        /* Trim trailing underscores */
        {int len = (int)strlen(safe_kw);
        while(len > 0 && safe_kw[len-1] == '_') safe_kw[--len] = '\0';}

        {int big = PROMPT_MAX_LEN * 2;
        char *buf = (char *)malloc(big);
        if(buf) {
            /* Create keyword subdirectory: output/<keyword>/ */
            {char dir[MAX_PATH];
            snprintf(dir, sizeof(dir), "output/%s", safe_kw);
            ensure_directory(dir);}

            /* -- JSON: all 4 templates + full data -- */
            {char path[MAX_PATH];
            snprintf(path, sizeof(path), "output/%s/%s.json", safe_kw, safe_kw);
            {int pos = 0;
            {time_t now = time(NULL); struct tm *t = localtime(&now);
            pos += snprintf(buf+pos, big-pos, "{\n  \"keyword\": \"%s\",\n", nlp->keyword);
            pos += snprintf(buf+pos, big-pos, "  \"timestamp\": \"%04d-%02d-%02dT%02d:%02d:%02d\",\n",
                t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);}

            /* Full data section */
            pos += snprintf(buf+pos, big-pos, "  \"data\": ");
            {char *json_data = (char *)malloc(PROMPT_MAX_LEN);
            if(json_data) {
                prompt_export_json(nlp, json_data, PROMPT_MAX_LEN);
                pos += snprintf(buf+pos, big-pos, "%s,\n", json_data);
                free(json_data);
            }}

            /* All 4 prompt templates */
            {char *tmpl = (char *)malloc(PROMPT_MAX_LEN);
            if(tmpl) {
                pos += snprintf(buf+pos, big-pos, "  \"prompts\": {\n");

                prompt_build_full(nlp, tmpl, PROMPT_MAX_LEN);
                {char *esc = (char *)malloc(PROMPT_MAX_LEN);
                if(esc) {
                    json_escape(tmpl, esc, PROMPT_MAX_LEN);
                    pos += snprintf(buf+pos, big-pos, "    \"full_system_prompt\": \"%s\",\n", esc);

                    prompt_build_keywords(nlp, tmpl, PROMPT_MAX_LEN);
                    json_escape(tmpl, esc, PROMPT_MAX_LEN);
                    pos += snprintf(buf+pos, big-pos, "    \"keywords_only\": \"%s\",\n", esc);

                    prompt_build_outline(nlp, tmpl, PROMPT_MAX_LEN);
                    json_escape(tmpl, esc, PROMPT_MAX_LEN);
                    pos += snprintf(buf+pos, big-pos, "    \"outline_only\": \"%s\",\n", esc);

                    prompt_build_competitive(nlp, tmpl, PROMPT_MAX_LEN);
                    json_escape(tmpl, esc, PROMPT_MAX_LEN);
                    pos += snprintf(buf+pos, big-pos, "    \"competitive_brief\": \"%s\"\n", esc);

                    free(esc);
                }}
                pos += snprintf(buf+pos, big-pos, "  }\n}\n");
                free(tmpl);
            }}
            {FILE *f = fopen(path, "w");
            if(f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved JSON: %s", path); }}
            }}

            /* -- Markdown: itemized report -- */
            {char path[MAX_PATH];
            snprintf(path, sizeof(path), "output/%s/%s.md", safe_kw, safe_kw);
            prompt_export_markdown(nlp, buf, big);
            {FILE *f = fopen(path, "w");
            if(f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved MD: %s", path); }}
            }

            /* -- TXT: segmented for parsing -- */
            {char path[MAX_PATH];
            snprintf(path, sizeof(path), "output/%s/%s.txt", safe_kw, safe_kw);
            {int pos = 0;
            pos += snprintf(buf+pos, big-pos, "========================================\n");
            pos += snprintf(buf+pos, big-pos, "KEYWORD: %s\n", nlp->keyword);
            pos += snprintf(buf+pos, big-pos, "========================================\n\n");

            pos += snprintf(buf+pos, big-pos, "[INTENT]\n");
            pos += snprintf(buf+pos, big-pos, "primary=%s\n", INTENT_NAMES_LC[nlp->intent.primary_intent]);
            pos += snprintf(buf+pos, big-pos, "content_type=%s\n", nlp->intent.content_type);
            pos += snprintf(buf+pos, big-pos, "tone=%s\n", nlp->intent.tone);
            pos += snprintf(buf+pos, big-pos, "recommended_words=%d\n", nlp->intent.recommended_words);
            pos += snprintf(buf+pos, big-pos, "recommended_headings=%d\n\n", nlp->intent.recommended_headings);

            pos += snprintf(buf+pos, big-pos, "[STATS]\n");
            pos += snprintf(buf+pos, big-pos, "avg_words=%.0f\n", nlp->stats.avg_word_count);
            pos += snprintf(buf+pos, big-pos, "min_words=%d\n", nlp->stats.min_word_count);
            pos += snprintf(buf+pos, big-pos, "max_words=%d\n", nlp->stats.max_word_count);
            pos += snprintf(buf+pos, big-pos, "avg_h2=%.1f\n", nlp->stats.avg_h2_count);
            pos += snprintf(buf+pos, big-pos, "avg_h3=%.1f\n\n", nlp->stats.avg_h3_count);

            pos += snprintf(buf+pos, big-pos, "[KEYWORDS]\n");
            for(int i = 0; i < nlp->keyword_count && pos < big-100; i++)
                pos += snprintf(buf+pos, big-pos, "%s|%.3f|%d|%s\n",
                    nlp->keywords[i].text, nlp->keywords[i].score,
                    nlp->keywords[i].frequency, nlp->keywords[i].source);
            pos += snprintf(buf+pos, big-pos, "\n");

            if(nlp->entity_count > 0) {
                pos += snprintf(buf+pos, big-pos, "[ENTITIES]\n");
                for(int i = 0; i < nlp->entity_count && pos < big-100; i++)
                    pos += snprintf(buf+pos, big-pos, "%s|%s|%d\n",
                        nlp->entities[i].text, nlp->entities[i].label, nlp->entities[i].frequency);
                pos += snprintf(buf+pos, big-pos, "\n");
            }

            pos += snprintf(buf+pos, big-pos, "[HEADINGS]\n");
            for(int i = 0; i < nlp->heading_count && pos < big-100; i++)
                pos += snprintf(buf+pos, big-pos, "%s|%s|%d\n",
                    nlp->headings[i].tag, nlp->headings[i].text, nlp->headings[i].count);
            pos += snprintf(buf+pos, big-pos, "\n");

            if(nlp->paa_count > 0) {
                pos += snprintf(buf+pos, big-pos, "[PAA]\n");
                for(int i = 0; i < nlp->paa_count; i++)
                    pos += snprintf(buf+pos, big-pos, "%s\n", nlp->paa[i]);
                pos += snprintf(buf+pos, big-pos, "\n");
            }
            if(nlp->related_count > 0) {
                pos += snprintf(buf+pos, big-pos, "[RELATED]\n");
                for(int i = 0; i < nlp->related_count; i++)
                    pos += snprintf(buf+pos, big-pos, "%s\n", nlp->related[i]);
                pos += snprintf(buf+pos, big-pos, "\n");
            }

            /* All 4 prompts in one file */
            pos += snprintf(buf+pos, big-pos, "[PROMPT:FULL]\n");
            pos += prompt_build_full(nlp, buf+pos, big-pos);
            pos += snprintf(buf+pos, big-pos, "\n\n[PROMPT:KEYWORDS]\n");
            pos += prompt_build_keywords(nlp, buf+pos, big-pos);
            pos += snprintf(buf+pos, big-pos, "\n\n[PROMPT:OUTLINE]\n");
            pos += prompt_build_outline(nlp, buf+pos, big-pos);
            pos += snprintf(buf+pos, big-pos, "\n\n[PROMPT:COMPETITIVE]\n");
            pos += prompt_build_competitive(nlp, buf+pos, big-pos);
            pos += snprintf(buf+pos, big-pos, "\n");

            {FILE *f = fopen(path, "w");
            if(f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved TXT: %s", path); }}
            }}

            free(buf);
            app_log(LOG_INFO, "Auto-save complete: output/%s/{json,md,txt}", safe_kw);
        }}}
        return 0;
    }

    /* ── Batch complete: lightweight UI refresh only (#6) ──
       Heavy session saves (JSON build + DB + file I/O) now happen in
       batch_pipeline_thread in engine.c, keeping the UI thread responsive. */
    case WM_APP+14: {
        g_batch_save_pending = 0;
        if(!ST || !ST->engine || !ST->engine->batch) return 0;
        BatchResults *br = ST->engine->batch;
        if(br->phase < 4) return 0;

        app_log(LOG_INFO, "CHECKPOINT: batch UI refresh (%d keywords)", br->keyword_count);

        /* Refresh history (sessions already saved by pipeline thread) */
        if(ST->db) {
            populate_history_list();
            if(g_history_count > 0) SendMessageA(hHistoryList, LB_SETCURSEL, 0, 0);
            on_history_load();
            refresh_results_list(NULL);
            g_pend_count = -1;  /* force pending refresh */
            refresh_pending_list();
        }

        /* Check credits (background) */
        start_credit_check(ST);

        setStatus("Batch complete: %d keywords analyzed", br->completed);
        br->phase = 0;  /* prevent re-processing */
        return 0;
    }

    /* ── Immediate pipeline-complete notification (posted from engine threads) ── */
    case WM_APP+15: {
        if(!ST || !ST->engine) return 0;
        if(engine_is_running(ST->engine)) return 0;  /* spurious -- still running */

        app_log(LOG_INFO, "CHECKPOINT: WM_APP+15 immediate pipeline-complete notification");

        EnableWindow(hSearch, TRUE);
        EnableWindow(hStop, FALSE);
        SetWindowTextA(hSearch, "Go");
        InvalidateRect(hSearch, NULL, TRUE);

        refresh_results_list(NULL);
        g_pend_count = -1;  /* force refresh even if count looks unchanged */
        refresh_pending_list();

        NLPResult *nlp15 = ST->engine->nlp_result;
        if(nlp15 && (nlp15->keyword_count > 0 || nlp15->heading_count > 0)) {
            if(ST->engine->generated_prompt)
                setPromptText(ST->engine->generated_prompt);
            populate_nlp_info(nlp15);

            int successful15 = 0;
            for(int i = 0; i < ST->engine->page_count; i++)
                if(ST->engine->pages[i].word_count > 50) successful15++;
            setStatus("Done: %d pages scraped, %d keywords, %d headings -- prompt ready!",
                     successful15, nlp15->keyword_count, nlp15->heading_count);
            if(hProgress) SendMessageA(hProgress, PBM_SETPOS, 100, 0);

            /* Deferred saves (file output + position tracking) */
            if(!g_save_pending) {
                g_save_pending = 1;
                PostMessage(hw, WM_APP+13, 0, 0);
            }

            save_settings();
            start_credit_check(ST);
            InvalidateRect(hw, NULL, FALSE);
        }

        /* Handle batch completion */
        if(ST->engine->batch && ST->engine->batch->phase >= 4) {
            if(!g_batch_save_pending) {
                g_batch_save_pending = 1;
                PostMessage(hw, WM_APP+14, 0, 0);
            }
        }

        /* Refresh history (sessions already saved by pipeline thread) */
        if(ST->db) {
            populate_history_list();
        }
        return 0;
    }

    case WM_CLOSE:
        if (engine_is_running(ST->engine)) {
            if (!themedDialog("Close",
                    "An analysis is still running. Stop and close?", 1))
                return 0;  /* user cancelled */
        }
        cancel_download();
        engine_nuke(ST->engine);
        DestroyWindow(h);
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;  /* allow Windows shutdown */

    case WM_ENDSESSION:
        if (wp) {  /* Windows is actually shutting down */
            cancel_download();
            engine_nuke(ST->engine);
            save_settings();
        }
        return 0;

    case WM_DESTROY:
        /* Save settings one last time before exit */
        save_settings();
        KillTimer(hw, ID_PROGRESS_TIMER);
        KillTimer(hw, ID_LOG_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        app_log(LOG_ERROR, "Exception caught in WndProc (msg=0x%04x, wp=0x%llx)", msg, (unsigned long long)wp);
    }
    return DefWindowProcA(h,msg,wp,lp);
}

/* ── Public API ───────────────────────────────────────────────── */

void tui_init(void) {
    /* Per-monitor DPI awareness (Win 8.1+), fallback to system-wide */
    {HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetDpiFunc)(int);
        SetDpiFunc fn = (SetDpiFunc)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) fn(2); /* PROCESS_PER_MONITOR_DPI_AWARE = 2 */
        else SetProcessDPIAware();
        FreeLibrary(shcore);
    } else {
        SetProcessDPIAware();
    }}
    INITCOMMONCONTROLSEX icc={sizeof(icc),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    mkfonts();
    rebuild_brushes();
}

void tui_shutdown(void) {
    DeleteObject(fNorm);DeleteObject(fBold);DeleteObject(fTitle);DeleteObject(fMono);
    DeleteObject(brBg);DeleteObject(brBg2);DeleteObject(brBg3);
}

void tui_run(AppState *state) {
    ST=state;
    sysinfo_detect(&SI);

    WNDCLASSEXA wc={0};
    wc.cbSize=sizeof(wc); wc.lpfnWndProc=proc; wc.hInstance=GetModuleHandle(NULL);
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=brBg;
    wc.lpszClassName="SerpPromptWriterWnd"; wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
    RegisterClassExA(&wc);

    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    int ww=dpi_scale(1024),wh=dpi_scale(768);
    if(ww>sw) ww=sw; if(wh>sh) wh=sh;
    hw=CreateWindowExA(0,"SerpPromptWriterWnd","SERP to Prompt Writer",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,(sw-ww)/2,(sh-wh)/2,ww,wh,NULL,NULL,GetModuleHandle(NULL),NULL);

    mkcontrols(hw);
    ShowWindow(hw,SW_SHOW);
    app_log(LOG_INFO, "UI initialized successfully");

    /* Check Serper credits on startup (background) */
    start_credit_check(ST);
    UpdateWindow(hw);

    /* Timer for periodic status updates (every 2 sec) */
    SetTimer(hw, ID_PROGRESS_TIMER, 2000, NULL);
    /* Timer for log updates (every 1 sec) */
    SetTimer(hw, ID_LOG_TIMER, 1000, NULL);

    MSG msg;
    while(GetMessage(&msg,NULL,0,0)>0){
        /* Ctrl+Enter in keyword field triggers search (Enter adds newline for batch) */
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && msg.hwnd==hKeyword
           && GetKeyState(VK_CONTROL)<0){
            SendMessage(hw,WM_COMMAND,MAKEWPARAM(ID_SEARCH,BN_CLICKED),(LPARAM)hSearch);
            continue;
        }
        /* Enter in key input triggers add */
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && msg.hwnd==hKeyInput){
            SendMessage(hw,WM_COMMAND,MAKEWPARAM(ID_KEY_ADD,BN_CLICKED),(LPARAM)hKeyAdd);
            continue;
        }
        /* Enter in proxy input triggers add */
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && msg.hwnd==hProxyInput){
            SendMessage(hw,WM_COMMAND,MAKEWPARAM(ID_PROXY_ADD,BN_CLICKED),(LPARAM)hProxyAdd);
            continue;
        }
        /* Enter in blocklist input triggers add */
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && msg.hwnd==hBlocklistEdit){
            SendMessage(hw,WM_COMMAND,MAKEWPARAM(ID_BLOCKLIST_ADD,BN_CLICKED),(LPARAM)hBlocklistAdd);
            continue;
        }
        /* Ctrl+1..5 switches tabs */
        if(msg.message==WM_KEYDOWN && GetKeyState(VK_CONTROL)<0 &&
           msg.wParam>='1' && msg.wParam<='5'){
            SendMessage(hw,WM_COMMAND,MAKEWPARAM(ID_TAB0+(int)(msg.wParam-'1'),BN_CLICKED),0);
            continue;
        }
        /* Ctrl+F focuses keyword on search tab */
        if(msg.message==WM_KEYDOWN && GetKeyState(VK_CONTROL)<0 && msg.wParam=='F'){
            if(curTab==TAB_SEARCH && hKeyword) SetFocus(hKeyword);
            continue;
        }
        TranslateMessage(&msg);DispatchMessage(&msg);
    }
    state->running=0;
}

#else
/* ── Non-Windows stubs ────────────────────────────────────────── */
void tui_init(void){}
void tui_shutdown(void){}
void tui_run(AppState *s){(void)s;fprintf(stderr,"GUI requires Windows.\n");}
#endif
