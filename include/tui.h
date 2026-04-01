#ifndef TUI_H
#define TUI_H

#include "config.h"
#include "database.h"
#include "serper.h"
#include "scraper.h"
#include "filter.h"
#include "engine.h"
#include "resmon.h"
#include "proxy.h"
#include "nlp.h"
#include "prompt.h"
#include "app_log.h"

typedef enum {
    TAB_SEARCH = 0,
    TAB_RESULTS,
    TAB_ANALYSIS,
    TAB_SETTINGS,
    TAB_LOG,
    TAB_COUNT
} TabType;

typedef struct {
    Config          *config;
    Database        *db;
    Blocklist       *blocklist;
    ResourceMonitor *resmon;
    ProxyPool       *proxy;
    SearchEngine    *engine;
    OnnxNLP         *onnx;

    TabType          current_tab;
    int              running;

    /* Search tab state */
    char             keyword[256];
    int              num_results;
    int              pages_depth;
    int              link_depth;
    int              use_proxies;

    /* Status */
    char             status_msg[512];
    int              status_color;      /* 0=green, 1=yellow, 2=red, 3=cyan */

    /* Current prompt options */
    PromptOptions    prompt_opts;
} AppState;

void tui_init(void);
void tui_shutdown(void);
void tui_run(AppState *state);

#endif
