#ifndef ONNX_NLP_H
#define ONNX_NLP_H

#include "nlp.h"

/*
 * ONNX Runtime NLP — specialized model inference for NER and embeddings.
 * GPU (CUDA/DirectML) and CPU support.
 * Models: bert-base-NER (~400MB), all-MiniLM-L6-v2 (~80MB)
 *
 * Falls back gracefully if ONNX Runtime not available (USE_ONNX not defined)
 * or if model files are missing.
 */

typedef struct OnnxNLP OnnxNLP;

/* Initialize ONNX NLP engine. model_dir = path to directory containing .onnx files.
   Returns 0 on success, -1 if ONNX unavailable (continues without it). */
int  onnx_nlp_init(OnnxNLP **ctx, const char *model_dir, int use_gpu);
void onnx_nlp_shutdown(OnnxNLP *ctx);

/* Is ONNX NLP available? (runtime check) */
int  onnx_nlp_available(OnnxNLP *ctx);

/* Check if models are downloaded */
int  onnx_nlp_models_present(const char *model_dir);

/* Download models from Hugging Face. Returns 0 on success.
   progress_cb called with (downloaded_mb, total_mb). */
typedef void (*onnx_download_cb)(double done_mb, double total_mb, void *userdata);
int  onnx_nlp_download_models(const char *model_dir, onnx_download_cb cb, void *userdata);

/* Named Entity Recognition — extract entities from text.
   Returns number of entities found. */
int  onnx_nlp_extract_entities(OnnxNLP *ctx, const char *text,
                                NLPEntity *out, int max_entities);

/* Batch NER across multiple texts, merging results. */
int  onnx_nlp_extract_entities_batch(OnnxNLP *ctx,
                                      const char **texts, const char **domains,
                                      int text_count,
                                      NLPEntity *out, int max_entities);

/* Sentence embedding — compute vector for semantic similarity.
   out_embedding must be at least 384 floats (MiniLM dimension). */
int  onnx_nlp_embed(OnnxNLP *ctx, const char *text,
                     float *out_embedding, int max_dim);

/* Batch sentence embedding — process multiple texts efficiently.
   out_embeddings must be at least text_count * max_dim floats.
   Returns embed dimension (384) on success, 0 on failure. */
int  onnx_nlp_embed_batch(OnnxNLP *ctx, const char **texts, int text_count,
                           float *out_embeddings, int max_dim);

/* Cosine similarity between two embeddings. */
float onnx_nlp_similarity(const float *a, const float *b, int dim);

/* Download cancellation flag -- set to 1 to abort in-flight downloads */
extern volatile int g_download_cancel;

/* Accessors for sharing ONNX Runtime env with NLI module */
void *onnx_nlp_get_api(OnnxNLP *ctx);   /* returns const OrtApi* */
void *onnx_nlp_get_env(OnnxNLP *ctx);   /* returns OrtEnv* */
void *onnx_nlp_get_opts(OnnxNLP *ctx);  /* returns OrtSessionOptions* */

#endif
