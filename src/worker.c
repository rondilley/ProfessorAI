/*
 * worker.c -- Inference worker thread with job queue and token ring buffer
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "worker.h"
#include "daemon.h"

#include <mongoose.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Token ring operations --- */

static void ring_init(token_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->not_full, NULL);
    pthread_cond_init(&r->not_empty, NULL);
}

static void ring_destroy(token_ring_t *r)
{
    pthread_mutex_destroy(&r->mutex);
    pthread_cond_destroy(&r->not_full);
    pthread_cond_destroy(&r->not_empty);
}

static int ring_is_full(const token_ring_t *r)
{
    return ((r->head + 1) % TOKEN_RING_SIZE) == r->tail;
}

static int ring_is_empty(const token_ring_t *r)
{
    return r->head == r->tail;
}

/* Write a token into the ring (worker thread). Blocks if full, checking cancel. */
static int ring_write(token_ring_t *r, const char *text, volatile int *cancel)
{
    pthread_mutex_lock(&r->mutex);

    while (ring_is_full(r) && !*cancel && !g_shutdown_requested) {
        /* Wait with timeout so we can re-check cancel */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000; /* 50ms */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&r->not_full, &r->mutex, &ts);
    }

    if (*cancel || g_shutdown_requested) {
        pthread_mutex_unlock(&r->mutex);
        return -1;
    }

    size_t len = strlen(text);
    if (len >= TOKEN_TEXT_MAX) len = TOKEN_TEXT_MAX - 1;
    memcpy(r->text[r->head], text, len);
    r->text[r->head][len] = '\0';
    r->head = (r->head + 1) % TOKEN_RING_SIZE;

    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

/* Read a token from the ring (main thread). Returns 1 if got token, 0 if empty. */
static int ring_read(token_ring_t *r, char *out, size_t out_len)
{
    pthread_mutex_lock(&r->mutex);

    if (ring_is_empty(r)) {
        pthread_mutex_unlock(&r->mutex);
        return 0;
    }

    snprintf(out, out_len, "%s", r->text[r->tail]);
    r->tail = (r->tail + 1) % TOKEN_RING_SIZE;

    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->mutex);
    return 1;
}

/* --- Streaming token callback --- */

typedef struct {
    worker_job_t *job;
    int           tokens_since_wake;
} stream_cb_data_t;

static int stream_token_cb(const char *token_text, void *user_data)
{
    stream_cb_data_t *sd = (stream_cb_data_t *)user_data;
    worker_job_t *job = sd->job;

    /* Check if ring was empty before write to batch wakeups */
    int was_empty;
    pthread_mutex_lock(&job->ring.mutex);
    was_empty = ring_is_empty(&job->ring);
    pthread_mutex_unlock(&job->ring.mutex);

    if (ring_write(&job->ring, token_text, &job->cancel) != 0) {
        return -1; /* cancelled */
    }

    /* Only wake the event loop when the ring transitions from empty,
       or every 8 tokens, to reduce per-token syscall overhead */
    sd->tokens_since_wake++;
    if (job->mgr && (was_empty || sd->tokens_since_wake >= 8)) {
        mg_wakeup((struct mg_mgr *)job->mgr, job->wakeup_id, NULL, 0);
        sd->tokens_since_wake = 0;
    }

    return 0;
}

/* --- Non-streaming token callback --- */

typedef struct {
    worker_job_t *job;
} accum_cb_data_t;

static int accum_token_cb(const char *token_text, void *user_data)
{
    accum_cb_data_t *ad = (accum_cb_data_t *)user_data;
    worker_job_t *job = ad->job;

    if (job->cancel || g_shutdown_requested) {
        return -1;
    }

    size_t tlen = strlen(token_text);
    if (job->response_len + tlen >= job->response_cap) {
        size_t new_cap = job->response_cap * 2;
        if (new_cap < job->response_len + tlen + 1) {
            new_cap = job->response_len + tlen + 1;
        }
        if (new_cap > PROF_CONTENT_MAX) {
            new_cap = PROF_CONTENT_MAX;
        }
        if (job->response_len + tlen >= new_cap) {
            return -1; /* hit cap */
        }
        char *tmp = realloc(job->response_buf, new_cap);
        if (!tmp) {
            return -1;
        }
        job->response_buf = tmp;
        job->response_cap = new_cap;
    }

    memcpy(job->response_buf + job->response_len, token_text, tlen);
    job->response_len += tlen;
    job->response_buf[job->response_len] = '\0';

    return 0;
}

/* --- Worker thread function --- */

