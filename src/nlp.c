#include "scraper.h"
#include "nlp.h"
#include "onnx_nlp.h"
#include "nli.h"
#include "app_log.h"
#include "utils.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>

/* Types and qsort comparators */
typedef struct { int idx; float score; } Scored;
typedef struct { char text[128]; int count; int doc_count; } NgramEntry;

static int cmp_scored_desc(const void *a, const void *b) {
    float d = ((const Scored*)b)->score - ((const Scored*)a)->score;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}
static int cmp_ngram_desc(const void *a, const void *b) {
    return ((const NgramEntry*)b)->count - ((const NgramEntry*)a)->count;
}
static int cmp_heading_desc(const void *a, const void *b) {
    return ((const NLPHeadingPattern*)b)->count - ((const NLPHeadingPattern*)a)->count;
}
static int cmp_int_asc(const void *a, const void *b) {
    return *(const int*)a - *(const int*)b;
}
static int cmp_scored_desc_kw(const void *a, const void *b) {
    float d = ((const NLPKeyword*)b)->score - ((const NLPKeyword*)a)->score;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}
typedef struct { char word[64]; int count; } WordFreq;
static int cmp_wordfreq_desc(const void *a, const void *b) {
    return ((const WordFreq*)b)->count - ((const WordFreq*)a)->count;
}

#ifdef _WIN32
#include <windows.h>
#endif

/* ── Stop words (English) ────────────────────────────────────── */

static const char *stop_words[] = {
    /* ── Function words only (~150). Let df*idf handle everything else. ──
       Nouns, verbs, adjectives deliberately NOT here -- they're SEO keywords.
       See Session 3b discussion: 600-word list was killing "gift", "price",
       "brand", "play", "game", "best", "toy", "dad" etc. ─────────────── */

    /* Articles, determiners */
    "a","an","the","this","that","these","those",
    "some","any","every","each","all","both","few","many","much",
    "several","another","other","such","no","not","nor",

    /* Pronouns */
    "i","me","my","myself","we","us","our","ours","ourselves",
    "you","your","yours","yourself","yourselves",
    "he","him","his","himself","she","her","hers","herself",
    "it","its","itself","they","them","their","theirs","themselves",
    "who","whom","whose","which","what",

    /* Prepositions */
    "in","on","at","to","for","from","with","by","of","about",
    "into","through","during","before","after","above","below",
    "between","under","over","out","up","down","off","against",

    /* Conjunctions */
    "and","but","or","so","yet","if","then","than","because",
    "while","when","where","how","why",

    /* Auxiliaries / modals */
    "is","am","are","was","were","be","been","being",
    "has","have","had","having","do","does","did","doing",
    "will","would","shall","should","can","could","may","might","must",
    "don't","doesn't","didn't","won't","wouldn't","can't","couldn't",
    "shouldn't","mustn't","isn't","aren't","wasn't","weren't","hasn't",
    "haven't","hadn't","shan't",

    /* Common filler -- truly zero-meaning in every context */
    "just","very","also","too","here","there","now","then",
    "only","even","still","already","really","well","quite",
    "however","therefore","though","although","yet","since",
    "re","ve","ll","s","t",

    /* Bare contraction fragments -- produced when smart quotes (U+2019)
       split didn't/doesn't/etc. into fragments.  These are always
       auxiliary verb stems with zero keyword value. */
    "didn","doesn","isn","aren","wasn","weren",
    "hasn","haven","hadn","wouldn","couldn","shouldn","mustn","shan",
    "don",  /* from don't */

    /* Everything else (verbs, nouns, adjectives, web chrome, months)
       is handled by the three-signal scoring system:
       - WikiFreq corpus ratio penalizes common English words mathematically
       - BERT similarity ensures only query-relevant words score high
       - No more whack-a-mole stop word editing */

    NULL
};

/* Hash set for O(1) stop word lookup */
#define STOP_HASH_SIZE 1021

typedef struct StopHashNode {
    const char *word;
    struct StopHashNode *next;
} StopHashNode;

static StopHashNode *g_stop_hash[STOP_HASH_SIZE];
static int g_stop_hash_built = 0;

static unsigned int stop_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % STOP_HASH_SIZE;
}

static void build_stop_hash(void) {
    if (g_stop_hash_built) return;
    memset(g_stop_hash, 0, sizeof(g_stop_hash));
    for (int i = 0; stop_words[i]; i++) {
        unsigned int h = stop_hash(stop_words[i]);
        StopHashNode *n = (StopHashNode *)malloc(sizeof(StopHashNode));
        if (!n) continue;
        n->word = stop_words[i];
        n->next = g_stop_hash[h];
        g_stop_hash[h] = n;
    }
    g_stop_hash_built = 1;
}

static int is_stop_word(const char *word) {
    if (!g_stop_hash_built) build_stop_hash();
    unsigned int h = stop_hash(word);
    StopHashNode *n = g_stop_hash[h];
    while (n) {
        if (strcmp(n->word, word) == 0) return 1;
        n = n->next;
    }
    return 0;
}

/* ── Wikipedia word frequency table (replaces stop words for content filtering) ── */

#define WIKI_FREQ_HASH_SIZE 65521  /* prime, ~1.3x 50K entries */

typedef struct WikiFreqNode {
    char word[48];
    int rank;       /* 1 = most common ("the"), 50000 = rarest */
    struct WikiFreqNode *next;
} WikiFreqNode;

static WikiFreqNode *g_wiki_hash[WIKI_FREQ_HASH_SIZE];
static int g_wiki_loaded = 0;
static int g_wiki_count = 0;

static unsigned int wiki_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % WIKI_FREQ_HASH_SIZE;
}

int wiki_freq_load(const char *path) {
    if (g_wiki_loaded) return g_wiki_count;
    memset(g_wiki_hash, 0, sizeof(g_wiki_hash));

    FILE *f = fopen(path, "r");
    if (!f) {
        app_log(LOG_WARN, "WikiFreq: %s not found -- scoring will use TF-IDF only", path);
        return 0;
    }

    char line[256];
    int rank = 0;
    while (fgets(line, sizeof(line), f) && rank < 50000) {
        char word[48];
        long long freq;
        if (sscanf(line, "%47s %lld", word, &freq) != 2) continue;
        rank++;

        /* Lowercase for matching */
        for (char *p = word; *p; p++) *p = (char)tolower((unsigned char)*p);

        unsigned int h = wiki_hash(word);
        WikiFreqNode *n = (WikiFreqNode *)malloc(sizeof(WikiFreqNode));
        if (!n) continue;
        snprintf(n->word, sizeof(n->word), "%s", word);
        n->rank = rank;
        n->next = g_wiki_hash[h];
        g_wiki_hash[h] = n;
    }
    fclose(f);

    g_wiki_count = rank;
    g_wiki_loaded = 1;
    app_log(LOG_INFO, "WikiFreq: loaded %d words from %s", rank, path);
    return rank;
}

int wiki_freq_rank(const char *word) {
    if (!g_wiki_loaded) return -1;
    unsigned int h = wiki_hash(word);
    WikiFreqNode *n = g_wiki_hash[h];
    while (n) {
        if (strcmp(n->word, word) == 0) return n->rank;
        n = n->next;
    }
    return -1;  /* not found -- likely a niche/brand term */
}

/* ── Tokenizer ───────────────────────────────────────────────── */

typedef struct {
    char **tokens;
    int    count;
    int    cap;
} TokenList;

static void token_init(TokenList *tl) {
    tl->count = 0;
    tl->cap = 1024;
    tl->tokens = (char **)malloc(tl->cap * sizeof(char *));
    if (!tl->tokens) { tl->cap = 0; }
}

static void token_add(TokenList *tl, const char *word) {
    if (tl->count >= tl->cap) {
        tl->cap *= 2;
        char **tmp = (char **)realloc(tl->tokens, tl->cap * sizeof(char *));
        if (!tmp) return;
        tl->tokens = tmp;
    }
    tl->tokens[tl->count++] = str_duplicate(word);
}

static void token_free(TokenList *tl) {
    for (int i = 0; i < tl->count; i++) free(tl->tokens[i]);
    free(tl->tokens);
    tl->tokens = NULL;
    tl->count = 0;
}

/* Check for UTF-8 smart quotes: U+2019 (right single quote) = E2 80 99,
   U+2018 (left single quote) = E2 80 98 */
static int is_utf8_apostrophe(const char *p) {
    unsigned char a = (unsigned char)p[0], b = (unsigned char)p[1], c = (unsigned char)p[2];
    return (a == 0xE2 && b == 0x80 && (c == 0x99 || c == 0x98));
}

/* Strip contraction suffix from word containing apostrophe.
   Returns the length of the base word (before apostrophe), or 0 to skip entirely. */
static int strip_contraction(char *word, int len) {
    /* Find last apostrophe */
    int apos = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (word[i] == '\'') { apos = i; break; }
    }
    if (apos < 0) return len;  /* no apostrophe */
    /* Truncate at apostrophe -- keeps base word */
    word[apos] = '\0';
    return apos;
}

static void tokenize(const char *text, TokenList *out) {
    token_init(out);
    char word[64];
    int wlen = 0;

    for (const char *p = text; ; p++) {
        /* Handle UTF-8 smart quotes as ASCII apostrophe */
        if (*p && is_utf8_apostrophe(p)) {
            if (wlen < 62) word[wlen++] = '\'';
            p += 2;  /* skip extra 2 bytes (loop increments past 3rd) */
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '\'') {
            if (wlen < 62) word[wlen++] = (char)tolower((unsigned char)*p);
        } else {
            if (wlen >= 2) {
                word[wlen] = '\0';
                /* Strip contraction suffix (don't->don, home's->home) */
                wlen = strip_contraction(word, wlen);
                if (wlen >= 2 && !is_stop_word(word))
                    token_add(out, word);
            }
            wlen = 0;
            if (*p == '\0') break;
        }
    }
}

/* ── TF-IDF engine ───────────────────────────────────────────── */

/* NOT THREAD SAFE - engine serializes calls to nlp_analyze.
   Hash table for TF-IDF vocab O(1) lookup */
#define TFIDF_HASH_SIZE 16381  /* prime */
typedef struct TfidfHashNode { int idx; struct TfidfHashNode *next; } TfidfHashNode;
static TfidfHashNode *g_tfidf_hash[TFIDF_HASH_SIZE];

static unsigned int tfidf_hash(const char *s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % TFIDF_HASH_SIZE;
}

static int vocab_find(TfidfEngine *eng, const char *word) {
    unsigned int h = tfidf_hash(word);
    TfidfHashNode *n = g_tfidf_hash[h];
    while (n) {
        if (strcmp(eng->vocab[n->idx].word, word) == 0) return n->idx;
        n = n->next;
    }
    return -1;
}

static int vocab_add(TfidfEngine *eng, const char *word) {
    if (eng->vocab_count >= eng->vocab_cap) {
        eng->vocab_cap *= 2;
        VocabEntry *tmp = (VocabEntry *)realloc(eng->vocab, eng->vocab_cap * sizeof(VocabEntry));
        if (!tmp) return -1;
        eng->vocab = tmp;
    }
    int idx = eng->vocab_count++;
    snprintf(eng->vocab[idx].word, sizeof(eng->vocab[idx].word), "%s", word);
    eng->vocab[idx].df = 0;
    eng->vocab[idx].idf = 0;
    /* Insert into hash table */
    unsigned int h = tfidf_hash(word);
    TfidfHashNode *n = (TfidfHashNode *)malloc(sizeof(TfidfHashNode));
    if (!n) { eng->vocab_count--; return -1; }
    n->idx = idx; n->next = g_tfidf_hash[h]; g_tfidf_hash[h] = n;
    return idx;
}

void tfidf_init(TfidfEngine *eng) {
    memset(eng, 0, sizeof(TfidfEngine));
    eng->vocab_cap = 4096;
    eng->vocab = (VocabEntry *)calloc(eng->vocab_cap, sizeof(VocabEntry));
    memset(g_tfidf_hash, 0, sizeof(g_tfidf_hash));
}

void tfidf_free(TfidfEngine *eng) {
    free(eng->vocab);
    eng->vocab = NULL;
    eng->vocab_count = 0;
    /* Free hash table */
    for(int i=0;i<TFIDF_HASH_SIZE;i++){
        TfidfHashNode *n=g_tfidf_hash[i];
        while(n){TfidfHashNode *next=n->next;free(n);n=next;}
        g_tfidf_hash[i]=NULL;
    }
}

