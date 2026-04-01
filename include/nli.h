#ifndef NLI_H
#define NLI_H

#include "onnx_nlp.h"

/*
 * Natural Language Inference for zero-shot content type classification.
 * Uses distilbart-mnli (BART architecture, GPT-2 BPE tokenizer).
 *
 * Input:  premise + hypothesis → tokenized as <s>premise</s></s>hypothesis</s>
 * Output: logits[3] → [entailment, neutral, contradiction]
 *
 * For content type: run 8 hypotheses per query, pick highest entailment.
 */

/* NLI context (BPE tokenizer state + ONNX session) */
typedef struct NLIContext NLIContext;

/* Init: load vocab.json, merges.txt, nli.onnx from model_dir.
   Shares the ONNX environment from the existing OnnxNLP context.
   Returns 0 on success, -1 on failure. */
int  nli_init(NLIContext **ctx, const char *model_dir, OnnxNLP *onnx_ctx);

/* Classify: run NLI on premise vs hypothesis, return entailment probability [0-1]. */
float nli_entailment(NLIContext *ctx, const char *premise, const char *hypothesis);

/* Zero-shot content type classification.
   Tests query against all content type hypotheses, returns the best type name.
   out_type: buffer for result string, out_confidence: entailment score.
   Returns 1 if classification succeeded, 0 if NLI unavailable. */
int  nli_classify_content_type(NLIContext *ctx, const char *query,
                                char *out_type, int out_size, float *out_confidence);

/* Cleanup */
void nli_shutdown(NLIContext *ctx);

/* Check availability */
int  nli_available(NLIContext *ctx);

#endif
