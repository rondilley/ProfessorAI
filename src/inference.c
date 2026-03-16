/*
 * inference.c -- llama.cpp model loading, generation, and parameter resolution
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "inference.h"
#include "daemon.h"

#include <llama.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Manual ChatML prompt builder (used when model has no template or
   llama_chat_apply_template returns empty) */
static char *build_chatml_prompt(const chat_message_t *messages, int32_t n,
                                  int32_t *out_len)
{
    /* Calculate needed size */
    size_t needed = 0;
    for (int32_t i = 0; i < n; i++) {
        /* <|im_start|>role\ncontent<|im_end|>\n */
        needed += 13 + strlen(messages[i].role) + 1 +
                  strlen(messages[i].content) + 12 + 1;
    }
    /* <|im_start|>assistant\n */
    needed += 24;

    char *buf = malloc(needed + 1);
    if (!buf) {
        *out_len = 0;
        return NULL;
    }

    size_t pos = 0;
    for (int32_t i = 0; i < n; i++) {
        int w = snprintf(buf + pos, needed + 1 - pos,
                         "<|im_start|>%s\n%s<|im_end|>\n",
                         messages[i].role, messages[i].content);
        if (w > 0) pos += (size_t)w;
    }
    int w = snprintf(buf + pos, needed + 1 - pos, "<|im_start|>assistant\n");
    if (w > 0) pos += (size_t)w;

    *out_len = (int32_t)pos;
    return buf;
}