void tfidf_add_document(TfidfEngine *eng, const char *text) {
    if (!text || !text[0]) return;
    eng->doc_count++;

    TokenList tokens;
    tokenize(text, &tokens);

    /* Track which words appear in this doc (for DF) */
    int *seen = (int *)calloc(eng->vocab_cap + tokens.count, sizeof(int));
    int seen_cap = eng->vocab_cap + tokens.count;

    for (int i = 0; i < tokens.count; i++) {
        int idx = vocab_find(eng, tokens.tokens[i]);
        if (idx < 0) idx = vocab_add(eng, tokens.tokens[i]);
        /* Ensure seen array is big enough */
        if (idx >= seen_cap) {
            int old_cap = seen_cap;
            seen_cap = idx + 1024;
            int *new_seen = (int *)realloc(seen, seen_cap * sizeof(int));
            if (!new_seen) { free(seen); token_free(&tokens); return; }
            seen = new_seen;
            memset(seen + old_cap, 0, (seen_cap - old_cap) * sizeof(int));
        }
        eng->vocab[idx].tf++;
        if (!seen[idx]) {
            eng->vocab[idx].df++;
            seen[idx] = 1;
        }
    }

    free(seen);
    token_free(&tokens);
}

void tfidf_compute(TfidfEngine *eng) {
    if (eng->doc_count < 1) return;
    for (int i = 0; i < eng->vocab_count; i++) {
        if (eng->vocab[i].df > 0)
            eng->vocab[i].idf = logf((float)eng->doc_count / eng->vocab[i].df);
        else
            eng->vocab[i].idf = 0;
    }
}

int tfidf_top_keywords(TfidfEngine *eng, NLPKeyword *out, int max_count) {
    /* Score = df * idf -- rewards words that are BOTH common across competitors AND
       discriminative vs general English. Pure IDF was wrong (rewarded rarity = garbage).
       #21: df*idf means "gift" in 8/10 pages scores high, "trunk" in 2/10 scores low. */
    Scored *scored = (Scored *)malloc(eng->vocab_count * sizeof(Scored));
    int n = 0;

    for (int i = 0; i < eng->vocab_count; i++) {
        if (eng->vocab[i].df < 2) continue;  /* Must appear in 2+ docs */
        if (strlen(eng->vocab[i].word) < 3) continue;  /* #22: lowered from 4 to 3 */
        /* Score = log(1+tf) * df * idf
           - tf (total mentions) breaks ties among words with same df
           - log dampens tf so a word mentioned 100x doesn't dominate one mentioned 10x
           - df * idf rewards cross-competitor relevance */
        float score = logf(1.0f + (float)eng->vocab[i].tf) * (float)eng->vocab[i].df * eng->vocab[i].idf;
        if (score < 0.1f) continue;  /* permissive -- three-signal rescoring handles final thresholds */
        scored[n].idx = i;
        scored[n].score = score;
        n++;
    }

    /* Sort by score descending */
    qsort(scored, n, sizeof(Scored), cmp_scored_desc);

    int count = n < max_count ? n : max_count;
    for (int i = 0; i < count; i++) {
        snprintf(out[i].text, sizeof(out[i].text), "%s", eng->vocab[scored[i].idx].word);
        snprintf(out[i].source, sizeof(out[i].source), "tfidf");
        out[i].score = scored[i].score;
        out[i].frequency = eng->vocab[scored[i].idx].df;
    }

    free(scored);
    return count;
}

/* ── N-gram extraction ───────────────────────────────────────── */

/* NgramEntry defined at file scope */

/* Hash table for O(1) ngram dedup during extraction */
#define NGRAM_HASH_SIZE 8191  /* prime */
typedef struct NgramHashNode { int idx; struct NgramHashNode *next; } NgramHashNode;

/* Hash set for O(1) permutation dedup in output loop */
#define PERM_HASH_SIZE 1021  /* prime, sized for max_count (~200) */
typedef struct PermHashNode { char key[128]; struct PermHashNode *next; } PermHashNode;

/* Check if a word is a stop/function word (prepositions, articles, conjunctions, etc.)
   Used to reject n-gram fragments like "cryptocurrency as", "as solar" */
static int is_stop_func_word(const char *w) {
    static const char *stops[] = {
        "as","the","a","an","in","on","at","to","for","of","with","by",
        "is","are","was","were","be","and","or","but","not","it",
        "this","that",NULL
    };
    for (int i = 0; stops[i]; i++)
        if (strcmp(w, stops[i]) == 0) return 1;
    return 0;
}

/* Check if an n-gram starts or ends with a stop/function word */
static int ngram_has_stop_boundary(const char *ngram) {
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s", ngram);
    /* Find first word */
    char *first = strtok(tmp, " ");
    if (!first) return 0;
    if (is_stop_func_word(first)) return 1;
    /* Find last word */
    char *last = first;
    char *tok = strtok(NULL, " ");
    while (tok) { last = tok; tok = strtok(NULL, " "); }
    if (is_stop_func_word(last)) return 1;
    return 0;
}

int nlp_extract_ngrams(const char **texts, int text_count,
                       NLPKeyword *out, int max_count) {
    NgramEntry *ngrams = (NgramEntry *)calloc(NLP_MAX_NGRAMS, sizeof(NgramEntry));
    if (!ngrams) return 0;
    int ngram_count = 0;

    /* Build a hash table for ngram dedup */
    NgramHashNode **ngram_ht = (NgramHashNode **)calloc(NGRAM_HASH_SIZE, sizeof(NgramHashNode *));
    if (!ngram_ht) { free(ngrams); return 0; }

    for (int d = 0; d < text_count; d++) {
        TokenList tokens;
        tokenize(texts[d], &tokens);

        /* Bigrams and trigrams */
        for (int size = 2; size <= 3; size++) {
            if (tokens.count < size) continue;  /* Guard: skip if fewer tokens than ngram size */
            for (int i = 0; i <= tokens.count - size; i++) {
                char ngram[128] = "";
                for (int j = 0; j < size; j++) {
                    if (j > 0) strcat(ngram, " ");
                    strncat(ngram, tokens.tokens[i + j], 40);
                }

                /* Hash-based find or add */
                unsigned int h = tfidf_hash(ngram) % NGRAM_HASH_SIZE;
                int found = -1;
                NgramHashNode *hn = ngram_ht[h];
                while (hn) {
                    if (strcmp(ngrams[hn->idx].text, ngram) == 0) { found = hn->idx; break; }
                    hn = hn->next;
                }
                if (found >= 0) {
                    ngrams[found].count++;
                } else if (ngram_count < NLP_MAX_NGRAMS) {
                    snprintf(ngrams[ngram_count].text, 128, "%s", ngram);
                    ngrams[ngram_count].count = 1;
                    ngrams[ngram_count].doc_count = 0;
                    /* Insert into hash table */
                    NgramHashNode *nn = (NgramHashNode *)malloc(sizeof(NgramHashNode));
                    if (nn) { nn->idx = ngram_count; nn->next = ngram_ht[h]; ngram_ht[h] = nn; }
                    ngram_count++;
                }
            }
        }
        token_free(&tokens);
    }

    /* Free ngram hash table */
    for (int i = 0; i < NGRAM_HASH_SIZE; i++) {
        NgramHashNode *n = ngram_ht[i];
        while (n) { NgramHashNode *next = n->next; free(n); n = next; }
    }
    free(ngram_ht);

    /* Sort by count descending */
    qsort(ngrams, ngram_count, sizeof(NgramEntry), cmp_ngram_desc);

    /* Hash set for O(1) permutation dedup (replaces O(n^2) linear scan) */
    PermHashNode **perm_ht = (PermHashNode **)calloc(PERM_HASH_SIZE, sizeof(PermHashNode *));
    if (!perm_ht) { free(ngrams); return 0; }

    int count = 0;
    for (int i = 0; i < ngram_count && count < max_count; i++) {
        if (ngrams[i].count < 2) break;

        /* Skip n-grams shorter than 5 characters total */
        if (strlen(ngrams[i].text) < 5) continue;

        /* Skip n-grams that start or end with a stop/function word
           (e.g. "cryptocurrency as", "as solar", "cost solar") */
        if (ngram_has_stop_boundary(ngrams[i].text)) continue;

        /* Skip n-grams where the same word appears more than once */
        {int has_dup = 0;
        char tmp2[128];
        snprintf(tmp2, sizeof(tmp2), "%s", ngrams[i].text);
        char *words[8]; int nw = 0;
        char *tok2 = strtok(tmp2, " ");
        while (tok2 && nw < 8) { words[nw++] = tok2; tok2 = strtok(NULL, " "); }
        for (int a = 0; a < nw && !has_dup; a++)
            for (int b = a+1; b < nw; b++)
                if (strcmp(words[a], words[b]) == 0) { has_dup = 1; break; }
        if (has_dup) continue;}

        /* Skip n-grams that match known web chrome / boilerplate patterns */
        {static const char *ngram_blacklist[] = {
            /* CTA / navigation */
            "read more", "learn more", "see more", "show more", "load more",
            "view recipe", "go recipe", "make recipe", "get recipe",
            "continue reading", "advertisement continue",
            /* Recipe card metadata */
            "test kitchen", "recipe creator", "kitchen approved",
            "star rating", "reviews average", "reviews contest",
            "contest winner", "winner total", "total time",
            "servings test", "min servings", "mins prep",
            "recipe photo", "recipe advertisement",
            /* Nutrition facts */
            "nutrition facts", "calories fat", "saturated fat",
            "fat saturated", "cholesterol mg", "mg sodium",
            "mg cholesterol", "sodium carbohydrate", "carbohydrate sugars",
            "sugars fiber", "fiber protein", "protein diabetic",
            "diabetic exchanges", "exchanges lean", "lean meat",
            "exchanges starch", "fat mg",
            /* Photo / image credits */
            "getty images", "istockphoto", "shutterstock", "stock photo",
            "food styling", "prop styling", "photo credit",
            "credit photo", "photo taste",
            /* Site branding fragments */
            "editorial team", "informational purposes",
            /* Infrastructure / UI chrome */
            "loading component",
            /* Navigation / template fragments */
            "see full list", "full list best", "list best",
            "view amazon", "history view", "view history",
            "deals drivenbuy", "car deals drivenbuy",
            "use value money", "ease use value", "ease use",
            "value money", "average rating",
            "for sale near", "sale near you",
            "confirm your address", "coverage in",
            "please confirm", "we have coverage",
            NULL
        };
        int chrome = 0;
        for (int b = 0; ngram_blacklist[b]; b++) {
            if (strstr(ngrams[i].text, ngram_blacklist[b])) { chrome = 1; break; }
        }
        if (chrome) continue;}

        /* Reject fragment n-grams: where one word is a generic function word
           paired with a content word (e.g., "tomatoes most", "tomatoes one").
           These are just noise from adjacent non-stop words. */
        {static const char *filler[] = {
            "most","one","like","more","many","much","new","first","last",
            "used","best","good","make","need","easy","get","use","set",
            "way","part","find","try","keep","help","let","see","put",
            "take","come","back","well","long","big","old","high","low",
            "full","right","left","start","end","top","next","sure",
            NULL
        };
        int is_frag = 0;
        char ftmp[128];
        snprintf(ftmp, sizeof(ftmp), "%s", ngrams[i].text);
        char *fw[4]; int fnw = 0;
        char *ftok = strtok(ftmp, " ");
        while (ftok && fnw < 4) { fw[fnw++] = ftok; ftok = strtok(NULL, " "); }
        /* For bigrams: if either word is a filler, it's a fragment */
        if (fnw == 2) {
            for (int f = 0; filler[f] && !is_frag; f++) {
                if (strcmp(fw[0], filler[f]) == 0 || strcmp(fw[1], filler[f]) == 0)
                    is_frag = 1;
            }
        }
        if (is_frag) continue;}

        /* Permutation dedup: "cost solar panel" is same as "solar panel cost".
           Sort words alphabetically and look up sorted key in a hash set (O(1)). */
        {char sorted_key[128] = "";
        char stmp[128];
        snprintf(stmp, sizeof(stmp), "%s", ngrams[i].text);
        char *sw[4]; int snw = 0;
        {char *st = strtok(stmp, " ");
        while (st && snw < 4) { sw[snw++] = st; st = strtok(NULL, " "); }}
        /* Simple bubble sort on word pointers */
        for (int a = 0; a < snw; a++)
            for (int b = a+1; b < snw; b++)
                if (strcmp(sw[a], sw[b]) > 0) { char *t = sw[a]; sw[a] = sw[b]; sw[b] = t; }
        {int sk = 0;
        for (int a = 0; a < snw; a++) {
            if (a > 0) sorted_key[sk++] = ' ';
            int wl = (int)strlen(sw[a]);
            if (sk + wl < 126) { memcpy(sorted_key + sk, sw[a], wl); sk += wl; }
        }
        sorted_key[sk] = '\0';}
        /* O(1) hash lookup instead of linear scan */
        unsigned int ph = 5381;
        {const char *ps = sorted_key;
        while (*ps) { ph = ((ph << 5) + ph) + (unsigned char)*ps++; }}
        ph %= PERM_HASH_SIZE;
        int perm_dup = 0;
        PermHashNode *pn = perm_ht[ph];
        while (pn) {
            if (strcmp(pn->key, sorted_key) == 0) { perm_dup = 1; break; }
            pn = pn->next;
        }
        if (perm_dup) continue;
        /* Insert sorted_key into hash set */
        {PermHashNode *nn = (PermHashNode *)malloc(sizeof(PermHashNode));
        if (nn) { snprintf(nn->key, sizeof(nn->key), "%s", sorted_key); nn->next = perm_ht[ph]; perm_ht[ph] = nn; }}
        }

        snprintf(out[count].text, sizeof(out[count].text), "%s", ngrams[i].text);
        snprintf(out[count].source, sizeof(out[count].source), "ngram");
        out[count].score = (float)ngrams[i].count;
        out[count].frequency = ngrams[i].count;
        count++;
    }

    /* Free permutation hash set */
    for (int i = 0; i < PERM_HASH_SIZE; i++) {
        PermHashNode *n = perm_ht[i];
        while (n) { PermHashNode *next = n->next; free(n); n = next; }
    }
    free(perm_ht);

    free(ngrams);
    return count;
}

