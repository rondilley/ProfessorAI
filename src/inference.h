/*
 * inference.h -- llama.cpp model loading, generation, and parameter resolution
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_INFERENCE_H
#define PROF_INFERENCE_H

#include "api_types.h"
#include "config.h"
#include "logger.h"

#include <stdint.h>

/* Forward declarations for llama.cpp types */
struct llama_model;
struct llama_context;
struct llama_vocab;

/* Token callback: return 0 to continue, non-zero to abort */
typedef int (*token_callback_t)(const char *token_text, void *user_data);

typedef struct {
    float   temperature;
    float   top_p;
    int32_t top_k;
    float   repeat_penalty;
    float   frequency_penalty;
    float   presence_penalty;
    int32_t max_tokens;
    int32_t max_inference_seconds;
    char    stop[PROF_STOP_MAX][PROF_STOP_LEN];
    int32_t n_stop;
} sample_params_t;

typedef struct {
    struct llama_model       *model;
    struct llama_context     *ctx;
    const struct llama_vocab *vocab;
    char                      chat_template[4096];
    int                       use_chatml_fallback;
    int32_t                   n_ctx;
    int32_t                   n_batch;
    logger_t                 *logger;
} inference_engine_t;

int  inference_init(inference_engine_t *eng, const config_t *cfg, logger_t *lg);
void inference_destroy(inference_engine_t *eng);

int  inference_complete(inference_engine_t *eng, const char *prompt,
                        const sample_params_t *params,
                        volatile int *cancel_flag,
                        token_callback_t cb, void *user_data,
                        int32_t *prompt_tokens, int32_t *completion_tokens,
                        char *finish_reason, size_t fr_len);

int  inference_chat(inference_engine_t *eng,
                    const chat_message_t *messages, int32_t n_messages,
                    const sample_params_t *params,
                    volatile int *cancel_flag,
                    token_callback_t cb, void *user_data,
                    int32_t *prompt_tokens, int32_t *completion_tokens,
                    char *finish_reason, size_t fr_len);

sample_params_t inference_resolve_params(const config_t *cfg,
                                         const chat_request_t *req);

sample_params_t inference_resolve_params_completion(const config_t *cfg,
                                                    const completion_request_t *req);

#endif /* PROF_INFERENCE_H */
