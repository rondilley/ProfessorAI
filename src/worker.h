/*
 * worker.h -- Inference worker thread with job queue and token ring buffer
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_WORKER_H
#define PROF_WORKER_H

#include "api_types.h"
#include "inference.h"
#include "logger.h"

#include <pthread.h>
#include <stdint.h>

/* Token ring buffer: worker writes, main thread reads */
#define TOKEN_RING_SIZE 256
#define TOKEN_TEXT_MAX  128

typedef enum {
    JOB_CHAT,
    JOB_COMPLETION
} job_type_t;

typedef enum {
    JOB_STATE_IDLE = 0,
    JOB_STATE_RUNNING,
    JOB_STATE_DONE,
    JOB_STATE_ERROR
} job_state_t;

typedef struct {
    char    text[TOKEN_RING_SIZE][TOKEN_TEXT_MAX];
    int     head;
    int     tail;
    int     done;           /* set by worker when generation complete */
    pthread_mutex_t mutex;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} token_ring_t;

typedef struct {
    /* Input (set by main thread before submit) */
    job_type_t           type;
    chat_request_t       chat_req;
    completion_request_t comp_req;
    sample_params_t      params;
    char                 request_id[PROF_ID_MAX];
    char                 model_id[PROF_MODEL_MAX];
    int                  stream;

    /* Stop sequences (copied from request) */
    char                 stop[PROF_STOP_MAX][PROF_STOP_LEN];
    int32_t              stop_count;

    /* Cancellation (main thread writes, worker reads) */
    volatile int         cancel;

    /* Output (set by worker) */
    job_state_t          state;
    char                 finish_reason[32];
    int32_t              prompt_tokens;
    int32_t              completion_tokens;

    /* Token ring for streaming */
    token_ring_t         ring;

    /* Non-streaming: accumulated response */
    char                *response_buf;
    size_t               response_len;
    size_t               response_cap;

    /* Mongoose wakeup (main thread sets before submit) */
    void                *mgr;           /* struct mg_mgr * */
    unsigned long        wakeup_id;     /* listener connection ID */
} worker_job_t;

typedef struct {
    pthread_t           thread;
    inference_engine_t *engine;
    logger_t           *logger;
    worker_job_t       *current_job;
    pthread_mutex_t     mutex;
    pthread_cond_t      job_ready;
    int                 running;
} worker_t;

int   worker_init(worker_t *w, inference_engine_t *eng, logger_t *lg);
void  worker_destroy(worker_t *w);
int   worker_submit(worker_t *w, worker_job_t *job);
int   worker_is_busy(const worker_t *w);
void  worker_cancel(worker_t *w);

/* Initialize/free a job struct */
void  worker_job_init(worker_job_t *job);
void  worker_job_free(worker_job_t *job);

#endif /* PROF_WORKER_H */
