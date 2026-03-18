/*
 * server.c -- HTTP server: routing, auth, admission control, SSE streaming
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "server.h"
#include "daemon.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Forward declarations */
static int  ring_read(token_ring_t *r, char *out, size_t out_len);
static int  ring_is_empty_unlocked(const token_ring_t *r);
static void drain_stream_ring(struct mg_connection *c, server_ctx_t *sctx);

/* Professor Farnsworth quotes for the What-If Machine */
static const char *farnsworth_quotes[] = {
    "Good news, everyone! I've taught the toaster to feel love.",
    "I don't want to live on this planet anymore.",
    "Sweet zombie Jesus!",
    "There's no scientific consensus that life is important.",
    "To shreds, you say?",
    "I suppose I could part with one and still be feared.",
    "If a dog craps anywhere in the universe, you can bet I won't be out of the loop.",
    "Quiet, you!",
    "Now I've often said 'good news' when sending you on a mission of extreme danger.",
    "Good news, everyone! I'm still technically alive!",
    "Bad news, nobody.",
    "My God, they're back! We're doomed!",
    "Wernstrom!",
    "So that's what things would be like if I'd invented the Fing-Longer. "
        "A man can dream though. A man can dream...",
    "Good news, everyone! Several years ago I tried to log on to AOL, "
        "and it just went through! We're online!",
    "Our crew is replaceable, your package isn't.",
    "Fuff!",
    "Eh wha?",
    "Oh my, yes.",
    "Planet Express is on the move. For this hip, young delivery company, "
        "tomorrow is today and today is yesterday."
};
#define FARNSWORTH_QUOTE_COUNT \
    (int)(sizeof(farnsworth_quotes) / sizeof(farnsworth_quotes[0]))

/* Per-connection data for tracking active jobs */
typedef struct {
    worker_job_t *job;
    int64_t       start_time_ms;
    char          method[8];
    char          path[64];
    int           headers_sent;  /* SSE headers already sent */
} conn_data_t;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_json_response(struct mg_connection *c, int status,
                               const char *json)
{
    mg_http_reply(c, status, "Content-Type: application/json\r\n",
                  "%s", json);
}

static void send_error(struct mg_connection *c, int http_status,
                       const char *message, const char *type)
{
    /* Track error in stats */
    server_ctx_t *sctx = (server_ctx_t *)c->mgr->userdata;
    if (sctx && sctx->stats) {
        switch (http_status) {
            case 400: atomic_fetch_add(&sctx->stats->err_400, 1); break;
            case 401: atomic_fetch_add(&sctx->stats->err_401, 1); break;
            case 405: atomic_fetch_add(&sctx->stats->err_405, 1); break;
            case 503: atomic_fetch_add(&sctx->stats->err_503, 1); break;
            default:  atomic_fetch_add(&sctx->stats->err_500, 1); break;
        }
    }

    /* Professor Farnsworth commentary (log only, not in API response) */
    if (sctx && sctx->lg) {
        switch (http_status) {
            case 400:
                LOG_DEBUG(sctx->lg, "Wernstrom! That request makes no sense.");
                break;
            case 404:
                LOG_DEBUG(sctx->lg, "Eh wha? I don't seem to have that endpoint.");
                break;
            case 405:
                LOG_DEBUG(sctx->lg, "You can't just DO that to my API!");
                break;
            case 500:
                LOG_WARN(sctx->lg, "My God, we're doomed!");
                break;
            case 503:
                LOG_DEBUG(sctx->lg, "Quiet, you! I'm doing science.");
                break;
            default:
                break;
        }
    }

    char *json = error_to_json(http_status, message, type);
    if (json) {
        send_json_response(c, http_status, json);
        free(json);
    } else {
        mg_http_reply(c, 500, "", "Internal error");
    }
}

static int check_auth(struct mg_http_message *hm, const config_t *cfg)
{
    if (cfg->api_key[0] == '\0') {
        return 0; /* no auth configured */
    }

    struct mg_str *auth = mg_http_get_header(hm, "Authorization");
    if (!auth) {
        return -1;
    }

    /* Build expected value: "Bearer <key>" */
    char expected[PROF_KEY_MAX + 8];
    snprintf(expected, sizeof(expected), "Bearer %s", cfg->api_key);

    /* Constant-time comparison -- always compare full expected length
       to avoid leaking key length via timing */
    size_t elen = strlen(expected);
    volatile int diff = (auth->len != elen) ? 1 : 0;
    size_t cmp_len = (auth->len < elen) ? auth->len : elen;
    for (size_t i = 0; i < cmp_len; i++) {
        diff |= (unsigned char)auth->buf[i] ^ (unsigned char)expected[i];
    }
    /* If lengths differ, diff is already non-zero; pad the loop
       to always run elen iterations for consistent timing */
    for (size_t i = cmp_len; i < elen; i++) {
        diff |= (unsigned char)expected[i];
    }

    return diff ? -1 : 0;
}