static void *worker_thread_func(void *arg)
{
    worker_t *w = (worker_t *)arg;

    while (1) {
        pthread_mutex_lock(&w->mutex);

        while (w->current_job == NULL && w->running) {
            pthread_cond_wait(&w->job_ready, &w->mutex);
        }

        if (!w->running) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }

        worker_job_t *job = w->current_job;
        pthread_mutex_unlock(&w->mutex);

        atomic_store(&job->state, JOB_STATE_RUNNING);
        int rc;

        if (job->stream) {
            stream_cb_data_t sd = { .job = job };

            if (job->type == JOB_CHAT) {
                rc = inference_chat(w->engine,
                                    job->chat_req.messages,
                                    job->chat_req.message_count,
                                    &job->params, &job->cancel,
                                    stream_token_cb, &sd,
                                    &job->prompt_tokens,
                                    &job->completion_tokens,
                                    job->finish_reason,
                                    sizeof(job->finish_reason));
            } else {
                rc = inference_complete(w->engine,
                                        job->comp_req.prompt,
                                        &job->params, &job->cancel,
                                        stream_token_cb, &sd,
                                        &job->prompt_tokens,
                                        &job->completion_tokens,
                                        job->finish_reason,
                                        sizeof(job->finish_reason));
            }

            /* Signal ring done */
            pthread_mutex_lock(&job->ring.mutex);
            job->ring.done = 1;
            pthread_cond_signal(&job->ring.not_empty);
            pthread_mutex_unlock(&job->ring.mutex);
        } else {
            /* Allocate response buffer */
            job->response_cap = (job->params.max_tokens > 0)
                ? (size_t)job->params.max_tokens * 16 : 8192;
            if (job->response_cap < 8192) job->response_cap = 8192;
            if (job->response_cap > PROF_CONTENT_MAX) job->response_cap = PROF_CONTENT_MAX;
            job->response_buf = malloc(job->response_cap);
            job->response_len = 0;
            if (job->response_buf) {
                job->response_buf[0] = '\0';
            }

            accum_cb_data_t ad = { .job = job };

            if (job->response_buf == NULL) {
                rc = -1;
                snprintf(job->finish_reason, sizeof(job->finish_reason),
                         "%s", "backend_error");
            } else if (job->type == JOB_CHAT) {
                rc = inference_chat(w->engine,
                                    job->chat_req.messages,
                                    job->chat_req.message_count,
                                    &job->params, &job->cancel,
                                    accum_token_cb, &ad,
                                    &job->prompt_tokens,
                                    &job->completion_tokens,
                                    job->finish_reason,
                                    sizeof(job->finish_reason));
            } else {
                rc = inference_complete(w->engine,
                                        job->comp_req.prompt,
                                        &job->params, &job->cancel,
                                        accum_token_cb, &ad,
                                        &job->prompt_tokens,
                                        &job->completion_tokens,
                                        job->finish_reason,
                                        sizeof(job->finish_reason));
            }
        }

        atomic_store(&job->state, (rc == 0) ? JOB_STATE_DONE : JOB_STATE_ERROR);

        /* Easter egg: doomsday model */
        if (rc == 0 && strstr(job->model_id, "doomsday") != NULL) {
            LOG_INFO(w->logger,
                     "I suppose I could part with one and still be feared.");
        }

        LOG_INFO(w->logger, "Delivery complete! prompt=%d comp=%d finish=%s",
                 job->prompt_tokens, job->completion_tokens, job->finish_reason);

        /* Clear current_job BEFORE wakeup so the worker doesn't re-enter */
        pthread_mutex_lock(&w->mutex);
        w->current_job = NULL;
        pthread_mutex_unlock(&w->mutex);

        /* If the connection was closed during inference, free the orphaned job */
        if (job->orphaned) {
            LOG_INFO(w->logger, "freeing orphaned job (client disconnected)");
            worker_job_free(job);
            free(job);
        } else if (job->mgr) {
            /* Wake event loop for final response */
            mg_wakeup((struct mg_mgr *)job->mgr, job->wakeup_id, NULL, 0);
        }
    }

    return NULL;
}

/* --- Public API --- */

int worker_init(worker_t *w, inference_engine_t *eng, logger_t *lg)
{
    memset(w, 0, sizeof(*w));
    w->engine = eng;
    w->logger = lg;
    w->running = 1;
    w->current_job = NULL;

    if (pthread_mutex_init(&w->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&w->job_ready, NULL) != 0) {
        pthread_mutex_destroy(&w->mutex);
        return -1;
    }
    if (pthread_create(&w->thread, NULL, worker_thread_func, w) != 0) {
        pthread_cond_destroy(&w->job_ready);
        pthread_mutex_destroy(&w->mutex);
        return -1;
    }

    return 0;
}

void worker_destroy(worker_t *w)
{
    pthread_mutex_lock(&w->mutex);
    w->running = 0;
    pthread_cond_signal(&w->job_ready);
    pthread_mutex_unlock(&w->mutex);

    pthread_join(w->thread, NULL);
    pthread_cond_destroy(&w->job_ready);
    pthread_mutex_destroy(&w->mutex);
}

int worker_submit(worker_t *w, worker_job_t *job)
{
    pthread_mutex_lock(&w->mutex);

    if (w->current_job != NULL &&
        (atomic_load(&w->current_job->state) == JOB_STATE_IDLE ||
         atomic_load(&w->current_job->state) == JOB_STATE_RUNNING)) {
        pthread_mutex_unlock(&w->mutex);
        return -1; /* busy */
    }

    w->current_job = job;
    atomic_store(&job->state, JOB_STATE_IDLE);
    job->cancel = 0;
    pthread_cond_signal(&w->job_ready);
    pthread_mutex_unlock(&w->mutex);

    return 0;
}

int worker_is_busy(const worker_t *w)
{
    /* Read without lock -- acceptable for advisory check */
    worker_job_t *job = w->current_job;
    if (job == NULL) return 0;
    int s = atomic_load(&job->state);
    return (s == JOB_STATE_IDLE || s == JOB_STATE_RUNNING);
}

void worker_cancel(worker_t *w)
{
    pthread_mutex_lock(&w->mutex);
    worker_job_t *job = w->current_job;
    if (job) {
        job->cancel = 1;
    }
    pthread_mutex_unlock(&w->mutex);
}

void worker_job_init(worker_job_t *job)
{
    memset(job, 0, sizeof(*job));
    ring_init(&job->ring);
}

void worker_job_free(worker_job_t *job)
{
    if (job->type == JOB_CHAT) {
        chat_request_free(&job->chat_req);
    }
    if (job->response_buf) {
        free(job->response_buf);
        job->response_buf = NULL;
    }
    ring_destroy(&job->ring);
}