static float clamp_f(float v, float lo, float hi)
{
    if (isnan(v) || isinf(v)) return lo;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int32_t clamp_i(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double elapsed_seconds(struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int inference_init(inference_engine_t *eng, const config_t *cfg, logger_t *lg)
{
    memset(eng, 0, sizeof(*eng));
    eng->logger = lg;

    llama_backend_init();

    LOG_INFO(lg, "loading model: %s", cfg->model_path);

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = cfg->n_gpu_layers;

    eng->model = llama_model_load_from_file(cfg->model_path, mparams);
    if (!eng->model) {
        LOG_FATAL(lg, "failed to load model: %s", cfg->model_path);
        llama_backend_free();
        return -1;
    }

    eng->vocab = llama_model_get_vocab(eng->model);

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx   = (uint32_t)cfg->n_ctx;
    cparams.n_batch = (uint32_t)cfg->n_batch;

    eng->ctx = llama_init_from_model(eng->model, cparams);
    if (!eng->ctx) {
        LOG_FATAL(lg, "failed to create llama context");
        llama_model_free(eng->model);
        llama_backend_free();
        return -1;
    }

    eng->n_ctx = cfg->n_ctx;
    eng->n_batch = cfg->n_batch;

    /* Get chat template from model metadata */
    const char *tmpl = llama_model_chat_template(eng->model, NULL);
    if (tmpl && tmpl[0] != '\0') {
        snprintf(eng->chat_template, sizeof(eng->chat_template), "%s", tmpl);
        eng->use_chatml_fallback = 0;
        LOG_INFO(lg, "using model chat template (%zu bytes)", strlen(tmpl));
    } else {
        eng->chat_template[0] = '\0';
        eng->use_chatml_fallback = 1;
        LOG_WARN(lg, "model has no chat template, using ChatML fallback");
    }

    LOG_INFO(lg, "model loaded: n_ctx=%d, n_gpu_layers=%d",
             cfg->n_ctx, cfg->n_gpu_layers);

    return 0;
}

void inference_destroy(inference_engine_t *eng)
{
    if (eng->ctx) {
        llama_free(eng->ctx);
        eng->ctx = NULL;
    }
    if (eng->model) {
        llama_model_free(eng->model);
        eng->model = NULL;
    }
    llama_backend_free();
}

int inference_complete(inference_engine_t *eng, const char *prompt,
                       const sample_params_t *params,
                       volatile int *cancel_flag,
                       token_callback_t cb, void *user_data,
                       int32_t *out_prompt_tokens, int32_t *out_completion_tokens,
                       char *finish_reason, size_t fr_len)
{
    snprintf(finish_reason, fr_len, "%s", "length");
    *out_prompt_tokens = 0;
    *out_completion_tokens = 0;

    /* Clear KV cache */
    llama_memory_clear(llama_get_memory(eng->ctx), true);

    /* Tokenize */
    int32_t n_prompt_max = eng->n_ctx;
    llama_token *tokens = malloc(sizeof(llama_token) * (size_t)n_prompt_max);
    if (!tokens) {
        snprintf(finish_reason, fr_len, "%s", "backend_error");
        return -1;
    }

    int32_t n_prompt = llama_tokenize(eng->vocab, prompt, (int32_t)strlen(prompt),
                                       tokens, n_prompt_max, false, true);
    if (n_prompt < 0) {
        LOG_ERROR(eng->logger, "tokenization failed");
        free(tokens);
        snprintf(finish_reason, fr_len, "%s", "backend_error");
        return -1;
    }

    /* Context overflow check */
    if (n_prompt >= eng->n_ctx) {
        LOG_WARN(eng->logger, "prompt too long: %d tokens >= n_ctx %d",
                 n_prompt, eng->n_ctx);
        free(tokens);
        snprintf(finish_reason, fr_len, "%s", "context_overflow");
        return -1;
    }

    *out_prompt_tokens = n_prompt;

    /* Cap max_tokens to available context */
    int32_t max_gen = params->max_tokens;
    if (n_prompt + max_gen > eng->n_ctx) {
        max_gen = eng->n_ctx - n_prompt;
    }
    if (max_gen <= 0) {
        free(tokens);
        snprintf(finish_reason, fr_len, "%s", "context_overflow");
        return -1;
    }

    /* Process prompt in batches of n_batch */
    {
        int32_t n_processed = 0;
        while (n_processed < n_prompt) {
            int32_t chunk = n_prompt - n_processed;
            if (chunk > eng->n_batch) chunk = eng->n_batch;

            struct llama_batch batch = llama_batch_get_one(
                tokens + n_processed, chunk);
            int rc = llama_decode(eng->ctx, batch);
            if (rc != 0) {
                LOG_ERROR(eng->logger, "llama_decode (prompt chunk at %d) failed: %d",
                          n_processed, rc);
                free(tokens);
                snprintf(finish_reason, fr_len, "%s",
                         rc == 1 ? "context_overflow" : "backend_error");
                return -1;
            }
            n_processed += chunk;
        }
    }

    free(tokens);

    /* Build sampler chain */
    struct llama_sampler_chain_params sparams = llama_sampler_chain_default_params();
    struct llama_sampler *chain = llama_sampler_chain_init(sparams);

    if (params->temperature <= 0.0f) {
        /* Greedy */
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    } else {
        if (params->repeat_penalty > 1.0f ||
            params->frequency_penalty != 0.0f ||
            params->presence_penalty != 0.0f) {
            llama_sampler_chain_add(chain,
                llama_sampler_init_penalties(
                    256,                        /* last_n */
                    params->repeat_penalty,
                    params->frequency_penalty,
                    params->presence_penalty));
        }
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(params->top_k));
        llama_sampler_chain_add(chain, llama_sampler_init_top_p(params->top_p, 1));
        llama_sampler_chain_add(chain, llama_sampler_init_temp(params->temperature));
        llama_sampler_chain_add(chain, llama_sampler_init_dist(0));
    }

    /* Generation loop */
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    llama_token eos = llama_vocab_eos(eng->vocab);

    /* Rolling window buffer for stop sequence checking */
    char stop_window[PROF_STOP_LEN + 1];
    int stop_window_len = 0;
    memset(stop_window, 0, sizeof(stop_window));

    int32_t n_generated = 0;

    for (int32_t i = 0; i < max_gen; i++) {
        /* Cancellation checks */
        if (g_shutdown_requested) {
            snprintf(finish_reason, fr_len, "%s", "abort");
            break;
        }
        if (cancel_flag && *cancel_flag) {
            snprintf(finish_reason, fr_len, "%s", "abort");
            break;
        }
        if (elapsed_seconds(&start_time) > (double)params->max_inference_seconds) {
            snprintf(finish_reason, fr_len, "%s", "time_limit");
            break;
        }

        /* Sample */
        llama_token token = llama_sampler_sample(chain, eng->ctx, -1);
        llama_sampler_accept(chain, token);

        /* EOS check */
        if (token == eos) {
            snprintf(finish_reason, fr_len, "%s", "stop");
            break;
        }

        /* Token to text */
        char piece[256];
        int32_t piece_len = llama_token_to_piece(eng->vocab, token, piece,
                                                  sizeof(piece) - 1, 0, true);
        if (piece_len < 0) {
            piece_len = 0;
        }
        piece[piece_len] = '\0';

        n_generated++;

        /* Stop sequence check (rolling window) */
        int stop_matched = 0;
        if (params->max_tokens > 0) { /* only if we have stop seqs via the request */
            /* Append to rolling window */
            int plen = piece_len;
            if (stop_window_len + plen >= (int)sizeof(stop_window)) {
                /* Shift window */
                int shift = stop_window_len + plen - (int)sizeof(stop_window) + 1;
                if (shift > 0 && shift < stop_window_len) {
                    memmove(stop_window, stop_window + shift,
                            (size_t)(stop_window_len - shift));
                    stop_window_len -= shift;
                } else {
                    stop_window_len = 0;
                }
            }
            memcpy(stop_window + stop_window_len, piece, (size_t)plen);
            stop_window_len += plen;
            stop_window[stop_window_len] = '\0';

            /* Check each stop sequence as suffix */
            /* Note: stop sequences come from the chat_request, not sample_params.
               We check them here via a simple pattern -- the caller passes stop
               sequences through the user_data or we'd need to thread them through.
               For now, stop sequences are not checked here -- they are checked in
               the worker layer which has access to the request. */
            (void)stop_matched;
        }

        /* Callback */
        if (cb) {
            int cb_rc = cb(piece, user_data);
            if (cb_rc != 0) {
                snprintf(finish_reason, fr_len, "%s", "abort");
                break;
            }
        }

        /* Decode next token */
        struct llama_batch one = llama_batch_get_one(&token, 1);
        int dec_rc = llama_decode(eng->ctx, one);
        if (dec_rc != 0) {
            LOG_ERROR(eng->logger, "llama_decode (token %d) failed: %d", i, dec_rc);
            snprintf(finish_reason, fr_len, "%s", "backend_error");
            break;
        }
    }

    *out_completion_tokens = n_generated;

    llama_sampler_free(chain);

    return 0;
}

int inference_chat(inference_engine_t *eng,
                   const chat_message_t *messages, int32_t n_messages,
                   const sample_params_t *params,
                   volatile int *cancel_flag,
                   token_callback_t cb, void *user_data,
                   int32_t *prompt_tokens, int32_t *completion_tokens,
                   char *finish_reason, size_t fr_len)
{
    char *formatted = NULL;

    if (eng->use_chatml_fallback) {
        /* Build ChatML prompt manually */
        int32_t flen = 0;
        formatted = build_chatml_prompt(messages, n_messages, &flen);
        if (!formatted || flen == 0) {
            free(formatted);
            snprintf(finish_reason, fr_len, "%s", "backend_error");
            return -1;
        }
        if (flen > PROF_MAX_PROMPT_BYTES) {
            LOG_WARN(eng->logger, "rendered prompt too large: %d bytes > %d",
                     flen, PROF_MAX_PROMPT_BYTES);
            free(formatted);
            snprintf(finish_reason, fr_len, "%s", "context_overflow");
            return -1;
        }
    } else {
        /* Use llama_chat_apply_template with model's template */
        struct llama_chat_message *chat_msgs = malloc(
            sizeof(struct llama_chat_message) * (size_t)n_messages);
        if (!chat_msgs) {
            snprintf(finish_reason, fr_len, "%s", "backend_error");
            return -1;
        }

        for (int32_t i = 0; i < n_messages; i++) {
            chat_msgs[i].role    = messages[i].role;
            chat_msgs[i].content = messages[i].content;
        }

        /* First call: get needed size */
        int32_t needed = llama_chat_apply_template(
            eng->chat_template, chat_msgs, (size_t)n_messages, true, NULL, 0);

        if (needed <= 0) {
            LOG_WARN(eng->logger, "llama_chat_apply_template returned %d, "
                     "falling back to ChatML", needed);
            free(chat_msgs);
            /* Fallback to manual ChatML */
            int32_t flen = 0;
            formatted = build_chatml_prompt(messages, n_messages, &flen);
            if (!formatted || flen == 0) {
                free(formatted);
                snprintf(finish_reason, fr_len, "%s", "backend_error");
                return -1;
            }
        } else {
            if (needed > PROF_MAX_PROMPT_BYTES) {
                LOG_WARN(eng->logger, "rendered prompt too large: %d bytes > %d",
                         needed, PROF_MAX_PROMPT_BYTES);
                free(chat_msgs);
                snprintf(finish_reason, fr_len, "%s", "context_overflow");
                return -1;
            }

            formatted = malloc((size_t)needed + 1);
            if (!formatted) {
                free(chat_msgs);
                snprintf(finish_reason, fr_len, "%s", "backend_error");
                return -1;
            }

            int32_t actual = llama_chat_apply_template(
                eng->chat_template, chat_msgs, (size_t)n_messages, true,
                formatted, (int32_t)needed + 1);

            free(chat_msgs);

            if (actual <= 0) {
                LOG_ERROR(eng->logger, "llama_chat_apply_template render failed");
                free(formatted);
                snprintf(finish_reason, fr_len, "%s", "backend_error");
                return -1;
            }
            formatted[actual] = '\0';
        }
    }

    /* Run completion */
    int rc = inference_complete(eng, formatted, params, cancel_flag,
                                cb, user_data,
                                prompt_tokens, completion_tokens,
                                finish_reason, fr_len);

    free(formatted);
    return rc;
}

sample_params_t inference_resolve_params(const config_t *cfg,
                                         const chat_request_t *req)
{
    sample_params_t p;
    memset(&p, 0, sizeof(p));

    p.temperature = isnan(req->temperature) ? cfg->temperature
                    : clamp_f(req->temperature, 0.0f, 2.0f);
    p.top_p = isnan(req->top_p) ? cfg->top_p
              : clamp_f(req->top_p, 0.01f, 1.0f);
    p.top_k = cfg->top_k; /* not in OpenAI API, always config default */
    p.repeat_penalty = isnan(req->repeat_penalty) ? cfg->repeat_penalty
                       : clamp_f(req->repeat_penalty, 1.0f, 2.0f);
    p.frequency_penalty = isnan(req->frequency_penalty) ? 0.0f
                          : clamp_f(req->frequency_penalty, 0.0f, 2.0f);
    p.presence_penalty = isnan(req->presence_penalty) ? 0.0f
                         : clamp_f(req->presence_penalty, 0.0f, 2.0f);
    p.max_tokens = (req->max_tokens == -1) ? cfg->max_tokens
                   : clamp_i(req->max_tokens, 1, cfg->n_ctx);
    p.max_inference_seconds = cfg->max_inference_seconds;

    return p;
}

sample_params_t inference_resolve_params_completion(const config_t *cfg,
                                                    const completion_request_t *req)
{
    sample_params_t p;
    memset(&p, 0, sizeof(p));

    p.temperature = isnan(req->temperature) ? cfg->temperature
                    : clamp_f(req->temperature, 0.0f, 2.0f);
    p.top_p = isnan(req->top_p) ? cfg->top_p
              : clamp_f(req->top_p, 0.01f, 1.0f);
    p.top_k = cfg->top_k;
    p.repeat_penalty = isnan(req->repeat_penalty) ? cfg->repeat_penalty
                       : clamp_f(req->repeat_penalty, 1.0f, 2.0f);
    p.frequency_penalty = 0.0f;
    p.presence_penalty  = 0.0f;
    p.max_tokens = (req->max_tokens == -1) ? cfg->max_tokens
                   : clamp_i(req->max_tokens, 1, cfg->n_ctx);
    p.max_inference_seconds = cfg->max_inference_seconds;

    return p;
}