static void log_access(logger_t *lg, struct mg_connection *c,
                       const char *method, const char *path,
                       int status, int64_t latency_ms,
                       int32_t prompt_tok, int32_t comp_tok)
{
    char ip[48];
    mg_snprintf(ip, sizeof(ip), "%M", mg_print_ip, &c->rem);

    if (prompt_tok >= 0) {
        LOG_INFO(lg, "[access] %s %s %s %d %lldms prompt=%d comp=%d",
                 ip, method, path, status, (long long)latency_ms,
                 prompt_tok, comp_tok);
    } else {
        LOG_INFO(lg, "[access] %s %s %s %d %lldms",
                 ip, method, path, status, (long long)latency_ms);
    }
}

#ifndef PROF_VERSION
#define PROF_VERSION "0.1.0"
#endif

static void handle_health(struct mg_connection *c, server_ctx_t *sctx)
{
    if (sctx->stats) atomic_fetch_add(&sctx->stats->req_health, 1);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"status\":\"ok\",\"version\":\"%s\"}", PROF_VERSION);
}

static void handle_whatif(struct mg_connection *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int idx = (int)((ts.tv_nsec / 1000) % FARNSWORTH_QUOTE_COUNT);
    const char *quote = farnsworth_quotes[idx];

    /* Escape JSON string */
    size_t qlen = strlen(quote);
    char escaped[1024];
    size_t pos = 0;
    for (size_t i = 0; i < qlen && pos < sizeof(escaped) - 2; i++) {
        if (quote[i] == '"' || quote[i] == '\\') {
            escaped[pos++] = '\\';
        }
        escaped[pos++] = quote[i];
    }
    escaped[pos] = '\0';

    char buf[1280];
    snprintf(buf, sizeof(buf),
             "{\"what_if\":\"%s\",\"source\":\"Professor Farnsworth\"}", escaped);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
}

static void handle_stats(struct mg_connection *c, server_ctx_t *sctx)
{
    if (!sctx->stats) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{}");
        return;
    }
    char *json = stats_to_json(sctx->stats);
    if (json) {
        send_json_response(c, 200, json);
        free(json);
    } else {
        send_error(c, 500, "Internal error", "backend_error");
    }
}

static void handle_models(struct mg_connection *c, server_ctx_t *sctx)
{
    if (sctx->stats) atomic_fetch_add(&sctx->stats->req_models, 1);
    char *json = models_list_to_json(sctx->model_id);
    if (json) {
        send_json_response(c, 200, json);
        free(json);
    } else {
        send_error(c, 500, "Internal error", "backend_error");
    }
}

