/*
 * Smoke test suite for SERP to Prompt Writer
 * Exercises core functions without GUI.
 * Build: cmake --build build --config Release --target test_smoke
 * Run:   build/Release/test_smoke.exe
 */
#include "config.h"
#include "utils.h"
#include "filter.h"
#include "nlp.h"
#include "scraper.h"
#include "prompt.h"
#include "database.h"
#include "onnx_nlp.h"
#include "app_log.h"
#include "threadpool.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#endif

static int g_pass = 0, g_fail = 0;

#define TEST(name) printf("  TEST: %-50s ", name)
#define PASS() do { printf("[PASS]\n"); g_pass++; } while(0)
#define FAIL(msg) do { printf("[FAIL] %s\n", msg); g_fail++; } while(0)
#define ASSERT(cond, msg) do { if(cond) PASS(); else FAIL(msg); } while(0)

/* ── String Utils ────────────────────────────────────────────── */

static void test_string_utils(void) {
    printf("\n=== String Utils ===\n");

    TEST("str_trim whitespace");
    { char s[] = "  hello world  "; str_trim(s); ASSERT(strcmp(s, "hello world") == 0, s); }

    TEST("str_trim empty");
    { char s[] = "   "; str_trim(s); ASSERT(s[0] == '\0', "not empty"); }

    TEST("str_starts_with true");
    ASSERT(str_starts_with("hello world", "hello"), "should match");

    TEST("str_starts_with false");
    ASSERT(!str_starts_with("hello world", "world"), "should not match");

    TEST("str_ends_with true");
    ASSERT(str_ends_with("hello.txt", ".txt"), "should match");

    TEST("str_ends_with false");
    ASSERT(!str_ends_with("hello.txt", ".jpg"), "should not match");

    TEST("str_contains true");
    ASSERT(str_contains("the quick brown fox", "brown"), "should find");

    TEST("str_contains_i case insensitive");
    ASSERT(str_contains_i("Hello World", "hello"), "should match case-insensitive");

    TEST("str_duplicate");
    { char *d = str_duplicate("test"); ASSERT(d && strcmp(d, "test") == 0, "mismatch"); free(d); }
}

/* ── URL Utils ───────────────────────────────────────────────── */

static void test_url_utils(void) {
    printf("\n=== URL Utils ===\n");

    TEST("url_extract_domain simple");
    { char d[256]; url_extract_domain("https://www.example.com/page", d, sizeof(d));
      ASSERT(strcmp(d, "example.com") == 0, d); }

    TEST("url_extract_domain no www");
    { char d[256]; url_extract_domain("https://api.example.com/v1", d, sizeof(d));
      ASSERT(strcmp(d, "api.example.com") == 0, d); }

    TEST("url_strip_www");
    { char d[] = "www.test.com"; url_strip_www(d); ASSERT(strcmp(d, "test.com") == 0, d); }

    TEST("url_strip_www no prefix");
    { char d[] = "test.com"; url_strip_www(d); ASSERT(strcmp(d, "test.com") == 0, d); }

    TEST("url_is_valid http");
    ASSERT(url_is_valid("http://example.com"), "should be valid");

    TEST("url_is_valid https");
    ASSERT(url_is_valid("https://example.com"), "should be valid");

    TEST("url_is_valid invalid");
    ASSERT(!url_is_valid("ftp://example.com"), "should be invalid");

    TEST("url_strip_tracking_params");
    { char c[2048];
      url_strip_tracking_params("https://example.com/page?q=test&utm_source=google&fbclid=abc", c, sizeof(c));
      ASSERT(str_contains(c, "q=test"), "should keep q");
      ASSERT(!str_contains(c, "utm_source"), "should strip utm");
      ASSERT(!str_contains(c, "fbclid"), "should strip fbclid"); }
}

/* ── Domain Filter ───────────────────────────────────────────── */

