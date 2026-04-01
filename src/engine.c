#include "engine.h"
#include "prompt.h"
#include "database.h"
#include "app_log.h"
#include "utils.h"
#include "cJSON.h"
#include "js_render.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ── Junk URL filter (affiliate/tracker/non-content patterns) ── */

static int is_junk_url(const char *url) {
    return str_contains_i(url, "click-") ||
           str_contains_i(url, "/redirect") ||
           str_contains_i(url, "affiliate") ||
           str_contains_i(url, "anrdoezrs") ||
           str_contains_i(url, "jdoqocy") ||
           str_contains_i(url, "dpbolvw") ||
           str_contains_i(url, "tkqlhce") ||
           str_contains_i(url, "awin1.com") ||
           str_contains_i(url, "shareasale") ||
           str_contains_i(url, "pntra.com") ||
           str_contains_i(url, "go.skimresources") ||
           str_contains_i(url, "cna.st/p/") ||
           str_contains_i(url, "rstyle.me") ||
           str_contains_i(url, "howl.me") ||
           str_contains_i(url, "narrativ.com") ||
           str_contains_i(url, "account.") ||
           str_contains_i(url, "login") ||
           str_contains_i(url, "signin") ||
           str_contains_i(url, "signup") ||
           str_contains_i(url, "/auth") ||
           str_contains_i(url, "checkout") ||
           str_contains_i(url, "/cart") ||
           str_contains_i(url, ".app.goo.gl") ||
           str_contains_i(url, "discord.gg") ||
           str_contains_i(url, "discord.com/invite");
}

/* ── Pipeline parameter passing ──────────────────────────────── */

typedef struct {
    SearchEngine *eng;
    char keyword[256];
    int num_results;
    int pages_depth;
    int link_depth;
} PipelineParams;

/* ── Scrape work item ────────────────────────────────────────── */

/* ScrapeJob defined in engine.h */

void scrape_worker(void *arg) {
    ScrapeJob *job = (ScrapeJob *)arg;
    if (job->eng->stop) { job->result->scraped = 1; app_log(LOG_DEBUG, "[%d] Skipped (stopped): %s", job->rank, job->url); free(job); return; }

    const char *proxy = job->proxy_url[0] ? job->proxy_url : NULL;
    app_log(LOG_DEBUG, "[%d] Fetching: %s%s", job->rank, job->url,
            proxy ? " (via proxy)" : "");

    app_log(LOG_DEBUG, "[%d] SCRAPE_START: %s", job->rank, job->url);
    __try {
        scraper_fetch_page(job->url, proxy, job->result);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        app_log(LOG_ERROR, "[%d] CRASH in scraper_fetch_page (exception 0x%08X): %s",
                job->rank, GetExceptionCode(), job->url);
        snprintf(job->result->error, sizeof(job->result->error), "Crash: exception 0x%08X", GetExceptionCode());
        http_thread_cleanup();  /* destroy corrupted curl handle so next job gets a fresh one */
    }
    app_log(LOG_DEBUG, "[%d] SCRAPE_DONE: %s (words=%d)", job->rank, job->result->domain, job->result->word_count);

    /* Store SERP context AFTER scrape (scraper_fetch_page memsets the page) */
    if (job->keyword[0])
        snprintf(job->result->keyword, sizeof(job->result->keyword), "%s", job->keyword);
    job->result->serp_position = job->rank;

    if (job->result->page_text && job->result->word_count > 0) {
        app_log(LOG_INFO, "[%d] OK: %s - %d words, %d headings, %d out-links, %.1fs",
                job->rank, job->result->domain, job->result->word_count,
                job->result->heading_count, job->result->outbound_count,
                job->result->scrape_time);
    } else {
        app_log(LOG_WARN, "[%d] FAIL: %s - %s (status=%d, %.1fs)",
                job->rank, job->result->domain, job->result->error,
                job->result->status_code, job->result->scrape_time);
    }

    /* Update DB with full scrape metadata + headings + keywords */
    if (job->eng->db && job->keyword[0]) {
        Database *db = (Database *)job->eng->db;
        ScrapedPage *p = job->result;
        if (p->word_count > 0) {
            db_update_serp_url_full(db, job->url, job->keyword, "scraped",
                p->word_count, p->heading_count, p->outbound_count, p->internal_count,
                p->scrape_time, p->used_js_fallback, p->is_crawled,
                p->is_crawled ? p->page_url : "");

            /* Save headings to DB */
            if (p->heading_count > 0) {
                /* Build arrays for the DB function */
                char (*h_tags)[8] = (char(*)[8])malloc(p->heading_count * 8);
                char (*h_texts)[512] = (char(*)[512])malloc(p->heading_count * 512);
                int *h_levels = (int *)malloc(p->heading_count * sizeof(int));
                if (h_tags && h_texts && h_levels) {
                    for (int i = 0; i < p->heading_count; i++) {
                        snprintf(h_tags[i], 8, "%s", p->headings[i].tag);
                        snprintf(h_texts[i], 512, "%s", p->headings[i].text);
                        h_levels[i] = p->headings[i].level;
                    }
                    db_save_serp_headings(db, job->url, job->keyword, h_tags, h_texts, h_levels, p->heading_count);
                }
                free(h_tags); free(h_texts); free(h_levels);
            }

            /* Save per-page keywords (if extracted) */
            if (p->page_keyword_count > 0)
                db_save_serp_page_keywords(db, job->url, job->keyword,
                    (const char(*)[64])p->page_keywords, p->page_keyword_count);

            /* Save filtered outbound links */
            if (p->link_count > 0) {
                const char **obl_urls = (const char **)malloc(p->link_count * sizeof(char *));
                const char **obl_anchors = (const char **)malloc(p->link_count * sizeof(char *));
                int obl_count = 0;
                if (obl_urls && obl_anchors) {
                    for (int i = 0; i < p->link_count; i++) {
                        if (!p->links[i].is_outbound) continue;
                        if (!url_is_valid(p->links[i].url)) continue;
                        if (filter_is_blocked(job->eng->blocklist, p->links[i].url)) continue;
                        if (filter_is_ad_tracker(p->links[i].url)) continue;
                        obl_urls[obl_count] = p->links[i].url;
                        obl_anchors[obl_count] = p->links[i].text;
                        obl_count++;
                    }
                    if (obl_count > 0)
                        db_save_obl_links(db, job->url, job->keyword, obl_urls, obl_anchors, obl_count);
                }
                free(obl_urls); free(obl_anchors);
            }
        } else {
            db_update_serp_url_full(db, job->url, job->keyword, "failed",
                0, 0, 0, 0, p->scrape_time, 0, 0, "");
        }
    }

    job->result->scraped = 1;  /* signal completion for NLP readiness check */

    app_log(LOG_DEBUG, "[%d] CALLBACK_START", job->rank);
    if (job->eng->on_page)
        job->eng->on_page(job->result, job->rank, job->eng->callback_data);
    app_log(LOG_DEBUG, "[%d] CALLBACK_DONE", job->rank);

    free(job);
}

/* ── Pipeline thread ─────────────────────────────────────────── */