/* ── Heading analysis ────────────────────────────────────────── */

int nlp_analyze_headings(const char **heading_texts, const char **heading_tags,
                         int heading_count,
                         NLPHeadingPattern *out, int max_count) {
    static const char *heading_blacklist[] = {
        /* Navigation / site chrome */
        "access denied", "newsletter", "subscribe", "sign up", "about us",
        "contact us", "privacy policy", "terms of service", "cookie policy",
        "follow us", "connect with us", "customer service", "help center",
        "categories", "policies", "our ratings", "newsletter signup",
        "footer", "navigation", "menu", "sidebar", "widget",
        "related posts", "recent posts", "popular posts", "comments",
        "leave a reply", "share this", "advertisement", "sponsored",
        "written by", "author", "contributor", "editor", "guest",
        "table of contents", "skip to content", "main content",
        "read more", "see more", "show more", "load more",
        "you may also like", "recommended for you", "trending now",
        /* Site sections / footer nav */
        "shop", "company", "about", "events", "education",
        "country/region", "select international", "explore more",
        "keep reading", "what to read next", "article sources",
        "more from", "latest comparisons", "recommended posts",
        /* Review template chrome */
        "what we like", "what we don't like", "what you should know",
        "why i liked it", "why i like it", "why we picked",
        "reasons to buy", "reasons to avoid", "pros", "cons",
        /* CTAs / promos / lead gen */
        "get up to", "for a limited time", "looking to expand",
        "nice to meet you", "never miss", "keep in touch",
        "free recipe", "sign-up for", "get new recipes",
        "order my new", "find a recipe", "monthly archive",
        "before you go", "keep building", "build job-ready",
        "welcome", "we'll be right back",
        /* JS rendering failures */
        "loading component", "loading...",
        /* Error pages */
        "we're sorry", "il semble que", "lo sentimos",
        "entschuldigung", "siamo spiacenti",
        /* Recipe / ecommerce chrome */
        "recipe ebook", "ebook bundle", "how we reviewed",
        /* Site branding taglines */
        "honest, objective", "all the latest tech",
        "a top-notch team", "your security is our",
        "lets start learning", "don't settle for",
        /* Booking / app / product widgets */
        "appointment booking", "please contact your",
        "shop in person", "growing zone",
        /* Blog self-promo */
        "life of dozer", "follow our",
        /* Cloudflare / access-denied error pages */
        "unable to access", "access denied", "please verify",
        "ray id", "cloudflare", "just a moment",
        "what can i do to resolve this",
        /* Promotional / discount CTAs */
        "% off", "off your first", "free shipping",
        "use code", "promo code", "limited time offer",
        "sign up and save", "join and save",
        /* Aggregator / e-commerce nav */
        "for sale near you", "find a dealer",
        "popular", "most popular",
        /* Interactive / form elements */
        "reply to comment", "leave a comment", "post a comment",
        "item added to", "add to cart", "add to bag",
        "new online", "just arrived", "in stock",
        "select options", "select size", "select color",
        "write a review", "rate this", "was this helpful",
        /* Site sections that appear as headings */
        "highlights", "quick links", "jump to",
        "most read", "editor picks", "trending",
        "more stories", "latest stories", "latest news",
        "you might also like", "you may also like",
        /* SaaS / app site nav (Canva, resume builders, etc.) */
        "product", "solutions", "features", "pricing",
        "all features", "all solutions", "all resources",
        "all templates", "create guides", "all create guides",
        "download for free", "download canva",
        "digital design", "print design",
        "images and photos", "videos and audio",
        "k-12 education", "higher education",
        "campus solutions", "business plans",
        /* E-commerce nav */
        "track your order", "delivery information",
        "returns", "size guide", "gift cards",
        "customer reviews", "customer service",
        "free delivery", "free returns",
        "buy now", "shop now", "shop all",
        /* Video embed sections */
        "watch the video", "video:", "video meal",
        "video inspiration",
        /* Site-specific chrome that appears in 2+ pages (same site) */
        "purchase options", "best in class",
        "on this page", "in this article",
        "from mayo clinic", "cookies on", "national doctors",
        "advertising", "sponsorship",
        "treatment in ", "training programs",
        "life-changing", "zen habits",
        "nutrition information", "about this recipe",
        /* AI-generated template headings (identical across multiple AI articles) */
        "overview", "key considerations", "in-depth analysis",
        "expert recommendations", "expert recommendation",
        /* Product/service ads masquerading as headings */
        "diagnostic package", "oral appliance",
        /* Site-branded sections */
        "favorite", "vogue",
        /* Site-specific branding that leaks as headings */
        "what makes", "why choose us", "why trust us",
        "our methodology", "how we test", "how we rate",
        "how we evaluate", "how we review", "how we chose",
        "how we selected", "how we picked",
        "meet the team", "meet the expert", "meet our",
        "about the author", "about this article",
        "editorial guidelines", "editorial standards",
        "our editorial", "editorial process",
        "fact-check", "fact check",
        "sources and references", "article sources",
        "references", "sources",
        "frequently asked questions",
        "key takeaways", "bottom line", "the verdict",
        "final thoughts", "final verdict", "our take",
        "updates", "latest updates", "recent updates",
        "related articles", "related content",
        /* Social / sharing */
        "share this", "follow us", "connect with us",
        "facebook", "twitter", "instagram", "pinterest",
        "youtube", "tiktok", "linkedin",
        /* Footer nav */
        "careers", "press", "investor relations",
        "accessibility", "do not sell my personal",
        "cookie preferences", "cookie settings",
        "sitemap", "site map",
        /* Audit 2026-03-31: heading junk found across 30 sessions */
        "thank you for registering", "deals", "shows",
        "our top tested picks", "our top picks", "top picks at a glance",
        "your cart", "just added to your cart", "your cart is empty",
        "why trust", "contact information", "need help",
        "financial tips", "disclosures",
        "deals happening", "best deals",
        "latest stories", "latest news",
        "sections", "more", "audience",
        "follow today", "stay up to date",
        "your facility",
        "our experts",
        NULL
    };

    /* Simple frequency count of heading text + tag combos */
    int pat_count = 0;

    for (int i = 0; i < heading_count && pat_count < max_count; i++) {
        /* Check heading against blacklist */
        {char h_lower[512];
        snprintf(h_lower, sizeof(h_lower), "%s", heading_texts[i]);
        str_to_lower(h_lower);
        int blacklisted = 0;
        for (int b = 0; heading_blacklist[b]; b++) {
            if (strcmp(h_lower, heading_blacklist[b]) == 0) { blacklisted = 1; break; }
            /* Partial match for "written by X", "by X" patterns */
            if (strstr(h_lower, heading_blacklist[b])) { blacklisted = 1; break; }
        }
        if (blacklisted) continue;}

        /* Reject headings ending with a period -- these are scraped sentences, not headings */
        {int hlen2 = (int)strlen(heading_texts[i]);
        if (hlen2 > 0 && heading_texts[i][hlen2 - 1] == '.') continue;}

        /* Reject headings starting with "Get " -- these are CTAs ("Get solar prices", "Get a free quote") */
        if ((heading_texts[i][0] == 'G' || heading_texts[i][0] == 'g') &&
            (heading_texts[i][1] == 'e' || heading_texts[i][1] == 'E') &&
            (heading_texts[i][2] == 't' || heading_texts[i][2] == 'T') &&
            heading_texts[i][3] == ' ') continue;

        /* Reject headings containing registered trademark symbols (brand-specific content) */
        if (strstr(heading_texts[i], "\xC2\xAE") ||   /* UTF-8 (R) */
            strstr(heading_texts[i], "\xE2\x84\xA2") || /* UTF-8 (TM) */
            strchr(heading_texts[i], '\xAE') ||         /* Latin-1 (R) */
            strstr(heading_texts[i], "(R)") ||
            strstr(heading_texts[i], "(TM)") ||
            strstr(heading_texts[i], "(r)") ||
            strstr(heading_texts[i], "(tm)")) continue;

        /* Detect author bylines: 2-3 short words, each capitalized (e.g. "Alexis Konovodoff") */
        {const char *ht = heading_texts[i];
        int word_count = 0, all_caps_start = 1, total_len = (int)strlen(ht);
        if (total_len > 0 && total_len < 40) {  /* bylines are short */
            const char *p = ht;
            while (*p) {
                while (*p && *p == ' ') p++;
                if (!*p) break;
                if (!(*p >= 'A' && *p <= 'Z')) all_caps_start = 0;
                while (*p && *p != ' ') p++;
                word_count++;
            }
            /* 2-3 words, each starting uppercase, no common heading words */
            if (word_count >= 2 && word_count <= 3 && all_caps_start &&
                !strstr(ht, " to ") && !strstr(ht, " for ") && !strstr(ht, " and ") &&
                !strstr(ht, " of ") && !strstr(ht, " the ") && !strstr(ht, " in ") &&
                !strstr(ht, ":") && !strstr(ht, "?") && !strstr(ht, "!")) {
                continue;  /* skip -- looks like a person's name */
            }
        }}

        /* Reject short template headings (e.g. "Benefits:", "How to:", "Overview")
           that appear on many pages — these are content templates, not article structure */
        {int hlen = (int)strlen(heading_texts[i]);
        if (hlen < 15) {
            /* Short headings are only useful if they contain the keyword or a colon-less descriptor */
            int has_colon = (strchr(heading_texts[i], ':') != NULL);
            if (has_colon) continue;  /* "Benefits:", "How to:", "Pros:" = template */
        }}

        /* Find existing pattern */
        int found = -1;
        for (int j = 0; j < pat_count; j++) {
            if (strcmp(out[j].tag, heading_tags[i]) == 0 &&
                str_contains_i(out[j].text, heading_texts[i])) {
                found = j; break;
            }
        }
        if (found >= 0) {
            out[found].count++;
        } else {
            snprintf(out[pat_count].text, sizeof(out[pat_count].text), "%s", heading_texts[i]);
            snprintf(out[pat_count].tag, sizeof(out[pat_count].tag), "%s", heading_tags[i]);
            out[pat_count].count = 1;
            pat_count++;
        }
    }

    /* Sort by count descending */
    qsort(out, pat_count, sizeof(NLPHeadingPattern), cmp_heading_desc);

    return pat_count;
}

/* ── BERT heading relevance pruning ──────────────────────────── */

static void nlp_score_headings_bert(NLPResult *result, OnnxNLP *onnx) {
    if (!onnx || !onnx_nlp_available(onnx)) return;
    if (result->heading_count == 0) return;

    float query_vec[384];
    int dim = onnx_nlp_embed(onnx, result->keyword, query_vec, 384);
    if (dim <= 0) return;

    const char **h_texts = (const char **)malloc(result->heading_count * sizeof(char *));
    float *h_embeds = (float *)calloc((size_t)result->heading_count * 384, sizeof(float));
    if (!h_texts || !h_embeds) { free(h_texts); free(h_embeds); return; }

    for (int i = 0; i < result->heading_count; i++)
        h_texts[i] = result->headings[i].text;

    int edim = onnx_nlp_embed_batch(onnx, h_texts, result->heading_count, h_embeds, 384);
    free(h_texts);
    if (edim <= 0) { free(h_embeds); return; }

    /* Drop headings with low relevance to query.
       Threshold 0.15 is generous — only removes clearly off-topic.
       Headings on 2+ pages are exempt (cross-page consensus is a strong signal). */
    int kept = 0;
    for (int i = 0; i < result->heading_count; i++) {
        float sim = onnx_nlp_similarity(query_vec, &h_embeds[i * 384], edim);
        if (sim >= 0.15f || result->headings[i].count >= 2) {
            if (kept != i) result->headings[kept] = result->headings[i];
            kept++;
        } else {
            app_log(LOG_DEBUG, "NLP: pruned heading '%s' (BERT sim=%.3f)",
                    result->headings[i].text, sim);
        }
    }
    if (kept < result->heading_count)
        app_log(LOG_INFO, "NLP: BERT pruned %d/%d headings",
                result->heading_count - kept, result->heading_count);
    result->heading_count = kept;
    free(h_embeds);
}