static void submit_inference_job(struct mg_connection *c,
                                 struct mg_http_message *hm,
                                 server_ctx_t *sctx,
                                 job_type_t type)
{
    /* Admission control */
    if (worker_is_busy(sctx->worker)) {
        send_error(c, 503, "Server busy, try again", "server_busy");
        return;
    }

    /* Allocate job + connection data */
    worker_job_t *job = calloc(1, sizeof(worker_job_t));
    conn_data_t  *cd  = calloc(1, sizeof(conn_data_t));
    if (!job || !cd) {
        free(job);
        free(cd);
        send_error(c, 500, "Internal error", "backend_error");
        return;
    }

    worker_job_init(job);
    job->type = type;
    job->mgr = c->mgr;
    job->wakeup_id = c->id;  /* wake up THIS client connection */

    cd->job = job;
    cd->start_time_ms = now_ms();
    cd->headers_sent = 0;

    /* Track request */
    if (sctx->stats) {
        if (type == JOB_CHAT)
            atomic_fetch_add(&sctx->stats->req_chat, 1);
        else
            atomic_fetch_add(&sctx->stats->req_completion, 1);
    }

    /* Parse request */
    int parse_rc;
    if (type == JOB_CHAT) {
        parse_rc = chat_request_parse(&job->chat_req, hm->body.buf, hm->body.len);
        if (parse_rc == 0) {
            job->stream = job->chat_req.stream;
            job->params = inference_resolve_params(sctx->cfg, &job->chat_req);
            /* Copy stop sequences */
            job->stop_count = job->chat_req.stop_count;
            for (int i = 0; i < job->stop_count; i++) {
                snprintf(job->stop[i], sizeof(job->stop[i]),
                         "%s", job->chat_req.stop[i]);
            }
        }
        snprintf(cd->method, sizeof(cd->method), "POST");
        snprintf(cd->path, sizeof(cd->path), "/v1/chat/completions");
    } else {
        parse_rc = completion_request_parse(&job->comp_req, hm->body.buf, hm->body.len);
        if (parse_rc == 0) {
            job->stream = job->comp_req.stream;
            job->params = inference_resolve_params_completion(sctx->cfg, &job->comp_req);
            job->stop_count = job->comp_req.stop_count;
            for (int i = 0; i < job->stop_count; i++) {
                snprintf(job->stop[i], sizeof(job->stop[i]),
                         "%s", job->comp_req.stop[i]);
            }
        }
        snprintf(cd->method, sizeof(cd->method), "POST");
        snprintf(cd->path, sizeof(cd->path), "/v1/completions");
    }

    if (parse_rc != 0) {
        worker_job_free(job);
        free(job);
        free(cd);
        send_error(c, 400, "Invalid request body", "invalid_json");
        return;
    }

    generate_request_id(job->request_id, sizeof(job->request_id),
                        type == JOB_CHAT ? "chatcmpl" : "cmpl");
    snprintf(job->model_id, sizeof(job->model_id), "%s", sctx->model_id);

    /* Submit to worker */
    if (worker_submit(sctx->worker, job) != 0) {
        worker_job_free(job);
        free(job);
        free(cd);
        send_error(c, 503, "Server busy, try again", "server_busy");
        return;
    }

    /* Store connection data */
    c->fn_data = cd;

    /* If streaming, send SSE headers now */
    if (job->stream) {
        mg_printf(c,
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: text/event-stream\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Connection: keep-alive\r\n\r\n");
        cd->headers_sent = 1;
    }
}

static void drain_stream_ring(struct mg_connection *c, server_ctx_t *sctx)
{
    conn_data_t *cd = (conn_data_t *)c->fn_data;
    if (!cd || !cd->job) return;

    worker_job_t *job = cd->job;

    if (!job->stream) {
        /* Non-streaming: check if job is done */
        int jstate = atomic_load(&job->state);
        if (jstate == JOB_STATE_DONE || jstate == JOB_STATE_ERROR) {
            int64_t latency = now_ms() - cd->start_time_ms;

            if (jstate == JOB_STATE_ERROR &&
                strcmp(job->finish_reason, "context_overflow") == 0) {
                send_error(c, 400, "Prompt too long for context window",
                           "invalid_request");
            } else if (jstate == JOB_STATE_ERROR) {
                send_error(c, 500, "Inference failed", "backend_error");
            } else if (job->type == JOB_CHAT) {
                chat_response_t resp;
                memset(&resp, 0, sizeof(resp));
                snprintf(resp.id, sizeof(resp.id), "%s", job->request_id);
                snprintf(resp.model, sizeof(resp.model), "%s", job->model_id);
                resp.created = (int64_t)time(NULL);
                if (job->response_buf && job->response_len > 0) {
                    snprintf(resp.content, sizeof(resp.content),
                             "%s", job->response_buf);
                }
                snprintf(resp.finish_reason, sizeof(resp.finish_reason),
                         "%s", job->finish_reason);
                resp.usage.prompt_tokens     = job->prompt_tokens;
                resp.usage.completion_tokens = job->completion_tokens;
                resp.usage.total_tokens      = (int32_t)((int64_t)job->prompt_tokens + job->completion_tokens);

                char *json = chat_response_to_json(&resp);
                if (json) {
                    send_json_response(c, 200, json);
                    free(json);
                } else {
                    send_error(c, 500, "Internal error", "backend_error");
                }
            } else {
                completion_response_t resp;
                memset(&resp, 0, sizeof(resp));
                snprintf(resp.id, sizeof(resp.id), "%s", job->request_id);
                snprintf(resp.model, sizeof(resp.model), "%s", job->model_id);
                resp.created = (int64_t)time(NULL);
                if (job->response_buf && job->response_len > 0) {
                    snprintf(resp.text, sizeof(resp.text),
                             "%s", job->response_buf);
                }
                snprintf(resp.finish_reason, sizeof(resp.finish_reason),
                         "%s", job->finish_reason);
                resp.usage.prompt_tokens     = job->prompt_tokens;
                resp.usage.completion_tokens = job->completion_tokens;
                resp.usage.total_tokens      = (int32_t)((int64_t)job->prompt_tokens + job->completion_tokens);

                char *json = completion_response_to_json(&resp);
                if (json) {
                    send_json_response(c, 200, json);
                    free(json);
                } else {
                    send_error(c, 500, "Internal error", "backend_error");
                }
            }

            log_access(sctx->lg, c, cd->method, cd->path,
                       (jstate == JOB_STATE_DONE) ? 200 : 500,
                       latency, job->prompt_tokens, job->completion_tokens);

            /* Track inference stats */
            if (sctx->stats) {
                atomic_fetch_add(&sctx->stats->inferences_total, 1);
                atomic_fetch_add(&sctx->stats->tokens_prompt_total, job->prompt_tokens);
                atomic_fetch_add(&sctx->stats->tokens_completion_total, job->completion_tokens);
                atomic_fetch_add(&sctx->stats->inference_ms_total, (int64_t)latency);
            }

            /* Cleanup */
            worker_job_free(job);
            free(job);
            free(cd);
            c->fn_data = NULL;
        }
        return;
    }

    /* Streaming: batch-drain ring buffer under a single lock */
    {
        char batch_tokens[32][TOKEN_TEXT_MAX];
        int batch_count;
        int64_t created = (int64_t)time(NULL);
        const char *obj_type = (job->type == JOB_CHAT)
            ? "chat.completion.chunk" : "text_completion.chunk";

        do {
            batch_count = 0;
            pthread_mutex_lock(&job->ring.mutex);
            while (!ring_is_empty_unlocked(&job->ring) && batch_count < 32) {
                size_t tlen = strlen(job->ring.text[job->ring.tail]);
                if (tlen >= TOKEN_TEXT_MAX) tlen = TOKEN_TEXT_MAX - 1;
                memcpy(batch_tokens[batch_count], job->ring.text[job->ring.tail], tlen);
                batch_tokens[batch_count][tlen] = '\0';
                job->ring.tail = (job->ring.tail + 1) % TOKEN_RING_SIZE;
                batch_count++;
            }
            if (batch_count > 0) {
                pthread_cond_signal(&job->ring.not_full);
            }
            pthread_mutex_unlock(&job->ring.mutex);

            /* Format SSE chunks directly without cJSON */
            for (int bi = 0; bi < batch_count; bi++) {
                if (job->type == JOB_CHAT) {
                    mg_printf(c,
                        "data: {\"id\":\"%s\",\"object\":\"%s\","
                        "\"created\":%lld,\"model\":\"%s\","
                        "\"choices\":[{\"index\":0,"
                        "\"delta\":{\"content\":\"%M\"},"
                        "\"finish_reason\":null}]}\n\n",
                        job->request_id, obj_type,
                        (long long)created, job->model_id,
                        MG_ESC(batch_tokens[bi]));
                } else {
                    mg_printf(c,
                        "data: {\"id\":\"%s\",\"object\":\"%s\","
                        "\"created\":%lld,\"model\":\"%s\","
                        "\"choices\":[{\"index\":0,"
                        "\"text\":\"%M\","
                        "\"finish_reason\":null}]}\n\n",
                        job->request_id, obj_type,
                        (long long)created, job->model_id,
                        MG_ESC(batch_tokens[bi]));
                }
            }
        } while (batch_count == 32); /* keep draining if ring was full */
    }

    /* Check if done */
    int done;
    pthread_mutex_lock(&job->ring.mutex);
    done = job->ring.done && ring_is_empty_unlocked(&job->ring);
    pthread_mutex_unlock(&job->ring.mutex);

    int sstate = atomic_load(&job->state);
    if (done || sstate == JOB_STATE_DONE || sstate == JOB_STATE_ERROR) {
        /* Send final chunk with finish_reason */
        stream_chunk_t final_chunk;
        memset(&final_chunk, 0, sizeof(final_chunk));
        snprintf(final_chunk.id, sizeof(final_chunk.id), "%s", job->request_id);
        snprintf(final_chunk.model, sizeof(final_chunk.model), "%s", job->model_id);
        final_chunk.created = (int64_t)time(NULL);
        snprintf(final_chunk.finish_reason, sizeof(final_chunk.finish_reason),
                 "%s", job->finish_reason);
        final_chunk.is_chat = (job->type == JOB_CHAT);

        char *json = stream_chunk_to_json(&final_chunk);
        if (json) {
            mg_printf(c, "data: %s\n\n", json);
            free(json);
        }

        mg_printf(c, "data: [DONE]\n\n");

        int64_t latency = now_ms() - cd->start_time_ms;
        log_access(sctx->lg, c, cd->method, cd->path, 200,
                   latency, job->prompt_tokens, job->completion_tokens);

        /* Track inference stats */
        if (sctx->stats) {
            atomic_fetch_add(&sctx->stats->inferences_total, 1);
            atomic_fetch_add(&sctx->stats->tokens_prompt_total, job->prompt_tokens);
            atomic_fetch_add(&sctx->stats->tokens_completion_total, job->completion_tokens);
            atomic_fetch_add(&sctx->stats->inference_ms_total, (int64_t)latency);
            atomic_fetch_add(&sctx->stats->req_stream, 1);
        }

        /* Cleanup */
        worker_job_free(job);
        free(job);
        free(cd);
        c->fn_data = NULL;

        /* Close connection after streaming */
        c->is_draining = 1;
    }
}

static int ring_is_empty_unlocked(const token_ring_t *r)
{
    return r->head == r->tail;
}

static int ring_read(token_ring_t *r, char *out, size_t out_len)
{
    pthread_mutex_lock(&r->mutex);
    if (r->head == r->tail) {
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }
    snprintf(out, out_len, "%s", r->text[r->tail]);
    r->tail = (r->tail + 1) % TOKEN_RING_SIZE;
    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->mutex);
    return 1;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    server_ctx_t *sctx = (server_ctx_t *)c->mgr->userdata;

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        int is_health = (mg_match(hm->uri, mg_str("/health"), NULL) ||
                         mg_match(hm->uri, mg_str("/v1/health"), NULL));
        int is_whatif = mg_match(hm->uri, mg_str("/whatif"), NULL);

        /* Body size check */
        if (hm->body.len > PROF_MAX_BODY_BYTES) {
            LOG_WARN(sctx->lg,
                     "Sweet three-toed sloth of ice planet Hoth! "
                     "Request body too large (%lu bytes)",
                     (unsigned long)hm->body.len);
            send_error(c, 400, "Request body too large", "invalid_request");
            return;
        }

        /* Auth check (skip for health and whatif) */
        if (!is_health && !is_whatif) {
            if (check_auth(hm, sctx->cfg) != 0) {
                char auth_ip[48];
                mg_snprintf(auth_ip, sizeof(auth_ip), "%M", mg_print_ip, &c->rem);
                LOG_WARN(sctx->lg,
                         "Sweet zombie Jesus! Unauthorized access attempt from %s",
                         auth_ip);
                send_error(c, 401, "Invalid or missing API key", "unauthorized");
                return;
            }
        }

        /* Route dispatch with method checking */
        if (is_whatif) {
            handle_whatif(c);
        }
        else if (is_health) {
            handle_health(c, sctx);
        }
        else if (mg_match(hm->uri, mg_str("/v1/stats"), NULL)) {
            handle_stats(c, sctx);
        }
        else if (mg_match(hm->uri, mg_str("/v1/models"), NULL)) {
            if (mg_strcmp(hm->method, mg_str("GET")) != 0) {
                send_error(c, 405, "Method Not Allowed", "method_not_allowed");
                return;
            }
            handle_models(c, sctx);
        }
        else if (mg_match(hm->uri, mg_str("/v1/chat/completions"), NULL)) {
            if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
                send_error(c, 405, "Method Not Allowed", "method_not_allowed");
                return;
            }
            submit_inference_job(c, hm, sctx, JOB_CHAT);
        }
        else if (mg_match(hm->uri, mg_str("/v1/completions"), NULL)) {
            if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
                send_error(c, 405, "Method Not Allowed", "method_not_allowed");
                return;
            }
            submit_inference_job(c, hm, sctx, JOB_COMPLETION);
        }
        else {
            send_error(c, 404, "Not Found", "not_found");
        }
    }
    else if (ev == MG_EV_WAKEUP) {
        /* Drain token ring / finalize response for this connection */
        if (c->fn_data) {
            drain_stream_ring(c, sctx);
        }
    }
    else if (ev == MG_EV_ACCEPT) {
        /* IP ACL check -- drop immediately if not on the list */
        if (sctx->cfg->acl_count > 0) {
            char ip[48];
            mg_snprintf(ip, sizeof(ip), "%M", mg_print_ip, &c->rem);

            /* Strip ::ffff: prefix to normalize IPv4-mapped IPv6 addresses */
            const char *norm_ip = ip;
            if (strncmp(ip, "::ffff:", 7) == 0) {
                norm_ip = ip + 7;
            }

            int allowed = 0;
            for (int i = 0; i < sctx->cfg->acl_count; i++) {
                const char *acl_ip = sctx->cfg->acl[i];
                if (strncmp(acl_ip, "::ffff:", 7) == 0) {
                    acl_ip = acl_ip + 7;
                }
                if (strcmp(norm_ip, acl_ip) == 0) {
                    allowed = 1;
                    break;
                }
            }
            if (!allowed) {
                /* Silent drop -- no HTTP response, no data, just close */
                if (sctx->stats) atomic_fetch_add(&sctx->stats->connections_rejected, 1);
                LOG_DEBUG(sctx->lg,
                         "You're not on the list. Good day, sir! "
                         "(rejected %s)", ip);
                c->is_closing = 1;
                return;
            }
        }

        sctx->active_connections++;
        if (sctx->stats) {
            atomic_fetch_add(&sctx->stats->connections_total, 1);
            sctx->stats->connections_active = sctx->active_connections;
        }
        if (sctx->active_connections > PROF_MAX_CONNECTIONS) {
            if (sctx->stats) atomic_fetch_add(&sctx->stats->connections_rejected, 1);
            LOG_WARN(sctx->lg,
                     "Sweet llamas of the Bahamas! Connection limit "
                     "reached (%d), dropping", sctx->active_connections);
            c->is_closing = 1;
        }
    }
    else if (ev == MG_EV_CLOSE) {
        if (sctx->active_connections > 0) {
            sctx->active_connections--;
        }
        if (sctx->stats) {
            sctx->stats->connections_active = sctx->active_connections;
        }
        /* Cancel job if this connection had one */
        conn_data_t *cd = (conn_data_t *)c->fn_data;
        if (cd && cd->job) {
            cd->job->cancel = 1;
            /* Mark the job as orphaned so the worker thread frees it
               when inference completes. Clear mgr to prevent wakeup
               attempts on the now-dead connection. */
            cd->job->mgr = NULL;
            cd->job->orphaned = 1;
            free(cd);
            c->fn_data = NULL;
        }
    }
}