static DWORD WINAPI pipeline_thread(LPVOID arg) {
    PipelineParams *params = (PipelineParams *)arg;
    SearchEngine *eng = params->eng;
    char kw[256];
    snprintf(kw, sizeof(kw), "%s", params->keyword);
    int num_results = params->num_results;
    int pages_depth = params->pages_depth;
    eng->link_depth = params->link_depth;
    free(params);

    app_log(LOG_INFO, "=== Pipeline starting for '%s' ===", kw);
    app_log(LOG_INFO, "Config: results=%d, pages=%d, depth=%d, proxies=%s, UAs=%d",
            num_results, pages_depth, eng->link_depth,
            eng->use_proxies ? "on" : "off", http_ua_count());

    /* ── Phase 1: Serper search ──────────────────────────────── */
    if (eng->on_progress)
        eng->on_progress("searching", 0.0f, "Querying Serper API...", eng->callback_data);

    int max_pages = pages_depth < 5 ? 5 : pages_depth;
    SerpResponse *responses = (SerpResponse *)calloc(max_pages, sizeof(SerpResponse));
    if (!responses) { app_log(LOG_ERROR, "Out of memory"); eng->running = 0; http_thread_cleanup(); return 1; }
    const char *api_key = config_next_api_key(eng->config);

    app_log(LOG_INFO, "Phase 1: Serper API search (key=%.8s..., %d remaining)",
            api_key ? api_key : "none", config_total_remaining(eng->config));

    if (!api_key) {
        app_log(LOG_ERROR, "No API keys available - add keys in Settings");
        if (eng->on_progress)
            eng->on_progress("error", 1.0f, "No API keys available", eng->callback_data);
        free(responses);
        http_thread_cleanup();
        eng->running = 0;
        return 1;
    }

    int resp_count = serper_search_pages(kw, num_results, pages_depth,
                                          api_key, NULL, responses, max_pages, &eng->stop);
    eng->serp_responses = responses;
    eng->serp_response_count = resp_count;

    if (eng->stop) { http_thread_cleanup(); eng->running = 0; return 0; }

    /* Collect organic results */
    int total_organic = 0;
    for (int i = 0; i < resp_count; i++)
        total_organic += responses[i].organic_count;

    if (total_organic == 0) {
        app_log(LOG_WARN, "No search results returned");
        if (eng->on_progress)
            eng->on_progress("error", 1.0f, "No results found", eng->callback_data);
        http_thread_cleanup();
        eng->running = 0;
        return 0;
    }

    /* ── Phase 2: Filter + Scrape ────────────────────────────── */
    /* Single pass: count, log filtered, collect URLs with hash-based dedup */
    char **urls = (char **)calloc(total_organic + 1, sizeof(char *));
    int *ranks = (int *)calloc(total_organic + 1, sizeof(int));
    if (!urls || !ranks) { free(urls); free(ranks); free(responses); eng->running = 0; http_thread_cleanup(); return 1; }

    /* DJB2 hash set for O(1) URL dedup */
    #define SERP_DEDUP_BUCKETS 1021
    typedef struct SerpDedupNode { const char *url; struct SerpDedupNode *next; } SerpDedupNode;
    SerpDedupNode *dedup_buckets[SERP_DEDUP_BUCKETS];
    memset(dedup_buckets, 0, sizeof(dedup_buckets));

    int url_count = 0;
    int filtered = 0;
    int blocked_count = 0;
    int dupes_skipped = 0;
    {int rank = 0;
    for (int r = 0; r < resp_count; r++) {
        for (int i = 0; i < responses[r].organic_count; i++) {
            if (filter_is_blocked(eng->blocklist, responses[r].organic[i].link)) {
                blocked_count++;
                app_log(LOG_DEBUG, "  Filtered: %s", responses[r].organic[i].domain);
                continue;
            }
            filtered++;
            rank++;

            /* Hash-based dedup: DJB2 */
            const char *link = responses[r].organic[i].link;
            unsigned int h = 5381;
            for (const char *s = link; *s; s++) h = ((h << 5) + h) + (unsigned char)*s;
            h %= SERP_DEDUP_BUCKETS;
            int dup = 0;
            for (SerpDedupNode *n = dedup_buckets[h]; n; n = n->next) {
                if (strcmp(n->url, link) == 0) { dup = 1; break; }
            }
            if (dup) { dupes_skipped++; continue; }

            urls[url_count] = str_duplicate(link);
            ranks[url_count] = rank;

            /* Insert into hash set (points into urls[] -- freed later) */
            SerpDedupNode *node = (SerpDedupNode *)malloc(sizeof(SerpDedupNode));
            if (node) {
                node->url = urls[url_count];
                node->next = dedup_buckets[h];
                dedup_buckets[h] = node;
            }

            url_count++;
            /* Save to DB with full Serper metadata (absolute rank) */
            if (eng->db) {
                SerpResult *sr = &responses[r].organic[i];
                int abs_pos = r * 10 + sr->position;  /* page 0-based * 10 + per-page position */
                db_save_serp_url((Database *)eng->db, sr->link, kw, sr->domain,
                                sr->title, sr->snippet, abs_pos, r+1);
            }
        }
    }}

    /* Free dedup hash nodes */
    for (int i = 0; i < SERP_DEDUP_BUCKETS; i++) {
        SerpDedupNode *n = dedup_buckets[i];
        while (n) { SerpDedupNode *next = n->next; free(n); n = next; }
    }
    #undef SERP_DEDUP_BUCKETS

    app_log(LOG_INFO, "Phase 2: %d SERP results, %d after domain filter (%d blocked)",
            total_organic, filtered, blocked_count);
    if (dupes_skipped > 0)
        app_log(LOG_INFO, "Dedup: skipped %d duplicate URLs across SERP pages", dupes_skipped);

    if (eng->on_progress) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Scraping %d pages...", url_count);
        eng->on_progress("scraping", 0.0f, msg, eng->callback_data);
    }

    /* Shuffle URLs to spread requests across domains */
    {int *order = (int *)malloc(url_count * sizeof(int));
    if (!order) { for (int i = 0; i < url_count; i++) free(urls[i]); free(urls); free(ranks); free(responses); eng->running = 0; http_thread_cleanup(); return 1; }
    for (int i = 0; i < url_count; i++) order[i] = i;
    shuffle_ints(order, url_count);
    app_log(LOG_INFO, "Shuffled %d URLs for distributed scraping", url_count);

    /* Allocate pages and submit in shuffled order */
    eng->pages = (ScrapedPage *)calloc(url_count + 1, sizeof(ScrapedPage));
    if (!eng->pages) {
        app_log(LOG_ERROR, "Failed to allocate %d ScrapedPages", url_count);
        free(order);
        for (int i = 0; i < url_count; i++) free(urls[i]);
        free(urls); free(ranks);
        eng->running = 0; http_thread_cleanup();
        return 1;
    }
    eng->page_count = url_count;

    if (eng->auto_scrape) {
        for (int s = 0; s < url_count && !eng->stop; s++) {
            int idx = order[s];
            ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
            if (!job) continue;
            job->eng = eng;
            snprintf(job->url, sizeof(job->url), "%s", urls[idx]);
            snprintf(job->keyword, sizeof(job->keyword), "%s", kw);
            job->rank = ranks[idx];
            job->result = &eng->pages[idx];
            tp_submit(&eng->scrape_pool, scrape_worker, job);
        }
    } else {
        app_log(LOG_INFO, "Auto-scrape disabled: %d URLs saved to pending queue only", url_count);
    }
    free(order);}

    /* Cleanup URL list */
    for (int i = 0; i < url_count; i++) free(urls[i]);
    free(urls); free(ranks);

    /* Wait for all scraping to complete */
    if (!eng->auto_scrape) {
        /* No scraping -- URLs are in pending queue, we're done */
        app_log(LOG_INFO, "Auto-scrape off: %d URLs in pending queue, pipeline stopping", eng->page_count);
        free(responses);
        if (eng->on_progress) eng->on_progress("done", 1.0f, "URLs saved to pending queue", eng->callback_data);
        eng->running = 0; http_thread_cleanup(); return 0;
    }
    app_log(LOG_INFO, "CHECKPOINT: waiting for %d scrape jobs to finish", eng->page_count);
    tp_wait(&eng->scrape_pool);
    app_log(LOG_INFO, "CHECKPOINT: tp_wait returned");

    if (eng->stop) { app_log(LOG_INFO, "CHECKPOINT: stopped by user"); http_thread_cleanup(); eng->running = 0; return 0; }

    int successful = 0;
    for (int i = 0; i < eng->page_count; i++)
        if (eng->pages[i].word_count > 50) successful++;

    app_log(LOG_INFO, "Phase 2 complete: %d/%d pages scraped successfully", successful, eng->page_count);

    /* Log per-page details */
    for (int i = 0; i < eng->page_count; i++) {
        ScrapedPage *p = &eng->pages[i];
        if (p->word_count > 0)
            app_log(LOG_DEBUG, "  [%d] %s: %d words, %d headings, %d outbound links, %.1fs%s",
                    i+1, p->domain, p->word_count, p->heading_count,
                    p->outbound_count, p->scrape_time,
                    p->used_js_fallback ? " (JS)" : "");
        else if (p->error[0])
            app_log(LOG_DEBUG, "  [%d] %s: FAILED - %s", i+1, p->domain, p->error);
    }

    app_log(LOG_INFO, "CHECKPOINT: extracting per-page keywords for %d pages", eng->page_count);
    /* Extract per-page keywords for browsing (#17) */
    for (int i = 0; i < eng->page_count; i++)
        nlp_extract_page_keywords(&eng->pages[i]);
    app_log(LOG_INFO, "CHECKPOINT: per-page keywords done");

    /* Remember SERP-only page count before crawl merge (for NLP analysis) */
    int serp_page_count = eng->page_count;

    /* ── Phase 2b: Outbound link crawling ────────────────────── */
    if (eng->link_depth > 0 && eng->auto_obl && successful > 0 && !eng->stop) {
        if (eng->on_progress) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Following outbound links (depth %d)...", eng->link_depth);
            eng->on_progress("crawling", 0.3f, msg, eng->callback_data);
        }
        app_log(LOG_INFO, "Phase 2b: crawling outbound links (depth=%d)", eng->link_depth);

        /* Collect outbound URLs from successful pages */
        int crawl_total = 0;
        int crawl_cap = 30;  /* max outbound pages to crawl -- keep it focused */
        char **crawl_urls = (char **)calloc(crawl_cap, sizeof(char *));
        int *crawl_source = (int *)calloc(crawl_cap, sizeof(int));  /* #26: source SERP page index */
        if (!crawl_urls || !crawl_source) { free(crawl_urls); free(crawl_source); eng->running = 0; http_thread_cleanup(); return 1; }

        for (int i = 0; i < eng->page_count && crawl_total < crawl_cap; i++) {
            ScrapedPage *p = &eng->pages[i];
            if (p->word_count <= 50) continue;
            for (int l = 0; l < p->link_count && crawl_total < crawl_cap; l++) {
                if (!p->links[l].is_outbound) continue;
                if (!url_is_valid(p->links[l].url)) continue;
                if (filter_is_blocked(eng->blocklist, p->links[l].url)) continue;
                if (filter_is_ad_tracker(p->links[l].url)) continue;
                if (is_junk_url(p->links[l].url)) continue;

                /* Dedup against already-scraped pages */
                int dup = 0;
                for (int j = 0; j < eng->page_count; j++)
                    if (strcmp(eng->pages[j].page_url, p->links[l].url) == 0) { dup = 1; break; }
                /* Dedup against already-queued crawl URLs */
                for (int j = 0; j < crawl_total && !dup; j++)
                    if (strcmp(crawl_urls[j], p->links[l].url) == 0) { dup = 1; break; }
                if (dup) continue;

                crawl_source[crawl_total] = i;  /* #26: track which SERP page this came from */
                crawl_urls[crawl_total++] = str_duplicate(p->links[l].url);
            }
        }

        if (crawl_total > 0) {
            app_log(LOG_INFO, "Crawling %d outbound URLs", crawl_total);
            {int *corder = (int *)malloc(crawl_total * sizeof(int));
            if (!corder) {
                app_log(LOG_ERROR, "Out of memory for crawl order -- skipping crawl");
            } else {
            for(int i=0;i<crawl_total;i++) corder[i]=i;
            shuffle_ints(corder, crawl_total);

            /* Scrape into a separate crawl_pages array, then merge after tp_wait */
            ScrapedPage *crawl_pages = (ScrapedPage *)calloc(crawl_total, sizeof(ScrapedPage));
            if (crawl_pages) {
                for (int s = 0; s < crawl_total && !eng->stop; s++) {
                    int ci = corder[s];
                    ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                    if (!job) continue;
                    job->eng = eng;
                    snprintf(job->url, sizeof(job->url), "%s", crawl_urls[ci]);
                    job->rank = eng->page_count + ci + 1;
                    job->result = &crawl_pages[ci];
                    tp_submit(&eng->scrape_pool, scrape_worker, job);
                }

                tp_wait(&eng->scrape_pool);

                /* Merge crawl results into eng->pages via a single new allocation */
                int new_total = eng->page_count + crawl_total;
                ScrapedPage *merged = (ScrapedPage *)calloc(new_total + 1, sizeof(ScrapedPage));
                if (merged) {
                    memcpy(merged, eng->pages, eng->page_count * sizeof(ScrapedPage));
                    memcpy(merged + eng->page_count, crawl_pages, crawl_total * sizeof(ScrapedPage));
                    ScrapedPage *old_pages = eng->pages;
                    eng->pages = merged;
                    eng->page_count = new_total;
                    free(old_pages);
                    /* crawl_pages data copied; free container only */
                    free(crawl_pages);

                    /* Mark crawled pages with source tracking (#26) */
                    for (int i = 0; i < crawl_total; i++) {
                        int pi = eng->page_count - crawl_total + i;
                        eng->pages[pi].is_crawled = 1;
                        eng->pages[pi].source_page_idx = crawl_source[i];
                    }
                    int crawl_success = 0;
                    for (int i = eng->page_count - crawl_total; i < eng->page_count; i++)
                        if (eng->pages[i].word_count > 50) crawl_success++;
                    app_log(LOG_INFO, "Crawl complete: %d/%d outbound pages scraped", crawl_success, crawl_total);
                    successful += crawl_success;
                } else {
                    app_log(LOG_ERROR, "OOM merging crawl results (%d pages)", new_total);
                    for (int ci = 0; ci < crawl_total; ci++)
                        scraper_free(&crawl_pages[ci]);
                    free(crawl_pages);
                }
            }
            } /* end corder else */
            free(corder);}
        } else {
            app_log(LOG_INFO, "No eligible outbound links to crawl");
        }

        /* Extract per-page keywords for crawled pages too (#17) */
        for (int i = serp_page_count; i < eng->page_count; i++)
            nlp_extract_page_keywords(&eng->pages[i]);

        for (int i = 0; i < crawl_total; i++) free(crawl_urls[i]);
        free(crawl_urls);
        free(crawl_source);
    }

    if (eng->stop) { app_log(LOG_INFO, "CHECKPOINT: stopped by user after crawl"); http_thread_cleanup(); eng->running = 0; return 0; }

    app_log(LOG_INFO, "CHECKPOINT: entering Phase 3 (successful=%d, serp_page_count=%d, page_count=%d)",
            successful, serp_page_count, eng->page_count);

    if (!eng->auto_nlp) {
        app_log(LOG_INFO, "Phase 3: NLP disabled, skipping analysis and prompt generation");
        if (eng->on_progress)
            eng->on_progress("done", 1.0f, "Scraping complete (NLP disabled)", eng->callback_data);
        free(responses);
        eng->running = 0; http_thread_cleanup(); return 0;
    }

    /* ── Phase 3: NLP Analysis ───────────────────────────────── */
    if (eng->on_progress) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Running NLP on %d pages...", successful);
        eng->on_progress("analyzing", 0.5f, msg, eng->callback_data);
    }
    app_log(LOG_INFO, "Phase 3: NLP analysis on %d SERP pages (%d total scraped)", serp_page_count, eng->page_count);

    /* Copy PAA and related BEFORE nlp_analyze so the scorer can use them */
    for (int r = 0; r < resp_count; r++) {
        for (int i = 0; i < responses[r].paa_count && eng->nlp_result->paa_count < 10; i++)
            {int pi = eng->nlp_result->paa_count;
            snprintf(eng->nlp_result->paa[pi], 512, "%s", responses[r].paa[i].question);
            snprintf(eng->nlp_result->paa_snippets[pi], 1024, "%s", responses[r].paa[i].snippet);
            snprintf(eng->nlp_result->paa_links[pi], 2048, "%s", responses[r].paa[i].link);
            eng->nlp_result->paa_count++;}
        for (int i = 0; i < responses[r].related_count && eng->nlp_result->related_count < 20; i++)
            snprintf(eng->nlp_result->related[eng->nlp_result->related_count++], 256,
                     "%s", responses[r].related[i]);
    }

    /* Save PAA + Related to DB for reuse from queue/results path */
    if (eng->db && eng->nlp_result->paa_count > 0)
        db_save_paa((Database *)eng->db, kw,
            (const char(*)[512])eng->nlp_result->paa,
            (const char(*)[1024])eng->nlp_result->paa_snippets,
            (const char(*)[2048])eng->nlp_result->paa_links,
            eng->nlp_result->paa_count);
    if (eng->db && eng->nlp_result->related_count > 0)
        db_save_related((Database *)eng->db, kw,
            (const char(*)[256])eng->nlp_result->related,
            eng->nlp_result->related_count);

    app_log(LOG_INFO, "CHECKPOINT: calling nlp_analyze");
    nlp_analyze(kw, eng->pages, serp_page_count, eng->nlp_result, eng->onnx, eng->nli);
    app_log(LOG_INFO, "CHECKPOINT: nlp_analyze returned (keywords=%d, headings=%d)",
            eng->nlp_result->keyword_count, eng->nlp_result->heading_count);

    app_log(LOG_INFO, "Top keywords:");
    for(int i = 0; i < eng->nlp_result->keyword_count && i < 20; i++)
        app_log(LOG_INFO, "  %.2f  %s (freq=%d, src=%s)",
                eng->nlp_result->keywords[i].score,
                eng->nlp_result->keywords[i].text,
                eng->nlp_result->keywords[i].frequency,
                eng->nlp_result->keywords[i].source);

    /* ONNX NER if available -- SERP pages only (#15: exclude crawled outbound) */
    if (eng->onnx && onnx_nlp_available(eng->onnx)) {
        app_log(LOG_INFO, "Phase 3b: ONNX NER entity extraction (SERP pages only)");
        const char **texts = (const char **)malloc(serp_page_count * sizeof(char *));
        const char **domains = (const char **)malloc(serp_page_count * sizeof(char *));
        if (texts && domains) {
            int valid = 0;
            for (int i = 0; i < serp_page_count; i++) {
                if (eng->pages[i].page_text && eng->pages[i].word_count > 50) {
                    texts[valid] = eng->pages[i].page_text;
                    domains[valid] = eng->pages[i].domain;
                    valid++;
                }
            }
            __try {
                eng->nlp_result->entity_count = onnx_nlp_extract_entities_batch(
                    eng->onnx, texts, domains, valid,
                    eng->nlp_result->entities, NLP_MAX_ENTITIES);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                app_log(LOG_ERROR, "ONNX NER crashed -- entity extraction skipped");
                eng->nlp_result->entity_count = 0;
            }
            app_log(LOG_INFO, "ONNX NER: extracted %d entities from %d pages",
                    eng->nlp_result->entity_count, valid);
            for(int e = 0; e < eng->nlp_result->entity_count && e < 20; e++)
                app_log(LOG_INFO, "  Entity: [%s] %s (freq=%d)",
                        eng->nlp_result->entities[e].label,
                        eng->nlp_result->entities[e].text,
                        eng->nlp_result->entities[e].frequency);
        }
        free(texts); free(domains);
    }

    app_log(LOG_INFO, "Phase 3 complete: %d keywords, %d headings, %d entities, intent=%s (%.0f%%)",
            eng->nlp_result->keyword_count, eng->nlp_result->heading_count,
            eng->nlp_result->entity_count,
            eng->nlp_result->intent.primary_intent == 0 ? "informational" :
            eng->nlp_result->intent.primary_intent == 1 ? "commercial" :
            eng->nlp_result->intent.primary_intent == 2 ? "transactional" : "navigational",
            eng->nlp_result->intent.confidence[eng->nlp_result->intent.primary_intent] * 100);

    /* PAA and related already copied before nlp_analyze (above) */

    /* ── Phase 4: Generate prompt ────────────────────────────── */
    app_log(LOG_INFO, "CHECKPOINT: entering Phase 4 (prompt generation)");
    eng->generated_prompt = (char *)malloc(PROMPT_MAX_LEN);
    if (eng->generated_prompt) {
        int plen = prompt_build_full(eng->nlp_result, eng->generated_prompt, PROMPT_MAX_LEN);
        app_log(LOG_INFO, "Prompt generated: %d chars", plen);
    }
    app_log(LOG_INFO, "CHECKPOINT: Phase 4 done");

    /* Save session to DB here in the pipeline thread (thread-safe via db write_lock)
       instead of deferring to the UI thread's WM_APP+13 handler */
    if (eng->db && eng->nlp_result->keyword_count > 0) {
        cJSON *root_j = cJSON_CreateObject();
        {char *nlp_json = (char *)malloc(PROMPT_MAX_LEN);
        if (nlp_json) {
            prompt_export_json(eng->nlp_result, nlp_json, PROMPT_MAX_LEN);
            cJSON *nlp_obj = cJSON_Parse(nlp_json);
            if (nlp_obj) {
                cJSON *child = nlp_obj->child;
                while (child) {
                    cJSON *next = child->next;
                    cJSON_DetachItemViaPointer(nlp_obj, child);
                    cJSON_AddItemToObject(root_j, child->string, child);
                    child = next;
                }
                cJSON_Delete(nlp_obj);
            }
            free(nlp_json);
        }}
        /* Pages array */
        {cJSON *pages_j = cJSON_CreateArray();
        for (int i = 0; i < serp_page_count; i++) {
            ScrapedPage *pg = &eng->pages[i];
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
            cJSON_AddNumberToObject(pj, "is_crawled", pg->is_crawled);
            cJSON_AddNumberToObject(pj, "source_idx", pg->source_page_idx);
            cJSON_AddNumberToObject(pj, "js_fallback", pg->used_js_fallback);
            cJSON_AddNumberToObject(pj, "serp_position", pg->serp_position);
            if (pg->keyword[0])
                cJSON_AddStringToObject(pj, "keyword", pg->keyword);
            if (pg->heading_count > 0) {
                cJSON *hdgs = cJSON_CreateArray();
                for (int hh = 0; hh < pg->heading_count && hh < 50; hh++) {
                    cJSON *hj = cJSON_CreateObject();
                    cJSON_AddStringToObject(hj, "tag", pg->headings[hh].tag);
                    cJSON_AddStringToObject(hj, "text", pg->headings[hh].text);
                    cJSON_AddItemToArray(hdgs, hj);
                }
                cJSON_AddItemToObject(pj, "page_headings", hdgs);
            }
            if (pg->page_keyword_count > 0) {
                cJSON *pkw = cJSON_CreateArray();
                for (int k = 0; k < pg->page_keyword_count; k++)
                    cJSON_AddItemToArray(pkw, cJSON_CreateString(pg->page_keywords[k]));
                cJSON_AddItemToObject(pj, "page_keywords", pkw);
            }
            cJSON_AddItemToArray(pages_j, pj);
        }
        cJSON_AddItemToObject(root_j, "pages", pages_j);}
        /* SERP organic results */
        if (responses && resp_count > 0) {
            cJSON *serp_j = cJSON_CreateArray();
            for (int r = 0; r < resp_count; r++) {
                for (int si = 0; si < responses[r].organic_count; si++) {
                    cJSON *sj = cJSON_CreateObject();
                    cJSON_AddNumberToObject(sj, "position", responses[r].organic[si].position);
                    cJSON_AddStringToObject(sj, "title", responses[r].organic[si].title);
                    cJSON_AddStringToObject(sj, "link", responses[r].organic[si].link);
                    cJSON_AddStringToObject(sj, "snippet", responses[r].organic[si].snippet);
                    cJSON_AddStringToObject(sj, "domain", responses[r].organic[si].domain);
                    if (responses[r].organic[si].date[0])
                        cJSON_AddStringToObject(sj, "date", responses[r].organic[si].date);
                    cJSON_AddItemToArray(serp_j, sj);
                }
            }
            cJSON_AddItemToObject(root_j, "serp_results", serp_j);
            if (responses[0].kg_title[0]) {
                cJSON *kg = cJSON_CreateObject();
                cJSON_AddStringToObject(kg, "title", responses[0].kg_title);
                cJSON_AddStringToObject(kg, "type", responses[0].kg_type);
                cJSON_AddStringToObject(kg, "description", responses[0].kg_description);
                cJSON_AddItemToObject(root_j, "knowledge_graph", kg);
            }
        }
        {char *final_json = cJSON_Print(root_j);
        if (final_json) {
            db_save_session((Database *)eng->db, kw, final_json);
            app_log(LOG_INFO, "Session saved (pipeline thread): '%s' (%d bytes)", kw, (int)strlen(final_json));
            cJSON_free(final_json);
        }}
        cJSON_Delete(root_j);
    }

    /* ── Phase 5: Finalize ───────────────────────────────────── */
    app_log(LOG_INFO, "CHECKPOINT: entering Phase 5 (finalize)");
    if (eng->on_progress)
        eng->on_progress("done", 1.0f, "Analysis complete!", eng->callback_data);

    app_log(LOG_INFO, "=== Pipeline complete for '%s' ===", kw);
    app_log(LOG_INFO, "  SERP results: %d total, %d after filter", total_organic, filtered);
    app_log(LOG_INFO, "  Pages scraped: %d successful out of %d", successful, eng->page_count);
    app_log(LOG_INFO, "  NLP: %d keywords, %d headings, %d entities",
            eng->nlp_result->keyword_count, eng->nlp_result->heading_count,
            eng->nlp_result->entity_count);
    app_log(LOG_INFO, "  Intent: %s, Type: %s, Tone: %s",
            eng->nlp_result->intent.primary_intent == 0 ? "informational" :
            eng->nlp_result->intent.primary_intent == 1 ? "commercial" :
            eng->nlp_result->intent.primary_intent == 2 ? "transactional" : "navigational",
            eng->nlp_result->intent.content_type, eng->nlp_result->intent.tone);
    app_log(LOG_INFO, "  PAA questions: %d, Related searches: %d",
            eng->nlp_result->paa_count, eng->nlp_result->related_count);
    app_log(LOG_INFO, "  Recommended: ~%d words, %d+ headings",
            eng->nlp_result->intent.recommended_words,
            eng->nlp_result->intent.recommended_headings);

    app_log(LOG_INFO, "Pipeline complete: %d pages, %d keywords, %d headings",
            successful, eng->nlp_result->keyword_count, eng->nlp_result->heading_count);

    app_log(LOG_INFO, "CHECKPOINT: pipeline thread cleanup and exit");
    http_thread_cleanup();
    eng->running = 0;

    /* Notify UI immediately instead of waiting for 2-second timer poll */
    {HWND hMain = FindWindowA("SerpPromptWriterWnd", NULL);
    if (hMain) PostMessageA(hMain, WM_APP+15, 0, 0);}

    return 0;
}

