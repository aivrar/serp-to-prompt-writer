#include "scraper.h"
#include "js_render.h"
#include "utils.h"
#include "app_log.h"
#include <gumbo.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ── Chrome detection (class/id/role attribute matching) ─────── */

static const char *chrome_skip_patterns[] = {
    "sidebar", "widget", "breadcrumb", "cookie", "banner",
    "advert", "popup", "modal", "cart", "share", "social",
    "comment", "newsletter", "subscribe",
    "gdpr", "consent", "toolbar", "pagination", "pager",
    NULL
};

static const char *chrome_role_patterns[] = {
    "navigation", "banner", "complementary", "contentinfo", "search",
    NULL
};

static int node_has_chrome_class(GumboNode *node) {
    if (node->type != GUMBO_NODE_ELEMENT) return 0;
    GumboAttribute *cls = gumbo_get_attribute(&node->v.element.attributes, "class");
    GumboAttribute *id  = gumbo_get_attribute(&node->v.element.attributes, "id");
    GumboAttribute *role = gumbo_get_attribute(&node->v.element.attributes, "role");

    if (role) {
        for (int i = 0; chrome_role_patterns[i]; i++)
            if (str_contains_i(role->value, chrome_role_patterns[i])) return 1;
    }
    for (int i = 0; chrome_skip_patterns[i]; i++) {
        if (cls && str_contains_i(cls->value, chrome_skip_patterns[i])) return 1;
        if (id && str_contains_i(id->value, chrome_skip_patterns[i])) return 1;
    }
    return 0;
}

/* ── Iterative tree traversal stack ──────────────────────────── */

#define TREE_STACK_INIT 2048

typedef struct {
    GumboNode **nodes;
    int top;
    int cap;
} TreeStack;

static void ts_init(TreeStack *s) {
    s->cap = TREE_STACK_INIT;
    s->top = 0;
    s->nodes = (GumboNode **)malloc(s->cap * sizeof(GumboNode *));
    if (!s->nodes) s->cap = 0;
}

static void ts_push(TreeStack *s, GumboNode *node) {
    if (s->top >= s->cap) {
        int new_cap = s->cap * 2;
        GumboNode **tmp = (GumboNode **)realloc(s->nodes, new_cap * sizeof(GumboNode *));
        if (!tmp) return;  /* keep old buffer, drop this node */
        s->nodes = tmp;
        s->cap = new_cap;
    }
    if (s->nodes) s->nodes[s->top++] = node;
}

static GumboNode *ts_pop(TreeStack *s) {
    return (s->top > 0 && s->nodes) ? s->nodes[--s->top] : NULL;
}

static void ts_free(TreeStack *s) {
    free(s->nodes);
    s->nodes = NULL;
    s->top = s->cap = 0;
}

/* Push children in reverse order so leftmost child is processed first */
static void ts_push_children(TreeStack *s, GumboNode *node) {
    if (node->type != GUMBO_NODE_ELEMENT) return;
    GumboVector *ch = &node->v.element.children;
    for (int i = (int)ch->length - 1; i >= 0; i--)
        ts_push(s, ch->data[i]);
}

static int is_skip_tag(GumboTag tag) {
    return tag == GUMBO_TAG_SCRIPT || tag == GUMBO_TAG_STYLE ||
           tag == GUMBO_TAG_NAV || tag == GUMBO_TAG_FOOTER ||
           tag == GUMBO_TAG_HEADER || tag == GUMBO_TAG_NOSCRIPT ||
           tag == GUMBO_TAG_SVG || tag == GUMBO_TAG_FORM ||
           tag == GUMBO_TAG_BUTTON || tag == GUMBO_TAG_SELECT ||
           tag == GUMBO_TAG_TEXTAREA || tag == GUMBO_TAG_IFRAME ||
           tag == GUMBO_TAG_ASIDE;
}

/* ── Content root finder (iterative) ────────────────────────── */

static int count_articles(GumboNode *root) {
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    int count = 0;
    while (s.top > 0) {
        GumboNode *node = ts_pop(&s);
        if (node->type != GUMBO_NODE_ELEMENT) continue;
        if (node->v.element.tag == GUMBO_TAG_ARTICLE) count++;
        ts_push_children(&s, node);
    }
    ts_free(&s);
    return count;
}

