#include "prompt.h"
#include "utils.h"
#include "app_log.h"
#include <stdio.h>
#include <string.h>

static const char *intent_names[] = {"informational","commercial","transactional","navigational"};
#define SAFE_INTENT(i) ((i) >= 0 && (i) < 4 ? intent_names[(i)] : "unknown")

static const char *template_names[] = {"Full System Prompt","Keywords Only","Outline Only","Competitive Brief"};

const char *prompt_template_name(int t) {
    if (t >= 0 && t < PROMPT_TEMPLATE_COUNT) return template_names[t];
    return "Unknown";
}

/* ── Append helpers ──────────────────────────────────────────── */

#define AP(buf, pos, max, ...) do { \
    if ((pos) < (max) - 1) { \
        int _w = snprintf((buf)+(pos), (max)-(pos), __VA_ARGS__); \
        if (_w > 0) { (pos) += _w; if ((pos) >= (max)) (pos) = (max) - 1; } \
    } \
} while(0)

/* ── Content-type matching helper ───────────────────────────── */
static int ct_is(const char *ct, const char *name) { return strcmp(ct, name) == 0; }

/* ── Shared helper: TARGET AUDIENCE (Change 6) ─────────────── */
static int emit_audience(const NLPResult *nlp, char *buf, int p, int max_len) {
    AP(buf,p,max_len, "=== TARGET AUDIENCE ===\n");
    int intent = nlp->intent.primary_intent;
    const char *ct = nlp->intent.content_type;
    if (intent == INTENT_INFORMATIONAL && ct_is(ct, "how-to"))
        AP(buf,p,max_len, "Beginners seeking step-by-step guidance. Assume no prior expertise. Define technical terms on first use.\n");
    else if (intent == INTENT_INFORMATIONAL && ct_is(ct, "guide"))
        AP(buf,p,max_len, "Readers seeking comprehensive understanding. Balance depth with accessibility.\n");
    else if (intent == INTENT_INFORMATIONAL && ct_is(ct, "recipe"))
        AP(buf,p,max_len, "Home cooks of varying skill levels. Be precise with measurements and timing.\n");
    else if (intent == INTENT_INFORMATIONAL && ct_is(ct, "health guide"))
        AP(buf,p,max_len, "People seeking health information. Use clear, non-alarmist language. Always recommend consulting a healthcare professional.\n");
    else if (intent == INTENT_COMMERCIAL)
        AP(buf,p,max_len, "Potential buyers in the research/comparison phase. They need unbiased, detailed information to make a decision.\n");
    else if (intent == INTENT_TRANSACTIONAL)
        AP(buf,p,max_len, "Ready-to-buy readers who need final confirmation. Focus on value, pricing clarity, and purchase confidence.\n");
    else
        AP(buf,p,max_len, "General audience seeking clear, accessible information.\n");
    AP(buf,p,max_len, "\n");
    return p;
}

/* ── Shared helper: OUTPUT FORMAT (Change 7) ───────────────── */
static int emit_output_format(const NLPResult *nlp, char *buf, int p, int max_len) {
    AP(buf,p,max_len, "=== OUTPUT FORMAT ===\n");
    AP(buf,p,max_len, "Format: Markdown with proper heading hierarchy (## H2, ### H3).\n");
    AP(buf,p,max_len, "Start with a compelling introduction paragraph (no heading) that hooks the reader and includes the primary keyword naturally.\n");
    AP(buf,p,max_len, "End with a meta description (under 160 characters) on its own line, prefixed with \"META:\"\n");
    const char *ct = nlp->intent.content_type;
    if (ct_is(ct, "how-to"))
        AP(buf,p,max_len, "Number each step. Use bold for key actions.\n");
    else if (ct_is(ct, "comparison"))
        AP(buf,p,max_len, "Include a comparison table. Present pros/cons for each option.\n");
    else if (ct_is(ct, "recipe"))
        AP(buf,p,max_len, "Use bulleted ingredient list, numbered instruction steps, and a nutrition summary.\n");
    else if (ct_is(ct, "listicle"))
        AP(buf,p,max_len, "Use numbered H2 headings for each item.\n");
    else if (ct_is(ct, "health guide"))
        AP(buf,p,max_len, "Use bullet points for symptoms/signs lists. Include a 'Key Takeaways' box.\n");
    AP(buf,p,max_len, "\n");
    return p;
}

/* ── Shared helper: WRITING INSTRUCTIONS (Change 8) ────────── */
static int emit_instructions(const NLPResult *nlp, const PromptOptions *opts, char *buf, int p, int max_len) {
    int inc_ent = (!opts || opts->include_entities) && nlp->entity_count > 0;
    int inc_paa = (!opts || opts->include_paa) && nlp->paa_count > 0;
    int inc_related = (!opts || opts->include_related) && nlp->related_count > 0;
    int upper_words = (int)(nlp->intent.recommended_words * 1.3f);
    int n = 0;

    AP(buf,p,max_len, "=== WRITING INSTRUCTIONS ===\n");
    {const char *ct = nlp->intent.content_type;
    const char *article = (ct[0]=='a'||ct[0]=='e'||ct[0]=='i'||ct[0]=='o'||ct[0]=='u') ? "an" : "a";
    AP(buf,p,max_len, "%d. Write %s %s about \"%s\" in a %s tone.\n",
       ++n, article, ct, nlp->keyword, nlp->intent.tone);}
    AP(buf,p,max_len, "%d. Target word count: %d-%d words.\n",
       ++n, nlp->intent.recommended_words, upper_words);
    AP(buf,p,max_len, "%d. Use %d+ H2 sections to match top competitors.\n",
       ++n, nlp->intent.recommended_headings);
    AP(buf,p,max_len, "%d. Follow the outline structure above. Expand each H2 into 150-300 words.\n", ++n);
    AP(buf,p,max_len, "%d. Use the primary keyword in: the title, first paragraph, at least 2 H2 headings, and the conclusion.\n", ++n);
    AP(buf,p,max_len, "%d. Weave secondary keywords into body paragraphs naturally. Never force or stuff.\n", ++n);
    if (inc_ent)
        AP(buf,p,max_len, "%d. Reference the named entities listed above to demonstrate topical expertise (E-E-A-T).\n", ++n);
    if (inc_paa)
        AP(buf,p,max_len, "%d. Answer each People Also Ask question in a dedicated section. Structure: concise 1-2 sentence answer first, then elaboration. This wins featured snippets.\n", ++n);
    if (inc_related)
        AP(buf,p,max_len, "%d. Cover the related subtopics as dedicated sections or substantial paragraphs.\n", ++n);
    if (nlp->stats.avg_word_count > 0)
        AP(buf,p,max_len, "%d. Competitor benchmark: avg %.0f words (range %d-%d). Aim for the upper quartile to signal depth.\n",
           ++n, nlp->stats.avg_word_count, nlp->stats.min_word_count, nlp->stats.max_word_count);
    AP(buf,p,max_len, "\n");
    return p;
}

