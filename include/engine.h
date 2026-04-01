#ifndef ENGINE_H
#define ENGINE_H

#include "threadpool.h"
#include "resmon.h"
#include "config.h"
#include "proxy.h"
#include "filter.h"
#include "serper.h"
#include "scraper.h"
#include "nlp.h"
#include "onnx_nlp.h"
#include "nli.h"

/* ── Analysis engine — full pipeline orchestrator ────────────── */

#define BATCH_MAX_KEYWORDS  256

/* ── Batch data structures (defined before SearchEngine) ────── */

typedef struct {
    char keyword[256];
    SerpResponse *responses;
    int response_count;
    int *page_indices;
    int page_index_count;
    NLPResult *nlp_result;
    char *generated_prompt;
    int paa_count;
    char paa[10][512];
    char paa_snippets[10][1024];
    char paa_links[10][2048];
    int related_count;
    char related[20][256];
    int retry_count;
} BatchKeyword;

typedef struct {
    BatchKeyword *keywords;
    int keyword_count;
    int completed;
    volatile int phase;     /* 1=serper, 2=collecting, 3=scraping, 4=nlp, 5=done */
    char progress_msg[256];
} BatchResults;

/* Progress callback */
typedef void (*engine_progress_fn)(const char *phase, float pct,
                                    const char *message, void *userdata);

/* Result callback — called when a page is scraped */
typedef void (*engine_page_fn)(const ScrapedPage *page, int rank, void *userdata);

typedef struct {
    /* Infrastructure (not owned, pointers to app state) */
    Config          *config;
    ProxyPool       *proxy;
    Blocklist       *blocklist;
    ResourceMonitor *resmon;
    OnnxNLP         *onnx;
    NLIContext       *nli;
    void            *db;            /* Database* -- void to avoid circular include */

    /* Thread pools (owned) */
    ThreadPool       scrape_pool;
    int              scrape_pool_created;

    /* Settings */
    int              scrape_threads;
    int              use_proxies;
    int              link_depth;

    /* Pipeline flags (set from UI checkboxes before launch) */
    int              auto_scrape;       /* 1 = scrape immediately, 0 = save to pending queue only */
    int              auto_nlp;          /* 1 = run NLP after scraping, 0 = scrape only */
    int              auto_obl;          /* 1 = crawl outbound links after NLP */
    int              auto_obl_nlp;      /* 1 = run NLP on OBL pages too */

    /* State */
    volatile int     running;
    volatile int     stop;
    HANDLE           pipeline_handle;

    /* Results */
    SerpResponse    *serp_responses;
    int              serp_response_count;
    ScrapedPage     *pages;
    int              page_count;
    NLPResult       *nlp_result;        /* heap allocated (~720KB, moved off stack) */
    char            *generated_prompt;  /* heap allocated */

    /* Callbacks */
    engine_progress_fn on_progress;
    engine_page_fn     on_page;
    void              *callback_data;

    /* Batch results (NULL for single keyword runs) */
    BatchResults      *batch;
} SearchEngine;

void engine_init(SearchEngine *eng);
void engine_shutdown(SearchEngine *eng);

/* Configure before running */
void engine_set_callbacks(SearchEngine *eng,
                          engine_progress_fn progress,
                          engine_page_fn page,
                          void *userdata);

/* Run full pipeline: search → filter → scrape → crawl → NLP → prompt.
   Runs in background thread. */
int  engine_run(SearchEngine *eng, const char *keyword,
                int num_results, int pages_depth, int link_depth);

/* Stop running pipeline */
void engine_cancel(SearchEngine *eng);

/* Emergency kill */
void engine_nuke(SearchEngine *eng);

/* Query state */
int  engine_is_running(SearchEngine *eng);

/* ── Scrape job (exposed for pending queue) ─────────────────── */

typedef struct {
    SearchEngine *eng;
    char          url[2048];
    char          proxy_url[256];
    char          keyword[256];
    int           rank;
    ScrapedPage  *result;
} ScrapeJob;

void scrape_worker(void *arg);

/* ── Batch pipeline ─────────────────────────────────────────── */

/* Run batch pipeline: all Serper calls first, then all scraping at once, then NLP per keyword.
   Runs in background thread. Results accessible via eng->batch after completion. */
int  engine_run_batch(SearchEngine *eng, const char **keywords, int keyword_count,
                      int num_results, int pages_depth, int link_depth);

#endif