/* ── Content stats ───────────────────────────────────────────── */

void nlp_compute_stats(const int *word_counts, const int *heading_counts,
                       const int *h2_counts, const int *h3_counts,
                       const int *outbound_counts, const int *internal_counts,
                       int page_count, NLPContentStats *stats) {
    memset(stats, 0, sizeof(NLPContentStats));
    if (page_count == 0) return;

    int wc_sum = 0, hc_sum = 0, h2_sum = 0, h3_sum = 0, out_sum = 0, int_sum = 0;
    stats->min_word_count = word_counts[0];
    stats->max_word_count = word_counts[0];

    for (int i = 0; i < page_count; i++) {
        wc_sum += word_counts[i];
        hc_sum += heading_counts[i];
        h2_sum += h2_counts[i];
        h3_sum += h3_counts[i];
        out_sum += outbound_counts[i];
        int_sum += internal_counts[i];
        if (word_counts[i] < stats->min_word_count) stats->min_word_count = word_counts[i];
        if (word_counts[i] > stats->max_word_count) stats->max_word_count = word_counts[i];
    }

    stats->avg_word_count = (float)wc_sum / page_count;
    stats->avg_heading_count = (float)hc_sum / page_count;
    stats->avg_h2_count = (float)h2_sum / page_count;
    stats->avg_h3_count = (float)h3_sum / page_count;
    stats->avg_outbound_links = (float)out_sum / page_count;
    stats->avg_internal_links = (float)int_sum / page_count;

    /* Median word count */
    int *sorted = (int *)malloc(page_count * sizeof(int));
    memcpy(sorted, word_counts, page_count * sizeof(int));
    qsort(sorted, page_count, sizeof(int), cmp_int_asc);
    stats->median_word_count = (float)sorted[page_count / 2];
    free(sorted);
}

/* ── Intent classification (full breakdown) ──────────────────── */

static const char *intent_signal_lists[INTENT_COUNT][32] = {
    /* INFORMATIONAL */
    {"what is","how to","why","guide","tutorial","learn","definition",
     "meaning","example","explain","overview","types of","benefits of",
     "difference between","history of","introduction",
     "symptoms","causes","signs","treatment","recipe","ingredients",
     "training","tips","ideas","exercises","remedies","steps", NULL},
    /* COMMERCIAL */
    {"best","top","review","comparison","vs","versus","alternative",
     "cheap","affordable","recommended","rated","pros and cons",
     "which","ranking","compare", NULL},
    /* TRANSACTIONAL */
    {"buy","price","cost","deal","discount","coupon","sale",
     "order","purchase","shop","free shipping","checkout",
     "subscribe","download","sign up", NULL},
    /* NAVIGATIONAL */
    {"login","sign in","official","website","homepage","contact",
     "support","help center","account","dashboard",
     "app","portal", NULL},
};

/* ── Content type names (used by heuristic scorer + NLI override) ── */

#define CONTENT_TYPE_COUNT 8

static const char *content_type_names[CONTENT_TYPE_COUNT] = {
    "how-to", "comparison", "review", "guide",
    "listicle", "article", "recipe", "health guide"
};

void nlp_classify_intent(const char *keyword, const char **texts, int text_count,
                         NLPIntent *intent, OnnxNLP *onnx, NLIContext *nli) {
    memset(intent, 0, sizeof(NLPIntent));

    char combined[8192];
    int clen = snprintf(combined, sizeof(combined), "%s ", keyword);
    for (int i = 0; i < text_count && clen < (int)sizeof(combined) - 500; i++)
        clen += snprintf(combined + clen, sizeof(combined) - clen, "%.500s ", texts[i]);
    str_to_lower(combined);

    /* Extract the keyword portion (first ~256 chars of combined) for Pass 1 */
    char kw_part[256];
    {
        size_t kw_len = strlen(keyword);
        if (kw_len >= sizeof(kw_part)) kw_len = sizeof(kw_part) - 1;
        memcpy(kw_part, combined, kw_len);   /* combined is already lowered */
        kw_part[kw_len] = '\0';
    }

    /* Two-pass intent scoring:
       Pass 1 = keyword only (3.0 pts), Pass 2 = full combined (1.0 pt) */
    float total = 0;
    for (int t = 0; t < INTENT_COUNT; t++) {
        intent->scores[t] = 0;
        intent->signal_count[t] = 0;
        for (int s = 0; intent_signal_lists[t][s]; s++) {
            int matched_kw   = (strstr(kw_part,  intent_signal_lists[t][s]) != NULL);
            int matched_full = (strstr(combined,  intent_signal_lists[t][s]) != NULL);
            if (matched_kw) {
                intent->scores[t] += 3.0f;
                if (intent->signal_count[t] < MAX_INTENT_SIGNALS)
                    snprintf(intent->signals[t][intent->signal_count[t]++],
                             32, "%s", intent_signal_lists[t][s]);
            } else if (matched_full) {
                intent->scores[t] += 1.0f;
                if (intent->signal_count[t] < MAX_INTENT_SIGNALS)
                    snprintf(intent->signals[t][intent->signal_count[t]++],
                             32, "%s", intent_signal_lists[t][s]);
            }
        }
        total += intent->scores[t];
    }

    /* Negative adjustments: strongly informational keywords suppress commercial/transactional */
    {
        static const char *info_overrides[] = {"recipe","recipes","symptoms","training","ideas","tips",NULL};
        for (int i = 0; info_overrides[i]; i++) {
            if (strstr(kw_part, info_overrides[i])) {
                intent->scores[INTENT_TRANSACTIONAL] -= 2.0f;
                intent->scores[INTENT_COMMERCIAL]    -= 2.0f;
                if (intent->scores[INTENT_TRANSACTIONAL] < 0) intent->scores[INTENT_TRANSACTIONAL] = 0;
                if (intent->scores[INTENT_COMMERCIAL]    < 0) intent->scores[INTENT_COMMERCIAL]    = 0;
                break;  /* one match is enough */
            }
        }
    }

    /* Recalculate total after adjustments */
    total = 0;
    for (int t = 0; t < INTENT_COUNT; t++) total += intent->scores[t];

    /* Normalize to confidence percentages */
    if (total < 1.0f) total = 1.0f;
    int best = 0;
    for (int t = 0; t < INTENT_COUNT; t++) {
        intent->confidence[t] = intent->scores[t] / total;
        if (intent->scores[t] > intent->scores[best]) best = t;
    }
    intent->primary_intent = best;

    /* Content type: BERT zero-shot classification (primary) with heuristic fallback.
       The old approach used strstr() on concatenated page text — a single "ingredients"
       from any page triggered "recipe" for carpet cleaning articles. */
    {int type_scores[CONTENT_TYPE_COUNT] = {0};

    /* === Heuristic fallback: query-focused scoring === */
    /* Strong signal from QUERY keywords (+3 each) */
    if (strstr(kw_part, "how to") || strstr(kw_part, "step by step") || strstr(kw_part, "tutorial"))
        type_scores[0] += 3;
    if (strstr(kw_part, " vs ") || strstr(kw_part, "versus") || strstr(kw_part, "comparison") || strstr(kw_part, "compared"))
        type_scores[1] += 3;
    if (strstr(kw_part, "review") || strstr(kw_part, "hands-on") || strstr(kw_part, "verdict"))
        type_scores[2] += 3;
    if (strstr(kw_part, "guide") || strstr(kw_part, "beginner") || strstr(kw_part, "comprehensive"))
        type_scores[3] += 3;
    if (strstr(kw_part, "top ") || strstr(kw_part, "best "))
        type_scores[4] += 3;
    if (strstr(kw_part, "recipe") || strstr(kw_part, "cook") || strstr(kw_part, "bake"))
        type_scores[6] += 4;
    if (strstr(kw_part, "symptoms") || strstr(kw_part, "diagnosis") || strstr(kw_part, "treatment") || strstr(kw_part, "deficiency"))
        type_scores[7] += 4;
    if (strstr(kw_part, "what is") || strstr(kw_part, "what are"))
        { type_scores[3] += 5; type_scores[0] -= 3; }
    if (strstr(kw_part, "cost") || strstr(kw_part, "price") || strstr(kw_part, "how much"))
        { type_scores[3] += 3; type_scores[0] -= 1; }

    /* Weak tiebreaker from page text: +1, but only if 2+ signals from same type.
       Prevents a single stray "ingredients" from flipping the classification. */
    {static const struct { const char *words[6]; int idx; } psig[] = {
        {{"step by step","instructions","tutorial","procedure",NULL},         0},
        {{"vs ","versus","comparison","compared","difference between",NULL},  1},
        {{"review","tested","hands-on","verdict","rating",NULL},              2},
        {{"complete guide","ultimate guide","comprehensive","in-depth",NULL}, 3},
        {{"top ","best ","ranked","picks",NULL},                              4},
        {{"recipe","ingredients","prep time","cook time","servings",NULL},    6},
        {{"symptoms","diagnosis","treatment","side effects","medication",NULL},7},
    };
    for (int s = 0; s < (int)(sizeof(psig)/sizeof(psig[0])); s++) {
        int hits = 0;
        for (int w = 0; psig[s].words[w]; w++)
            if (strstr(combined, psig[s].words[w])) hits++;
        if (hits >= 2) type_scores[psig[s].idx] += 1;
    }}

    type_scores[5] = 1;  /* "article" baseline */

    int best_type = 0;
    for (int i = 1; i < CONTENT_TYPE_COUNT; i++)
        if (type_scores[i] > type_scores[best_type]) best_type = i;
    snprintf(intent->content_type, sizeof(intent->content_type), "%s", content_type_names[best_type]);

    /* === NLI override: distilbart-mnli zero-shot classification === */
    {char nli_type[32]; float nli_conf = 0;
    if (nli_classify_content_type(nli, keyword, nli_type, sizeof(nli_type), &nli_conf))
        snprintf(intent->content_type, sizeof(intent->content_type), "%s", nli_type);}
    }

    /* Tone -- count occurrences, not just presence.
       High-frequency words (you, your) are weighted lower since they
       appear in all web writing.  Rarer signals carry more weight. */
    {int formal = 0, casual = 0, tech = 0;

    /* Strong formal signals (weight 3) */
    const char *formal_strong[] = {"methodology","furthermore","whereas","hereby",
        "notwithstanding","aforementioned","pursuant","henceforth",NULL};
    /* Moderate formal (weight 1) */
    const char *formal_mod[] = {"research","analysis","study","findings","concluded",
        "evidence suggests","data indicates","peer-reviewed","literature",
        "hypothesis","statistically","clinical","efficacy",NULL};

    /* Strong casual signals (weight 3) */
    const char *casual_strong[] = {"honestly","totally","gonna","gotta","awesome",
        "amazing","super easy","no-brainer","game changer","hands down",NULL};
    /* Moderate casual (weight 1) -- "you"/"your" excluded, too universal */
    const char *casual_mod[] = {"let's","easy","simple","quick","tips","hack",
        "check out","don't worry","the thing is","here's the deal",NULL};

    /* Technical (weight 2) */
    const char *tech_w[] = {"api","configure","implementation","algorithm","protocol",
        "framework","repository","middleware","endpoint","regex",
        "compile","runtime","deprecat","refactor",NULL};

    for (int i = 0; formal_strong[i]; i++) if (strstr(combined, formal_strong[i])) formal += 3;
    for (int i = 0; formal_mod[i]; i++)    if (strstr(combined, formal_mod[i])) formal++;
    for (int i = 0; casual_strong[i]; i++) if (strstr(combined, casual_strong[i])) casual += 3;
    for (int i = 0; casual_mod[i]; i++)    if (strstr(combined, casual_mod[i])) casual++;
    for (int i = 0; tech_w[i]; i++)        if (strstr(combined, tech_w[i])) tech += 2;

    if (tech > formal && tech > casual)
        snprintf(intent->tone, sizeof(intent->tone), "technical");
    else if (formal > casual)
        snprintf(intent->tone, sizeof(intent->tone), "formal");
    else
        snprintf(intent->tone, sizeof(intent->tone), "conversational");
    }
}

/* ── Content Gap Analysis ────────────────────────────────────── */