/* ── Public API ──────────────────────────────────────────────── */

void engine_init(SearchEngine *eng) {
    memset(eng, 0, sizeof(SearchEngine));
    eng->scrape_threads = 16;
    eng->pipeline_handle = NULL;
    eng->auto_scrape = 1;
    eng->auto_nlp = 1;
    eng->auto_obl = 0;
    eng->auto_obl_nlp = 0;
    eng->nlp_result = (NLPResult *)calloc(1, sizeof(NLPResult));
}

void engine_shutdown(SearchEngine *eng) {
    if (eng->pipeline_handle) {
        eng->stop = 1;
        WaitForSingleObject(eng->pipeline_handle, 5000);
        CloseHandle(eng->pipeline_handle);
        eng->pipeline_handle = NULL;
    }
    engine_cancel(eng);
    if (eng->scrape_pool_created)
        tp_destroy(&eng->scrape_pool);
    free(eng->serp_responses);
    if (eng->pages) {
        for (int i = 0; i < eng->page_count; i++)
            scraper_free(&eng->pages[i]);
        free(eng->pages);
    }
    free(eng->generated_prompt);
    free(eng->nlp_result);
    eng->nlp_result = NULL;

    /* Free batch results if any */
    if (eng->batch) {
        for (int i = 0; i < eng->batch->keyword_count; i++) {
            free(eng->batch->keywords[i].responses);
            free(eng->batch->keywords[i].page_indices);
            free(eng->batch->keywords[i].nlp_result);
            free(eng->batch->keywords[i].generated_prompt);
        }
        free(eng->batch->keywords);
        free(eng->batch);
        eng->batch = NULL;
    }
}

