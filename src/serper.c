#include "serper.h"
#include "utils.h"
#include "app_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define SERPER_URL "https://google.serper.dev/search"

int serper_search(const char *keyword, int num, int page,
                  const char *api_key, const char *proxy_url,
                  SerpResponse *response) {
    memset(response, 0, sizeof(SerpResponse));
    snprintf(response->keyword, sizeof(response->keyword), "%s", keyword);
    response->page = page;

    if (!api_key || !api_key[0]) {
        snprintf(response->error, sizeof(response->error), "No API key");
        return -1;
    }

    /* Build JSON payload */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "q", keyword);
    cJSON_AddNumberToObject(root, "num", num);
    cJSON_AddStringToObject(root, "gl", "us");
    cJSON_AddStringToObject(root, "hl", "en");
    if (page > 1) cJSON_AddNumberToObject(root, "page", page);

    char *json_body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_body) {
        snprintf(response->error, sizeof(response->error), "JSON build failed");
        return -1;
    }

    /* Execute request */
    HttpBuffer buf;
    int rc;
    if (proxy_url && proxy_url[0])
        rc = http_post_json_proxy(SERPER_URL, json_body, api_key, proxy_url, &buf);
    else
        rc = http_post_json(SERPER_URL, json_body, api_key, &buf);
    free(json_body);

    if (rc != 0) {
        snprintf(response->error, sizeof(response->error), "HTTP request failed");
        return -1;
    }

    /* Parse JSON response */
    cJSON *resp = cJSON_Parse(buf.data);
    http_buffer_free(&buf);
    if (!resp) {
        snprintf(response->error, sizeof(response->error), "JSON parse failed");
        return -1;
    }

    /* Organic results */
    cJSON *organic = cJSON_GetObjectItem(resp, "organic");
    if (organic && cJSON_IsArray(organic)) {
        int count = cJSON_GetArraySize(organic);
        for (int i = 0; i < count && response->organic_count < MAX_SERP_RESULTS; i++) {
            cJSON *item = cJSON_GetArrayItem(organic, i);
            SerpResult *r = &response->organic[response->organic_count];

            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "position")) && cJSON_IsNumber(j))
                r->position = j->valueint;
            else
                r->position = i + 1;
            if ((j = cJSON_GetObjectItem(item, "title")) && cJSON_IsString(j))
                snprintf(r->title, sizeof(r->title), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "link")) && cJSON_IsString(j))
                snprintf(r->link, sizeof(r->link), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "snippet")) && cJSON_IsString(j))
                snprintf(r->snippet, sizeof(r->snippet), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "date")) && cJSON_IsString(j))
                snprintf(r->date, sizeof(r->date), "%s", j->valuestring);

            url_extract_domain(r->link, r->domain, sizeof(r->domain));
            response->organic_count++;
        }
    }

    /* People Also Ask */
    cJSON *paa = cJSON_GetObjectItem(resp, "peopleAlsoAsk");
    if (paa && cJSON_IsArray(paa)) {
        int count = cJSON_GetArraySize(paa);
        for (int i = 0; i < count && response->paa_count < MAX_PAA; i++) {
            cJSON *item = cJSON_GetArrayItem(paa, i);
            PeopleAlsoAsk *p = &response->paa[response->paa_count];
            cJSON *j;
            if ((j = cJSON_GetObjectItem(item, "question")) && cJSON_IsString(j))
                snprintf(p->question, sizeof(p->question), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "snippet")) && cJSON_IsString(j))
                snprintf(p->snippet, sizeof(p->snippet), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "link")) && cJSON_IsString(j))
                snprintf(p->link, sizeof(p->link), "%s", j->valuestring);
            response->paa_count++;
        }
    }

    /* Related searches */
    cJSON *related = cJSON_GetObjectItem(resp, "relatedSearches");
    if (related && cJSON_IsArray(related)) {
        int count = cJSON_GetArraySize(related);
        for (int i = 0; i < count && response->related_count < MAX_RELATED; i++) {
            cJSON *item = cJSON_GetArrayItem(related, i);
            cJSON *j = cJSON_GetObjectItem(item, "query");
            if (j && cJSON_IsString(j))
                snprintf(response->related[response->related_count], 256, "%s", j->valuestring);
            response->related_count++;
        }
    }

    /* Knowledge graph */
    cJSON *kg = cJSON_GetObjectItem(resp, "knowledgeGraph");
    if (kg && cJSON_IsObject(kg)) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(kg, "title")) && cJSON_IsString(j))
            snprintf(response->kg_title, sizeof(response->kg_title), "%s", j->valuestring);
        if ((j = cJSON_GetObjectItem(kg, "type")) && cJSON_IsString(j))
            snprintf(response->kg_type, sizeof(response->kg_type), "%s", j->valuestring);
        if ((j = cJSON_GetObjectItem(kg, "description")) && cJSON_IsString(j))
            snprintf(response->kg_description, sizeof(response->kg_description), "%s", j->valuestring);
    }

    cJSON_Delete(resp);

    app_log(LOG_INFO, "Serper: '%s' page %d -- %d results, %d PAA, %d related",
            keyword, page, response->organic_count, response->paa_count, response->related_count);
    return 0;
}

int serper_search_pages(const char *keyword, int num, int pages,
                        const char *api_key, const char *proxy_url,
                        SerpResponse *responses, int max_responses,
                        volatile int *stop_flag) {
    int fetched = 0;
    for (int p = 1; p <= pages && fetched < max_responses; p++) {
        if (stop_flag && *stop_flag) break;

        int rc = serper_search(keyword, num, p, api_key, proxy_url,
                               &responses[fetched]);
        fetched++;

        if (rc != 0 || responses[fetched-1].error[0]) break;
        if (responses[fetched-1].organic_count == 0) break;

        /* Small delay between pages */
        if (p < pages) Sleep(500);
    }
    return fetched;
}

int serper_check_credits(const char *api_key, const char *proxy_url) {
    if (!api_key || !api_key[0]) return -1;

    HttpBuffer buf;
    /* Serper account endpoint */
    int rc;
    if (proxy_url && proxy_url[0])
        rc = http_post_json_proxy("https://google.serper.dev/account", "{}", api_key, proxy_url, &buf);
    else
        rc = http_post_json("https://google.serper.dev/account", "{}", api_key, &buf);
    if (rc != 0) {
        app_log(LOG_WARN, "Serper credit check failed: HTTP error");
        return -1;
    }

    /* Log raw response for debugging */
    app_log(LOG_DEBUG, "Serper account response: %.500s", buf.data ? buf.data : "(null)");

    cJSON *resp = cJSON_Parse(buf.data);
    http_buffer_free(&buf);
    if (!resp) {
        app_log(LOG_WARN, "Serper credit check: JSON parse failed");
        return -1;
    }

    int credits = -1;
    cJSON *j;
    /* Serper returns {"balance":N,"rateLimit":N} */
    if ((j = cJSON_GetObjectItem(resp, "balance")) && cJSON_IsNumber(j)) credits = j->valueint;
    if (credits < 0 && (j = cJSON_GetObjectItem(resp, "credits")) && cJSON_IsNumber(j)) credits = j->valueint;

    cJSON_Delete(resp);

    if (credits >= 0)
        app_log(LOG_INFO, "Serper credits: %d remaining", credits);
    else
        app_log(LOG_WARN, "Serper credit check: no recognized credit field in response");

    return credits;
}