/* ── Shared helper: READER GOAL (Change 9) ─────────────────── */
static int emit_reader_goal(const NLPResult *nlp, char *buf, int p, int max_len) {
    AP(buf,p,max_len, "=== READER GOAL ===\n");
    int intent = nlp->intent.primary_intent;
    if (intent == INTENT_TRANSACTIONAL)
        AP(buf,p,max_len, "Goal: Guide the reader toward a purchase decision. End with a clear recommendation and call-to-action.\n");
    else if (intent == INTENT_COMMERCIAL)
        AP(buf,p,max_len, "Goal: Help the reader compare options and narrow their choice. End with a decision framework or top pick.\n");
    else if (intent == INTENT_NAVIGATIONAL)
        AP(buf,p,max_len, "Goal: Direct the reader efficiently to the resource they need.\n");
    else
        AP(buf,p,max_len, "Goal: Fully satisfy the reader's information need. End with actionable takeaways they can apply immediately.\n");
    AP(buf,p,max_len, "\n");
    return p;
}

/* ── Shared helper: LINKING GUIDANCE (Change 10) ───────────── */
static int emit_linking(const NLPResult *nlp, char *buf, int p, int max_len) {
    /* Competitor stats count ALL page links (nav, footer, sidebar = 100-200),
       not just article body links.  Use fixed reasonable ranges instead. */
    int internal = 5;  /* recommend 3-8 internal links */
    int external = 3;  /* recommend 2-5 external links */
    (void)nlp;  /* stats.avg_internal_links / avg_outbound_links are total-page counts, not usable */
    AP(buf,p,max_len, "=== LINKING GUIDANCE ===\n");
    AP(buf,p,max_len, "Include %d internal links to related content on your site, placed naturally within relevant paragraphs.\n", internal);
    AP(buf,p,max_len, "Include %d authoritative external sources (studies, official docs, reputable publications).\n", external);
    AP(buf,p,max_len, "\n");
    return p;
}

/* ── Fallback outline generation helper (Change 5) ─────────── */
static int emit_fallback_outline(const NLPResult *nlp, char *buf, int p, int max_len) {
    const char *ct = nlp->intent.content_type;
    AP(buf,p,max_len, "(Template outline for %s content:)\n", ct);
    if (ct_is(ct, "how-to")) {
        AP(buf,p,max_len, "H2: What You'll Need\nH2: Step-by-Step Instructions\nH2: Tips and Best Practices\nH2: Common Mistakes to Avoid\nH2: Frequently Asked Questions\n");
    } else if (ct_is(ct, "comparison")) {
        AP(buf,p,max_len, "H2: Overview\nH2: Side-by-Side Comparison\nH2: Detailed Analysis\nH2: Our Verdict\nH2: FAQs\n");
    } else if (ct_is(ct, "review")) {
        AP(buf,p,max_len, "H2: Overview and First Impressions\nH2: Key Features\nH2: Pros and Cons\nH2: Who Is This For?\nH2: Final Verdict\n");
    } else if (ct_is(ct, "guide")) {
        AP(buf,p,max_len, "H2: Introduction\nH2: Key Concepts\nH2: Detailed Walkthrough\nH2: Best Practices\nH2: Conclusion\n");
    } else if (ct_is(ct, "listicle")) {
        AP(buf,p,max_len, "H2: Introduction\nH2: Our Top Picks\nH2: How We Evaluated\nH2: Conclusion\n");
    } else if (ct_is(ct, "recipe")) {
        AP(buf,p,max_len, "H2: About This Recipe\nH2: Ingredients\nH2: Step-by-Step Instructions\nH2: Tips for Success\nH2: Variations and Substitutions\nH2: Nutrition Information\n");
    } else if (ct_is(ct, "health guide")) {
        AP(buf,p,max_len, "H2: Overview\nH2: Symptoms and Signs\nH2: Causes and Risk Factors\nH2: Treatment Options\nH2: When to See a Doctor\nH2: Frequently Asked Questions\n");
    } else {
        AP(buf,p,max_len, "H2: Overview\nH2: Key Considerations\nH2: In-Depth Analysis\nH2: Expert Recommendations\nH2: Conclusion\n");
    }
    return p;
}

/* ── Full System Prompt ──────────────────────────────────────── */

