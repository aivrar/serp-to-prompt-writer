#include "tui.h"
#include "app_log.h"
#include "utils.h"
#include "scraper.h"
#include "sysinfo.h"
#include "onnx_nlp.h"
#include "js_render.h"
#include "resources.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* Extract an embedded resource to a file if the file doesn't exist.
   Resources are compiled into the exe via resources.rc. */
static int extract_resource(int resource_id, const char *dest_path) {
    if (file_exists(dest_path)) return 0;  /* already exists */
#ifdef _WIN32
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(resource_id), RT_DATAFILE);
    if (!hRes) return -1;
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return -1;
    DWORD size = SizeofResource(NULL, hRes);
    void *data = LockResource(hData);
    if (!data || size == 0) return -1;
    FILE *f = fopen(dest_path, "wb");
    if (!f) return -1;
    fwrite(data, 1, size, f);
    fclose(f);
    app_log(LOG_INFO, "Extracted %s (%lu bytes)", dest_path, (unsigned long)size);
    return 0;
#else
    (void)resource_id; (void)dest_path;
    return -1;
#endif
}

static int app_main(void) {
    /* 1. Set working directory to exe location */
    set_exe_directory();

    /* 2. Init logging */
    log_init();

    /* 3. Create runtime directories */
    ensure_directory("data");
    ensure_directory("output");
    ensure_directory("models");

    /* 4. Init CURL + seed RNG */
    curl_global_init(CURL_GLOBAL_ALL);
    srand((unsigned int)time(NULL));
    app_log(LOG_INFO, "SERP to Prompt Writer starting...");

    /* 4a. First-run: extract embedded data files from exe resources */
    extract_resource(IDR_USER_AGENTS, "data/user_agents.txt");
    extract_resource(IDR_BLOCKLIST,   "data/blocklist.txt");
    extract_resource(IDR_WIKI_FREQ,   "data/wiki_freq.txt");

    /* 4b. Load random user agents */
    http_load_user_agents("data/user_agents.txt");

    /* 5. Load config — create template .env on first run */
    if (!file_exists(".env")) {
        FILE *f = fopen(".env", "w");
        if (f) {
            fputs("# SERP to Prompt Writer — Configuration\n", f);
            fputs("# Add your Serper.dev API key below (required)\n", f);
            fputs("# Get a free key at https://serper.dev\n", f);
            fputs("#\n", f);
            fputs("# SERPER_API=\"your-api-key-here\"\n", f);
            fputs("#\n", f);
            fputs("# Multiple keys for round-robin rotation:\n", f);
            fputs("# SERPER_API_2=\"second-key\"\n", f);
            fputs("# SERPER_API_3=\"third-key\"\n", f);
            fputs("#\n", f);
            fputs("# Optional proxy credentials:\n", f);
            fputs("# PROXY_USER=\"username\"\n", f);
            fputs("# PROXY_PASS=\"password\"\n", f);
            fclose(f);
            app_log(LOG_INFO, "Created template .env — add your Serper API key");
#ifdef _WIN32
            MessageBoxA(NULL,
                "Welcome to SERP to Prompt Writer!\n\n"
                "A template .env file has been created.\n"
                "Add your Serper.dev API key to get started.\n\n"
                "You can also add it via Settings > Connection.",
                "First Run Setup",
                MB_OK | MB_ICONINFORMATION);
#endif
        }
    }
    Config config;
    config_load(&config, ".env");

    /* 6. Apply DB tuning from config */
    const char *v;
    if ((v = config_get(&config, "DB_CACHE_MB")))     g_db_cache_mb = atoi(v);
    if ((v = config_get(&config, "DB_MMAP_MB")))       g_db_mmap_mb = atoi(v);
    if ((v = config_get(&config, "DB_BUSY_TIMEOUT")))  g_db_busy_timeout = atoi(v);

    /* 7. Open database */
    Database db;
    if (db_open(&db, "data/serp_analyzer.db") != 0) {
        app_log(LOG_ERROR, "Failed to open database");
#ifdef _WIN32
        MessageBoxA(NULL, "Failed to open database.\nCheck write permissions.",
                    "SERP to Prompt Writer", MB_OK | MB_ICONERROR);
#endif
        curl_global_cleanup();
        log_shutdown();
        return 1;
    }

    /* 8. Load blocklist */
    Blocklist blocklist;
    if (filter_load_blocklist(&blocklist, "data/blocklist.txt") != 0) {
        blocklist.count = 0;
        app_log(LOG_WARN, "Blocklist not found: data/blocklist.txt");
    }

    /* 9. Detect system info */
    SystemInfo sysinfo;
    sysinfo_detect(&sysinfo);

    const char *warning = sysinfo_capability_warning(&sysinfo);
    if (warning) app_log(LOG_WARN, "System: %s", warning);

    /* 9b. Init CDP browser pool (sized from detected hardware) */
    js_render_init(sysinfo.cpu_cores, sysinfo.total_ram_mb);

    /* 10. Init resource monitor */
    ResourceMonitor resmon;
    resmon_init(&resmon, &sysinfo);

    /* 11. Init proxy pool */
    ProxyPool proxy;
    proxy_init(&proxy);
    proxy_load_file(&proxy, "data/proxies.txt");

    /* Load proxy credentials from config */
    {const char *pu = config_get(&config, "PROXY_USER");
    const char *pp2 = config_get(&config, "PROXY_PASS");
    if (pu && pu[0] && pp2 && pp2[0])
        proxy_set_credentials(&proxy, pu, pp2);}

    /* 12. Init ONNX NLP (optional) */
    OnnxNLP *onnx = NULL;
    onnx_nlp_init(&onnx, "models", sysinfo.gpu_count > 0);

    /* 12b. Init NLI classifier (optional, uses ONNX env from above) */
    NLIContext *nli = NULL;
    nli_init(&nli, "models", onnx);

    /* 13. Init engine */
    SearchEngine engine;
    engine_init(&engine);
    engine.config = &config;
    engine.proxy = &proxy;
    engine.blocklist = &blocklist;
    engine.resmon = &resmon;
    engine.onnx = onnx;
    engine.nli = nli;
    engine.db = &db;

    /* 14. Build app state and run UI */
    AppState state;
    memset(&state, 0, sizeof(state));
    state.config = &config;
    state.db = &db;
    state.blocklist = &blocklist;
    state.resmon = &resmon;
    state.proxy = &proxy;
    state.engine = &engine;
    state.onnx = onnx;
    state.current_tab = TAB_SEARCH;
    state.num_results = 10;
    state.pages_depth = 1;
    state.prompt_opts.template_type = PROMPT_FULL;
    state.prompt_opts.include_entities = 1;
    state.prompt_opts.include_keywords = 1;
    state.prompt_opts.include_outline = 1;
    state.prompt_opts.include_stats = 1;
    state.prompt_opts.include_paa = 1;
    state.prompt_opts.include_related = 1;
    snprintf(state.status_msg, sizeof(state.status_msg),
             "Ready. Enter a keyword and click Analyze.");

    tui_init();
    tui_run(&state);
    tui_shutdown();

    /* 15. Cleanup (reverse order) */
    app_log(LOG_INFO, "Shutting down...");
    engine_shutdown(&engine);
    js_render_shutdown();
    nli_shutdown(nli);
    onnx_nlp_shutdown(onnx);
    proxy_shutdown(&proxy);
    resmon_shutdown(&resmon);
    db_close(&db);
    config_shutdown();
    curl_global_cleanup();
    log_shutdown();

    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance; (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    return app_main();
}
#else
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    return app_main();
}
#endif
