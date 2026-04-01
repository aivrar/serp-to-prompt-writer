#include "filter.h"
#include "utils.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Ad/tracker domains (always filtered) */
static const char *ad_trackers[] = {
    "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "google-analytics.com", "googletagmanager.com",
    "facebook.net", "fbcdn.net", "hotjar.com", "segment.com",
    "mixpanel.com", "optimizely.com", "taboola.com", "outbrain.com",
    "criteo.com", "moatads.com", "amazon-adsystem.com",
    /* Affiliate networks */
    "anrdoezrs.net", "jdoqocy.com", "dpbolvw.net", "tkqlhce.com",
    "awin1.com", "shareasale.com", "pntra.com", "go.skimresources.com",
    "commission-junction.com", "cj.com", "impact.com", "partnerize.com",
    /* URL shorteners (#23) */
    "amzn.to", "bit.ly", "tinyurl.com", "t.co", "goo.gl", "ow.ly",
    "buff.ly", "is.gd", "v.gd", "shorte.st", "cutt.ly", "rb.gy",
    /* Non-content domains (OBL junk from logs) */
    "onetrust.com", "privacyportal.onetrust.com",
    "attnl.tv", "attentive.com",
    "strava.com", "threads.com", "apps.apple.com", "play.google.com",
    "comenity.net", "levelaccess.com", "essentialaccessibility.com",
    "termly.io", "cookiebot.com", "trustarc.com",
    "amazon.jobs", "jobs.lever.co", "greenhouse.io",
    "zappos.app.link",
    /* Cloud storage / CDN (not content pages) */
    "amazonaws.com", "cloudfront.net",
    "blob.core.windows.net", "storage.googleapis.com",
    "wixstatic.com", "usrfiles.com", "wixmp.com",
    "squarespace-cdn.com", "shopifycdn.com",
    NULL
};

int filter_load_blocklist(Blocklist *bl, const char *path) {
    memset(bl, 0, sizeof(Blocklist));
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof(line), f) && bl->count < MAX_BLOCKLIST) {
        str_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        str_to_lower(line);
        url_strip_www(line);
        snprintf(bl->domains[bl->count], 256, "%s", line);
        bl->count++;
    }
    fclose(f);
    app_log(LOG_INFO, "Blocklist: loaded %d domains from %s", bl->count, path);
    return 0;
}

/* Internal helpers that accept an already-extracted lowercase domain */
static int filter_is_gov_domain(const char *domain) {
    return str_ends_with(domain, ".gov") || str_contains(domain, ".gov.");
}

static int filter_is_ad_tracker_domain(const char *domain) {
    size_t dlen = strlen(domain);
    for (int i = 0; ad_trackers[i]; i++) {
        if (strcmp(domain, ad_trackers[i]) == 0) return 1;
        size_t alen = strlen(ad_trackers[i]);
        if (dlen > alen + 1) {
            const char *suffix = domain + dlen - alen;
            if (*(suffix - 1) == '.' && strcmp(suffix, ad_trackers[i]) == 0)
                return 1;
        }
    }
    return 0;
}

int filter_is_blocked(const Blocklist *bl, const char *url) {
    char domain[256];
    url_extract_domain(url, domain, sizeof(domain));
    str_to_lower(domain);

    /* Check TLD */
    if (filter_is_gov_domain(domain)) return 1;

    /* Check ad/tracker */
    if (filter_is_ad_tracker_domain(domain)) return 1;

    /* Check blocklist (exact + subdomain match) */
    size_t dlen = strlen(domain);
    for (int i = 0; i < bl->count; i++) {
        if (strcmp(domain, bl->domains[i]) == 0) return 1;
        /* Subdomain match: "en.wikipedia.org" matches "wikipedia.org" */
        size_t blen = strlen(bl->domains[i]);
        if (dlen > blen + 1) {
            const char *suffix = domain + dlen - blen;
            if (*(suffix - 1) == '.' && strcmp(suffix, bl->domains[i]) == 0)
                return 1;
        }
    }
    return 0;
}

/* Public wrappers: extract domain then delegate to internal helpers */
int filter_is_gov(const char *url) {
    char domain[256];
    url_extract_domain(url, domain, sizeof(domain));
    str_to_lower(domain);
    return filter_is_gov_domain(domain);
}

int filter_is_ad_tracker(const char *url) {
    char domain[256];
    url_extract_domain(url, domain, sizeof(domain));
    str_to_lower(domain);
    return filter_is_ad_tracker_domain(domain);
}

void filter_add_domain(Blocklist *bl, const char *domain) {
    if (bl->count >= MAX_BLOCKLIST) return;
    char lower[256];
    snprintf(lower, sizeof(lower), "%s", domain);
    str_to_lower(lower);
    url_strip_www(lower);
    /* Dedup */
    for (int i = 0; i < bl->count; i++)
        if (strcmp(bl->domains[i], lower) == 0) return;
    snprintf(bl->domains[bl->count], 256, "%s", lower);
    bl->count++;
}

void filter_remove_domain(Blocklist *bl, const char *domain) {
    char lower[256];
    snprintf(lower, sizeof(lower), "%s", domain);
    str_to_lower(lower);
    for (int i = 0; i < bl->count; i++) {
        if (strcmp(bl->domains[i], lower) == 0) {
            for (int j = i; j < bl->count - 1; j++)
                memcpy(bl->domains[j], bl->domains[j+1], 256);
            bl->count--;
            return;
        }
    }
}

int filter_save_blocklist(const Blocklist *bl, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { app_log(LOG_ERROR, "Blocklist: failed to write %s", path); return -1; }
    for (int i = 0; i < bl->count; i++)
        fprintf(f, "%s\n", bl->domains[i]);
    fclose(f);
    return 0;
}