static int is_content_match(GumboNode *node) {
    if (node->type != GUMBO_NODE_ELEMENT) return 0;
    GumboTag tag = node->v.element.tag;
    if (is_skip_tag(tag)) return 0;

    /* Priority 1: <main> */
    if (tag == GUMBO_TAG_MAIN) return 1;
    /* Priority 2: role="main" */
    {GumboAttribute *role = gumbo_get_attribute(&node->v.element.attributes, "role");
    if (role && str_contains_i(role->value, "main")) return 1;}
    /* Priority 3: specific content IDs */
    if (tag == GUMBO_TAG_DIV || tag == GUMBO_TAG_SECTION) {
        GumboAttribute *id = gumbo_get_attribute(&node->v.element.attributes, "id");
        if (id) {
            const char *v = id->value;
            if (strcmp(v, "content") == 0 || strcmp(v, "main-content") == 0 ||
                strcmp(v, "main_content") == 0 || strcmp(v, "primary-content") == 0 ||
                strcmp(v, "post-content") == 0 || strcmp(v, "article-content") == 0 ||
                strcmp(v, "page-content") == 0 || strcmp(v, "entry-content") == 0 ||
                strcmp(v, "site-content") == 0 || strcmp(v, "body-content") == 0)
                return 1;
        }
    }
    /* Priority 4: <article> */
    if (tag == GUMBO_TAG_ARTICLE) return 1;
    /* Priority 5: content class patterns */
    if (tag == GUMBO_TAG_DIV || tag == GUMBO_TAG_SECTION) {
        GumboAttribute *cls = gumbo_get_attribute(&node->v.element.attributes, "class");
        if (cls && (str_contains_i(cls->value, "post-content") ||
                    str_contains_i(cls->value, "entry-content") ||
                    str_contains_i(cls->value, "article-content") ||
                    str_contains_i(cls->value, "article-body") ||
                    str_contains_i(cls->value, "post-body") ||
                    str_contains_i(cls->value, "page-content")))
            return 1;
    }
    return 0;
}

static GumboNode *find_content_root(GumboNode *root) {
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    GumboNode *found = NULL;
    while (s.top > 0 && !found) {
        GumboNode *node = ts_pop(&s);
        if (node->type != GUMBO_NODE_ELEMENT) continue;
        if (is_skip_tag(node->v.element.tag)) continue;
        if (node_has_chrome_class(node)) continue;
        if (is_content_match(node)) { found = node; break; }
        ts_push_children(&s, node);
    }
    ts_free(&s);
    return found;
}

/* ── Smart content root selection ───────────────────────────── */

static GumboNode *get_content_root(GumboOutput *output) {
    int num_articles = count_articles(output->root);
    GumboNode *root = find_content_root(output->root);

    /* If we found an <article> but there are multiple, reject it (listing page) */
    if (root && root->type == GUMBO_NODE_ELEMENT &&
        root->v.element.tag == GUMBO_TAG_ARTICLE && num_articles > 1)
        root = NULL;

    if (!root) root = output->root;

    if (root != output->root) {
        GumboAttribute *id = (root->type == GUMBO_NODE_ELEMENT) ?
            gumbo_get_attribute(&root->v.element.attributes, "id") : NULL;
        app_log(LOG_DEBUG, "Content root: <%s%s%s>",
                (root->type == GUMBO_NODE_ELEMENT) ?
                    gumbo_normalized_tagname(root->v.element.tag) : "?",
                id ? " id=" : "", id ? id->value : "");
    }
    return root;
}

/* ── Text extraction (iterative, also counts <p> tags) ─────── */

static void extract_text_iterative(GumboNode *root, char **buf, size_t *len, size_t *cap, int *p_count) {
    if (p_count) *p_count = 0;
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    while (s.top > 0) {
        GumboNode *node = ts_pop(&s);
        if (node->type == GUMBO_NODE_TEXT) {
            const char *text = node->v.text.text;
            size_t tlen = strlen(text);
            while (*len + tlen + 2 >= *cap) {
                *cap *= 2;
                char *nb = (char *)realloc(*buf, *cap);
                if (!nb) { ts_free(&s); return; }
                *buf = nb;
            }
            if (*len > 0 && (*buf)[*len - 1] != ' ') (*buf)[(*len)++] = ' ';
            memcpy(*buf + *len, text, tlen);
            *len += tlen;
            (*buf)[*len] = '\0';
            continue;
        }
        if (node->type != GUMBO_NODE_ELEMENT) continue;
        if (p_count && node->v.element.tag == GUMBO_TAG_P) (*p_count)++;
        if (is_skip_tag(node->v.element.tag)) continue;
        if (node_has_chrome_class(node)) continue;
        ts_push_children(&s, node);
    }
    ts_free(&s);
}

/* ── Heading extraction (iterative) ─────────────────────────── */

