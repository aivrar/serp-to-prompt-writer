#ifndef SCRAPER_H
#define SCRAPER_H

#define MAX_LINKS           500
#define MAX_LINK_TEXT       256
#define MAX_CONTEXT_LEN     512
#define MAX_HEADINGS        200

typedef struct {
    char url[2048];
    char text[MAX_LINK_TEXT];
    char context[MAX_CONTEXT_LEN];
    int  is_outbound;
} ExtractedLink;

typedef struct {
    char tag[8];        /* "h1".."h6" */
    char text[512];
    int  level;         /* 1-6 */
} ExtractedHeading;

typedef struct {
    char page_url[2048];
    char final_url[2048];
    char domain[256];
    char page_title[512];
    char meta_description[1024];
    char meta_keywords[512];
    char *page_text;                    /* heap allocated, caller frees via scraper_free */
    int  word_count;

    ExtractedHeading headings[MAX_HEADINGS];
    int  heading_count;

    ExtractedLink *links;              /* heap allocated, grows on demand */
    int  link_count;
    int  link_cap;                     /* current allocation capacity */
    int  outbound_count;
    int  internal_count;

    int  p_tag_count;                  /* <p> tags in content root -- low count + high words = non-article */
    int  is_non_article;               /* flagged: product grid, coupon listing, etc. */

    int  status_code;
    int  used_js_fallback;
    double scrape_time;
    char error[256];

    /* Per-page keyword extraction (filled by nlp_extract_page_keywords) */
    char page_keywords[30][64];     /* top keywords from this specific page */
    int  page_keyword_count;

    /* Completion tracking for concurrent NLP */
    volatile int scraped;           /* set to 1 by scrape worker when done (success or fail) */

    /* SERP context -- set by scrape_worker from ScrapeJob */
    char keyword[256];              /* search keyword this page belongs to */
    int  serp_position;             /* ranking position in SERP results */

    /* Source tracking (#26) */
    int  is_crawled;                /* 0 = SERP page, 1 = outbound crawled page */
    int  source_page_idx;           /* index of SERP page this was crawled from (-1 for SERP pages) */
} ScrapedPage;

/* Scrape a page (text-first, headless fallback if JS detected) */
int  scraper_fetch_page(const char *url, const char *proxy_url, ScrapedPage *page);
void scraper_free(ScrapedPage *page);

/* Check if page text looks JS-rendered (too short, signals present) */
int  scraper_needs_js(const char *html, int word_count);

#endif