static void test_filter(void) {
    printf("\n=== Domain Filter ===\n");

    Blocklist bl;
    int rc = filter_load_blocklist(&bl, "data/blocklist.txt");
    TEST("blocklist loaded");
    ASSERT(rc == 0 && bl.count > 0, "failed to load");

    TEST("wikipedia blocked");
    ASSERT(filter_is_blocked(&bl, "https://en.wikipedia.org/wiki/Test"), "should block");

    TEST("amazon blocked");
    ASSERT(filter_is_blocked(&bl, "https://www.amazon.com/dp/B001"), "should block");

    TEST("reddit blocked");
    ASSERT(filter_is_blocked(&bl, "https://www.reddit.com/r/test"), "should block");

    TEST("youtube blocked");
    ASSERT(filter_is_blocked(&bl, "https://www.youtube.com/watch?v=abc"), "should block");

    TEST("random site passes");
    ASSERT(!filter_is_blocked(&bl, "https://www.mybusiness.com/article"), "should pass");

    TEST(".gov blocked");
    ASSERT(filter_is_gov("https://www.whitehouse.gov/policy"), "should block gov");

    TEST("ad tracker blocked");
    ASSERT(filter_is_ad_tracker("https://www.googleadservices.com/pagead"), "should block");

    TEST("normal site not ad");
    ASSERT(!filter_is_ad_tracker("https://www.example.com"), "should not block");
}

/* ── TF-IDF ──────────────────────────────────────────────────── */

static void test_tfidf(void) {
    printf("\n=== TF-IDF ===\n");

    TfidfEngine eng;
    tfidf_init(&eng);

    tfidf_add_document(&eng, "running shoes are great for marathon training and daily running exercise fitness health");
    tfidf_add_document(&eng, "the best running shoes for beginners include nike adidas brooks stability cushioning arch support");
    tfidf_add_document(&eng, "trail running shoes differ from road running shoes in traction durability waterproof grip outsole");
    tfidf_add_document(&eng, "compare running shoe brands nike adidas brooks new balance asics for performance comfort price value");
    tfidf_add_document(&eng, "marathon training plan requires proper running shoes nutrition hydration rest recovery stretching warmup");
    tfidf_compute(&eng);

    TEST("vocab built");
    ASSERT(eng.vocab_count > 5, "too few words");

    TEST("doc count correct");
    ASSERT(eng.doc_count == 5, "should be 5");

    NLPKeyword keywords[20];
    int count = tfidf_top_keywords(&eng, keywords, 20);
    TEST("keywords extracted");
    ASSERT(count > 0, "no keywords");

    TEST("discriminative keyword found (not in all docs)");
    { int found = 0;
      for (int i = 0; i < count; i++)
          if (str_contains(keywords[i].text, "nike") || str_contains(keywords[i].text, "trail")
              || str_contains(keywords[i].text, "marathon") || str_contains(keywords[i].text, "stability"))
              found = 1;
      ASSERT(found, "no discriminative keywords found"); }

    tfidf_free(&eng);
    TEST("cleanup ok");
    PASS();
}

/* ── Intent Classification ───────────────────────────────────── */

static void test_intent(void) {
    printf("\n=== Intent Classification ===\n");

    NLPIntent intent;
    const char *texts[] = {
        "best running shoes for flat feet reviewed and compared",
        "top 10 running shoes 2024 comparison guide",
    };

    nlp_classify_intent("best running shoes", texts, 2, &intent, NULL, NULL);
    TEST("commercial intent detected");
    ASSERT(intent.primary_intent == INTENT_COMMERCIAL, "should be commercial");

    TEST("confidence > 0");
    ASSERT(intent.confidence[INTENT_COMMERCIAL] > 0, "should have confidence");

    TEST("content type detected");
    ASSERT(intent.content_type[0] != '\0', "should have content type");

    TEST("tone detected");
    ASSERT(intent.tone[0] != '\0', "should have tone");

    const char *info_texts[] = {
        "what is marathon training and how to prepare",
        "guide to understanding different types of running shoes",
    };
    nlp_classify_intent("how to train for marathon", info_texts, 2, &intent, NULL, NULL);
    TEST("informational intent detected");
    ASSERT(intent.primary_intent == INTENT_INFORMATIONAL, "should be informational");
}

