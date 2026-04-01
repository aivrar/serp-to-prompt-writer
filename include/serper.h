#ifndef SERPER_H
#define SERPER_H

#define MAX_SERP_RESULTS    100
#define MAX_PAA             10
#define MAX_RELATED         20

typedef struct {
    int  position;
    char title[512];
    char link[2048];
    char snippet[1024];
    char domain[256];
    char date[64];
} SerpResult;

typedef struct {
    char question[512];
    char snippet[1024];
    char link[2048];
} PeopleAlsoAsk;

typedef struct {
    char keyword[256];
    int  page;

    SerpResult    organic[MAX_SERP_RESULTS];
    int           organic_count;

    PeopleAlsoAsk paa[MAX_PAA];
    int           paa_count;

    char          related[MAX_RELATED][256];
    int           related_count;

    /* Knowledge graph */
    char          kg_title[256];
    char          kg_type[128];
    char          kg_description[1024];

    double        search_time;
    char          error[256];
} SerpResponse;

/* Search via Serper API. api_key required. proxy_url can be NULL. */
int  serper_search(const char *keyword, int num, int page,
                   const char *api_key, const char *proxy_url,
                   SerpResponse *response);

/* Paginated search -- fills array of SerpResponse. Returns pages fetched. */
int  serper_search_pages(const char *keyword, int num, int pages,
                         const char *api_key, const char *proxy_url,
                         SerpResponse *responses, int max_responses,
                         volatile int *stop_flag);

/* Check account credits. Returns credits remaining, or -1 on error.
   proxy_url can be NULL for no proxy. */
int  serper_check_credits(const char *api_key, const char *proxy_url);

#endif