void nlp_content_gap(NLPResult *result, const char *user_content) {
    if (!user_content || !user_content[0]) return;

    NLPContentGap *gap = &result->gap;
    memset(gap, 0, sizeof(NLPContentGap));

    /* Lowercase user content for matching */
    size_t ulen = strlen(user_content);
    char *lower = (char *)malloc(ulen + 1);
    if (!lower) return;
    for (size_t i = 0; i <= ulen; i++)
        lower[i] = (char)tolower((unsigned char)user_content[i]);

    /* Count user's words */
    gap->user_word_count = 0;
    { int in_word = 0;
      for (const char *p = lower; *p; p++) {
          if (isspace((unsigned char)*p)) in_word = 0;
          else if (!in_word) { in_word = 1; gap->user_word_count++; }
      }
    }

    /* Word count ratio vs competitors */
    if (result->stats.avg_word_count > 0)
        gap->word_count_ratio = (float)gap->user_word_count / result->stats.avg_word_count;

    /* Find missing keywords - SERP keywords not in user's content */
    int found = 0;
    for (int i = 0; i < result->keyword_count; i++) {
        char kw_lower[256];
        snprintf(kw_lower, sizeof(kw_lower), "%s", result->keywords[i].text);
        str_to_lower(kw_lower);
        if (strstr(lower, kw_lower)) {
            found++;
        } else if (gap->missing_keyword_count < GAP_MAX_MISSING) {
            gap->missing_keywords[gap->missing_keyword_count++] = result->keywords[i];
        }
    }
    gap->keyword_coverage = result->keyword_count > 0
        ? (float)found / result->keyword_count * 100.0f : 100.0f;

    /* Find missing headings - SERP heading patterns not in user's content */
    int h_found = 0;
    for (int i = 0; i < result->heading_count; i++) {
        if (result->headings[i].count < 2) continue; /* only check popular headings */
        char h_lower[512];
        snprintf(h_lower, sizeof(h_lower), "%s", result->headings[i].text);
        str_to_lower(h_lower);
        if (strstr(lower, h_lower)) {
            h_found++;
        } else if (gap->missing_heading_count < GAP_MAX_MISSING) {
            gap->missing_headings[gap->missing_heading_count++] = result->headings[i];
        }
    }
    int popular_headings = 0;
    for (int i = 0; i < result->heading_count; i++)
        if (result->headings[i].count >= 2) popular_headings++;
    gap->heading_coverage = popular_headings > 0
        ? (float)h_found / popular_headings * 100.0f : 100.0f;

    /* Recommendations */
    int target_words = (int)(result->stats.avg_word_count * 1.1f);
    gap->words_needed = target_words > gap->user_word_count
        ? target_words - gap->user_word_count : 0;
    gap->headings_needed = result->intent.recommended_headings > 0
        ? result->intent.recommended_headings : 5;

    result->gap_computed = 1;
    free(lower);

    app_log(LOG_INFO, "Gap analysis: keyword coverage %.0f%%, heading coverage %.0f%%, "
            "missing %d keywords, %d headings, need %d more words",
            gap->keyword_coverage, gap->heading_coverage,
            gap->missing_keyword_count, gap->missing_heading_count, gap->words_needed);
}

/* ── Full analysis orchestrator ──────────────────────────────── */

/* ── Text cleaning: strip boilerplate lines before NLP ──────── */

/* Returns 1 if the line (lowered) matches any boilerplate pattern */
static int is_boilerplate_line(const char *line_lower, int len) {
    /* ── Attribution / credit lines (any length) ─────────────── */
    if (strstr(line_lower, "getty images")) return 1;
    if (strstr(line_lower, "istockphoto")) return 1;
    if (strstr(line_lower, "shutterstock")) return 1;
    if (strstr(line_lower, "alamy")) return 1;
    if (strstr(line_lower, "adobe stock")) return 1;
    if (strstr(line_lower, "unsplash")) return 1;
    if (strstr(line_lower, "dreamstime")) return 1;
    if (strstr(line_lower, "food styling")) return 1;
    if (strstr(line_lower, "prop styling")) return 1;
    if (strstr(line_lower, "photo credit")) return 1;
    if (strstr(line_lower, "image credit")) return 1;
    if (strstr(line_lower, "courtesy of")) return 1;
    if (strstr(line_lower, "all rights reserved")) return 1;

    /* ── "Byline: Name" patterns — short lines only ─────────── */
    if (len < 100) {
        /* Photo/image credits */
        if (strncmp(line_lower, "photo:", 6) == 0) return 1;
        if (strncmp(line_lower, "photo by", 8) == 0) return 1;
        if (strncmp(line_lower, "photograph:", 11) == 0) return 1;
        if (strncmp(line_lower, "photograph ", 11) == 0) return 1;
        if (strncmp(line_lower, "photography:", 12) == 0) return 1;
        if (strncmp(line_lower, "photographed by", 15) == 0) return 1;
        if (strncmp(line_lower, "illustration:", 13) == 0) return 1;
        if (strncmp(line_lower, "illustrated by", 14) == 0) return 1;
        if (strncmp(line_lower, "graphics by", 11) == 0) return 1;
        if (strncmp(line_lower, "video by", 8) == 0) return 1;
        if (strncmp(line_lower, "credit:", 7) == 0) return 1;
        if (strncmp(line_lower, "image:", 6) == 0) return 1;
        if (strncmp(line_lower, "source:", 7) == 0) return 1;
        if (strncmp(line_lower, "via ", 4) == 0) return 1;
        /* Author/editor/reviewer bylines */
        if (strncmp(line_lower, "written by", 10) == 0) return 1;
        if (strncmp(line_lower, "reviewed by", 11) == 0) return 1;
        if (strncmp(line_lower, "medically reviewed", 18) == 0) return 1;
        if (strncmp(line_lower, "fact-checked by", 15) == 0) return 1;
        if (strncmp(line_lower, "fact checked by", 15) == 0) return 1;
        if (strncmp(line_lower, "edited by", 9) == 0) return 1;
        if (strncmp(line_lower, "updated by", 10) == 0) return 1;
        if (strncmp(line_lower, "reported by", 11) == 0) return 1;
        if (strncmp(line_lower, "contributor:", 12) == 0) return 1;
        /* Author bios: "[Name] is a [title] at [publication]" */
        if (strstr(line_lower, " is a writer") || strstr(line_lower, " is an editor") ||
            strstr(line_lower, " is a reporter") || strstr(line_lower, " is a journalist") ||
            strstr(line_lower, " is a contributor") || strstr(line_lower, " is a freelance"))
            return 1;
    }

    /* ── URLs and file paths ─────────────────────────────────── */
    if (strncmp(line_lower, "http://", 7) == 0 || strncmp(line_lower, "https://", 8) == 0) return 1;
    if (strncmp(line_lower, "mailto:", 7) == 0) return 1;
    if (len < 120 && (strstr(line_lower, ".htm") || strstr(line_lower, ".php") ||
                       strstr(line_lower, ".asp") || strstr(line_lower, ".pdf") ||
                       strstr(line_lower, ".jpg") || strstr(line_lower, ".png") ||
                       strstr(line_lower, ".svg") || strstr(line_lower, "://www.")))
        return 1;

    /* ── Nutrition labels ────────────────────────────────────── */
    if (strstr(line_lower, "nutrition facts")) return 1;
    if (strstr(line_lower, "total fat") && strstr(line_lower, "cholesterol")) return 1;
    if (strstr(line_lower, "saturated fat") && strstr(line_lower, "sodium")) return 1;
    if (strstr(line_lower, "dietary fiber") && strstr(line_lower, "protein")) return 1;
    if (strstr(line_lower, "diabetic exchanges")) return 1;

    /* ── Recipe card metadata ────────────────────────────────── */
    if (strstr(line_lower, "test kitchen approved")) return 1;
    if (strstr(line_lower, "recipe creator")) return 1;

    /* ── CTAs / ads / newsletter ─────────────────────────────── */
    if (strstr(line_lower, "advertisement") && strstr(line_lower, "continue reading")) return 1;
    if (strstr(line_lower, "subscribe to our newsletter")) return 1;
    if (strstr(line_lower, "sign up for our")) return 1;
    if (strstr(line_lower, "get our free")) return 1;
    if (strstr(line_lower, "download our app")) return 1;
    if (strstr(line_lower, "follow us on")) return 1;
    if (strstr(line_lower, "share this article")) return 1;
    if (strstr(line_lower, "print this page")) return 1;
    if (strstr(line_lower, "share on facebook")) return 1;
    if (strstr(line_lower, "share on twitter")) return 1;
    if (strstr(line_lower, "pin it")) return 1;

    /* ── Legal / compliance ──────────────────────────────────── */
    if (strstr(line_lower, "for informational purposes only")) return 1;
    if (strstr(line_lower, "not a solicitation")) return 1;
    if (strstr(line_lower, "should not be construed as")) return 1;
    if (strstr(line_lower, "consult your doctor")) return 1;
    if (strstr(line_lower, "not intended as medical advice")) return 1;
    if (strstr(line_lower, "affiliate links")) return 1;
    if (strstr(line_lower, "we may earn a commission")) return 1;
    if (strstr(line_lower, "we use cookies")) return 1;
    if (strstr(line_lower, "accept all cookies")) return 1;
    if (strstr(line_lower, "privacy policy")) return 1;
    if (strstr(line_lower, "terms of service")) return 1;
    if (strstr(line_lower, "terms and conditions")) return 1;

    /* ── Copyright lines ─────────────────────────────────────── */
    if (len < 80 && (strncmp(line_lower, "\xc2\xa9", 2) == 0 ||    /* © UTF-8 */
                      strncmp(line_lower, "copyright", 9) == 0 ||
                      strncmp(line_lower, "(c) ", 4) == 0))
        return 1;

    /* ── Bot protection / infrastructure ─────────────────────── */
    if (strstr(line_lower, "perimeterx")) return 1;
    if (strstr(line_lower, "javascript is required")) return 1;
    if (strstr(line_lower, "enable javascript")) return 1;
    if (strstr(line_lower, "captcha")) return 1;
    if (strstr(line_lower, "cloudflare")) return 1;

    /* ── Social/engagement counts (short lines) ──────────────── */
    if (len < 40) {
        if (strstr(line_lower, " shares")) return 1;
        if (strstr(line_lower, " likes")) return 1;
        if (strstr(line_lower, " comments")) return 1;
        if (strstr(line_lower, " views")) return 1;
    }

    return 0;
}

/* Clean page text in-place: blank out boilerplate lines with spaces.
   Works line-by-line (splits on \n). Preserves string length for safety. */
static void nlp_clean_text(char *text) {
    char *p = text;
    while (*p) {
        /* Find end of line */
        char *eol = p;
        while (*eol && *eol != '\n') eol++;

        int line_len = (int)(eol - p);
        if (line_len > 0 && line_len < 512) {
            /* Lowercase copy for matching */
            char lbuf[512];
            int j = 0;
            for (int k = 0; k < line_len && k < 511; k++)
                lbuf[j++] = (char)tolower((unsigned char)p[k]);
            lbuf[j] = '\0';

            if (is_boilerplate_line(lbuf, line_len)) {
                /* Blank the line with spaces */
                for (char *c = p; c < eol; c++) *c = ' ';
            }
        }

        p = *eol ? eol + 1 : eol;
    }
}

/* ── Language detection: skip non-English pages ────────────── */

/* Quick heuristic: count common English function words in first ~2000 words.
   English text has 30-50% function words.  Below 8% = likely foreign language. */
static int is_english_text(const char *text) {
    static const char *markers[] = {
        "the","a","an","is","are","was","were","to","of","and",
        "in","for","with","on","at","by","from","this","that","it",
        "not","or","but","as","be","have","has","had","you","your",
        NULL
    };
    int total = 0, hits = 0;
    char word[64];
    int wlen = 0;

    for (const char *p = text; total < 2000; p++) {
        if (isalpha((unsigned char)*p)) {
            if (wlen < 62) word[wlen++] = (char)tolower((unsigned char)*p);
        } else {
            if (wlen >= 1) {
                word[wlen] = '\0';
                total++;
                for (int m = 0; markers[m]; m++) {
                    if (strcmp(word, markers[m]) == 0) { hits++; break; }
                }
            }
            wlen = 0;
            if (*p == '\0') break;
        }
    }
    if (total < 50) return 1;  /* too short to judge -- assume English */
    float ratio = (float)hits / total;
    return ratio >= 0.08f;  /* 8% threshold */
}