static void extract_headings(GumboNode *root, ScrapedPage *page) {
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    while (s.top > 0) {
        GumboNode *node = ts_pop(&s);
        if (node->type != GUMBO_NODE_ELEMENT) continue;

        GumboTag tag = node->v.element.tag;
        if (tag >= GUMBO_TAG_H1 && tag <= GUMBO_TAG_H6 && page->heading_count < MAX_HEADINGS) {
            int level = tag - GUMBO_TAG_H1 + 1;
            char text[512] = "";
            size_t tlen = 0;
            /* Get text from direct children and one level of nesting */
            GumboVector *children = &node->v.element.children;
            for (unsigned int i = 0; i < children->length && tlen < 510; i++) {
                GumboNode *child = children->data[i];
                if (child->type == GUMBO_NODE_TEXT) {
                    size_t clen = strlen(child->v.text.text);
                    if (tlen + clen < 510) { memcpy(text + tlen, child->v.text.text, clen); tlen += clen; }
                } else if (child->type == GUMBO_NODE_ELEMENT) {
                    GumboVector *gc = &child->v.element.children;
                    for (unsigned int j = 0; j < gc->length && tlen < 510; j++) {
                        GumboNode *gchild = gc->data[j];
                        if (gchild->type == GUMBO_NODE_TEXT) {
                            size_t clen = strlen(gchild->v.text.text);
                            if (tlen + clen < 510) { memcpy(text + tlen, gchild->v.text.text, clen); tlen += clen; }
                        }
                    }
                }
            }
            text[tlen] = '\0';
            str_trim(text);
            if (text[0] && strlen(text) > 1) {
                ExtractedHeading *h = &page->headings[page->heading_count];
                snprintf(h->tag, sizeof(h->tag), "h%d", level);
                snprintf(h->text, sizeof(h->text), "%s", text);
                h->level = level;
                {char *sp = h->text; char *dp = h->text;
                while(*sp) { if((unsigned char)*sp >= 32 && (unsigned char)*sp < 128) *dp++ = *sp; sp++; }
                *dp = '\0';}
                str_trim(h->text);
                page->heading_count++;
            }
            /* Don't push heading's children -- we already extracted their text */
            continue;
        }
        ts_push_children(&s, node);
    }
    ts_free(&s);
}

/* ── Link extraction (iterative) ─────────────────────────────── */

static void extract_links(GumboNode *root, ScrapedPage *page, const char *page_domain) {
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    while (s.top > 0) {
        GumboNode *node = ts_pop(&s);
        if (node->type != GUMBO_NODE_ELEMENT) continue;

        if (node->v.element.tag == GUMBO_TAG_A && page->link_count < MAX_LINKS) {
            GumboAttribute *href = gumbo_get_attribute(&node->v.element.attributes, "href");
            if (href && href->value[0]) {
                const char *raw = href->value;
                if (raw[0] != '#' && strncmp(raw, "javascript:", 11) != 0 &&
                    strncmp(raw, "mailto:", 7) != 0 && strncmp(raw, "tel:", 4) != 0) {
                    /* Grow links array on demand */
                    if (page->link_count >= page->link_cap) {
                        int new_cap = page->link_cap ? page->link_cap * 2 : 32;
                        if (new_cap > MAX_LINKS) new_cap = MAX_LINKS;
                        ExtractedLink *grown = (ExtractedLink *)realloc(page->links, new_cap * sizeof(ExtractedLink));
                        if (!grown) { ts_push_children(&s, node); continue; }
                        memset(grown + page->link_cap, 0, (new_cap - page->link_cap) * sizeof(ExtractedLink));
                        page->links = grown;
                        page->link_cap = new_cap;
                    }
                    ExtractedLink *link = &page->links[page->link_count];
                    url_resolve_relative(page->page_url, raw, link->url, sizeof(link->url));
                    /* Get link text */
                    link->text[0] = '\0';
                    GumboVector *children = &node->v.element.children;
                    for (unsigned int i = 0; i < children->length; i++) {
                        GumboNode *c = children->data[i];
                        if (c->type == GUMBO_NODE_TEXT) {
                            size_t cur = strlen(link->text);
                            snprintf(link->text + cur, MAX_LINK_TEXT - cur, "%s", c->v.text.text);
                        }
                    }
                    str_trim(link->text);
                    char link_domain[256];
                    url_extract_domain(link->url, link_domain, sizeof(link_domain));
                    link->is_outbound = (strcmp(link_domain, page_domain) != 0);
                    if (link->is_outbound) page->outbound_count++;
                    else page->internal_count++;
                    page->link_count++;
                }
            }
        }
        ts_push_children(&s, node);
    }
    ts_free(&s);
}