/* ── Prompt Generation ───────────────────────────────────────── */

static void test_prompt(void) {
    printf("\n=== Prompt Generation ===\n");

    NLPResult nlp;
    memset(&nlp, 0, sizeof(nlp));
    snprintf(nlp.keyword, sizeof(nlp.keyword), "best running shoes");
    nlp.intent.primary_intent = INTENT_COMMERCIAL;
    nlp.intent.confidence[INTENT_COMMERCIAL] = 0.8f;
    snprintf(nlp.intent.content_type, sizeof(nlp.intent.content_type), "listicle");
    snprintf(nlp.intent.tone, sizeof(nlp.intent.tone), "conversational");
    nlp.intent.recommended_words = 1500;
    nlp.intent.recommended_headings = 8;
    nlp.stats.avg_word_count = 1200;
    nlp.stats.min_word_count = 800;
    nlp.stats.max_word_count = 2000;

    snprintf(nlp.keywords[0].text, 256, "running shoes");
    nlp.keywords[0].score = 0.5f;
    nlp.keywords[0].frequency = 3;
    nlp.keyword_count = 1;

    char buf[PROMPT_MAX_LEN];

    TEST("full prompt generates");
    { int len = prompt_build_full(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 100, "too short"); }

    TEST("full prompt contains keyword");
    ASSERT(str_contains(buf, "best running shoes"), "keyword missing");

    TEST("keywords only generates");
    { int len = prompt_build_keywords(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 50, "too short"); }

    TEST("outline generates");
    { int len = prompt_build_outline(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 50, "too short"); }

    TEST("competitive brief generates");
    { int len = prompt_build_competitive(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 50, "too short"); }

    TEST("json export valid");
    { int len = prompt_export_json(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 100, "too short");
      ASSERT(str_contains(buf, "\"keyword\""), "missing keyword field");
      ASSERT(str_contains(buf, "\"keywords\""), "missing keywords array"); }

    TEST("markdown export valid");
    { int len = prompt_export_markdown(&nlp, buf, PROMPT_MAX_LEN);
      ASSERT(len > 100, "too short");
      ASSERT(str_contains(buf, "# SERP Analysis"), "missing header"); }
}

/* ── JSON Escaping ───────────────────────────────────────────── */

