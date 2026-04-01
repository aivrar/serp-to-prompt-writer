/*
 * nli.c — Natural Language Inference for content type classification.
 * Uses distilbart-mnli ONNX model with GPT-2 BPE tokenizer.
 *
 * Zero-shot classification: for each content type, test
 *   premise=query, hypothesis="This text is about {type description}"
 * and pick the type with highest entailment probability.
 */

#include "nli.h"
#include "app_log.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef USE_ONNX
#include <onnxruntime_c_api.h>
#endif

/* ── BPE tokenizer ──────────────────────────────────────────── */

/* GPT-2 byte-to-unicode mapping: maps each byte (0-255) to a Unicode
   code point that won't collide with normal text. */
static int g_byte_to_unicode[256];
static int g_unicode_to_byte[65536];  /* sparse reverse map */

static void init_byte_unicode_map(void) {
    static int done = 0;
    if (done) return;
    /* GPT-2 mapping: printable ASCII stays, others get shifted to 256+ */
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) {
            g_byte_to_unicode[b] = b;
        } else {
            g_byte_to_unicode[b] = 256 + n;
            n++;
        }
    }
    memset(g_unicode_to_byte, -1, sizeof(g_unicode_to_byte));
    for (int b = 0; b < 256; b++)
        g_unicode_to_byte[g_byte_to_unicode[b]] = b;
    done = 1;
}

/* Vocab: token string → ID */
#define BPE_VOCAB_HASH 65521  /* prime */

typedef struct BpeVocabNode {
    char *token;
    int id;
    struct BpeVocabNode *next;
} BpeVocabNode;

/* Merge rules */
typedef struct {
    char *a;
    char *b;
} BpeMerge;

struct NLIContext {
    /* BPE tokenizer */
    BpeVocabNode *vocab_hash[BPE_VOCAB_HASH];
    int vocab_size;
    BpeMerge *merges;
    int merge_count;
    int bos_id, eos_id, pad_id;

    /* ONNX */
#ifdef USE_ONNX
    const OrtApi *api;
    OrtSession *session;
#endif
    int available;
};

/* ── Vocab loading (vocab.json) ─────────────────────────────── */

static unsigned int bpe_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % BPE_VOCAB_HASH;
}

static void vocab_insert(NLIContext *ctx, const char *token, int id) {
    unsigned int h = bpe_hash(token);
    BpeVocabNode *n = (BpeVocabNode *)malloc(sizeof(BpeVocabNode));
    if (!n) return;
    n->token = str_duplicate(token);
    n->id = id;
    n->next = ctx->vocab_hash[h];
    ctx->vocab_hash[h] = n;
}

static int vocab_lookup(NLIContext *ctx, const char *token) {
    unsigned int h = bpe_hash(token);
    BpeVocabNode *n = ctx->vocab_hash[h];
    while (n) {
        if (strcmp(n->token, token) == 0) return n->id;
        n = n->next;
    }
    return -1;
}

/* Simple JSON parser for vocab.json: {"token": id, ...}
   Only handles the flat string→int format. */