void engine_set_callbacks(SearchEngine *eng,
                          engine_progress_fn progress,
                          engine_page_fn page,
                          void *userdata) {
    eng->on_progress = progress;
    eng->on_page = page;
    eng->callback_data = userdata;
}

int engine_run(SearchEngine *eng, const char *keyword,
               int num_results, int pages_depth, int link_depth) {
    if (eng->running) return -1;
    eng->running = 1;
    eng->stop = 0;

    /* Close previous pipeline thread handle (#2: handle leak fix) */
    if (eng->pipeline_handle) {
        CloseHandle(eng->pipeline_handle);
        eng->pipeline_handle = NULL;
    }

    /* Clean previous results */
    free(eng->serp_responses); eng->serp_responses = NULL;
    if (eng->pages) {
        for (int i = 0; i < eng->page_count; i++) scraper_free(&eng->pages[i]);
        free(eng->pages);
    }
    eng->pages = NULL;
    eng->page_count = 0;
    free(eng->generated_prompt); eng->generated_prompt = NULL;
    if (!eng->nlp_result) {
        eng->nlp_result = (NLPResult *)calloc(1, sizeof(NLPResult));
        if (!eng->nlp_result) { app_log(LOG_ERROR, "Failed to allocate NLPResult"); eng->running = 0; return -1; }
    } else {
        memset(eng->nlp_result, 0, sizeof(NLPResult));
    }

    /* Create scrape pool (thread count set by user or auto-detected at startup) */
    if (!eng->scrape_pool_created) {
        int threads = eng->scrape_threads;
        if (threads < 2) threads = 2;
        if (threads > 256) threads = 256;
        tp_create(&eng->scrape_pool, threads, 0);
        eng->scrape_pool_created = 1;
        app_log(LOG_INFO, "Scrape pool created: %d threads", threads);
    }

    /* Pass params via a heap-allocated struct (pipeline thread frees it) */
    PipelineParams *pp = (PipelineParams *)calloc(1, sizeof(PipelineParams));
    if (!pp) { eng->running = 0; return -1; }
    pp->eng = eng;
    snprintf(pp->keyword, sizeof(pp->keyword), "%s", keyword);
    pp->num_results = num_results;
    pp->pages_depth = pages_depth;
    pp->link_depth = link_depth;

    /* Launch pipeline thread and store handle */
    eng->pipeline_handle = CreateThread(NULL, 0, pipeline_thread, pp, 0, NULL);
    if (!eng->pipeline_handle) { free(pp); eng->running = 0; return -1; }

    return 0;
}

