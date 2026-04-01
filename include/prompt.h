#ifndef PROMPT_H
#define PROMPT_H

#include "nlp.h"

#define PROMPT_MAX_LEN  (512 * 1024)  /* 512KB -- enough for 2000 keywords with full score breakdown */

/* Template types */
#define PROMPT_FULL             0
#define PROMPT_KEYWORDS_ONLY    1
#define PROMPT_OUTLINE_ONLY     2
#define PROMPT_COMPETITIVE      3
#define PROMPT_TEMPLATE_COUNT   4

typedef struct {
    int  include_entities;
    int  include_keywords;
    int  include_outline;
    int  include_stats;
    int  include_paa;
    int  include_related;
    int  template_type;     /* PROMPT_FULL, PROMPT_KEYWORDS_ONLY, etc. */
    int  max_keywords;      /* 0 = all, >0 = cap at this count */
    int  max_entities;      /* 0 = all, >0 = cap at this count */
} PromptOptions;

/* Build prompt into buf. Returns length written. */
int  prompt_build(const NLPResult *nlp, const PromptOptions *opts,
                  char *buf, int max_len);

/* Convenience: build specific templates (opts=NULL for defaults) */
int  prompt_build_full(const NLPResult *nlp, char *buf, int max_len);
int  prompt_build_keywords(const NLPResult *nlp, char *buf, int max_len);
int  prompt_build_outline(const NLPResult *nlp, char *buf, int max_len);
int  prompt_build_competitive(const NLPResult *nlp, char *buf, int max_len);

/* Options-aware variants (respect max_keywords, max_entities) */
int  prompt_build_keywords_opts(const NLPResult *nlp, const PromptOptions *opts,
                                char *buf, int max_len);
int  prompt_build_outline_opts(const NLPResult *nlp, const PromptOptions *opts,
                               char *buf, int max_len);
int  prompt_build_competitive_opts(const NLPResult *nlp, const PromptOptions *opts,
                                   char *buf, int max_len);

/* Export as JSON (structured data + all 4 prompts) */
int  prompt_export_json(const NLPResult *nlp, char *buf, int max_len);

/* Export as Markdown (formatted report) */
int  prompt_export_markdown(const NLPResult *nlp, char *buf, int max_len);

/* Export content gap analysis as text */
int  prompt_build_gap_report(const NLPResult *nlp, char *buf, int max_len);

/* Template name string */
const char *prompt_template_name(int template_type);

#endif