/* ── Meta extraction (iterative) ─────────────────────────────── */

static void extract_meta(GumboNode *root, ScrapedPage *page) {
    TreeStack s; ts_init(&s);
    ts_push(&s, root);
    while (s.top > 0) {
        GumboNode *node = ts_pop(&s);
        if (node->type != GUMBO_NODE_ELEMENT) continue;

        if (node->v.element.tag == GUMBO_TAG_TITLE && !page->page_title[0]) {
            GumboVector *children = &node->v.element.children;
            for (unsigned int i = 0; i < children->length; i++) {
                GumboNode *child_node = (GumboNode *)children->data[i];
                if (child_node->type == GUMBO_NODE_TEXT) {
                    snprintf(page->page_title, sizeof(page->page_title), "%s", child_node->v.text.text);
                    str_trim(page->page_title);
                    break;
                }
            }
        }
        if (node->v.element.tag == GUMBO_TAG_META) {
            GumboAttribute *name = gumbo_get_attribute(&node->v.element.attributes, "name");
            GumboAttribute *content = gumbo_get_attribute(&node->v.element.attributes, "content");
            if (name && content) {
                if (str_contains_i(name->value, "description") && !page->meta_description[0])
                    snprintf(page->meta_description, sizeof(page->meta_description), "%s", content->value);
                if (str_contains_i(name->value, "keywords") && !page->meta_keywords[0])
                    snprintf(page->meta_keywords, sizeof(page->meta_keywords), "%s", content->value);
            }
        }
        ts_push_children(&s, node);
    }
    ts_free(&s);
}

/* ── Word count ──────────────────────────────────────────────── */

static int count_words(const char *text) {
    if (!text) return 0;
    int count = 0, in_word = 0;
    for (const char *p = text; *p; p++) {
        if (isspace((unsigned char)*p)) { in_word = 0; }
        else if (!in_word) { in_word = 1; count++; }
    }
    return count;
}

/* ── JS detection ────────────────────────────────────────────── */

int scraper_needs_js(const char *html, int word_count) {
    if (word_count < 200) return 1;
    /* Check for JS framework signals with short text */
    if (word_count < 500) {
        if (str_contains_i(html, "enable javascript")) return 1;
        if (str_contains_i(html, "javascript is required")) return 1;
        if (str_contains_i(html, "__next_data__")) return 1;
        if (str_contains_i(html, "react-root")) return 1;
    }
    return 0;
}

/* ── Main scraper function ───────────────────────────────────── */