int nlp_analyze(const char *keyword, ScrapedPage *pages, int page_count,
                NLPResult *result, OnnxNLP *onnx, NLIContext *nli) {
    /* Preserve PAA/related data (copied by engine before this call) */
    char saved_paa[10][512];
    char saved_paa_snippets[10][1024];
    char saved_paa_links[10][2048];
    int saved_paa_count = result->paa_count;
    char saved_related[20][256];
    int saved_related_count = result->related_count;
    memcpy(saved_paa, result->paa, sizeof(saved_paa));
    memcpy(saved_paa_snippets, result->paa_snippets, sizeof(saved_paa_snippets));
    memcpy(saved_paa_links, result->paa_links, sizeof(saved_paa_links));
    memcpy(saved_related, result->related, sizeof(saved_related));

    memset(result, 0, sizeof(NLPResult));
    snprintf(result->keyword, sizeof(result->keyword), "%s", keyword);

    /* Restore PAA/related */
    result->paa_count = saved_paa_count;
    result->related_count = saved_related_count;
    memcpy(result->paa, saved_paa, sizeof(result->paa));
    memcpy(result->paa_snippets, saved_paa_snippets, sizeof(result->paa_snippets));
    memcpy(result->paa_links, saved_paa_links, sizeof(result->paa_links));
    memcpy(result->related, saved_related, sizeof(result->related));

    if (page_count == 0) return 0;

#ifdef _WIN32
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
#endif

    app_log(LOG_INFO, "NLP: analyzing %d pages for '%s'", page_count, keyword);

    /* Collect texts -- strdup and clean boilerplate before NLP */
    char **cleaned = (char **)malloc(page_count * sizeof(char *));
    const char **texts = (const char **)malloc(page_count * sizeof(char *));
    if (!cleaned || !texts) {
        free(cleaned); free(texts);
        app_log(LOG_ERROR, "NLP: malloc failed for %d pages", page_count);
        return -1;
    }
    int valid = 0;
    int skipped_non_article = 0;
    int skipped_non_english = 0;
    for (int i = 0; i < page_count; i++) {
        if (pages[i].page_text && pages[i].word_count > 50) {
            if (pages[i].is_non_article) { skipped_non_article++; continue; }
            if (!is_english_text(pages[i].page_text)) {
                skipped_non_english++;
                app_log(LOG_INFO, "NLP: skipped non-English page: %s", pages[i].domain);
                continue;
            }
            cleaned[valid] = str_duplicate(pages[i].page_text);
            if (cleaned[valid]) {
                nlp_clean_text(cleaned[valid]);
                texts[valid] = cleaned[valid];
                valid++;
            }
        }
    }
    if (skipped_non_article > 0)
        app_log(LOG_INFO, "NLP: skipped %d non-article pages (product grids, listings)", skipped_non_article);
    if (skipped_non_english > 0)
        app_log(LOG_INFO, "NLP: skipped %d non-English pages", skipped_non_english);

    /* TF-IDF single-word keywords first (most reliable after stop word filtering) */
    TfidfEngine tfidf;
    tfidf_init(&tfidf);
    for (int i = 0; i < valid; i++)
        tfidf_add_document(&tfidf, texts[i]);
    tfidf_compute(&tfidf);
    NLPKeyword *tfidf_kw = (NLPKeyword *)calloc(NLP_MAX_KEYWORDS, sizeof(NLPKeyword));
    if (!tfidf_kw) {
        tfidf_free(&tfidf);
        for (int ci = 0; ci < valid; ci++) free(cleaned[ci]);
        free(cleaned); free(texts);
        return -1;
    }
    int tfidf_count = tfidf_top_keywords(&tfidf, tfidf_kw, NLP_MAX_KEYWORDS);
    tfidf_free(&tfidf);
    for (int i = 0; i < tfidf_count && result->keyword_count < NLP_MAX_KEYWORDS; i++)
        result->keywords[result->keyword_count++] = tfidf_kw[i];
    app_log(LOG_INFO, "NLP: %d TF-IDF keywords (placed first)", tfidf_count);

    /* N-grams after TF-IDF (supplementary multi-word phrases) */
    NLPKeyword ngrams[200];
    int ngram_count = nlp_extract_ngrams(texts, valid, ngrams, 200);
    /* Append all n-grams -- keep multi-word phrases even if they contain TF-IDF words.
       (#16: "sterling silver necklace" kept alongside "silver") */
    int ngrams_added = 0;
    for (int i = 0; i < ngram_count && result->keyword_count < NLP_MAX_KEYWORDS; i++) {
        /* Only skip exact duplicates (same text already in list) */
        int exact_dup = 0;
        for (int j = 0; j < result->keyword_count; j++) {
            if (strcmp(result->keywords[j].text, ngrams[i].text) == 0) { exact_dup = 1; break; }
        }
        if (!exact_dup) {
            result->keywords[result->keyword_count++] = ngrams[i];
            ngrams_added++;
        }
    }
    app_log(LOG_INFO, "NLP: %d n-grams found, %d appended after TF-IDF", ngram_count, ngrams_added);

    /* Headings — apply same page-quality filters as text collection,
       plus heading-to-word ratio filter to catch boilerplate shells
       (e.g. bhf.org.uk: 43 words, 57 nav headings = ratio 1.30). */
    const char **h_texts = NULL;
    const char **h_tags = NULL;
    int *h_pages = NULL;
    int total_headings = 0;
    for (int i = 0; i < page_count; i++) total_headings += pages[i].heading_count;

    if (total_headings > 0) {
        h_texts = (const char **)malloc(total_headings * sizeof(char *));
        h_tags = (const char **)malloc(total_headings * sizeof(char *));
        h_pages = (int *)malloc(total_headings * sizeof(int));
        int idx = 0;
        for (int i = 0; i < page_count; i++) {
            if (!pages[i].page_text || pages[i].word_count <= 50) continue;
            if (pages[i].is_non_article) continue;
            /* Heading/word ratio filter: normal articles 0.01-0.03, boilerplate 0.10+ */
            if (pages[i].word_count > 0) {
                float hw_ratio = (float)pages[i].heading_count / pages[i].word_count;
                if (hw_ratio > 0.10f) {
                    app_log(LOG_INFO, "NLP: skipped headings from %s (h/w ratio %.2f, %dh/%dw)",
                            pages[i].domain, hw_ratio, pages[i].heading_count, pages[i].word_count);
                    continue;
                }
            }
            /* Cap headings per page: no real article has 50+ headings.
               Canva.com had 77 (mostly site navigation inside content area). */
            int max_from_page = pages[i].heading_count < 30 ? pages[i].heading_count : 30;
            for (int j = 0; j < max_from_page; j++) {
                h_texts[idx] = pages[i].headings[j].text;
                h_tags[idx] = pages[i].headings[j].tag;
                h_pages[idx] = i;
                idx++;
            }
        }
        result->heading_count = nlp_analyze_headings(h_texts, h_tags,
                                                      idx,
                                                      result->headings, NLP_MAX_HEADINGS);
        free(h_texts); free(h_tags); free(h_pages);
    }
    app_log(LOG_INFO, "NLP: %d heading patterns found", result->heading_count);
    nlp_score_headings_bert(result, onnx);

    /* Content stats (only include successful pages with word_count > 50) */
    int *wc = (int *)malloc(page_count * sizeof(int));
    int *hc = (int *)malloc(page_count * sizeof(int));
    int *h2c = (int *)calloc(page_count, sizeof(int));
    int *h3c = (int *)calloc(page_count, sizeof(int));
    int *outc = (int *)calloc(page_count, sizeof(int));
    int *intc = (int *)calloc(page_count, sizeof(int));
    if (!wc || !hc || !h2c || !h3c || !outc || !intc) {
        free(wc); free(hc); free(h2c); free(h3c); free(outc); free(intc);
        for (int ci = 0; ci < valid; ci++) free(cleaned[ci]);
        free(cleaned); free(texts); free(tfidf_kw);
        return -1;
    }
    int stats_count = 0;
    for (int i = 0; i < page_count; i++) {
        if (pages[i].word_count <= 50) continue;  /* Skip failed pages */
        wc[stats_count] = pages[i].word_count;
        hc[stats_count] = pages[i].heading_count;
        outc[stats_count] = pages[i].outbound_count;
        intc[stats_count] = pages[i].internal_count;
        h2c[stats_count] = 0;
        h3c[stats_count] = 0;
        for (int j = 0; j < pages[i].heading_count; j++) {
            if (pages[i].headings[j].level == 2) h2c[stats_count]++;
            else if (pages[i].headings[j].level == 3) h3c[stats_count]++;
        }
        stats_count++;
    }
    nlp_compute_stats(wc, hc, h2c, h3c, outc, intc, stats_count, &result->stats);
    free(wc); free(hc); free(h2c); free(h3c); free(outc); free(intc);

    /* Intent */
    nlp_classify_intent(keyword, texts, valid, &result->intent, onnx, nli);
    result->intent.recommended_words = (int)(result->stats.avg_word_count * 1.1f);
    result->intent.recommended_headings = (int)(result->stats.avg_h2_count + 2);
    if (result->intent.recommended_headings < 5) result->intent.recommended_headings = 5;
    if (result->intent.recommended_headings > 15) result->intent.recommended_headings = 15;

    for (int ci = 0; ci < valid; ci++) free(cleaned[ci]);
    free(cleaned); free(texts); free(tfidf_kw);

    /* Three-signal rescoring: wiki freq ratio + BERT similarity + Google PAA/related */
    wiki_freq_load("data/wiki_freq.txt");
    nlp_rescore_keywords(result, onnx);

#ifdef _WIN32
    QueryPerformanceCounter(&t1);
    result->analysis_time = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
#endif

    app_log(LOG_INFO, "NLP: complete in %.1fs - %d keywords, %d headings, intent=%d",
            result->analysis_time, result->keyword_count, result->heading_count,
            result->intent.primary_intent);
    return 0;
}

/* ── Junk keyword filter ─────────────────────────────────────── */