static int load_vocab_json(NLIContext *ctx, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = (char *)malloc(sz + 1);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);

    int count = 0;
    char *p = data;
    while (*p) {
        /* Find next quoted key */
        char *q1 = strchr(p, '"');
        if (!q1) break;
        /* Find closing quote, skipping escaped quotes.
           A quote is escaped if preceded by an odd number of backslashes. */
        char *q2 = q1 + 1;
        for (;;) {
            if (!*q2) break;
            if (*q2 == '"') {
                int bs = 0;
                const char *b = q2 - 1;
                while (b >= q1 + 1 && *b == '\\') { bs++; b--; }
                if (bs % 2 == 0) break;  /* even backslashes = real quote */
            }
            q2++;
        }
        if (!*q2) break;
        /* Handle escaped characters in key */
        int klen = (int)(q2 - q1 - 1);
        char key[256];
        if (klen >= (int)sizeof(key)) klen = (int)sizeof(key) - 1;
        /* Copy with escape handling */
        {int ki = 0;
        const char *s = q1 + 1;
        while (s < q2 && ki < (int)sizeof(key) - 1) {
            if (*s == '\\' && s + 1 < q2) {
                s++;
                if (*s == 'n') key[ki++] = '\n';
                else if (*s == 't') key[ki++] = '\t';
                else if (*s == 'u') {
                    /* \uXXXX — decode to UTF-8 */
                    if (s + 4 < q2) {
                        unsigned int cp = 0;
                        for (int j = 1; j <= 4; j++) {
                            char c = s[j];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= c - '0';
                            else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                            else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                        }
                        s += 4;
                        /* UTF-8 encode */
                        if (cp < 0x80) {
                            key[ki++] = (char)cp;
                        } else if (cp < 0x800) {
                            key[ki++] = (char)(0xC0 | (cp >> 6));
                            key[ki++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            key[ki++] = (char)(0xE0 | (cp >> 12));
                            key[ki++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            key[ki++] = (char)(0x80 | (cp & 0x3F));
                        }
                    }
                }
                else key[ki++] = *s;
                s++;
            } else {
                key[ki++] = *s++;
            }
        }
        key[ki] = '\0';}

        /* Find colon then integer value */
        char *colon = strchr(q2 + 1, ':');
        if (!colon) break;
        int id = atoi(colon + 1);
        vocab_insert(ctx, key, id);
        count++;

        /* Move past the value */
        p = colon + 1;
        while (*p && *p != ',' && *p != '}') p++;
        if (*p) p++;
    }
    free(data);
    ctx->vocab_size = count;
    return 0;
}

/* ── Merge rules loading ────────────────────────────────────── */

static int load_merges(NLIContext *ctx, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /* Count lines (skip header) */
    char line[512];
    int count = 0;
    fgets(line, sizeof(line), f);  /* skip "#version: 0.2" */
    while (fgets(line, sizeof(line), f)) count++;
    rewind(f);
    fgets(line, sizeof(line), f);  /* skip header again */

    ctx->merges = (BpeMerge *)calloc(count, sizeof(BpeMerge));
    ctx->merge_count = 0;

    while (fgets(line, sizeof(line), f) && ctx->merge_count < count) {
        /* Each line: "token_a token_b\n" */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        ctx->merges[ctx->merge_count].a = str_duplicate(line);
        ctx->merges[ctx->merge_count].b = str_duplicate(sp + 1);
        ctx->merge_count++;
    }
    fclose(f);
    return 0;
}

/* ── BPE encoding ───────────────────────────────────────────── */

/* Convert a byte string to GPT-2 unicode representation (UTF-8 encoded).
   Each input byte maps to a specific unicode character via g_byte_to_unicode. */
static char *bytes_to_bpe_chars(const char *text, int len) {
    /* Max 4 UTF-8 bytes per character */
    char *out = (char *)malloc(len * 4 + 1);
    if (!out) return NULL;
    int oi = 0;
    for (int i = 0; i < len; i++) {
        int cp = g_byte_to_unicode[(unsigned char)text[i]];
        if (cp < 0x80) {
            out[oi++] = (char)cp;
        } else if (cp < 0x800) {
            out[oi++] = (char)(0xC0 | (cp >> 6));
            out[oi++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[oi++] = (char)(0xE0 | (cp >> 12));
            out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[oi++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[oi] = '\0';
    return out;
}

/* Simple word-level pre-tokenization: split on spaces, prepending Ġ (U+0120)
   to tokens that follow a space (GPT-2 convention). */
typedef struct { char **tokens; int count; int cap; } TokenList;

static void tl_add(TokenList *tl, const char *s) {
    if (tl->count >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 32;
        tl->tokens = (char **)realloc(tl->tokens, tl->cap * sizeof(char *));
    }
    tl->tokens[tl->count++] = str_duplicate(s);
}

static void tl_free(TokenList *tl) {
    for (int i = 0; i < tl->count; i++) free(tl->tokens[i]);
    free(tl->tokens);
    memset(tl, 0, sizeof(*tl));
}

static void pretokenize(const char *text, TokenList *out) {
    memset(out, 0, sizeof(*out));
    const char *p = text;
    while (*p) {
        /* Collect one word (up to next space or end) */
        int has_space = 0;
        if (*p == ' ') { has_space = 1; p++; }
        const char *start = p;
        while (*p && *p != ' ') p++;
        int wlen = (int)(p - start);
        if (wlen == 0) continue;

        /* Convert to BPE chars with optional Ġ prefix */
        char *bpe_word = bytes_to_bpe_chars(start, wlen);
        if (!bpe_word) continue;

        if (has_space) {
            /* Prepend Ġ (U+0120 = 0xC4 0xA0 in UTF-8) */
            char *prefixed = (char *)malloc(strlen(bpe_word) + 3);
            prefixed[0] = (char)0xC4;
            prefixed[1] = (char)0xA0;
            strcpy(prefixed + 2, bpe_word);
            tl_add(out, prefixed);
            free(prefixed);
        } else {
            tl_add(out, bpe_word);
        }
        free(bpe_word);
    }
}

/* Apply BPE merges to a single word token. Modifies token list in place.
   Uses the greedy BPE algorithm: find the highest-priority merge pair,
   apply it, repeat. */
static void apply_bpe(NLIContext *ctx, char **chars, int *nchars) {
    if (*nchars <= 1) return;

    /* Represent the word as a list of subtoken strings */
    /* For efficiency, work with a simple array and compact on merge */
    for (int m = 0; m < ctx->merge_count; m++) {
        const char *ma = ctx->merges[m].a;
        const char *mb = ctx->merges[m].b;

        /* Scan for adjacent pair matching this merge */
        int found = 0;
        for (int i = 0; i < *nchars - 1; i++) {
            if (strcmp(chars[i], ma) == 0 && strcmp(chars[i + 1], mb) == 0) {
                /* Merge: concatenate chars[i] + chars[i+1] */
                char *merged = (char *)malloc(strlen(chars[i]) + strlen(chars[i + 1]) + 1);
                strcpy(merged, chars[i]);
                strcat(merged, chars[i + 1]);
                free(chars[i]);
                free(chars[i + 1]);
                chars[i] = merged;
                /* Shift remaining */
                for (int j = i + 1; j < *nchars - 1; j++)
                    chars[j] = chars[j + 1];
                (*nchars)--;
                found = 1;
                i--;  /* re-check same position */
            }
        }
        if (*nchars <= 1) break;
        (void)found;
    }
}

/* Tokenize text into BPE token IDs. Returns count of tokens. */
static int bpe_encode(NLIContext *ctx, const char *text, int *out_ids, int max_ids) {
    TokenList words;
    pretokenize(text, &words);

    int total = 0;
    for (int w = 0; w < words.count && total < max_ids; w++) {
        /* Split word into individual UTF-8 characters */
        const char *s = words.tokens[w];
        int slen = (int)strlen(s);

        /* Split into individual UTF-8 chars */
        char **chars = (char **)malloc(slen * sizeof(char *));
        int nchars = 0;
        int i = 0;
        while (i < slen) {
            int clen = 1;
            unsigned char c = (unsigned char)s[i];
            if (c >= 0xF0) clen = 4;
            else if (c >= 0xE0) clen = 3;
            else if (c >= 0xC0) clen = 2;
            if (i + clen > slen) clen = slen - i;
            chars[nchars] = (char *)malloc(clen + 1);
            memcpy(chars[nchars], s + i, clen);
            chars[nchars][clen] = '\0';
            nchars++;
            i += clen;
        }

        /* Apply BPE merges */
        apply_bpe(ctx, chars, &nchars);

        /* Look up each subtoken in vocab */
        for (int j = 0; j < nchars && total < max_ids; j++) {
            int id = vocab_lookup(ctx, chars[j]);
            if (id >= 0) {
                out_ids[total++] = id;
            } else {
                /* Unknown token — use <unk> */
                out_ids[total++] = 3;
            }
            free(chars[j]);
        }
        free(chars);
    }

    tl_free(&words);
    return total;
}

/* ── NLI ONNX inference ─────────────────────────────────────── */

/* Content type hypotheses for zero-shot classification */
#define NLI_TYPE_COUNT 8

static const char *nli_type_names[NLI_TYPE_COUNT] = {
    "how-to", "comparison", "review", "guide",
    "listicle", "article", "recipe", "health guide"
};

static const char *nli_hypotheses[NLI_TYPE_COUNT] = {
    "This text is about a step-by-step how-to tutorial.",
    "This text is about a product comparison or versus review.",
    "This text is about a detailed product review with ratings.",
    "This text is about a comprehensive informational guide.",
    "This text is about a ranked list of the best products.",
    "This text is about a general article.",
    "This text is about a cooking recipe.",
    "This text is about a health and medical guide.",
};

/* ── Public API ─────────────────────────────────────────────── */

int nli_init(NLIContext **ctx, const char *model_dir, OnnxNLP *onnx_ctx) {
    *ctx = (NLIContext *)calloc(1, sizeof(NLIContext));
    if (!*ctx) return -1;

    init_byte_unicode_map();

    /* Load vocab */
    char path[512];
    snprintf(path, sizeof(path), "%s/nli_vocab.json", model_dir);
    if (load_vocab_json(*ctx, path) != 0) {
        app_log(LOG_WARN, "NLI: vocab not found: %s", path);
        free(*ctx); *ctx = NULL; return -1;
    }
    app_log(LOG_INFO, "NLI: vocab loaded (%d tokens)", (*ctx)->vocab_size);

    /* Load merges */
    snprintf(path, sizeof(path), "%s/nli_merges.txt", model_dir);
    if (load_merges(*ctx, path) != 0) {
        app_log(LOG_WARN, "NLI: merges not found: %s", path);
        nli_shutdown(*ctx); *ctx = NULL; return -1;
    }
    app_log(LOG_INFO, "NLI: %d merge rules loaded", (*ctx)->merge_count);

    /* Special tokens */
    (*ctx)->bos_id = 0;   /* <s> */
    (*ctx)->eos_id = 2;   /* </s> */
    (*ctx)->pad_id = 1;   /* <pad> */

#ifdef USE_ONNX
    /* Load ONNX model, sharing environment from onnx_nlp */
    const OrtApi *api = (const OrtApi *)onnx_nlp_get_api(onnx_ctx);
    OrtEnv *env = (OrtEnv *)onnx_nlp_get_env(onnx_ctx);
    OrtSessionOptions *opts = (OrtSessionOptions *)onnx_nlp_get_opts(onnx_ctx);

    if (!api || !env || !opts) {
        app_log(LOG_WARN, "NLI: ONNX Runtime not available");
        (*ctx)->available = 0;
        return 0;  /* non-fatal — NLI just won't be used */
    }

    (*ctx)->api = api;

    snprintf(path, sizeof(path), "%s/nli.onnx", model_dir);
    if (!file_exists(path)) {
        app_log(LOG_INFO, "NLI: model not found: %s (requires manual ONNX export — see docs)", path);
        (*ctx)->available = 0;
        return 0;
    }

    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    OrtStatus *status = api->CreateSession(env, wpath, opts, &(*ctx)->session);
    if (status) {
        app_log(LOG_WARN, "NLI: model load failed: %s", api->GetErrorMessage(status));
        api->ReleaseStatus(status);
        (*ctx)->available = 0;
        return 0;
    }
    app_log(LOG_INFO, "NLI: distilbart-mnli model loaded from %s", path);
    (*ctx)->available = 1;
#else
    (*ctx)->available = 0;
#endif
    return 0;
}

int nli_available(NLIContext *ctx) {
    return ctx && ctx->available;
}

float nli_entailment(NLIContext *ctx, const char *premise, const char *hypothesis) {
    if (!ctx || !ctx->available) return -1.0f;

#ifdef USE_ONNX
    /* Tokenize: <s> premise </s></s> hypothesis </s> */
    int ids[512];
    int attn[512];
    int n = 0;

    ids[n++] = ctx->bos_id;  /* <s> */

    /* Encode premise */
    int prem_ids[200];
    int prem_count = bpe_encode(ctx, premise, prem_ids, 200);
    for (int i = 0; i < prem_count && n < 500; i++)
        ids[n++] = prem_ids[i];

    if (n < 512) ids[n++] = ctx->eos_id;  /* </s> */
    if (n < 512) ids[n++] = ctx->eos_id;  /* </s> (BART double separator) */

    /* Encode hypothesis */
    int hyp_ids[200];
    int hyp_count = bpe_encode(ctx, hypothesis, hyp_ids, 200);
    for (int i = 0; i < hyp_count && n < 511; i++)
        ids[n++] = hyp_ids[i];

    if (n < 512) ids[n++] = ctx->eos_id;  /* </s> */

    /* Attention mask: all 1s */
    for (int i = 0; i < n; i++) attn[i] = 1;

    /* Convert to int64 for ONNX */
    int64_t ids64[512], attn64[512];
    for (int i = 0; i < n; i++) { ids64[i] = ids[i]; attn64[i] = attn[i]; }

    /* Run inference */
    const OrtApi *api = ctx->api;
    OrtMemoryInfo *mem_info;
    api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_info);

    int64_t shape[] = {1, n};
    OrtValue *input_ids_val = NULL, *attn_val = NULL;
    api->CreateTensorWithDataAsOrtValue(mem_info, ids64, n * sizeof(int64_t),
                                         shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_ids_val);
    api->CreateTensorWithDataAsOrtValue(mem_info, attn64, n * sizeof(int64_t),
                                         shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &attn_val);

    const char *input_names[] = {"input_ids", "attention_mask"};
    const char *output_names[] = {"logits"};
    OrtValue *inputs[] = {input_ids_val, attn_val};
    OrtValue *output = NULL;

    OrtStatus *status = api->Run(ctx->session, NULL, input_names, (const OrtValue *const *)inputs, 2,
                                  output_names, 1, &output);

    float entailment = -1.0f;
    if (!status && output) {
        float *logits = NULL;
        api->GetTensorMutableData(output, (void **)&logits);
        if (logits) {
            /* Softmax over 3 logits: [entailment, neutral, contradiction] */
            float max_l = logits[0];
            for (int i = 1; i < 3; i++) if (logits[i] > max_l) max_l = logits[i];
            float sum = 0;
            float probs[3];
            for (int i = 0; i < 3; i++) { probs[i] = expf(logits[i] - max_l); sum += probs[i]; }
            for (int i = 0; i < 3; i++) probs[i] /= sum;
            /* BART-MNLI output order: contradiction=0, neutral=1, entailment=2 */
            entailment = probs[2];
        }
        api->ReleaseValue(output);
    } else if (status) {
        app_log(LOG_ERROR, "NLI: inference failed: %s", api->GetErrorMessage(status));
        api->ReleaseStatus(status);
    }

    api->ReleaseValue(input_ids_val);
    api->ReleaseValue(attn_val);
    api->ReleaseMemoryInfo(mem_info);

    return entailment;
#else
    (void)premise; (void)hypothesis;
    return -1.0f;
#endif
}

int nli_classify_content_type(NLIContext *ctx, const char *query,
                               char *out_type, int out_size, float *out_confidence) {
    if (!ctx || !ctx->available) return 0;

    float best_score = -1.0f;
    int best_idx = 5;  /* default: article */

    for (int i = 0; i < NLI_TYPE_COUNT; i++) {
        float score = nli_entailment(ctx, query, nli_hypotheses[i]);
        app_log(LOG_DEBUG, "NLI: '%s' vs '%s' = %.4f", query, nli_type_names[i], score);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_score < 0.10f) {
        app_log(LOG_INFO, "NLI: low confidence (%.3f) for '%s', deferring", best_score, query);
        return 0;
    }

    snprintf(out_type, out_size, "%s", nli_type_names[best_idx]);
    if (out_confidence) *out_confidence = best_score;
    app_log(LOG_INFO, "NLI: '%s' -> '%s' (%.3f)", query, nli_type_names[best_idx], best_score);
    return 1;
}

void nli_shutdown(NLIContext *ctx) {
    if (!ctx) return;
#ifdef USE_ONNX
    if (ctx->session && ctx->api) ctx->api->ReleaseSession(ctx->session);
#endif
    /* Free vocab */
    for (int i = 0; i < BPE_VOCAB_HASH; i++) {
        BpeVocabNode *n = ctx->vocab_hash[i];
        while (n) { BpeVocabNode *next = n->next; free(n->token); free(n); n = next; }
    }
    /* Free merges */
    for (int i = 0; i < ctx->merge_count; i++) {
        free(ctx->merges[i].a);
        free(ctx->merges[i].b);
    }
    free(ctx->merges);
    free(ctx);
}