static void test_json_escape(void) {
    printf("\n=== JSON Escaping ===\n");

    /* Test via prompt_export_json with special chars */
    NLPResult nlp;
    memset(&nlp, 0, sizeof(nlp));
    snprintf(nlp.keyword, sizeof(nlp.keyword), "test \"quoted\" keyword");
    nlp.intent.primary_intent = 0;
    snprintf(nlp.intent.content_type, sizeof(nlp.intent.content_type), "article");
    snprintf(nlp.intent.tone, sizeof(nlp.intent.tone), "formal");

    snprintf(nlp.keywords[0].text, 256, "word\twith\ttabs");
    nlp.keywords[0].score = 1.0f;
    nlp.keyword_count = 1;

    char buf[PROMPT_MAX_LEN];
    prompt_export_json(&nlp, buf, PROMPT_MAX_LEN);

    TEST("quotes escaped in keyword");
    ASSERT(str_contains(buf, "\\\"quoted\\\""), "quotes not escaped");

    TEST("tabs escaped in keywords");
    ASSERT(str_contains(buf, "\\t"), "tabs not escaped");

    TEST("json doesn't contain raw control chars");
    { int has_raw_ctrl = 0;
      for (int i = 0; buf[i]; i++)
          if (buf[i] > 0 && buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r') has_raw_ctrl = 1;
      ASSERT(!has_raw_ctrl, "raw control chars found"); }
}

/* ── Database Round-trip ─────────────────────────────────────── */

static void test_database(void) {
    printf("\n=== Database ===\n");

    Database db;
    int rc = db_open(&db, "data/test_smoke.db");
    TEST("db open");
    ASSERT(rc == 0, "failed to open");

    TEST("save setting");
    ASSERT(db_save_setting(&db, "test_key", "test_value") == 0, "save failed");

    TEST("load setting");
    { char val[256];
      ASSERT(db_load_setting(&db, "test_key", val, sizeof(val)) == 0, "load failed");
      ASSERT(strcmp(val, "test_value") == 0, val); }

    TEST("save session");
    ASSERT(db_save_session(&db, "test_keyword", "{\"test\":true}") == 0, "save failed");

    TEST("load session");
    { char *json = NULL;
      ASSERT(db_load_session(&db, "test_keyword", &json) == 0, "load failed");
      ASSERT(json && str_contains(json, "test"), "wrong data");
      free(json); }

    TEST("list sessions");
    { char sessions[10][256]; int count = db_list_sessions(&db, sessions, 10);
      ASSERT(count >= 1, "no sessions"); }

    TEST("delete session");
    ASSERT(db_delete_session(&db, "test_keyword") == 0, "delete failed");

    TEST("session gone after delete");
    { char *json = NULL;
      ASSERT(db_load_session(&db, "test_keyword", &json) != 0, "should not find deleted");
      free(json); }

    db_close(&db);
    remove("data/test_smoke.db");
    TEST("cleanup ok");
    PASS();
}

/* ── NLP Session Restore ─────────────────────────────────────── */

static void test_nlp_restore(void) {
    printf("\n=== NLP Session Restore ===\n");

    /* Create a result, export to JSON, parse back, compare */
    NLPResult orig;
    memset(&orig, 0, sizeof(orig));
    snprintf(orig.keyword, sizeof(orig.keyword), "test keyword");
    orig.intent.primary_intent = INTENT_COMMERCIAL;
    orig.intent.confidence[INTENT_COMMERCIAL] = 0.75f;
    snprintf(orig.intent.content_type, sizeof(orig.intent.content_type), "listicle");
    snprintf(orig.intent.tone, sizeof(orig.intent.tone), "conversational");
    orig.intent.recommended_words = 1500;
    orig.stats.avg_word_count = 1200;
    orig.stats.min_word_count = 800;
    orig.stats.max_word_count = 2000;
    orig.stats.avg_h2_count = 5.5f;
    snprintf(orig.keywords[0].text, 256, "test");
    orig.keywords[0].score = 0.9f;
    orig.keywords[0].frequency = 3;
    orig.keyword_count = 1;
    snprintf(orig.paa[0], 512, "What is test?");
    orig.paa_count = 1;
    snprintf(orig.related[0], 256, "test related");
    orig.related_count = 1;

    char json[PROMPT_MAX_LEN];
    prompt_export_json(&orig, json, PROMPT_MAX_LEN);

    NLPResult restored;
    int rc = nlp_from_json(json, &restored);

    TEST("json parse succeeds");
    ASSERT(rc == 0, "parse failed");

    TEST("keyword restored");
    ASSERT(strcmp(restored.keyword, "test keyword") == 0, restored.keyword);

    TEST("intent restored");
    ASSERT(restored.intent.primary_intent == INTENT_COMMERCIAL, "wrong intent");

    TEST("keywords restored");
    ASSERT(restored.keyword_count == 1, "wrong count");
    ASSERT(strcmp(restored.keywords[0].text, "test") == 0, "wrong keyword text");

    TEST("stats restored");
    ASSERT(restored.stats.min_word_count == 800, "wrong min words");

    TEST("paa restored");
    ASSERT(restored.paa_count == 1, "wrong paa count");
    ASSERT(str_contains(restored.paa[0], "What is test"), "wrong paa text");

    TEST("related restored");
    ASSERT(restored.related_count == 1, "wrong related count");
}

/* ── Thread Pool ─────────────────────────────────────────────── */

static volatile long g_tp_counter = 0;

static void test_tp_work(void *arg) {
    (void)arg;
    InterlockedIncrement(&g_tp_counter);
    Sleep(1);
}

static void test_threadpool(void) {
    printf("\n=== Thread Pool ===\n");

    ThreadPool tp;
    int rc = tp_create(&tp, 4, 0);
    TEST("pool created");
    ASSERT(rc == 0, "create failed");

    g_tp_counter = 0;
    for (int i = 0; i < 100; i++)
        tp_submit(&tp, test_tp_work, NULL);

    tp_wait(&tp);
    TEST("100 jobs completed");
    ASSERT(g_tp_counter == 100, "wrong count");

    TEST("stats correct");
    ASSERT(tp.total_submitted == 100, "wrong submitted");
    ASSERT(tp.total_completed == 100, "wrong completed");

    tp_destroy(&tp);
    TEST("pool destroyed");
    PASS();
}

/* ── Content Gap Analysis ────────────────────────────────────── */

static void test_content_gap(void) {
    printf("\n=== Content Gap Analysis ===\n");

    NLPResult nlp;
    memset(&nlp, 0, sizeof(nlp));
    snprintf(nlp.keyword, sizeof(nlp.keyword), "running shoes");
    snprintf(nlp.keywords[0].text, 256, "stability"); nlp.keywords[0].score = 0.5f; nlp.keywords[0].frequency = 3;
    snprintf(nlp.keywords[1].text, 256, "cushioning"); nlp.keywords[1].score = 0.4f; nlp.keywords[1].frequency = 2;
    snprintf(nlp.keywords[2].text, 256, "pronation"); nlp.keywords[2].score = 0.3f; nlp.keywords[2].frequency = 2;
    nlp.keyword_count = 3;
    snprintf(nlp.headings[0].text, 512, "Best Running Shoes"); nlp.headings[0].count = 3;
    snprintf(nlp.headings[0].tag, 8, "h2");
    nlp.heading_count = 1;
    nlp.stats.avg_word_count = 1500;

    /* Article that covers "stability" but not "cushioning" or "pronation" */
    nlp_content_gap(&nlp, "These stability running shoes are great for daily use and marathon training.");

    TEST("gap computed flag set");
    ASSERT(nlp.gap_computed == 1, "flag not set");

    TEST("keyword coverage < 100%");
    ASSERT(nlp.gap.keyword_coverage < 100.0f, "should have gaps");

    TEST("missing keywords found");
    ASSERT(nlp.gap.missing_keyword_count > 0, "should find missing");

    TEST("cushioning is missing");
    { int found = 0;
      for (int i = 0; i < nlp.gap.missing_keyword_count; i++)
          if (str_contains(nlp.gap.missing_keywords[i].text, "cushioning")) found = 1;
      ASSERT(found, "cushioning should be missing"); }

    TEST("word count ratio computed");
    ASSERT(nlp.gap.word_count_ratio > 0 && nlp.gap.word_count_ratio < 1.0f, "ratio wrong");

    char report[PROMPT_MAX_LEN];
    prompt_build_gap_report(&nlp, report, PROMPT_MAX_LEN);
    TEST("gap report generated");
    ASSERT(strlen(report) > 50, "report too short");
    ASSERT(str_contains(report, "MISSING"), "should say MISSING");
}

/* ── Main ────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    log_init();
    curl_global_init(CURL_GLOBAL_ALL);
    srand((unsigned int)42); /* deterministic for tests */

    printf("========================================\n");
    printf("  SERP to Prompt Writer - Smoke Tests\n");
    printf("========================================\n");

    test_string_utils();
    test_url_utils();
    test_filter();
    test_tfidf();
    test_intent();
    test_prompt();
    test_json_escape();
    test_database();
    test_nlp_restore();
    test_threadpool();
    test_content_gap();

    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", g_pass, g_fail);
    printf("========================================\n");

    curl_global_cleanup();
    log_shutdown();

    return g_fail > 0 ? 1 : 0;
}