int scraper_fetch_page(const char *url, const char *proxy_url, ScrapedPage *page) {
    memset(page, 0, sizeof(ScrapedPage));
    snprintf(page->page_url, sizeof(page->page_url), "%s", url);
    url_extract_domain(url, page->domain, sizeof(page->domain));

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#endif

    /* Fetch HTML */
    HttpBuffer buf;
    int rc;
    if (proxy_url && proxy_url[0])
        rc = http_get_proxy(url, proxy_url, &buf);
    else
        rc = http_get_retry(url, &buf, 0);

    if (rc != 0) {
        snprintf(page->error, sizeof(page->error), "HTTP GET failed");
#ifdef _WIN32
        QueryPerformanceCounter(&t1);
        page->scrape_time = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
#endif
        return -1;
    }

    /* Reject non-HTML content (PDF, images, binary) before parsing */
    if (buf.data && buf.size > 4) {
        int binary = 0;
        if (buf.data[0] == '%' && buf.data[1] == 'P' && buf.data[2] == 'D' && buf.data[3] == 'F') binary = 1;       /* %PDF */
        else if ((unsigned char)buf.data[0] == 0x89 && buf.data[1] == 'P' && buf.data[2] == 'N' && buf.data[3] == 'G') binary = 1; /* PNG */
        else if ((unsigned char)buf.data[0] == 0xFF && (unsigned char)buf.data[1] == 0xD8) binary = 1;                  /* JPEG */
        else if (buf.data[0] == 'G' && buf.data[1] == 'I' && buf.data[2] == 'F') binary = 1;                           /* GIF */
        else if (buf.data[0] == 'P' && buf.data[1] == 'K') binary = 1;                                                  /* ZIP/DOCX */
        if (!binary) {
            /* Check for NUL bytes in first 512 bytes -- binary content */
            int check = buf.size < 512 ? (int)buf.size : 512;
            for (int i = 0; i < check; i++) {
                if (buf.data[i] == '\0') { binary = 1; break; }
            }
        }
        if (binary) {
            app_log(LOG_INFO, "Skipping non-HTML content: %s", page->domain);
            snprintf(page->error, sizeof(page->error), "Non-HTML content");
            http_buffer_free(&buf);
#ifdef _WIN32
            QueryPerformanceCounter(&t1);
            page->scrape_time = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
#endif
            return -1;
        }
    }

    /* Parse with Gumbo */
    GumboOutput *output = gumbo_parse(buf.data);
    if (!output) {
        snprintf(page->error, sizeof(page->error), "HTML parse failed");
        http_buffer_free(&buf);
        return -1;
    }

    /* Extract everything */
    extract_meta(output->root, page);
    extract_headings(output->root, page);
    extract_links(output->root, page, page->domain);

    /* Extract body text (from content root, not whole document) */
    size_t text_cap = 65536;
    size_t text_len = 0;
    page->page_text = (char *)malloc(text_cap);
    if (page->page_text) {
        page->page_text[0] = '\0';
        GumboNode *content = get_content_root(output);
        extract_text_iterative(content, &page->page_text, &text_len, &text_cap, &page->p_tag_count);
        page->word_count = count_words(page->page_text);

        /* Flag non-article pages: high word count but very few <p> tags */
        if (page->word_count > 200 && page->p_tag_count < 3) {
            page->is_non_article = 1;
            app_log(LOG_INFO, "Non-article flagged: %s (%d words, %d <p> tags)",
                    page->domain, page->word_count, page->p_tag_count);
        }
    }

    /* Check if page needs JS rendering -- retry with headless browser via CDP.
       Triggers on: (1) low word count with JS framework signals, or
                    (2) zero words but substantial HTML body (JS shell). */
    page->used_js_fallback = 0;
    int needs_js = (page->word_count > 0 && scraper_needs_js(buf.data, page->word_count))
                || (page->word_count == 0 && buf.size > 500);
    if (needs_js) {
        app_log(LOG_INFO, "JS detected on %s (%d words) -- trying CDP render", page->domain, page->word_count);
        gumbo_destroy_output(&kGumboDefaultOptions, output);
        http_buffer_free(&buf);

        char *js_html = js_render_page(url, 15000);
        if (js_html) {
            GumboOutput *js_out = gumbo_parse(js_html);
            if (js_out) {
                /* Clear old extraction, redo with JS-rendered HTML */
                free(page->page_text); page->page_text = NULL;
                page->heading_count = 0; page->link_count = 0;
                page->outbound_count = 0; page->internal_count = 0;

                extract_meta(js_out->root, page);
                extract_headings(js_out->root, page);
                extract_links(js_out->root, page, page->domain);

                size_t js_cap = 65536, js_len = 0;
                page->page_text = (char *)malloc(js_cap);
                if (page->page_text) {
                    page->page_text[0] = '\0';
                    GumboNode *js_content = get_content_root(js_out);
                    int js_p_count = 0;
                    extract_text_iterative(js_content, &page->page_text, &js_len, &js_cap, &js_p_count);
                    int new_wc = count_words(page->page_text);
                    if (new_wc > page->word_count) {
                        int old_wc = page->word_count;
                        page->word_count = new_wc;
                        page->used_js_fallback = 1;
                        page->p_tag_count = js_p_count;
                        page->is_non_article = 0;
                        if (new_wc > 200 && page->p_tag_count < 3) {
                            page->is_non_article = 1;
                            app_log(LOG_INFO, "Non-article flagged (JS): %s (%d words, %d <p> tags)",
                                    page->domain, new_wc, page->p_tag_count);
                        }
                        app_log(LOG_INFO, "JS fallback success: %s now %d words (was %d)",
                                page->domain, new_wc, old_wc);
                    } else {
                        app_log(LOG_DEBUG, "JS fallback no improvement: %s (%d vs %d words)",
                                page->domain, new_wc, page->word_count);
                    }
                }
                gumbo_destroy_output(&kGumboDefaultOptions, js_out);
            }
            free(js_html);
        }

        /* Re-read the timing */
#ifdef _WIN32
        QueryPerformanceCounter(&t1);
        page->scrape_time = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
#endif
        page->status_code = 200;
        return 0;
    }

    gumbo_destroy_output(&kGumboDefaultOptions, output);
    size_t http_body_size = buf.size;
    http_buffer_free(&buf);

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    page->scrape_time = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
#endif

    page->status_code = 200;
    if (page->word_count == 0)
        snprintf(page->error, sizeof(page->error), "No text extracted (JS-rendered or empty)");
    return 0;
}

void scraper_free(ScrapedPage *page) {
    free(page->page_text);
    page->page_text = NULL;
    free(page->links);
    page->links = NULL;
    page->link_count = 0;
    page->link_cap = 0;
}