int nlp_is_junk_keyword(const char *text, float score_sem) {
    if (!text) return 1;
    size_t len = strlen(text);

    /* Too short */
    if (len < 2) return 1;

    /* Contains apostrophe */
    if (strchr(text, '\'')) return 1;

    /* Bare possessive ending in 's (check last 2 chars) */
    if (len >= 2 && text[len-2] == '\'' && text[len-1] == 's') return 1;

    /* Lowercase copy for case-insensitive checks */
    char lower[256];
    size_t cpy = len < sizeof(lower)-1 ? len : sizeof(lower)-1;
    for (size_t i = 0; i < cpy; i++)
        lower[i] = (char)tolower((unsigned char)text[i]);
    lower[cpy] = '\0';

    /* URL fragments */
    if (strstr(lower, ".com") || strstr(lower, ".net") ||
        strstr(lower, ".org") || strstr(lower, ".io")  ||
        strstr(lower, ".htm") || strstr(lower, ".html") ||
        strstr(lower, ".php") || strstr(lower, ".asp")  ||
        strstr(lower, "https") || strstr(lower, "http:") ||
        strstr(lower, "www."))
        return 1;

    /* Common contraction fragments */
    static const char *contractions[] = {
        "doesn","isn","wasn","weren","hasn","hadn",
        "wouldn","couldn","shouldn","didn","aren","mustn",
        "ll","ve","re", NULL
    };
    for (int i = 0; contractions[i]; i++)
        if (strcmp(lower, contractions[i]) == 0) return 1;

    /* Website / publisher names (single-word filter).
       Audit 2026-03-31: added forbes, tomshardware, amazon, walmart,
       target, cloudflare, hopkinsmedicine, helpguide, stepchange,
       and other site names that leaked as single keywords. */
    static const char *publishers[] = {
        "nerdwallet","investopedia","pcgamesn","wirecutter",
        "healthline","webmd","bankrate","reddit","quora",
        "wikipedia","youtube","buzzfeed","allrecipes",
        "epicurious","bonappetit","cnet","techradar",
        "tomsguide","tomshardware","verywellhealth","mayoclinic",
        "clevelandclinic","linkedin","coinmarketcap","solarreviews",
        "facebook","twitter","instagram","pinterest","tiktok",
        "glassdoor","yelp","tripadvisor","trustpilot",
        "bbb","crunchbase","zillow","realtor","redfin",
        "zappos","wayfair","etsy","ebay","aliexpress",
        /* Added from audit — leaked as single words in top-200 */
        "forbes","amazon","walmart","target",
        "cloudflare","hopkinsmedicine","helpguide","stepchange",
        "pcmag","laptopmag","tomsguide","tomshardware",
        "verywellfit","verywellmind",
        NULL
    };
    for (int i = 0; publishers[i]; i++)
        if (strcmp(lower, publishers[i]) == 0) return 1;

    /* Stock tickers + crypto tickers */
    static const char *tickers[] = {
        "aapl","msft","wmt","amzn","goog","meta",
        "tsla","nvda","jpm","googl",
        "xrp","eth","btc","bnb","sol","ada","doge",
        "dot","avax","matic","shib",
        NULL
    };
    for (int i = 0; tickers[i]; i++)
        if (strcmp(lower, tickers[i]) == 0) return 1;

    /* Journalist/photographer credits that leak from page bylines */
    if (strncmp(lower, "photograph ", 11) == 0 ||
        strncmp(lower, "photo by ", 9) == 0 ||
        strncmp(lower, "written by ", 11) == 0 ||
        strncmp(lower, "reviewed by ", 12) == 0 ||
        strncmp(lower, "edited by ", 10) == 0 ||
        strncmp(lower, "medically reviewed ", 19) == 0 ||
        strncmp(lower, "fact checked ", 13) == 0 ||
        strncmp(lower, "illustration ", 13) == 0 ||
        strncmp(lower, "credit ", 7) == 0)
        return 1;

    /* Proper name detection: 2-3 capitalized words (e.g. "Julian Chokkattu").
       Two tiers:
       1. Low BERT similarity (< 0.10) + capitalized → definitely junk
       2. Any BERT score + capitalized + NO words in Wikipedia freq table → proper name
          (real content keywords like "Solar Panel" have "solar" and "panel" in wiki;
           person names like "Julian Chokkattu" have neither) */
    if (len < 40 && strchr(text, ' ')) {
        int nwords = 0, all_cap = 1;
        const char *p = text;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (!(*p >= 'A' && *p <= 'Z')) all_cap = 0;
            while (*p && *p != ' ') p++;
            nwords++;
        }
        if (nwords >= 2 && nwords <= 3 && all_cap) {
            /* Tier 1: low BERT — definitely not content */
            if (score_sem < 0.10f) return 1;
            /* Tier 2: check if ANY word is a common English word (wiki freq) */
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", lower);
            int any_wiki = 0;
            char *sv = NULL;
            char *w = strtok_s(tmp, " ", &sv);
            while (w) {
                if (wiki_freq_rank(w) >= 0) { any_wiki = 1; break; }
                w = strtok_s(NULL, " ", &sv);
            }
            if (!any_wiki) return 1;  /* no common words → person name */
        }
    }

    /* Web chrome single words that leak through TF-IDF.
       Only words that are NEVER legitimate content keywords.
       Audit 2026-03-31: added review template, editorial filler,
       web/UI, academic/citation, and publisher terms found across
       30 sessions (see 3-31_issues.md Issues 4-7). */
    static const char *chrome_words[] = {
        /* Original set */
        "faqs","faq","takeaways","takeaway","vetted","sponsored",
        "advertisement","advertiser","affiliate","disclaimer",
        "disclosure","editorial","newsletter","subscribe",
        "subscribers","unsubscribe","podcast","podcasts",
        "copyright","sitemap","login","logout","signup",
        "signin","username","password","captcha",
        "permalink","trackback","pingback","rss",
        "ibotta","yummy",

        /* Review template chrome (8 sessions each for specs/cons/testers) */
        "specs","cons","pros","testers","tester","excels",
        "testimonials","roundup","unboxing","paywall","msrp",

        /* Editorial filler — reviewer voice with zero keyword value */
        "pricier","seamlessly","thankfully","classy","refreshed",
        "shiniest","clunky","impressively","wholeheartedly",
        "bevy","buffs","fooled","whistles","pluses",

        /* Web/UI terms */
        "inbox","tabs","chatbot","browse","opt",

        /* Academic/citation fragments from health authority sites */
        "htm","exp","doi","ncbi","pmc","nih","nccih",
        "eview","addbeh","freephone","helpline","webchat",

        /* Publisher/site names leaking as single words */
        "forbes","cloudflare","hopkinsmedicine","helpguide","stepchange",

        /* Retail cross-sell */
        "tasty","usability",

        NULL
    };
    for (int i = 0; chrome_words[i]; i++)
        if (strcmp(lower, chrome_words[i]) == 0) return 1;

    /* N-gram fragments containing editorial/byline/attribution patterns.
       These leak from article headers, author bios, and review templates. */
    if (strstr(lower, " by ") && (
        strstr(lower, "reviewed by") || strstr(lower, "written by") ||
        strstr(lower, "edited by") || strstr(lower, "tested by") ||
        strstr(lower, "updated by") || strstr(lower, "verified by") ||
        strstr(lower, "checked by") || strstr(lower, "curated by")))
        return 1;

    /* N-grams with generic filler words that add no SEO value.
       "want build", "feel running", "one best", "most budget" etc.
       Only filter when the filler is a component, not the whole keyword. */
    if (strchr(text, ' ')) {
        static const char *fillers[] = {
            "want","feel","most","one","issues",
            "moved","important","great","spp",
            "tested","performed","saved","per",
            "often","include","without","look",
            NULL
        };
        /* Check if n-gram STARTS or ENDS with a filler */
        for (int i = 0; fillers[i]; i++) {
            int flen = (int)strlen(fillers[i]);
            /* starts with "filler " */
            if (strncmp(lower, fillers[i], flen) == 0 && lower[flen] == ' ')
                return 1;
            /* ends with " filler" */
            if (len > (size_t)(flen + 1) &&
                strcmp(lower + len - flen, fillers[i]) == 0 &&
                lower[len - flen - 1] == ' ')
                return 1;
        }
    }

    /* N-grams containing retail/publisher names as a component
       (catches "fryer amazon", "expert clark", "subscribe pack hacker") */
    static const char *publisher_fragments[] = {
        "nerdwallet","investopedia","wirecutter","healthline",
        "webmd","bankrate","buzzfeed","techradar","tomsguide",
        "tomshardware","pcgamesn","allrecipes","epicurious",
        "verywellhealth","mayoclinic","clevelandclinic",
        "coinmarketcap","solarreviews","cnet","forbes",
        "amazon","ebay","walmart","target",
        NULL
    };
    if (strchr(text, ' ')) {
        for (int i = 0; publisher_fragments[i]; i++)
            if (strstr(lower, publisher_fragments[i])) return 1;
    }

    /* Single words with near-zero BERT similarity to the query are off-topic
       (e.g. "towels", "ketchup" from recipe sidebars / product cross-sells).
       Multi-word n-grams already have phrase-level relevance, so skip them. */
    if (!strchr(text, ' ') && score_sem < 0.05f) return 1;

    return 0;
}

/* ── Three-signal keyword rescoring ──────────────────────────── */

void nlp_rescore_keywords(NLPResult *result, OnnxNLP *onnx) {
    if (result->keyword_count == 0) return;

    app_log(LOG_INFO, "Rescoring %d keywords with three-signal system", result->keyword_count);

    /* 1. Embed the search query once */
    float query_vec[384];
    int has_bert = 0;
    if (onnx && onnx_nlp_available(onnx)) {
        int dim = onnx_nlp_embed(onnx, result->keyword, query_vec, 384);
        if (dim > 0) has_bert = 1;
    }
    if (!has_bert)
        app_log(LOG_WARN, "BERT embeddings unavailable -- scoring with frequency ratio + TF-IDF only");

    /* 2. Build PAA/related word set for Google boost */
    #define GOOGLE_HASH_SIZE 1021
    typedef struct GNode { char word[64]; struct GNode *next; } GNode;
    GNode *google_hash[GOOGLE_HASH_SIZE];
    memset(google_hash, 0, sizeof(google_hash));
    {
        /* Tokenize PAA questions and related searches */
        for (int i = 0; i < result->paa_count; i++) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", result->paa[i]);
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            char *tok = strtok(tmp, " ?.,!;:'\"()-");
            while (tok) {
                if (strlen(tok) >= 3) {
                    unsigned int h = stop_hash(tok) % GOOGLE_HASH_SIZE;
                    /* Check if already in set */
                    int exists = 0;
                    GNode *gn = google_hash[h];
                    while (gn) { if (strcmp(gn->word, tok) == 0) { exists = 1; break; } gn = gn->next; }
                    if (!exists) {
                        gn = (GNode *)malloc(sizeof(GNode));
                        if (gn) { snprintf(gn->word, 64, "%s", tok); gn->next = google_hash[h]; google_hash[h] = gn; }
                    }
                }
                tok = strtok(NULL, " ?.,!;:'\"()-");
            }
        }
        for (int i = 0; i < result->related_count; i++) {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", result->related[i]);
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            char *tok = strtok(tmp, " ?.,!;:'\"()-");
            while (tok) {
                if (strlen(tok) >= 3) {
                    unsigned int h = stop_hash(tok) % GOOGLE_HASH_SIZE;
                    int exists = 0;
                    GNode *gn = google_hash[h];
                    while (gn) { if (strcmp(gn->word, tok) == 0) { exists = 1; break; } gn = gn->next; }
                    if (!exists) {
                        gn = (GNode *)malloc(sizeof(GNode));
                        if (gn) { snprintf(gn->word, 64, "%s", tok); gn->next = google_hash[h]; google_hash[h] = gn; }
                    }
                }
                tok = strtok(NULL, " ?.,!;:'\"()-");
            }
        }
    }

    /* 3. Find max TF-IDF score for normalization */
    float max_tfidf = 0.01f;
    for (int i = 0; i < result->keyword_count; i++)
        if (result->keywords[i].score > max_tfidf) max_tfidf = result->keywords[i].score;

    /* 3b. Batch-embed all keywords at once (instead of one ONNX call per keyword) */
    float *all_embeds = NULL;
    int batch_dim = 0;
    if (has_bert) {
        const char **kw_texts = (const char **)malloc(result->keyword_count * sizeof(const char *));
        all_embeds = (float *)calloc((size_t)result->keyword_count * 384, sizeof(float));
        if (kw_texts && all_embeds) {
            for (int i = 0; i < result->keyword_count; i++)
                kw_texts[i] = result->keywords[i].text;
            batch_dim = onnx_nlp_embed_batch(onnx, kw_texts, result->keyword_count, all_embeds, 384);
            if (batch_dim > 0)
                app_log(LOG_INFO, "Batch-embedded %d keywords (dim=%d)", result->keyword_count, batch_dim);
        }
        free(kw_texts);
        if (batch_dim <= 0) {
            /* Batch failed -- free and fall back to per-keyword below */
            free(all_embeds);
            all_embeds = NULL;
            app_log(LOG_WARN, "Batch embedding failed, falling back to per-keyword");
        }
    }

    /* 4. Score each keyword */
    for (int i = 0; i < result->keyword_count; i++) {
        NLPKeyword *kw = &result->keywords[i];

        /* Signal 1: Corpus frequency ratio */
        int rank = wiki_freq_rank(kw->text);
        if (rank > 0)
            kw->score_cfr = sqrtf((float)rank / 50000.0f);
        else
            kw->score_cfr = 0.85f;  /* not in wiki = likely niche/brand term */

        /* Signal 2: BERT semantic similarity */
        kw->score_sem = 0.0f;
        if (has_bert) {
            float *kw_vec = NULL;
            float kw_vec_single[384];
            int dim = 0;

            if (all_embeds && batch_dim > 0) {
                /* Use pre-computed batch embedding */
                kw_vec = &all_embeds[i * 384];
                dim = batch_dim;
            } else {
                /* Fallback: individual embedding call */
                memset(kw_vec_single, 0, sizeof(kw_vec_single));
                dim = onnx_nlp_embed(onnx, kw->text, kw_vec_single, 384);
                kw_vec = kw_vec_single;
            }

            if (dim > 0) {
                float sim = onnx_nlp_similarity(query_vec, kw_vec, dim);
                if (i < 3)
                    app_log(LOG_DEBUG, "BERT sim '%s' vs query: raw=%.4f dim=%d qvec[0]=%.4f kwvec[0]=%.4f",
                            kw->text, sim, dim, query_vec[0], kw_vec[0]);
                /* Clamp to [0,1] */
                if (sim < 0.0f) sim = 0.0f;
                if (sim > 1.0f) sim = 1.0f;
                kw->score_sem = sim;
            }
        }

        /* Signal 3: Google PAA/related boost */
        kw->google_boost = 0.0f;
        {
            /* For n-grams, check if ANY word appears in Google set */
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", kw->text);
            for (char *p = tmp; *p; p++) *p = (char)tolower((unsigned char)*p);
            char *tok = strtok(tmp, " ");
            while (tok) {
                unsigned int h = stop_hash(tok) % GOOGLE_HASH_SIZE;
                GNode *gn = google_hash[h];
                while (gn) {
                    if (strcmp(gn->word, tok) == 0) { kw->google_boost = 0.15f; break; }
                    gn = gn->next;
                }
                if (kw->google_boost > 0) break;
                tok = strtok(NULL, " ");
            }
        }

        /* Normalized TF-IDF */
        kw->score_tfidf = kw->score / max_tfidf;

        /* Combined score */
        if (has_bert) {
            kw->score = (0.35f * kw->score_cfr) + (0.45f * kw->score_sem)
                      + (0.20f * kw->score_tfidf) + kw->google_boost;
        } else {
            kw->score = (0.55f * kw->score_cfr) + (0.30f * kw->score_tfidf)
                      + kw->google_boost;
        }
    }

    free(all_embeds);  /* NULL-safe */

    /* 4b. Zero out junk keywords so they sink below all thresholds */
    for (int i = 0; i < result->keyword_count; i++) {
        if (nlp_is_junk_keyword(result->keywords[i].text, result->keywords[i].score_sem))
            result->keywords[i].score = 0.0f;
    }

    /* 4c. Uniform-frequency boilerplate filter: multi-word keywords that appear
       on 80%+ of pages at similar rates are likely bylines, nav labels, or
       footer text — not real content keywords. */
    {int max_freq = 1;
    for (int i = 0; i < result->keyword_count; i++)
        if (result->keywords[i].frequency > max_freq) max_freq = result->keywords[i].frequency;
    for (int i = 0; i < result->keyword_count; i++) {
        NLPKeyword *kw = &result->keywords[i];
        if (kw->score <= 0) continue;  /* already junked */
        if (!strchr(kw->text, ' ')) continue;  /* single words handled elsewhere */
        float page_ratio = (float)kw->frequency / max_freq;
        if (page_ratio >= 0.80f && kw->score_sem < 0.40f) {
            app_log(LOG_DEBUG, "Boilerplate keyword: '%s' (freq=%d/%d=%.0f%%, sem=%.2f)",
                    kw->text, kw->frequency, max_freq, page_ratio * 100, kw->score_sem);
            kw->score = 0.0f;
        }
    }}

    /* 5. Re-sort by final score descending */
    qsort(result->keywords, result->keyword_count, sizeof(NLPKeyword), cmp_scored_desc_kw);

    /* Log top rescored keywords */
    app_log(LOG_INFO, "Rescored top keywords:");
    for (int i = 0; i < result->keyword_count && i < 20; i++)
        app_log(LOG_INFO, "  %.3f  %s (cfr=%.2f sem=%.2f tfidf=%.2f goog=%.2f src=%s)",
                result->keywords[i].score, result->keywords[i].text,
                result->keywords[i].score_cfr, result->keywords[i].score_sem,
                result->keywords[i].score_tfidf, result->keywords[i].google_boost,
                result->keywords[i].source);

    /* Free Google hash */
    for (int i = 0; i < GOOGLE_HASH_SIZE; i++) {
        GNode *gn = google_hash[i];
        while (gn) { GNode *next = gn->next; free(gn); gn = next; }
    }
}