static int build_full_opts(const NLPResult *nlp, const PromptOptions *opts, char *buf, int max_len) {
    int p = 0;
    int max_kw = (opts && opts->max_keywords > 0) ? opts->max_keywords : 0; /* 0 = all */
    int max_ent = (opts && opts->max_entities > 0) ? opts->max_entities : 0;
    int inc_kw = (!opts || opts->include_keywords);
    int inc_ent = (!opts || opts->include_entities);
    int inc_outline = (!opts || opts->include_outline);
    int inc_stats = (!opts || opts->include_stats);
    int inc_paa = (!opts || opts->include_paa);
    int inc_related = (!opts || opts->include_related);

    AP(buf,p,max_len, "You are writing an SEO-optimized article about: \"%s\"\n\n", nlp->keyword);

    /* Intent */
    AP(buf,p,max_len, "=== CONTENT GUIDELINES ===\n");
    AP(buf,p,max_len, "Search Intent: %s\n", SAFE_INTENT(nlp->intent.primary_intent));
    AP(buf,p,max_len, "Content Type: %s\n", nlp->intent.content_type);
    AP(buf,p,max_len, "Tone: %s\n", nlp->intent.tone);
    if (nlp->intent.recommended_words > 0)
        AP(buf,p,max_len, "Target Word Count: %d words\n", nlp->intent.recommended_words);
    AP(buf,p,max_len, "\n");

    /* Target Audience (Change 6) */
    p = emit_audience(nlp, buf, p, max_len);

    /* Output Format (Change 7) */
    p = emit_output_format(nlp, buf, p, max_len);

    /* Keywords -- user controls count via max_keywords (0 = smart defaults) */
    if (inc_kw && nlp->keyword_count > 0) {
        int pri_max = max_kw > 0 ? max_kw : 20;
        int sec_max = max_kw > 0 ? max_kw : 15;
        /* Dynamic threshold: take top pri_max by score, not a fixed cutoff.
           Keywords are already sorted by score descending from nlp_rescore. */
        float pri_thresh = 0.40f, sec_thresh = 0.25f;
        /* If too many above threshold, raise it to fit the cap */
        {int above = 0;
        for (int i = 0; i < nlp->keyword_count; i++)
            if (nlp->keywords[i].score > pri_thresh) above++;
        if (above > pri_max && nlp->keyword_count > pri_max)
            pri_thresh = nlp->keywords[pri_max].score + 0.001f;}
        /* Count with adjusted thresholds */
        int pri_avail = 0, sec_avail = 0;
        for (int i = 0; i < nlp->keyword_count; i++) {
            if (nlp->keywords[i].score > pri_thresh) pri_avail++;
            else if (nlp->keywords[i].score > sec_thresh) sec_avail++;
        }
        if (pri_avail > 0 || sec_avail > 0) {
            AP(buf,p,max_len, "=== SEMANTIC KEYWORDS (use naturally throughout) ===\n");
            if (pri_avail > 0) {
                int primary = 0;
                int ngram_max = pri_max * 3 / 4;  /* 15 slots for phrases */
                int tfidf_max = pri_max - ngram_max;  /* 5 slots for single words */
                AP(buf,p,max_len, "Primary keyword phrases:\n");
                /* First: top n-gram phrases (multi-word) */
                for (int i = 0; i < nlp->keyword_count && primary < ngram_max; i++) {
                    if (nlp->keywords[i].score > pri_thresh &&
                        strcmp(nlp->keywords[i].source, "ngram") == 0) {
                        AP(buf,p,max_len, "  - %s\n", nlp->keywords[i].text);
                        primary++;
                    }
                }
                /* Then: top TF-IDF single words (diverse vocabulary) */
                AP(buf,p,max_len, "Primary single-word terms:\n");
                {int tfidf_added = 0;
                for (int i = 0; i < nlp->keyword_count && tfidf_added < tfidf_max; i++) {
                    if (nlp->keywords[i].score > sec_thresh &&
                        strcmp(nlp->keywords[i].source, "tfidf") == 0) {
                        /* Skip if this word already appears inside an n-gram phrase (primary OR secondary) */
                        int in_phrase = 0;
                        for (int j = 0; j < nlp->keyword_count && !in_phrase; j++) {
                            if (strcmp(nlp->keywords[j].source, "ngram") == 0 &&
                                nlp->keywords[j].score > sec_thresh &&
                                strstr(nlp->keywords[j].text, nlp->keywords[i].text))
                                in_phrase = 1;
                        }
                        if (!in_phrase) {
                            AP(buf,p,max_len, "  - %s\n", nlp->keywords[i].text);
                            tfidf_added++;
                            primary++;
                        }
                    }
                }}
            }
            if (sec_avail > 0) {
                int secondary = 0;
                AP(buf,p,max_len, "Secondary keywords (include where relevant):\n");
                for (int i = 0; i < nlp->keyword_count && secondary < sec_max; i++) {
                    if (nlp->keywords[i].score > sec_thresh && nlp->keywords[i].score <= pri_thresh) {
                        AP(buf,p,max_len, "  - %s\n", nlp->keywords[i].text);
                        secondary++;
                    }
                }
            }
            AP(buf,p,max_len, "\n");
        }
    }

    /* Entities -- user controls count via max_entities (0 = smart default of 25) */
    if (inc_ent && nlp->entity_count > 0) {
        int ent_cap = max_ent > 0 ? max_ent : 25;
        AP(buf,p,max_len, "=== KEY ENTITIES (reference for topical authority) ===\n");
        for (int i = 0; i < nlp->entity_count && i < ent_cap; i++) {
            AP(buf,p,max_len, "  - %s (%s, freq=%d)\n",
               nlp->entities[i].text, nlp->entities[i].label, nlp->entities[i].frequency);
        }
        AP(buf,p,max_len, "\n");
    }

    /* Outline -- show top headings by frequency (sorted by count desc already).
       Include count=1 entries if we don't have enough 2+ entries. Cap at 15.
       Change 5: H2/H3 prefix display + fallback outline if shown < 3.
       For recipe/listicle: prefer fallback outline (scraped headings are recipe titles, not structure).
       Filter: skip headings that share zero words with the keyword (off-topic). */
    if (inc_outline) {
        AP(buf,p,max_len, "=== RECOMMENDED ARTICLE OUTLINE ===\n");
        int shown = 0;
        int use_scraped = 1;
        /* Recipe/listicle scraped headings are individual item names, not article structure */
        if (ct_is(nlp->intent.content_type, "recipe") ||
            ct_is(nlp->intent.content_type, "listicle"))
            use_scraped = 0;

        /* Build keyword word set for relevance check */
        char kw_lower[256];
        snprintf(kw_lower, sizeof(kw_lower), "%s", nlp->keyword);
        str_to_lower(kw_lower);

        if (use_scraped && nlp->heading_count > 0) {
            /* First pass: headings appearing in 2+ pages (strong signals).
               No keyword-relevance filter here -- if multiple competitors
               use the same heading, it's a real structural pattern. */
            for (int i = 0; i < nlp->heading_count && shown < 12; i++) {
                if (nlp->headings[i].count >= 2) {
                    /* Skip numbered list items from single-site lists (e.g. "1. Simple Fit") */
                    {const char *ht = nlp->headings[i].text;
                    if (ht[0] >= '0' && ht[0] <= '9' && (ht[1] == '.' || (ht[1] >= '0' && ht[1] <= '9' && ht[2] == '.')))
                        continue;}

                    if (nlp->headings[i].tag[1] == '2')
                        AP(buf,p,max_len, "H2: %s (in %d pages)\n",
                           nlp->headings[i].text, nlp->headings[i].count);
                    else
                        AP(buf,p,max_len, "  H3: %s (in %d pages)\n",
                           nlp->headings[i].text, nlp->headings[i].count);
                    shown++;
                }
            }
            /* Second pass: if outline is thin, add top unique headings (count=1) */
            if (shown < 6) {
                for (int i = 0; i < nlp->heading_count && shown < 10; i++) {
                    if (nlp->headings[i].count == 1 && nlp->headings[i].tag[1] == '2') {
                        /* Same relevance check */
                        {char h_low[512];
                        snprintf(h_low, sizeof(h_low), "%s", nlp->headings[i].text);
                        str_to_lower(h_low);
                        int relevant = 0;
                        char kw_tmp[256];
                        snprintf(kw_tmp, sizeof(kw_tmp), "%s", kw_lower);
                        char *w = strtok(kw_tmp, " ");
                        while (w) {
                            if (strlen(w) > 2 && strstr(h_low, w)) { relevant = 1; break; }
                            w = strtok(NULL, " ");
                        }
                        if (!relevant) continue;}
                        AP(buf,p,max_len, "H2: %s\n", nlp->headings[i].text);
                        shown++;
                    }
                }
            }
        }
        /* Fallback: if fewer than 3 relevant scraped headings, generate template outline */
        if (shown < 3) {
            if (shown > 0)
                AP(buf,p,max_len, "\n");
            p = emit_fallback_outline(nlp, buf, p, max_len);
        }
        AP(buf,p,max_len, "\n");
    }

    /* Stats */
    if (inc_stats) {
        AP(buf,p,max_len, "=== COMPETITIVE METRICS ===\n");
        AP(buf,p,max_len, "Average article length: %.0f words\n", nlp->stats.avg_word_count);
        AP(buf,p,max_len, "Range: %d - %d words\n", nlp->stats.min_word_count, nlp->stats.max_word_count);
        AP(buf,p,max_len, "Average H2: %.1f  H3: %.1f\n", nlp->stats.avg_h2_count, nlp->stats.avg_h3_count);
        AP(buf,p,max_len, "\n");
    }

    /* PAA */
    if (inc_paa && nlp->paa_count > 0) {
        AP(buf,p,max_len, "=== QUESTIONS TO ANSWER (People Also Ask) ===\n");
        for (int i = 0; i < nlp->paa_count; i++)
            AP(buf,p,max_len, "  - %s\n", nlp->paa[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Related */
    if (inc_related && nlp->related_count > 0) {
        AP(buf,p,max_len, "=== RELATED TOPICS ===\n");
        for (int i = 0; i < nlp->related_count; i++)
            AP(buf,p,max_len, "  - %s\n", nlp->related[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Writing Instructions (Change 8) */
    p = emit_instructions(nlp, opts, buf, p, max_len);

    /* Reader Goal (Change 9) */
    p = emit_reader_goal(nlp, buf, p, max_len);

    /* Linking Guidance (Change 10) */
    p = emit_linking(nlp, buf, p, max_len);

    return p;
}

int prompt_build_full(const NLPResult *nlp, char *buf, int max_len) {
    return build_full_opts(nlp, NULL, buf, max_len);
}

int prompt_build_keywords(const NLPResult *nlp, char *buf, int max_len) {
    return prompt_build_keywords_opts(nlp, NULL, buf, max_len);
}

int prompt_build_keywords_opts(const NLPResult *nlp, const PromptOptions *opts,
                               char *buf, int max_len) {
    int p = 0;
    int max_kw  = (opts && opts->max_keywords > 0)  ? opts->max_keywords  : 30;
    int max_ent = (opts && opts->max_entities > 0)   ? opts->max_entities  : 30;

    AP(buf,p,max_len, "TARGET KEYWORD: %s\n\n", nlp->keyword);

    /* Primary keywords (high confidence) */
    int pri = 0;
    for (int i = 0; i < nlp->keyword_count && pri < max_kw; i++) {
        if (nlp->keywords[i].score > 0.40f) pri++;
    }
    if (pri > 0) {
        AP(buf,p,max_len, "PRIMARY KEYWORDS (%d):\n", pri);
        int shown = 0;
        for (int i = 0; i < nlp->keyword_count && shown < max_kw; i++) {
            if (nlp->keywords[i].score > 0.40f) {
                AP(buf,p,max_len, "  %-35s  %.0f%%  x%d\n",
                   nlp->keywords[i].text,
                   nlp->keywords[i].score * 100,
                   nlp->keywords[i].frequency);
                shown++;
            }
        }
        AP(buf,p,max_len, "\n");
    }

    /* Secondary keywords (medium confidence) */
    int sec = 0;
    for (int i = 0; i < nlp->keyword_count && sec < max_kw; i++) {
        if (nlp->keywords[i].score > 0.25f && nlp->keywords[i].score <= 0.40f) sec++;
    }
    if (sec > 0) {
        AP(buf,p,max_len, "SECONDARY KEYWORDS (%d):\n", sec);
        int shown = 0;
        for (int i = 0; i < nlp->keyword_count && shown < max_kw; i++) {
            if (nlp->keywords[i].score > 0.25f && nlp->keywords[i].score <= 0.40f) {
                AP(buf,p,max_len, "  %-35s  %.0f%%  x%d\n",
                   nlp->keywords[i].text,
                   nlp->keywords[i].score * 100,
                   nlp->keywords[i].frequency);
                shown++;
            }
        }
        AP(buf,p,max_len, "\n");
    }

    if (nlp->entity_count > 0) {
        int ent_cap = nlp->entity_count > max_ent ? max_ent : nlp->entity_count;
        AP(buf,p,max_len, "ENTITIES (%d):\n", ent_cap);
        for (int i = 0; i < nlp->entity_count && i < max_ent; i++)
            AP(buf,p,max_len, "  %-35s  %-8s  x%d\n",
               nlp->entities[i].text, nlp->entities[i].label, nlp->entities[i].frequency);
        AP(buf,p,max_len, "\n");
    }
    if (nlp->related_count > 0) {
        AP(buf,p,max_len, "RELATED SEARCHES (%d):\n", nlp->related_count);
        for (int i = 0; i < nlp->related_count; i++)
            AP(buf,p,max_len, "  %s\n", nlp->related[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Shared rich sections */
    p = emit_audience(nlp, buf, p, max_len);
    p = emit_output_format(nlp, buf, p, max_len);
    p = emit_instructions(nlp, opts, buf, p, max_len);
    p = emit_reader_goal(nlp, buf, p, max_len);
    p = emit_linking(nlp, buf, p, max_len);

    return p;
}

int prompt_build_outline(const NLPResult *nlp, char *buf, int max_len) {
    return prompt_build_outline_opts(nlp, NULL, buf, max_len);
}

int prompt_build_outline_opts(const NLPResult *nlp, const PromptOptions *opts,
                              char *buf, int max_len) {
    int p = 0;
    /* max_keywords controls how many keyword-bearing headings to show */
    int max_kw = (opts && opts->max_keywords > 0) ? opts->max_keywords : 12;

    AP(buf,p,max_len, "RECOMMENDED OUTLINE: \"%s\"\n\n", nlp->keyword);
    AP(buf,p,max_len, "Target: ~%d words, %s tone, %s format\n\n",
       nlp->intent.recommended_words, nlp->intent.tone, nlp->intent.content_type);
    AP(buf,p,max_len, "# Introduction\n  (Hook + overview)\n\n");
    int shown = 0;
    for (int i = 0; i < nlp->heading_count && shown < max_kw; i++) {
        if (nlp->headings[i].count >= 2) {
            if (nlp->headings[i].tag[1] == '2')
                AP(buf,p,max_len, "## %s\n", nlp->headings[i].text);
            else
                AP(buf,p,max_len, "  ### %s\n", nlp->headings[i].text);
            shown++;
        }
    }
    AP(buf,p,max_len, "\n## Conclusion\n\n");
    if (nlp->paa_count > 0) {
        AP(buf,p,max_len, "## FAQ\n");
        for (int i = 0; i < nlp->paa_count; i++)
            AP(buf,p,max_len, "  Q: %s\n", nlp->paa[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Shared rich sections */
    p = emit_audience(nlp, buf, p, max_len);
    p = emit_output_format(nlp, buf, p, max_len);
    p = emit_instructions(nlp, opts, buf, p, max_len);
    p = emit_reader_goal(nlp, buf, p, max_len);
    p = emit_linking(nlp, buf, p, max_len);

    return p;
}

int prompt_build_competitive(const NLPResult *nlp, char *buf, int max_len) {
    return prompt_build_competitive_opts(nlp, NULL, buf, max_len);
}

int prompt_build_competitive_opts(const NLPResult *nlp, const PromptOptions *opts,
                                  char *buf, int max_len) {
    int p = 0;
    int max_kw = (opts && opts->max_keywords > 0) ? opts->max_keywords : 10;

    AP(buf,p,max_len, "COMPETITIVE BRIEF: \"%s\"\n\n", nlp->keyword);
    AP(buf,p,max_len, "=== SERP LANDSCAPE ===\n");
    AP(buf,p,max_len, "Intent: %s (%.0f%%)\n", SAFE_INTENT(nlp->intent.primary_intent), nlp->intent.confidence[nlp->intent.primary_intent]*100);
    AP(buf,p,max_len, "Type: %s | Tone: %s\n\n", nlp->intent.content_type, nlp->intent.tone);
    AP(buf,p,max_len, "=== BENCHMARKS ===\n");
    AP(buf,p,max_len, "Avg words: %.0f | Range: %d-%d\n", nlp->stats.avg_word_count, nlp->stats.min_word_count, nlp->stats.max_word_count);
    AP(buf,p,max_len, "Avg H2: %.1f | H3: %.1f\n\n", nlp->stats.avg_h2_count, nlp->stats.avg_h3_count);
    AP(buf,p,max_len, "=== TO RANK YOU NEED ===\n");
    AP(buf,p,max_len, "1. At least %d words\n", (int)(nlp->stats.avg_word_count * 1.1f));
    AP(buf,p,max_len, "2. %d+ H2 sections\n", nlp->intent.recommended_headings);
    AP(buf,p,max_len, "3. Cover these keywords:\n");
    int shown = 0;
    for (int i = 0; i < nlp->keyword_count && shown < max_kw; i++)
        if (nlp->keywords[i].frequency >= 2) {
            AP(buf,p,max_len, "   - %s (in %d pages)\n", nlp->keywords[i].text, nlp->keywords[i].frequency);
            shown++;
        }
    AP(buf,p,max_len, "\n");

    /* Shared rich sections */
    p = emit_audience(nlp, buf, p, max_len);
    p = emit_output_format(nlp, buf, p, max_len);
    p = emit_instructions(nlp, opts, buf, p, max_len);
    p = emit_reader_goal(nlp, buf, p, max_len);
    p = emit_linking(nlp, buf, p, max_len);

    return p;
}

int prompt_build(const NLPResult *nlp, const PromptOptions *opts, char *buf, int max_len) {
    if (!opts) return build_full_opts(nlp, NULL, buf, max_len);
    switch (opts->template_type) {
        case PROMPT_KEYWORDS_ONLY: return prompt_build_keywords_opts(nlp, opts, buf, max_len);
        case PROMPT_OUTLINE_ONLY:  return prompt_build_outline_opts(nlp, opts, buf, max_len);
        case PROMPT_COMPETITIVE:   return prompt_build_competitive_opts(nlp, opts, buf, max_len);
        default:                   return build_full_opts(nlp, opts, buf, max_len);
    }
}

int prompt_export_json(const NLPResult *nlp, char *buf, int max_len) {
    /* Build JSON string manually — comprehensive: all NLP fields persisted */
    int p = 0;
    {char esc_kw[512]; json_escape(nlp->keyword, esc_kw, sizeof(esc_kw));
    AP(buf,p,max_len, "{\n  \"keyword\": \"%s\",\n", esc_kw);}
    {char esc_intent[64]; json_escape(SAFE_INTENT(nlp->intent.primary_intent), esc_intent, sizeof(esc_intent));
    AP(buf,p,max_len, "  \"intent\": \"%s\",\n", esc_intent);}
    {char esc_ct[128]; json_escape(nlp->intent.content_type, esc_ct, sizeof(esc_ct));
    AP(buf,p,max_len, "  \"content_type\": \"%s\",\n", esc_ct);}
    {char esc_tone[128]; json_escape(nlp->intent.tone, esc_tone, sizeof(esc_tone));
    AP(buf,p,max_len, "  \"tone\": \"%s\",\n", esc_tone);}
    AP(buf,p,max_len, "  \"recommended_words\": %d,\n", nlp->intent.recommended_words);
    AP(buf,p,max_len, "  \"recommended_headings\": %d,\n", nlp->intent.recommended_headings);
    AP(buf,p,max_len, "  \"analysis_time\": %.2f,\n", nlp->analysis_time);
    AP(buf,p,max_len, "  \"keyword_count\": %d,\n", nlp->keyword_count);
    AP(buf,p,max_len, "  \"entity_count\": %d,\n", nlp->entity_count);
    AP(buf,p,max_len, "  \"heading_count\": %d,\n", nlp->heading_count);

    /* Keywords — full score breakdown */
    AP(buf,p,max_len, "  \"keywords\": [\n");
    for (int i = 0; i < nlp->keyword_count; i++) {
        char esc[512]; json_escape(nlp->keywords[i].text, esc, sizeof(esc));
        AP(buf,p,max_len, "    {\"text\":\"%s\",\"score\":%.3f,\"freq\":%d,"
           "\"src\":\"%s\",\"cfr\":%.3f,\"sem\":%.3f,\"tfidf\":%.3f,\"boost\":%.3f}%s\n",
           esc, nlp->keywords[i].score, nlp->keywords[i].frequency,
           nlp->keywords[i].source, nlp->keywords[i].score_cfr,
           nlp->keywords[i].score_sem, nlp->keywords[i].score_tfidf,
           nlp->keywords[i].google_boost,
           (i < nlp->keyword_count - 1) ? "," : "");
    }
    AP(buf,p,max_len, "  ],\n");

    /* Entities — with source_count */
    AP(buf,p,max_len, "  \"entities\": [\n");
    for (int i = 0; i < nlp->entity_count; i++) {
        char esc[256]; json_escape(nlp->entities[i].text, esc, sizeof(esc));
        AP(buf,p,max_len, "    {\"text\":\"%s\",\"label\":\"%s\",\"freq\":%d,\"sources\":%d}%s\n",
           esc, nlp->entities[i].label, nlp->entities[i].frequency,
           nlp->entities[i].source_count,
           (i < nlp->entity_count - 1) ? "," : "");
    }
    AP(buf,p,max_len, "  ],\n");

    /* Stats — complete */
    AP(buf,p,max_len, "  \"stats\": {\n");
    AP(buf,p,max_len, "    \"avg_words\":%.0f,\"median_words\":%.0f,\"min_words\":%d,\"max_words\":%d,\n",
       nlp->stats.avg_word_count, nlp->stats.median_word_count,
       nlp->stats.min_word_count, nlp->stats.max_word_count);
    AP(buf,p,max_len, "    \"avg_h2\":%.1f,\"avg_h3\":%.1f,\"avg_headings\":%.1f,\n",
       nlp->stats.avg_h2_count, nlp->stats.avg_h3_count, nlp->stats.avg_heading_count);
    AP(buf,p,max_len, "    \"avg_outbound\":%.1f,\"avg_internal\":%.1f\n",
       nlp->stats.avg_outbound_links, nlp->stats.avg_internal_links);
    AP(buf,p,max_len, "  },\n");

    /* Intent breakdown with signals */
    AP(buf,p,max_len, "  \"intent_breakdown\": {\n");
    for (int t = 0; t < INTENT_COUNT; t++) {
        AP(buf,p,max_len, "    \"%s\": {\"confidence\":%.0f,\"score\":%.1f,\"signals\":[",
           intent_names[t], nlp->intent.confidence[t]*100, nlp->intent.scores[t]);
        for (int s = 0; s < nlp->intent.signal_count[t] && s < 10; s++) {
            char esc[64]; json_escape(nlp->intent.signals[t][s], esc, sizeof(esc));
            AP(buf,p,max_len, "%s\"%s\"", s>0?",":"", esc);
        }
        AP(buf,p,max_len, "]}%s\n", t < INTENT_COUNT-1 ? "," : "");
    }
    AP(buf,p,max_len, "  },\n");

    /* Headings — all of them */
    AP(buf,p,max_len, "  \"heading_patterns\": [\n");
    for (int i = 0; i < nlp->heading_count; i++) {
        char esc[1024]; json_escape(nlp->headings[i].text, esc, sizeof(esc));
        AP(buf,p,max_len, "    {\"tag\":\"%s\",\"text\":\"%s\",\"count\":%d}%s\n",
           nlp->headings[i].tag, esc, nlp->headings[i].count,
           (i < nlp->heading_count - 1) ? "," : "");
    }
    AP(buf,p,max_len, "  ],\n");

    /* PAA — with snippets and links */
    AP(buf,p,max_len, "  \"people_also_ask\": [\n");
    for (int i = 0; i < nlp->paa_count; i++) {
        char eq[1024], es[2048], el[4096];
        json_escape(nlp->paa[i], eq, sizeof(eq));
        json_escape(nlp->paa_snippets[i], es, sizeof(es));
        json_escape(nlp->paa_links[i], el, sizeof(el));
        AP(buf,p,max_len, "    {\"question\":\"%s\",\"snippet\":\"%s\",\"link\":\"%s\"}%s\n",
           eq, es, el, i<nlp->paa_count-1?",":"");
    }
    AP(buf,p,max_len, "  ],\n");

    /* Related */
    AP(buf,p,max_len, "  \"related_searches\": [\n");
    for (int i = 0; i < nlp->related_count; i++) {
        char esc[512]; json_escape(nlp->related[i], esc, sizeof(esc));
        AP(buf,p,max_len, "    \"%s\"%s\n", esc, i<nlp->related_count-1?",":"");
    }
    AP(buf,p,max_len, "  ]\n}\n");

    return p;
}

/* ── Markdown export ─────────────────────────────────────────── */

int prompt_export_markdown(const NLPResult *nlp, char *buf, int max_len) {
    int p = 0;
    AP(buf,p,max_len, "# SERP Analysis: %s\n\n", nlp->keyword);

    /* Intent */
    AP(buf,p,max_len, "## Search Intent\n\n");
    AP(buf,p,max_len, "| Intent | Confidence | Signals |\n");
    AP(buf,p,max_len, "|--------|-----------|--------|\n");
    for (int t = 0; t < INTENT_COUNT; t++) {
        const char *marker = (t == nlp->intent.primary_intent) ? " **PRIMARY**" : "";
        AP(buf,p,max_len, "| %s | %.0f%%%s |", intent_names[t], nlp->intent.confidence[t]*100, marker);
        for (int s = 0; s < nlp->intent.signal_count[t] && s < 5; s++)
            AP(buf,p,max_len, " `%s`", nlp->intent.signals[t][s]);
        AP(buf,p,max_len, " |\n");
    }
    AP(buf,p,max_len, "\n- **Content Type:** %s\n", nlp->intent.content_type);
    AP(buf,p,max_len, "- **Tone:** %s\n", nlp->intent.tone);
    AP(buf,p,max_len, "- **Recommended:** ~%d words, %d+ headings\n\n",
       nlp->intent.recommended_words, nlp->intent.recommended_headings);

    /* Competitor stats */
    AP(buf,p,max_len, "## Competitor Benchmarks\n\n");
    AP(buf,p,max_len, "| Metric | Value |\n");
    AP(buf,p,max_len, "|--------|------|\n");
    AP(buf,p,max_len, "| Avg word count | %.0f |\n", nlp->stats.avg_word_count);
    AP(buf,p,max_len, "| Word range | %d - %d |\n", nlp->stats.min_word_count, nlp->stats.max_word_count);
    AP(buf,p,max_len, "| Avg H2 sections | %.1f |\n", nlp->stats.avg_h2_count);
    AP(buf,p,max_len, "| Avg H3 sections | %.1f |\n", nlp->stats.avg_h3_count);
    AP(buf,p,max_len, "| Avg outbound links | %.1f |\n\n", nlp->stats.avg_outbound_links);

    /* Keywords */
    if (nlp->keyword_count > 0) {
        AP(buf,p,max_len, "## Semantic Keywords (%d found)\n\n", nlp->keyword_count);
        AP(buf,p,max_len, "| Keyword | Score | Frequency | Source |\n");
        AP(buf,p,max_len, "|---------|-------|-----------|--------|\n");
        for (int i = 0; i < nlp->keyword_count && i < 40; i++)
            AP(buf,p,max_len, "| %s | %.3f | %d | %s |\n",
               nlp->keywords[i].text, nlp->keywords[i].score,
               nlp->keywords[i].frequency, nlp->keywords[i].source);
        AP(buf,p,max_len, "\n");
    }

    /* Headings */
    if (nlp->heading_count > 0) {
        AP(buf,p,max_len, "## Heading Patterns (%d found)\n\n", nlp->heading_count);
        for (int i = 0; i < nlp->heading_count && i < 20; i++) {
            if (nlp->headings[i].count >= 2) {
                const char *prefix = (nlp->headings[i].tag[1] == '2') ? "##" : "###";
                AP(buf,p,max_len, "%s %s *(used by %d pages)*\n",
                   prefix, nlp->headings[i].text, nlp->headings[i].count);
            }
        }
        AP(buf,p,max_len, "\n");
    }

    /* PAA */
    if (nlp->paa_count > 0) {
        AP(buf,p,max_len, "## People Also Ask\n\n");
        for (int i = 0; i < nlp->paa_count; i++)
            AP(buf,p,max_len, "- %s\n", nlp->paa[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Related */
    if (nlp->related_count > 0) {
        AP(buf,p,max_len, "## Related Searches\n\n");
        for (int i = 0; i < nlp->related_count; i++)
            AP(buf,p,max_len, "- %s\n", nlp->related[i]);
        AP(buf,p,max_len, "\n");
    }

    /* Content gap (if computed) */
    if (nlp->gap_computed) {
        AP(buf,p,max_len, "## Content Gap Analysis\n\n");
        AP(buf,p,max_len, "- **Keyword coverage:** %.0f%%\n", nlp->gap.keyword_coverage);
        AP(buf,p,max_len, "- **Heading coverage:** %.0f%%\n", nlp->gap.heading_coverage);
        AP(buf,p,max_len, "- **Your word count:** %d (competitors avg: %.0f)\n",
           nlp->gap.user_word_count, nlp->stats.avg_word_count);
        if (nlp->gap.words_needed > 0)
            AP(buf,p,max_len, "- **Words to add:** %d\n", nlp->gap.words_needed);
        if (nlp->gap.missing_keyword_count > 0) {
            AP(buf,p,max_len, "\n### Missing Keywords\n\n");
            for (int i = 0; i < nlp->gap.missing_keyword_count && i < 20; i++)
                AP(buf,p,max_len, "- %s (score: %.3f)\n",
                   nlp->gap.missing_keywords[i].text, nlp->gap.missing_keywords[i].score);
        }
        if (nlp->gap.missing_heading_count > 0) {
            AP(buf,p,max_len, "\n### Missing Headings\n\n");
            for (int i = 0; i < nlp->gap.missing_heading_count && i < 15; i++)
                AP(buf,p,max_len, "- [%s] %s\n",
                   nlp->gap.missing_headings[i].tag, nlp->gap.missing_headings[i].text);
        }
        AP(buf,p,max_len, "\n");
    }

    /* Prompt */
    AP(buf,p,max_len, "---\n\n## Generated System Prompt\n\n```\n");
    p += prompt_build_full(nlp, buf+p, max_len-p);
    AP(buf,p,max_len, "\n```\n");

    return p;
}

/* ── Content Gap Report ──────────────────────────────────────── */

int prompt_build_gap_report(const NLPResult *nlp, char *buf, int max_len) {
    if (!nlp->gap_computed) {
        return snprintf(buf, max_len, "No content gap analysis available.\n"
                        "Paste your existing content to analyze gaps.");
    }
    int p = 0;
    const NLPContentGap *gap = &nlp->gap;

    AP(buf,p,max_len, "=== CONTENT GAP ANALYSIS: \"%s\" ===\n\n", nlp->keyword);

    AP(buf,p,max_len, "COVERAGE SCORES:\n");
    AP(buf,p,max_len, "  Keyword coverage:  %.0f%% (%d/%d SERP keywords found in your content)\n",
       gap->keyword_coverage,
       nlp->keyword_count - gap->missing_keyword_count, nlp->keyword_count);
    AP(buf,p,max_len, "  Heading coverage:  %.0f%%\n", gap->heading_coverage);
    AP(buf,p,max_len, "  Word count ratio:  %.0f%% (you: %d, competitors avg: %.0f)\n\n",
       gap->word_count_ratio * 100, gap->user_word_count, nlp->stats.avg_word_count);

    if (gap->words_needed > 0)
        AP(buf,p,max_len, "ACTION: Add approximately %d more words.\n\n", gap->words_needed);

    if (gap->missing_keyword_count > 0) {
        AP(buf,p,max_len, "MISSING KEYWORDS (add these to your content):\n");
        for (int i = 0; i < gap->missing_keyword_count && i < 30; i++)
            AP(buf,p,max_len, "  - %s (importance: %.3f)\n",
               gap->missing_keywords[i].text, gap->missing_keywords[i].score);
        AP(buf,p,max_len, "\n");
    }

    if (gap->missing_heading_count > 0) {
        AP(buf,p,max_len, "MISSING HEADINGS (consider adding these sections):\n");
        for (int i = 0; i < gap->missing_heading_count && i < 15; i++)
            AP(buf,p,max_len, "  [%s] %s (in %d competitor pages)\n",
               gap->missing_headings[i].tag, gap->missing_headings[i].text,
               gap->missing_headings[i].count);
    }

    return p;
}