int server_init(struct mg_mgr *mgr, server_ctx_t *sctx)
{
    /* Suppress mongoose debug noise -- only show errors */
    mg_log_set(MG_LL_ERROR);

    mg_mgr_init(mgr);
    mg_wakeup_init(mgr);
    mgr->userdata = sctx;

    char url[128];
    snprintf(url, sizeof(url), "http://%s", sctx->cfg->listen_addr);

    struct mg_connection *listener = mg_http_listen(mgr, url, ev_handler, NULL);
    if (!listener) {
        LOG_FATAL(sctx->lg, "failed to bind: %s", sctx->cfg->listen_addr);
        return -1;
    }

    sctx->listener_id = listener->id;

    LOG_INFO(sctx->lg, "listening on %s", sctx->cfg->listen_addr);
    if (sctx->cfg->acl_count > 0) {
        LOG_INFO(sctx->lg, "IP ACL enabled: %d allowed addresses", sctx->cfg->acl_count);
        for (int i = 0; i < sctx->cfg->acl_count; i++) {
            LOG_INFO(sctx->lg, "  ACL allow: %s", sctx->cfg->acl[i]);
        }
    }
    sctx->ready = 1;

    return 0;
}

void server_run(struct mg_mgr *mgr, int poll_ms)
{
    server_ctx_t *sctx = (server_ctx_t *)mgr->userdata;
    while (!g_shutdown_requested) {
        mg_mgr_poll(mgr, poll_ms);
        /* Update generating flag */
        if (sctx->stats && sctx->worker) {
            atomic_store(&sctx->stats->generating,
                         worker_is_busy(sctx->worker) ? 1 : 0);
        }
        /* Periodic stats report */
        if (sctx->stats) {
            stats_check_report(sctx->stats, sctx->lg);
        }
    }
}

void server_destroy(struct mg_mgr *mgr)
{
    mg_mgr_free(mgr);
}