/* ── Per-page keyword extraction ─────────────────────────────── */

void nlp_extract_page_keywords(ScrapedPage *page) {
    page->page_keyword_count = 0;
    if (!page->page_text || page->word_count <= 50) return;

    /* Tokenize page text and count word frequencies */
    TokenList tokens;
    tokenize(page->page_text, &tokens);

    /* Simple frequency count -- most frequent non-stop words */
    WordFreq *freqs = (WordFreq *)calloc(tokens.count, sizeof(WordFreq));
    if (!freqs) { token_free(&tokens); return; }
    int freq_count = 0;

    for (int i = 0; i < tokens.count; i++) {
        if (strlen(tokens.tokens[i]) < 4) continue;
        /* Find existing */
        int found = -1;
        for (int j = 0; j < freq_count; j++) {
            if (strcmp(freqs[j].word, tokens.tokens[i]) == 0) { found = j; break; }
        }
        if (found >= 0) {
            freqs[found].count++;
        } else if (freq_count < tokens.count) {
            snprintf(freqs[freq_count].word, 64, "%s", tokens.tokens[i]);
            freqs[freq_count].count = 1;
            freq_count++;
        }
    }
    token_free(&tokens);

    /* Sort by frequency descending */
    qsort(freqs, freq_count, sizeof(WordFreq), cmp_wordfreq_desc);

    /* Take top 30 with count >= 2 */
    for (int i = 0; i < freq_count && page->page_keyword_count < 30; i++) {
        if (freqs[i].count < 2) break;
        snprintf(page->page_keywords[page->page_keyword_count], 64, "%s", freqs[i].word);
        page->page_keyword_count++;
    }

    free(freqs);
}

/* ── Session restore - JSON → NLPResult ──────────────────────── */

int nlp_from_json(const char *json, NLPResult *result) {
    memset(result, 0, sizeof(NLPResult));
    cJSON *root = cJSON_Parse(json);
    if (!root) { app_log(LOG_ERROR, "nlp_from_json: parse failed"); return -1; }

    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "keyword")) && cJSON_IsString(j))
        snprintf(result->keyword, sizeof(result->keyword), "%s", j->valuestring);

    /* Intent */
    if ((j = cJSON_GetObjectItem(root, "intent")) && cJSON_IsString(j)) {
        if (strcmp(j->valuestring, "informational") == 0) result->intent.primary_intent = 0;
        else if (strcmp(j->valuestring, "commercial") == 0) result->intent.primary_intent = 1;
        else if (strcmp(j->valuestring, "transactional") == 0) result->intent.primary_intent = 2;
        else result->intent.primary_intent = 3;
    }
    if ((j = cJSON_GetObjectItem(root, "content_type")) && cJSON_IsString(j))
        snprintf(result->intent.content_type, sizeof(result->intent.content_type), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "tone")) && cJSON_IsString(j))
        snprintf(result->intent.tone, sizeof(result->intent.tone), "%s", j->valuestring);
    if ((j = cJSON_GetObjectItem(root, "recommended_words")) && cJSON_IsNumber(j))
        result->intent.recommended_words = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "recommended_headings")) && cJSON_IsNumber(j))
        result->intent.recommended_headings = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "analysis_time")) && cJSON_IsNumber(j))
        result->analysis_time = j->valuedouble;

    /* Intent breakdown — new format (objects with confidence/score/signals) or old (flat numbers) */
    cJSON *ib = cJSON_GetObjectItem(root, "intent_breakdown");
    if (ib) {
        const char *inames[] = {"informational","commercial","transactional","navigational"};
        for (int t = 0; t < INTENT_COUNT; t++) {
            cJSON *entry = cJSON_GetObjectItem(ib, inames[t]);
            if (!entry) continue;
            if (cJSON_IsNumber(entry)) {
                /* Old format: just a number */
                result->intent.confidence[t] = (float)(entry->valuedouble / 100.0);
            } else if (cJSON_IsObject(entry)) {
                /* New format: {confidence, score, signals[]} */
                if ((j = cJSON_GetObjectItem(entry, "confidence")) && cJSON_IsNumber(j))
                    result->intent.confidence[t] = (float)(j->valuedouble / 100.0);
                if ((j = cJSON_GetObjectItem(entry, "score")) && cJSON_IsNumber(j))
                    result->intent.scores[t] = (float)j->valuedouble;
                cJSON *sigs = cJSON_GetObjectItem(entry, "signals");
                if (sigs && cJSON_IsArray(sigs)) {
                    int sc = cJSON_GetArraySize(sigs);
                    if (sc > MAX_INTENT_SIGNALS) sc = MAX_INTENT_SIGNALS;
                    for (int s = 0; s < sc; s++) {
                        cJSON *sig = cJSON_GetArrayItem(sigs, s);
                        if (sig && cJSON_IsString(sig))
                            snprintf(result->intent.signals[t][s], 32, "%s", sig->valuestring);
                    }
                    result->intent.signal_count[t] = sc;
                }
            }
        }
    }

    /* Stats — all fields */
    cJSON *st = cJSON_GetObjectItem(root, "stats");
    if (st) {
        if ((j = cJSON_GetObjectItem(st, "avg_words")) && cJSON_IsNumber(j))
            result->stats.avg_word_count = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "median_words")) && cJSON_IsNumber(j))
            result->stats.median_word_count = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "min_words")) && cJSON_IsNumber(j))
            result->stats.min_word_count = j->valueint;
        if ((j = cJSON_GetObjectItem(st, "max_words")) && cJSON_IsNumber(j))
            result->stats.max_word_count = j->valueint;
        if ((j = cJSON_GetObjectItem(st, "avg_h2")) && cJSON_IsNumber(j))
            result->stats.avg_h2_count = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "avg_h3")) && cJSON_IsNumber(j))
            result->stats.avg_h3_count = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "avg_headings")) && cJSON_IsNumber(j))
            result->stats.avg_heading_count = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "avg_outbound")) && cJSON_IsNumber(j))
            result->stats.avg_outbound_links = (float)j->valuedouble;
        if ((j = cJSON_GetObjectItem(st, "avg_internal")) && cJSON_IsNumber(j))
            result->stats.avg_internal_links = (float)j->valuedouble;
    }

    /* Keywords — with full score breakdown */
    cJSON *kws = cJSON_GetObjectItem(root, "keywords");
    if (kws && cJSON_IsArray(kws)) {
        int n = cJSON_GetArraySize(kws);
        for (int i = 0; i < n && result->keyword_count < NLP_MAX_KEYWORDS; i++) {
            cJSON *item = cJSON_GetArrayItem(kws, i);
            NLPKeyword *kw = &result->keywords[result->keyword_count];
            if ((j = cJSON_GetObjectItem(item, "text")) && cJSON_IsString(j))
                snprintf(kw->text, sizeof(kw->text), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "score")) && cJSON_IsNumber(j))
                kw->score = (float)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "freq")) && cJSON_IsNumber(j))
                kw->frequency = j->valueint;
            /* Source: new format has "src", old format doesn't */
            if ((j = cJSON_GetObjectItem(item, "src")) && cJSON_IsString(j))
                snprintf(kw->source, sizeof(kw->source), "%s", j->valuestring);
            else
                snprintf(kw->source, sizeof(kw->source), "saved");
            /* Score breakdown (new format only) */
            if ((j = cJSON_GetObjectItem(item, "cfr")) && cJSON_IsNumber(j))
                kw->score_cfr = (float)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "sem")) && cJSON_IsNumber(j))
                kw->score_sem = (float)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "tfidf")) && cJSON_IsNumber(j))
                kw->score_tfidf = (float)j->valuedouble;
            if ((j = cJSON_GetObjectItem(item, "boost")) && cJSON_IsNumber(j))
                kw->google_boost = (float)j->valuedouble;
            result->keyword_count++;
        }
    }

    /* Entities — with source_count */
    cJSON *ents = cJSON_GetObjectItem(root, "entities");
    if (ents && cJSON_IsArray(ents)) {
        int n = cJSON_GetArraySize(ents);
        for (int i = 0; i < n && result->entity_count < NLP_MAX_ENTITIES; i++) {
            cJSON *item = cJSON_GetArrayItem(ents, i);
            NLPEntity *e = &result->entities[result->entity_count];
            if ((j = cJSON_GetObjectItem(item, "text")) && cJSON_IsString(j))
                snprintf(e->text, sizeof(e->text), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "label")) && cJSON_IsString(j))
                snprintf(e->label, sizeof(e->label), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "freq")) && cJSON_IsNumber(j))
                e->frequency = j->valueint;
            if ((j = cJSON_GetObjectItem(item, "sources")) && cJSON_IsNumber(j))
                e->source_count = j->valueint;
            result->entity_count++;
        }
    }

    /* Heading patterns */
    cJSON *hps = cJSON_GetObjectItem(root, "heading_patterns");
    if (hps && cJSON_IsArray(hps)) {
        int n = cJSON_GetArraySize(hps);
        for (int i = 0; i < n && result->heading_count < NLP_MAX_HEADINGS; i++) {
            cJSON *item = cJSON_GetArrayItem(hps, i);
            NLPHeadingPattern *h = &result->headings[result->heading_count];
            if ((j = cJSON_GetObjectItem(item, "text")) && cJSON_IsString(j))
                snprintf(h->text, sizeof(h->text), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "tag")) && cJSON_IsString(j))
                snprintf(h->tag, sizeof(h->tag), "%s", j->valuestring);
            if ((j = cJSON_GetObjectItem(item, "count")) && cJSON_IsNumber(j))
                h->count = j->valueint;
            result->heading_count++;
        }
    }

    /* PAA — new format (objects with question/snippet/link) or old (plain strings) */
    cJSON *paa = cJSON_GetObjectItem(root, "people_also_ask");
    if (paa && cJSON_IsArray(paa)) {
        int n = cJSON_GetArraySize(paa);
        for (int i = 0; i < n && result->paa_count < 10; i++) {
            cJSON *item = cJSON_GetArrayItem(paa, i);
            int pi = result->paa_count;
            if (cJSON_IsString(item)) {
                /* Old format: plain string */
                snprintf(result->paa[pi], 512, "%s", item->valuestring);
            } else if (cJSON_IsObject(item)) {
                /* New format: {question, snippet, link} */
                if ((j = cJSON_GetObjectItem(item, "question")) && cJSON_IsString(j))
                    snprintf(result->paa[pi], 512, "%s", j->valuestring);
                if ((j = cJSON_GetObjectItem(item, "snippet")) && cJSON_IsString(j))
                    snprintf(result->paa_snippets[pi], 1024, "%s", j->valuestring);
                if ((j = cJSON_GetObjectItem(item, "link")) && cJSON_IsString(j))
                    snprintf(result->paa_links[pi], 2048, "%s", j->valuestring);
            }
            result->paa_count++;
        }
    }

    /* Related searches */
    cJSON *rel = cJSON_GetObjectItem(root, "related_searches");
    if (rel && cJSON_IsArray(rel)) {
        int n = cJSON_GetArraySize(rel);
        for (int i = 0; i < n && result->related_count < 20; i++) {
            cJSON *item = cJSON_GetArrayItem(rel, i);
            if (cJSON_IsString(item))
                snprintf(result->related[result->related_count++], 256, "%s", item->valuestring);
        }
    }

    /* Compute recommended headings if not set */
    if (result->intent.recommended_headings == 0)
        result->intent.recommended_headings = (int)result->stats.avg_h2_count + 2;
    if (result->intent.recommended_headings < 5)
        result->intent.recommended_headings = 5;

    cJSON_Delete(root);
    app_log(LOG_INFO, "Session restored: '%s' - %d keywords, %d headings, %d entities",
            result->keyword, result->keyword_count, result->heading_count, result->entity_count);
    return 0;
}