void engine_cancel(SearchEngine *eng) {
    eng->stop = 1;
}

void engine_nuke(SearchEngine *eng) {
    eng->stop = 1;
    js_render_cancel();  /* unblock threads waiting for CDP slots */
    if (eng->pipeline_handle) {
        WaitForSingleObject(eng->pipeline_handle, 2000);
        CloseHandle(eng->pipeline_handle);
        eng->pipeline_handle = NULL;
    }
    if (eng->scrape_pool_created)
        tp_destroy_fast(&eng->scrape_pool);
    eng->scrape_pool_created = 0;
    eng->running = 0;
    app_log(LOG_WARN, "NUKE: engine killed");
}

int engine_is_running(SearchEngine *eng) {
    return eng->running;
}

/* ── Batch pipeline ─────────────────────────────────────────── */

typedef struct {
    SearchEngine *eng;
    char **keywords;
    int keyword_count;
    int num_results;
    int pages_depth;
    int link_depth;
} BatchPipelineParams;

static DWORD WINAPI batch_pipeline_thread(LPVOID arg) {
    BatchPipelineParams *bp = (BatchPipelineParams *)arg;
    SearchEngine *eng = bp->eng;
    int kw_count = bp->keyword_count;
    int num_results = bp->num_results;
    int pages_depth = bp->pages_depth;

    BatchResults *batch = eng->batch;
    batch->phase = 1;

    app_log(LOG_INFO, "=== BATCH pipeline starting: %d keywords ===", kw_count);

    /* ── Setup: API key, thread pool, URL arrays ─────────────── */
    const char *api_key = config_next_api_key(eng->config);
    if (!api_key) {
        app_log(LOG_ERROR, "No API keys available");
        if (eng->on_progress)
            eng->on_progress("error", 1.0f, "No API keys available", eng->callback_data);
        free(bp->keywords);
        free(bp);
        eng->running = 0;
        return 1;
    }

    /* Create scrape pool (thread count set by user or auto-detected at startup) */
    if (!eng->scrape_pool_created) {
        int threads = eng->scrape_threads;
        if (threads < 2) threads = 2;
        if (threads > 256) threads = 256;
        tp_create(&eng->scrape_pool, threads, 0);
        eng->scrape_pool_created = 1;
        app_log(LOG_INFO, "Scrape pool created: %d threads", threads);
    }

    int max_urls = kw_count * 50;
    char **all_urls = (char **)calloc(max_urls, sizeof(char *));
    int *url_ranks = (int *)calloc(max_urls, sizeof(int));
    int *url_kw_idx = (int *)calloc(max_urls, sizeof(int));
    ScrapedPage *all_pages = (ScrapedPage *)calloc(max_urls + 1, sizeof(ScrapedPage));
    if (!all_urls || !url_ranks || !url_kw_idx || !all_pages) {
        app_log(LOG_ERROR, "BATCH: failed to allocate URL/page arrays");
        free(all_urls); free(url_ranks); free(url_kw_idx); free(all_pages);
        free(bp->keywords); free(bp);
        eng->running = 0;
        return 1;
    }
    int total_urls = 0;
    int total_dupes = 0;

    /* DJB2 hash set for O(1) batch URL dedup */
    #define BATCH_DEDUP_BUCKETS 1021
    typedef struct BatchDedupNode { int url_idx; struct BatchDedupNode *next; } BatchDedupNode;
    BatchDedupNode *batch_dedup[BATCH_DEDUP_BUCKETS];
    memset(batch_dedup, 0, sizeof(batch_dedup));

    /* ── Phase 1: Serper API calls + concurrent scraping ───── */
    for (int k = 0; k < kw_count && !eng->stop; k++) {
        BatchKeyword *bk = &batch->keywords[k];
        snprintf(bk->keyword, sizeof(bk->keyword), "%s", bp->keywords[k]);

        snprintf(batch->progress_msg, sizeof(batch->progress_msg),
                 "Serper: %d/%d - %s (%d URLs scraping)", k+1, kw_count, bk->keyword, total_urls);
        if (eng->on_progress)
            eng->on_progress("searching", (float)k / kw_count * 0.3f, batch->progress_msg, eng->callback_data);

        bk->responses = (SerpResponse *)calloc(pages_depth < 5 ? 5 : pages_depth, sizeof(SerpResponse));
        if (!bk->responses) continue;

        bk->response_count = serper_search_pages(bk->keyword, num_results, pages_depth,
                                                  api_key, NULL, bk->responses,
                                                  pages_depth < 5 ? 5 : pages_depth, &eng->stop);

        /* Collect PAA and related */
        for (int r = 0; r < bk->response_count; r++) {
            for (int i = 0; i < bk->responses[r].paa_count && bk->paa_count < 10; i++)
                {int pi = bk->paa_count;
                snprintf(bk->paa[pi], 512, "%s", bk->responses[r].paa[i].question);
                snprintf(bk->paa_snippets[pi], 1024, "%s", bk->responses[r].paa[i].snippet);
                snprintf(bk->paa_links[pi], 2048, "%s", bk->responses[r].paa[i].link);
                bk->paa_count++;}
            for (int i = 0; i < bk->responses[r].related_count && bk->related_count < 20; i++)
                snprintf(bk->related[bk->related_count++], 256, "%s", bk->responses[r].related[i]);
        }

        /* Save PAA + Related to DB */
        if (eng->db && bk->paa_count > 0)
            db_save_paa((Database *)eng->db, bk->keyword,
                (const char(*)[512])bk->paa, (const char(*)[1024])bk->paa_snippets,
                (const char(*)[2048])bk->paa_links, bk->paa_count);
        if (eng->db && bk->related_count > 0)
            db_save_related((Database *)eng->db, bk->keyword,
                (const char(*)[256])bk->related, bk->related_count);

        /* Save to DB + dedup + submit scrape jobs immediately */
        bk->page_indices = (int *)calloc(50, sizeof(int));
        bk->page_index_count = 0;
        int rank = 0;

        for (int r = 0; r < bk->response_count; r++) {
            for (int i = 0; i < bk->responses[r].organic_count && total_urls < max_urls; i++) {
                SerpResult *sr = &bk->responses[r].organic[i];
                if (filter_is_blocked(eng->blocklist, sr->link)) continue;
                rank++;

                /* Save to DB (absolute rank = page*10 + per-page position) */
                if (eng->db) {
                    int abs_pos = r * 10 + sr->position;
                    db_save_serp_url((Database *)eng->db, sr->link, bk->keyword, sr->domain,
                                    sr->title, sr->snippet, abs_pos, r+1);
                }

                /* Hash-based global dedup: DJB2 */
                unsigned int bh = 5381;
                for (const char *s = sr->link; *s; s++) bh = ((bh << 5) + bh) + (unsigned char)*s;
                bh %= BATCH_DEDUP_BUCKETS;
                int dup = 0;
                for (BatchDedupNode *dn = batch_dedup[bh]; dn; dn = dn->next) {
                    if (strcmp(all_urls[dn->url_idx], sr->link) == 0) {
                        if (bk->page_index_count < 50)
                            bk->page_indices[bk->page_index_count++] = dn->url_idx;
                        dup = 1;
                        total_dupes++;
                        break;
                    }
                }
                if (!dup) {
                    int idx = total_urls;
                    if (bk->page_index_count < 50)
                        bk->page_indices[bk->page_index_count++] = idx;
                    all_urls[idx] = str_duplicate(sr->link);
                    url_ranks[idx] = rank;
                    url_kw_idx[idx] = k;

                    /* Insert into hash set */
                    BatchDedupNode *bdn = (BatchDedupNode *)malloc(sizeof(BatchDedupNode));
                    if (bdn) { bdn->url_idx = idx; bdn->next = batch_dedup[bh]; batch_dedup[bh] = bdn; }

                    total_urls++;

                    if (eng->auto_scrape) {
                        /* Submit scrape job immediately so scraping runs while Serper calls continue */
                        ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                        if (job) {
                            job->eng = eng;
                            snprintf(job->url, sizeof(job->url), "%s", sr->link);
                            snprintf(job->keyword, sizeof(job->keyword), "%s", bk->keyword);
                            job->rank = rank;
                            job->result = &all_pages[idx];
                            tp_submit(&eng->scrape_pool, scrape_worker, job);
                        }
                    } else {
                        /* Store context on page and mark scraped
                           so the polling phase doesn't hang waiting for scrape completion. */
                        snprintf(all_pages[idx].keyword, sizeof(all_pages[idx].keyword), "%s", bk->keyword);
                        all_pages[idx].serp_position = rank;
                        all_pages[idx].scraped = 1;
                    }
                }
            }
        }

        app_log(LOG_INFO, "Serper %d/%d: '%s' - %d results, %d total URLs queued",
                k+1, kw_count, bk->keyword,
                bk->response_count > 0 ? bk->responses[0].organic_count : 0, total_urls);
    }

    /* Free batch dedup hash set */
    for (int i = 0; i < BATCH_DEDUP_BUCKETS; i++) {
        BatchDedupNode *n = batch_dedup[i];
        while (n) { BatchDedupNode *next = n->next; free(n); n = next; }
    }
    #undef BATCH_DEDUP_BUCKETS

    if (eng->stop || total_urls == 0) {
        /* Wait for in-flight scrape workers to finish before freeing all_pages,
           since job->result points into all_pages[]. Without this wait, workers
           write to freed memory causing access violations. */
        if (eng->scrape_pool_created && total_urls > 0)
            tp_wait(&eng->scrape_pool);
        for (int i = 0; i < total_urls; i++) { scraper_free(&all_pages[i]); free(all_urls[i]); }
        free(all_pages); free(all_urls); free(url_ranks); free(url_kw_idx);
        free(bp->keywords); free(bp);
        eng->running = 0;
        http_thread_cleanup();
        return 0;
    }

    /* ── Wait for scrape jobs (already submitted during Serper loop) ── */
    if (eng->auto_scrape) {
        app_log(LOG_INFO, "BATCH: waiting for %d scrape jobs to finish (%d dupes skipped)",
                total_urls, total_dupes);
        tp_wait(&eng->scrape_pool);
        app_log(LOG_INFO, "BATCH: scraping complete (%d URLs)", total_urls);
    } else {
        app_log(LOG_INFO, "BATCH: %d URLs saved to pending queue (%d dupes skipped), scraping disabled",
                total_urls, total_dupes);
    }

    /* ── Phase 2: NLP as keywords become ready ────────────────── */
    batch->phase = 2;

    int *kw_done = (int *)calloc(kw_count, sizeof(int));
    int nlp_completed = 0;

    if (!eng->auto_scrape || !eng->auto_nlp) {
        /* No scraping or no NLP -- mark all keywords done, skip to next phase */
        if (!eng->auto_scrape)
            app_log(LOG_INFO, "BATCH Phase 2: skipped (auto_scrape off, URLs in pending queue)");
        else {
            /* auto_scrape on but auto_nlp off: scraping already done in waves above */
            app_log(LOG_INFO, "BATCH Phase 2: NLP disabled, skipping analysis");
        }
        nlp_completed = kw_count;
        for (int k = 0; k < kw_count; k++) kw_done[k] = 1;
    } else {
        /* Scraping already done in waves above — NLP runs immediately for each keyword */
        app_log(LOG_INFO, "BATCH Phase 2: running NLP for %d keywords (%d pages scraped)", kw_count, total_urls);
    }

    while (nlp_completed < kw_count && !eng->stop) {
        int made_progress = 0;

        for (int k = 0; k < kw_count && !eng->stop; k++) {
            if (kw_done[k]) continue;
            BatchKeyword *bk = &batch->keywords[k];
            if (!bk->page_indices || bk->page_index_count == 0) {
                kw_done[k] = 1; nlp_completed++; made_progress = 1;
                continue;
            }

            /* Check if all pages for this keyword are scraped */
            int all_ready = 1;
            for (int i = 0; i < bk->page_index_count; i++) {
                int pi = bk->page_indices[i];
                if (pi >= 0 && pi < total_urls && !all_pages[pi].scraped) {
                    all_ready = 0; break;
                }
            }
            if (!all_ready) continue;

            /* Check if any pages actually have content (not all failed) */
            {int has_content = 0;
            for (int i = 0; i < bk->page_index_count; i++) {
                int pi = bk->page_indices[i];
                if (pi >= 0 && pi < total_urls && all_pages[pi].word_count > 50) has_content++;
            }
            if (has_content == 0) {
                /* Retry once: reset scraped flags and resubmit to threadpool */
                if (bk->retry_count < 1) {
                    bk->retry_count++;
                    app_log(LOG_WARN, "BATCH retry '%s': all %d pages failed, resubmitting",
                            bk->keyword, bk->page_index_count);
                    for (int i = 0; i < bk->page_index_count; i++) {
                        int pi = bk->page_indices[i];
                        if (pi >= 0 && pi < total_urls) {
                            all_pages[pi].scraped = 0;
                            all_pages[pi].word_count = 0;
                            ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                            if (job) {
                                job->eng = eng;
                                snprintf(job->url, sizeof(job->url), "%s", all_urls[pi]);
                                snprintf(job->keyword, sizeof(job->keyword), "%s", bk->keyword);
                                job->rank = url_ranks[pi];
                                job->result = &all_pages[pi];
                                tp_submit(&eng->scrape_pool, scrape_worker, job);
                            }
                        }
                    }
                    continue;  /* poll again later */
                }
                app_log(LOG_WARN, "BATCH NLP skipped '%s': all %d pages failed after retry",
                        bk->keyword, bk->page_index_count);
                kw_done[k] = 1; nlp_completed++; made_progress = 1; continue;
            }}

            /* All pages ready -- extract per-page keywords and run NLP */
            for (int i = 0; i < bk->page_index_count; i++) {
                int pi = bk->page_indices[i];
                if (pi >= 0 && pi < total_urls && all_pages[pi].word_count > 50 && all_pages[pi].page_keyword_count == 0)
                    nlp_extract_page_keywords(&all_pages[pi]);
            }

            snprintf(batch->progress_msg, sizeof(batch->progress_msg),
                     "NLP: %d/%d - %s", nlp_completed+1, kw_count, bk->keyword);
            if (eng->on_progress)
                eng->on_progress("analyzing", 0.3f + (float)nlp_completed / kw_count * 0.7f,
                                 batch->progress_msg, eng->callback_data);

            /* Gather this keyword's pages */
            int kw_page_count = bk->page_index_count;
            ScrapedPage *kw_pages = (ScrapedPage *)calloc(kw_page_count + 1, sizeof(ScrapedPage));
            if (!kw_pages) { kw_done[k] = 1; nlp_completed++; made_progress = 1; continue; }

            for (int i = 0; i < kw_page_count; i++) {
                int pi = bk->page_indices[i];
                if (pi >= 0 && pi < total_urls)
                    kw_pages[i] = all_pages[pi];
            }

            bk->nlp_result = (NLPResult *)calloc(1, sizeof(NLPResult));
            if (!bk->nlp_result) { free(kw_pages); kw_done[k] = 1; nlp_completed++; made_progress = 1; continue; }

            bk->nlp_result->paa_count = bk->paa_count;
            bk->nlp_result->related_count = bk->related_count;
            memcpy(bk->nlp_result->paa, bk->paa, sizeof(bk->paa));
            memcpy(bk->nlp_result->paa_snippets, bk->paa_snippets, sizeof(bk->paa_snippets));
            memcpy(bk->nlp_result->paa_links, bk->paa_links, sizeof(bk->paa_links));
            memcpy(bk->nlp_result->related, bk->related, sizeof(bk->related));

            nlp_analyze(bk->keyword, kw_pages, kw_page_count, bk->nlp_result, eng->onnx, eng->nli);

            /* ONNX NER entity extraction */
            if (eng->onnx && onnx_nlp_available(eng->onnx)) {
                const char **ner_texts = (const char **)malloc(kw_page_count * sizeof(char *));
                const char **ner_doms = (const char **)malloc(kw_page_count * sizeof(char *));
                int ner_valid = 0;
                if (ner_texts && ner_doms) {
                    for (int ni = 0; ni < kw_page_count; ni++) {
                        if (kw_pages[ni].page_text && kw_pages[ni].word_count > 50) {
                            ner_texts[ner_valid] = kw_pages[ni].page_text;
                            ner_doms[ner_valid] = kw_pages[ni].domain;
                            ner_valid++;
                        }
                    }
                    if (ner_valid > 0) {
                        __try {
                            bk->nlp_result->entity_count = onnx_nlp_extract_entities_batch(
                                eng->onnx, ner_texts, ner_doms, ner_valid,
                                bk->nlp_result->entities, NLP_MAX_ENTITIES);
                        } __except(EXCEPTION_EXECUTE_HANDLER) {
                            bk->nlp_result->entity_count = 0;
                        }
                        app_log(LOG_INFO, "BATCH NER: %d entities from %d pages for '%s'",
                                bk->nlp_result->entity_count, ner_valid, bk->keyword);
                    }
                }
                free(ner_texts); free(ner_doms);
            }

            bk->generated_prompt = (char *)malloc(PROMPT_MAX_LEN);
            if (bk->generated_prompt)
                prompt_build_full(bk->nlp_result, bk->generated_prompt, PROMPT_MAX_LEN);

            /* Save session to DB right here in the pipeline thread (thread-safe via
               db write_lock) instead of deferring to the UI thread's WM_APP+14 handler,
               which would block the UI for seconds when building JSON for all keywords. */
            if (eng->db && bk->nlp_result->keyword_count > 0) {
                cJSON *root_j = cJSON_CreateObject();
                /* NLP data */
                {char *nlp_json = (char *)malloc(PROMPT_MAX_LEN);
                if (nlp_json) {
                    prompt_export_json(bk->nlp_result, nlp_json, PROMPT_MAX_LEN);
                    cJSON *nlp_obj = cJSON_Parse(nlp_json);
                    if (nlp_obj) {
                        cJSON *child = nlp_obj->child;
                        while (child) {
                            cJSON *next = child->next;
                            cJSON_DetachItemViaPointer(nlp_obj, child);
                            cJSON_AddItemToObject(root_j, child->string, child);
                            child = next;
                        }
                        cJSON_Delete(nlp_obj);
                    }
                    free(nlp_json);
                }}
                /* Pages array for this keyword */
                {cJSON *pages_j = cJSON_CreateArray();
                for (int pi = 0; pi < bk->page_index_count; pi++) {
                    int idx = bk->page_indices[pi];
                    if (idx < 0 || idx >= total_urls) continue;
                    ScrapedPage *pg = &all_pages[idx];
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
                    cJSON_AddNumberToObject(pj, "is_crawled", pg->is_crawled);
                    cJSON_AddNumberToObject(pj, "source_idx", pg->source_page_idx);
                    cJSON_AddNumberToObject(pj, "js_fallback", pg->used_js_fallback);
                    cJSON_AddNumberToObject(pj, "serp_position", pg->serp_position);
                    if (pg->keyword[0])
                        cJSON_AddStringToObject(pj, "keyword", pg->keyword);
                    if (pg->heading_count > 0) {
                        cJSON *hdgs = cJSON_CreateArray();
                        for (int hh = 0; hh < pg->heading_count && hh < 50; hh++) {
                            cJSON *hj = cJSON_CreateObject();
                            cJSON_AddStringToObject(hj, "tag", pg->headings[hh].tag);
                            cJSON_AddStringToObject(hj, "text", pg->headings[hh].text);
                            cJSON_AddItemToArray(hdgs, hj);
                        }
                        cJSON_AddItemToObject(pj, "page_headings", hdgs);
                    }
                    if (pg->page_keyword_count > 0) {
                        cJSON *kws_j = cJSON_CreateArray();
                        for (int kk = 0; kk < pg->page_keyword_count; kk++)
                            cJSON_AddItemToArray(kws_j, cJSON_CreateString(pg->page_keywords[kk]));
                        cJSON_AddItemToObject(pj, "page_keywords", kws_j);
                    }
                    cJSON_AddItemToArray(pages_j, pj);
                }
                cJSON_AddItemToObject(root_j, "pages", pages_j);}
                /* SERP results for this keyword */
                if (bk->responses && bk->response_count > 0) {
                    cJSON *serp_j = cJSON_CreateArray();
                    for (int r = 0; r < bk->response_count; r++) {
                        for (int si = 0; si < bk->responses[r].organic_count; si++) {
                            cJSON *sj = cJSON_CreateObject();
                            cJSON_AddNumberToObject(sj, "position", bk->responses[r].organic[si].position);
                            cJSON_AddStringToObject(sj, "title", bk->responses[r].organic[si].title);
                            cJSON_AddStringToObject(sj, "link", bk->responses[r].organic[si].link);
                            cJSON_AddStringToObject(sj, "snippet", bk->responses[r].organic[si].snippet);
                            cJSON_AddStringToObject(sj, "domain", bk->responses[r].organic[si].domain);
                            cJSON_AddItemToArray(serp_j, sj);
                        }
                    }
                    cJSON_AddItemToObject(root_j, "serp_results", serp_j);
                }
                {char *final_json = cJSON_Print(root_j);
                if (final_json) {
                    db_save_session((Database *)eng->db, bk->keyword, final_json);
                    app_log(LOG_INFO, "Batch session saved (pipeline thread): '%s' (%d kw, %d bytes)",
                            bk->keyword, bk->nlp_result->keyword_count, (int)strlen(final_json));
                    cJSON_free(final_json);
                } else {
                    app_log(LOG_ERROR, "Batch session save FAILED for '%s': cJSON_Print returned NULL", bk->keyword);
                }}
                cJSON_Delete(root_j);

                /* Auto-save to output directory */
                {char safe_kw[128];
                snprintf(safe_kw, sizeof(safe_kw), "%s", bk->keyword);
                for (char *sp = safe_kw; *sp; sp++)
                    if (!isalnum((unsigned char)*sp) && *sp != '-') *sp = '_';
                {int sklen = (int)strlen(safe_kw);
                while (sklen > 0 && safe_kw[sklen-1] == '_') safe_kw[--sklen] = '\0';}
                {char dir[260];
                snprintf(dir, sizeof(dir), "output/%s", safe_kw);
                ensure_directory(dir);}
                /* Save all 3 formats: JSON, MD, TXT */
                {int big = 512 * 1024;
                char *buf = (char *)malloc(big);
                if (buf) {
                    /* JSON */
                    {char path[260];
                    snprintf(path, sizeof(path), "output/%s/%s.json", safe_kw, safe_kw);
                    prompt_export_json(bk->nlp_result, buf, big);
                    /* Append prompts */
                    {int pos = (int)strlen(buf);
                    if (pos > 2 && buf[pos-2] == '}') {
                        pos -= 2;  /* back up before closing } */
                        pos += snprintf(buf+pos, big-pos, ",\n  \"prompts\": {\n");
                        {char *tmpl = (char *)malloc(PROMPT_MAX_LEN);
                        char *esc = (char *)malloc(PROMPT_MAX_LEN);
                        if (tmpl && esc) {
                            prompt_build_full(bk->nlp_result, tmpl, PROMPT_MAX_LEN);
                            json_escape(tmpl, esc, PROMPT_MAX_LEN);
                            pos += snprintf(buf+pos, big-pos, "    \"full_system_prompt\": \"%s\",\n", esc);
                            prompt_build_keywords(bk->nlp_result, tmpl, PROMPT_MAX_LEN);
                            json_escape(tmpl, esc, PROMPT_MAX_LEN);
                            pos += snprintf(buf+pos, big-pos, "    \"keywords_only\": \"%s\",\n", esc);
                            prompt_build_outline(bk->nlp_result, tmpl, PROMPT_MAX_LEN);
                            json_escape(tmpl, esc, PROMPT_MAX_LEN);
                            pos += snprintf(buf+pos, big-pos, "    \"outline_only\": \"%s\",\n", esc);
                            prompt_build_competitive(bk->nlp_result, tmpl, PROMPT_MAX_LEN);
                            json_escape(tmpl, esc, PROMPT_MAX_LEN);
                            pos += snprintf(buf+pos, big-pos, "    \"competitive_brief\": \"%s\"\n", esc);
                        }
                        free(tmpl); free(esc);}
                        pos += snprintf(buf+pos, big-pos, "  }\n}\n");
                    }}
                    {FILE *f = fopen(path, "w");
                    if (f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved JSON: %s", path); }}}

                    /* MD */
                    {char path[260];
                    snprintf(path, sizeof(path), "output/%s/%s.md", safe_kw, safe_kw);
                    prompt_export_markdown(bk->nlp_result, buf, big);
                    {FILE *f = fopen(path, "w");
                    if (f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved MD: %s", path); }}}

                    /* TXT */
                    {char path[260];
                    snprintf(path, sizeof(path), "output/%s/%s.txt", safe_kw, safe_kw);
                    {int pos = 0;
                    pos += snprintf(buf+pos, big-pos, "========================================\n");
                    pos += snprintf(buf+pos, big-pos, "KEYWORD: %s\n", bk->nlp_result->keyword);
                    pos += snprintf(buf+pos, big-pos, "========================================\n\n");
                    pos += snprintf(buf+pos, big-pos, "[INTENT]\nprimary=%s\ncontent_type=%s\ntone=%s\n",
                        bk->nlp_result->intent.primary_intent == 0 ? "informational" :
                        bk->nlp_result->intent.primary_intent == 1 ? "commercial" :
                        bk->nlp_result->intent.primary_intent == 2 ? "transactional" : "navigational",
                        bk->nlp_result->intent.content_type, bk->nlp_result->intent.tone);
                    pos += snprintf(buf+pos, big-pos, "recommended_words=%d\nrecommended_headings=%d\n\n",
                        bk->nlp_result->intent.recommended_words, bk->nlp_result->intent.recommended_headings);
                    pos += snprintf(buf+pos, big-pos, "[KEYWORDS]\n");
                    for (int ki = 0; ki < bk->nlp_result->keyword_count && pos < big-100; ki++)
                        pos += snprintf(buf+pos, big-pos, "%s|%.3f|%d|%s\n",
                            bk->nlp_result->keywords[ki].text, bk->nlp_result->keywords[ki].score,
                            bk->nlp_result->keywords[ki].frequency, bk->nlp_result->keywords[ki].source);
                    pos += snprintf(buf+pos, big-pos, "\n[PROMPT:FULL]\n");
                    pos += prompt_build_full(bk->nlp_result, buf+pos, big-pos);
                    pos += snprintf(buf+pos, big-pos, "\n");
                    }
                    {FILE *f = fopen(path, "w");
                    if (f) { fputs(buf, f); fclose(f); app_log(LOG_INFO, "Auto-saved TXT: %s", path); }}}

                    free(buf);
                    app_log(LOG_INFO, "Auto-save complete: output/%s/{json,md,txt}", safe_kw);
                }}}
            }

            app_log(LOG_INFO, "BATCH NLP %d/%d: '%s' - %d keywords, %d headings",
                    nlp_completed+1, kw_count, bk->keyword,
                    bk->nlp_result->keyword_count, bk->nlp_result->heading_count);

            batch->completed = nlp_completed + 1;
            free(kw_pages);
            kw_done[k] = 1;
            nlp_completed++;
            made_progress = 1;
        }

        if (!made_progress && nlp_completed < kw_count)
            Sleep(100);  /* brief wait for more scrapes to finish */
    }
    free(kw_done);

    /* Count final scrape stats */
    int total_successful = 0;
    for (int i = 0; i < total_urls; i++)
        if (all_pages[i].word_count > 50) total_successful++;
    app_log(LOG_INFO, "BATCH: %d/%d pages scraped successfully, %d/%d keywords analyzed",
            total_successful, total_urls, nlp_completed, kw_count);

    /* ── Phase 3: OBL crawling (post-NLP, never touches prompts) ── */
    int link_depth = bp->link_depth;
    int serp_url_count = total_urls;  /* remember pre-OBL count for OBL NLP slice */
    if (link_depth > 0 && eng->auto_obl && total_successful > 0 && !eng->stop) {
        batch->phase = 3;
        snprintf(batch->progress_msg, sizeof(batch->progress_msg),
                 "Crawling outbound links (depth %d)...", link_depth);
        if (eng->on_progress)
            eng->on_progress("crawling", 0.92f, batch->progress_msg, eng->callback_data);
        app_log(LOG_INFO, "BATCH Phase 3: crawling outbound links (depth=%d)", link_depth);

        /* Collect outbound URLs from all successful SERP pages */
        int crawl_total = 0;
        int crawl_cap = kw_count * 30;  /* up to 30 OBL per keyword */
        if (crawl_cap > 500) crawl_cap = 500;  /* global sanity cap */
        char **crawl_urls = (char **)calloc(crawl_cap, sizeof(char *));
        int *crawl_source = (int *)calloc(crawl_cap, sizeof(int));
        if (crawl_urls && crawl_source) {

            for (int i = 0; i < total_urls && crawl_total < crawl_cap; i++) {
                ScrapedPage *p = &all_pages[i];
                if (p->word_count <= 50) continue;
                for (int l = 0; l < p->link_count && crawl_total < crawl_cap; l++) {
                    if (!p->links[l].is_outbound) continue;
                    if (!url_is_valid(p->links[l].url)) continue;
                    if (filter_is_blocked(eng->blocklist, p->links[l].url)) continue;
                    if (filter_is_ad_tracker(p->links[l].url)) continue;
                    if (is_junk_url(p->links[l].url)) continue;

                    /* Dedup against SERP pages */
                    int dup = 0;
                    for (int j = 0; j < total_urls; j++)
                        if (strcmp(all_pages[j].page_url, p->links[l].url) == 0) { dup = 1; break; }
                    /* Dedup against already-queued crawl URLs */
                    for (int j = 0; j < crawl_total && !dup; j++)
                        if (strcmp(crawl_urls[j], p->links[l].url) == 0) { dup = 1; break; }
                    if (dup) continue;

                    crawl_source[crawl_total] = i;
                    crawl_urls[crawl_total++] = str_duplicate(p->links[l].url);
                }
            }

            if (crawl_total > 0) {
                app_log(LOG_INFO, "BATCH OBL: crawling %d outbound URLs", crawl_total);
                int *corder = (int *)malloc(crawl_total * sizeof(int));
                if (corder) {
                    for (int i = 0; i < crawl_total; i++) corder[i] = i;
                    shuffle_ints(corder, crawl_total);

                    ScrapedPage *crawl_pages = (ScrapedPage *)calloc(crawl_total, sizeof(ScrapedPage));
                    if (crawl_pages) {
                        for (int s = 0; s < crawl_total && !eng->stop; s++) {
                            int ci = corder[s];
                            ScrapeJob *job = (ScrapeJob *)calloc(1, sizeof(ScrapeJob));
                            if (!job) continue;
                            job->eng = eng;
                            snprintf(job->url, sizeof(job->url), "%s", crawl_urls[ci]);
                            job->rank = total_urls + ci + 1;
                            job->result = &crawl_pages[ci];
                            tp_submit(&eng->scrape_pool, scrape_worker, job);
                        }

                        tp_wait(&eng->scrape_pool);

                        /* Merge OBL pages into all_pages via realloc on eng->pages (set later) */
                        int new_total = total_urls + crawl_total;
                        ScrapedPage *merged = (ScrapedPage *)calloc(new_total + 1, sizeof(ScrapedPage));
                        if (merged) {
                            memcpy(merged, all_pages, total_urls * sizeof(ScrapedPage));
                            memcpy(merged + total_urls, crawl_pages, crawl_total * sizeof(ScrapedPage));
                            free(all_pages);
                            all_pages = merged;
                            free(crawl_pages);

                            /* Mark OBL pages */
                            for (int i = 0; i < crawl_total; i++) {
                                int pi = total_urls + i;
                                all_pages[pi].is_crawled = 1;
                                all_pages[pi].source_page_idx = crawl_source[i];
                            }

                            /* Per-page keywords for browsing */
                            for (int i = total_urls; i < new_total; i++)
                                nlp_extract_page_keywords(&all_pages[i]);

                            int crawl_success = 0;
                            for (int i = total_urls; i < new_total; i++)
                                if (all_pages[i].word_count > 50) crawl_success++;
                            app_log(LOG_INFO, "BATCH OBL complete: %d/%d outbound pages scraped",
                                    crawl_success, crawl_total);
                            total_urls = new_total;
                        } else {
                            app_log(LOG_ERROR, "BATCH OBL: OOM merging crawl results (%d pages)", new_total);
                            for (int ci = 0; ci < crawl_total; ci++)
                                scraper_free(&crawl_pages[ci]);
                            free(crawl_pages);
                        }
                    }
                    free(corder);
                }
            } else {
                app_log(LOG_INFO, "BATCH OBL: no eligible outbound links to crawl");
            }

            for (int i = 0; i < crawl_total; i++) free(crawl_urls[i]);
        }
        free(crawl_urls);
        free(crawl_source);
    }

    /* Phase 3b (OBL NLP) removed: OBL is browse-only data that does not feed into prompts,
       so running NLP on outbound-link pages was wasted computation. */

    /* ── Phase 4: Done ──────────────────────────────────────── */
    batch->phase = 4;
    if (eng->on_progress)
        eng->on_progress("done", 1.0f, "Batch complete!", eng->callback_data);

    app_log(LOG_INFO, "=== BATCH pipeline complete: %d keywords, %d URLs scraped ===",
            kw_count, total_urls);

    /* Store shared pages on engine for cleanup */
    eng->pages = all_pages;
    eng->page_count = total_urls;

    /* Copy last keyword's result as the "active" result for UI display */
    if (batch->completed > 0) {
        int last = batch->completed - 1;
        if (batch->keywords[last].nlp_result) {
            memcpy(eng->nlp_result, batch->keywords[last].nlp_result, sizeof(NLPResult));
            free(eng->generated_prompt);
            eng->generated_prompt = batch->keywords[last].generated_prompt ?
                str_duplicate(batch->keywords[last].generated_prompt) : NULL;
        }
    }

    for (int i = 0; i < total_urls; i++) free(all_urls[i]);
    free(all_urls); free(url_ranks); free(url_kw_idx);
    for (int i = 0; i < kw_count; i++) free(bp->keywords[i]);
    free(bp->keywords); free(bp);

    http_thread_cleanup();
    eng->running = 0;

    /* Notify UI immediately instead of waiting for 2-second timer poll */
    {HWND hMain = FindWindowA("SerpPromptWriterWnd", NULL);
    if (hMain) PostMessageA(hMain, WM_APP+15, 0, 0);}

    return 0;
}

int engine_run_batch(SearchEngine *eng, const char **keywords, int keyword_count,
                     int num_results, int pages_depth, int link_depth) {
    if (eng->running) return -1;
    if (keyword_count <= 0 || keyword_count > BATCH_MAX_KEYWORDS) return -1;
    eng->running = 1;
    eng->stop = 0;

    /* Close previous handle */
    if (eng->pipeline_handle) {
        CloseHandle(eng->pipeline_handle);
        eng->pipeline_handle = NULL;
    }

    /* Clean previous results */
    free(eng->serp_responses); eng->serp_responses = NULL;
    if (eng->pages) {
        for (int i = 0; i < eng->page_count; i++) scraper_free(&eng->pages[i]);
        free(eng->pages);
    }
    eng->pages = NULL; eng->page_count = 0;
    free(eng->generated_prompt); eng->generated_prompt = NULL;
    if (!eng->nlp_result) {
        eng->nlp_result = (NLPResult *)calloc(1, sizeof(NLPResult));
        if (!eng->nlp_result) { eng->running = 0; return -1; }
    } else {
        memset(eng->nlp_result, 0, sizeof(NLPResult));
    }

    /* Free previous batch */
    if (eng->batch) {
        for (int i = 0; i < eng->batch->keyword_count; i++) {
            free(eng->batch->keywords[i].responses);
            free(eng->batch->keywords[i].page_indices);
            free(eng->batch->keywords[i].nlp_result);
            free(eng->batch->keywords[i].generated_prompt);
        }
        free(eng->batch->keywords);
        free(eng->batch);
    }

    /* Allocate batch */
    eng->batch = (BatchResults *)calloc(1, sizeof(BatchResults));
    if (!eng->batch) { eng->running = 0; return -1; }
    eng->batch->keywords = (BatchKeyword *)calloc(keyword_count, sizeof(BatchKeyword));
    if (!eng->batch->keywords) { free(eng->batch); eng->batch = NULL; eng->running = 0; return -1; }
    eng->batch->keyword_count = keyword_count;

    /* Create scrape pool */
    if (!eng->scrape_pool_created) {
        int threads = eng->scrape_threads;
        if (threads < 4) threads = 4;
        if (threads > 128) threads = 128;
        tp_create(&eng->scrape_pool, threads, 0);
        eng->scrape_pool_created = 1;

    }

    /* Build params */
    BatchPipelineParams *bp = (BatchPipelineParams *)calloc(1, sizeof(BatchPipelineParams));
    if (!bp) { eng->running = 0; return -1; }
    bp->eng = eng;
    bp->keyword_count = keyword_count;
    bp->num_results = num_results;
    bp->pages_depth = pages_depth;
    bp->link_depth = link_depth;
    bp->keywords = (char **)malloc(keyword_count * sizeof(char *));
    if (!bp->keywords) { free(bp); eng->running = 0; return -1; }
    for (int i = 0; i < keyword_count; i++)
        bp->keywords[i] = str_duplicate(keywords[i]);

    eng->pipeline_handle = CreateThread(NULL, 0, batch_pipeline_thread, bp, 0, NULL);
    if (!eng->pipeline_handle) {
        for (int i = 0; i < keyword_count; i++) free(bp->keywords[i]);
        free(bp->keywords); free(bp);
        eng->running = 0;
        return -1;
    }

    return 0;
}
