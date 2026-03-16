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

    /* Constant-time-ish comparison */
    size_t elen = strlen(expected);
    if (auth->len != elen) {
        return -1;
    }

    volatile int diff = 0;
    for (size_t i = 0; i < elen; i++) {
        diff |= (unsigned char)auth->buf[i] ^ (unsigned char)expected[i];
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

static void handle_health(struct mg_connection *c, server_ctx_t *sctx)
{
    if (sctx->stats) atomic_fetch_add(&sctx->stats->req_health, 1);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"status\":\"ok\"}");
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
        if (job->state == JOB_STATE_DONE || job->state == JOB_STATE_ERROR) {
            int64_t latency = now_ms() - cd->start_time_ms;

            if (job->state == JOB_STATE_ERROR &&
                strcmp(job->finish_reason, "context_overflow") == 0) {
                send_error(c, 400, "Prompt too long for context window",
                           "invalid_request");
            } else if (job->state == JOB_STATE_ERROR) {
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
                resp.usage.total_tokens      = job->prompt_tokens + job->completion_tokens;

                char *json = chat_response_to_json(&resp);
                if (json) {
                    send_json_response(c, 200, json);
                    free(json);
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
                resp.usage.total_tokens      = job->prompt_tokens + job->completion_tokens;

                char *json = completion_response_to_json(&resp);
                if (json) {
                    send_json_response(c, 200, json);
                    free(json);
                }
            }

            log_access(sctx->lg, c, cd->method, cd->path,
                       (job->state == JOB_STATE_DONE) ? 200 : 500,
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

    /* Streaming: drain ring buffer */
    char token_text[TOKEN_TEXT_MAX];
    while (ring_read(&job->ring, token_text, sizeof(token_text))) {
        stream_chunk_t chunk;
        memset(&chunk, 0, sizeof(chunk));
        snprintf(chunk.id, sizeof(chunk.id), "%s", job->request_id);
        snprintf(chunk.model, sizeof(chunk.model), "%s", job->model_id);
        chunk.created = (int64_t)time(NULL);
        snprintf(chunk.delta_content, sizeof(chunk.delta_content), "%s", token_text);
        chunk.is_chat = (job->type == JOB_CHAT);

        char *json = stream_chunk_to_json(&chunk);
        if (json) {
            mg_printf(c, "data: %s\n\n", json);
            free(json);
        }
    }

    /* Check if done */
    int done;
    pthread_mutex_lock(&job->ring.mutex);
    done = job->ring.done && ring_is_empty_unlocked(&job->ring);
    pthread_mutex_unlock(&job->ring.mutex);

    if (done || job->state == JOB_STATE_DONE || job->state == JOB_STATE_ERROR) {
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

        /* Body size check */
        if (hm->body.len > PROF_MAX_BODY_BYTES) {
            send_error(c, 400, "Request body too large", "invalid_request");
            return;
        }

        /* Auth check (skip for health) */
        if (!is_health) {
            if (check_auth(hm, sctx->cfg) != 0) {
                send_error(c, 401, "Invalid or missing API key", "unauthorized");
                return;
            }
        }

        /* Route dispatch with method checking */
        if (is_health) {
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

            int allowed = 0;
            for (int i = 0; i < sctx->cfg->acl_count; i++) {
                if (strcmp(ip, sctx->cfg->acl[i]) == 0) {
                    allowed = 1;
                    break;
                }
            }
            if (!allowed) {
                /* Silent drop -- no HTTP response, no data, just close */
                if (sctx->stats) atomic_fetch_add(&sctx->stats->connections_rejected, 1);
                LOG_DEBUG(sctx->lg, "ACL rejected connection from %s", ip);
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
            LOG_WARN(sctx->lg, "connection limit reached (%d), dropping",
                     sctx->active_connections);
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
            /* Don't free here -- worker thread may still be using the job.
               It will be freed when the wakeup handler sees the done state. */
        }
    }
}

int server_init(struct mg_mgr *mgr, server_ctx_t *sctx)
{
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
