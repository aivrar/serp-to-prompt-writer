#ifndef JS_RENDER_H
#define JS_RENDER_H

/*
 * Render a JS-heavy page using headless Edge/Chrome via Chrome DevTools Protocol.
 * Uses WinHTTP WebSocket (built into Windows 10+) -- no external dependencies.
 *
 * Pool is sized dynamically based on CPU cores and RAM.
 * Call js_render_init() once at startup with detected system info.
 */

/* Store system info for pool sizing.  Must be called before first render.
   If not called, defaults to 4 cores / 8192 MB (= 4 browser slots). */
void js_render_init(int cpu_cores, int total_ram_mb);

/* Render a page.  Returns malloc'd HTML string on success, NULL on failure.
   Caller must free() the returned string. */
char *js_render_page(const char *url, int timeout_ms);

/* Signal all waiting threads to give up immediately (for engine cancel/nuke). */
void js_render_cancel(void);

/* Cancel + kill all browser processes + free resources.  Call at app exit. */
void js_render_shutdown(void);

#endif
