#ifndef FILTER_H
#define FILTER_H

#define MAX_BLOCKLIST 1024

typedef struct {
    char domains[MAX_BLOCKLIST][256];
    int  count;
} Blocklist;

int  filter_load_blocklist(Blocklist *bl, const char *path);
int  filter_save_blocklist(const Blocklist *bl, const char *path);
int  filter_is_blocked(const Blocklist *bl, const char *url);
int  filter_is_gov(const char *url);
int  filter_is_ad_tracker(const char *url);
void filter_add_domain(Blocklist *bl, const char *domain);
void filter_remove_domain(Blocklist *bl, const char *domain);


#endif
