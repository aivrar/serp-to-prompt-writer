#ifndef NLP_H
#define NLP_H

/* ── Pure C NLP: TF-IDF, n-grams, keyword extraction, intent ── */

typedef struct OnnxNLP OnnxNLP;      /* forward decl */
typedef struct NLIContext NLIContext; /* forward decl */

#define NLP_MAX_KEYWORDS    2000
#define NLP_MAX_ENTITIES    200
#define NLP_MAX_HEADINGS    100
#define NLP_MAX_VOCAB       50000
#define NLP_MAX_NGRAMS      10000

/* ── Keyword result ──────────────────────────────────────────── */
typedef struct {
    char    text[256];
    char    source[16];     /* "tfidf", "ngram", "onnx" */
    float   score;          /* final combined score (used for sorting & thresholds) */
    int     frequency;      /* how many documents contain it */
    /* Score breakdown (three-signal scoring system) */
    float   score_cfr;      /* corpus frequency ratio (wiki rank) */
    float   score_sem;      /* BERT semantic similarity to query */
    float   score_tfidf;    /* normalized TF-IDF component */
    float   google_boost;   /* PAA/related search boost */
} NLPKeyword;

/* ── Entity result (from ONNX NER) ───────────────────────────── */
typedef struct {
    char    text[128];
    char    label[16];      /* PER, ORG, LOC, MISC, PRODUCT, etc. */
    int     frequency;
    int     source_count;   /* how many pages mention it */
} NLPEntity;

/* ── Heading pattern (across SERP pages) ─────────────────────── */
typedef struct {
    char    text[512];
    char    tag[8];         /* h2, h3 */
    int     count;          /* how many pages have this heading */
} NLPHeadingPattern;

/* ── Content statistics ──────────────────────────────────────── */
typedef struct {
    float   avg_word_count;
    float   median_word_count;
    int     min_word_count;
    int     max_word_count;
    float   avg_h2_count;
    float   avg_h3_count;
    float   avg_heading_count;
    float   avg_outbound_links;
    float   avg_internal_links;
} NLPContentStats;

/* ── Intent classification ───────────────────────────────────── */
#define INTENT_INFORMATIONAL  0
#define INTENT_COMMERCIAL     1
#define INTENT_TRANSACTIONAL  2
#define INTENT_NAVIGATIONAL   3
#define INTENT_COUNT          4

#define MAX_INTENT_SIGNALS    20

typedef struct {
    int     primary_intent;     /* INTENT_* */
    float   scores[INTENT_COUNT];       /* raw score per intent */
    float   confidence[INTENT_COUNT];   /* normalized 0-1 per intent */
    /* Signal words that triggered each intent */
    char    signals[INTENT_COUNT][MAX_INTENT_SIGNALS][32];
    int     signal_count[INTENT_COUNT];
    char    content_type[32];
    char    tone[32];
    int     recommended_words;
    int     recommended_headings;
} NLPIntent;

/* ── Content Gap Analysis ────────────────────────────────────── */
#define GAP_MAX_MISSING     100

typedef struct {
    /* Missing from user's content vs SERP leaders */
    NLPKeyword  missing_keywords[GAP_MAX_MISSING];
    int         missing_keyword_count;
    NLPHeadingPattern missing_headings[GAP_MAX_MISSING];
    int         missing_heading_count;
    /* Coverage scores */
    float       keyword_coverage;   /* 0-100%: how many SERP keywords user covers */
    float       heading_coverage;   /* 0-100%: how many heading patterns user covers */
    float       word_count_ratio;   /* user words / avg competitor words */
    int         user_word_count;
    /* Recommendations */
    int         words_needed;       /* additional words to add */
    int         headings_needed;    /* additional headings to add */
} NLPContentGap;

/* ── Complete NLP result ─────────────────────────────────────── */
typedef struct {
    char    keyword[256];

    NLPKeyword  keywords[NLP_MAX_KEYWORDS];
    int         keyword_count;

    NLPEntity   entities[NLP_MAX_ENTITIES];
    int         entity_count;

    NLPHeadingPattern headings[NLP_MAX_HEADINGS];
    int         heading_count;

    NLPContentStats stats;
    NLPIntent   intent;

    char        paa[10][512];           /* People Also Ask - questions */
    char        paa_snippets[10][1024]; /* PAA answer snippets */
    char        paa_links[10][2048];    /* PAA source URLs */
    int         paa_count;
    char        related[20][256];   /* Related searches */
    int         related_count;

    NLPContentGap gap;              /* content gap analysis (filled by nlp_content_gap) */
    int           gap_computed;     /* 1 if gap analysis was run */

    double      analysis_time;
} NLPResult;

/* ── TF-IDF engine (pure C) ─────────────────────────────────── */

typedef struct {
    char    word[64];
    int     df;             /* document frequency (how many pages contain it) */
    int     tf;             /* total term frequency (total occurrences across all pages) */
    float   idf;            /* inverse doc frequency */
} VocabEntry;

typedef struct {
    VocabEntry *vocab;
    int         vocab_count;
    int         vocab_cap;
    int         doc_count;
} TfidfEngine;

void tfidf_init(TfidfEngine *eng);
void tfidf_free(TfidfEngine *eng);
void tfidf_add_document(TfidfEngine *eng, const char *text);
void tfidf_compute(TfidfEngine *eng);
int  tfidf_top_keywords(TfidfEngine *eng, NLPKeyword *out, int max_count);

/* ── N-gram extraction (pure C) ──────────────────────────────── */
int  nlp_extract_ngrams(const char **texts, int text_count,
                        NLPKeyword *out, int max_count);

/* ── Heading analysis ────────────────────────────────────────── */
int  nlp_analyze_headings(const char **heading_texts, const char **heading_tags,
                          int heading_count,
                          NLPHeadingPattern *out, int max_count);

/* ── Content stats ───────────────────────────────────────────── */
void nlp_compute_stats(const int *word_counts, const int *heading_counts,
                       const int *h2_counts, const int *h3_counts,
                       const int *outbound_counts, const int *internal_counts,
                       int page_count, NLPContentStats *stats);

/* ── Intent classification (heuristic) ───────────────────────── */
void nlp_classify_intent(const char *keyword, const char **texts, int text_count,
                         NLPIntent *intent, OnnxNLP *onnx, NLIContext *nli);

/* ── Content gap analysis ────────────────────────────────────── */
void nlp_content_gap(NLPResult *result, const char *user_content);

/* ── Session restore — parse JSON back into NLPResult ────────── */
int  nlp_from_json(const char *json, NLPResult *result);

/* ── Wikipedia frequency table ───────────────────────────────── */
int  wiki_freq_load(const char *path);
int  wiki_freq_rank(const char *word);

/* ── Full analysis orchestrator ──────────────────────────────── */
#include "scraper.h"
int  nlp_analyze(const char *keyword, ScrapedPage *pages, int page_count,
                 NLPResult *result, OnnxNLP *onnx, NLIContext *nli);

/* ── Junk keyword filter ────────────────────────────────────── */
int nlp_is_junk_keyword(const char *text, float score_sem);

/* ── Three-signal keyword rescoring ─────────────────────────── */
void nlp_rescore_keywords(NLPResult *result, OnnxNLP *onnx);

/* ── Per-page keyword extraction (for browsing individual pages) ── */
void nlp_extract_page_keywords(ScrapedPage *page);

#endif
