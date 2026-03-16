/*
 * stats.h -- Runtime statistics tracking and reporting
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_STATS_H
#define PROF_STATS_H

#include "logger.h"

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    /* Request counters */
    atomic_int_fast64_t req_chat;
    atomic_int_fast64_t req_completion;
    atomic_int_fast64_t req_stream;
    atomic_int_fast64_t req_health;
    atomic_int_fast64_t req_models;

    /* Error counters */
    atomic_int_fast64_t err_400;
    atomic_int_fast64_t err_401;
    atomic_int_fast64_t err_405;
    atomic_int_fast64_t err_503;
    atomic_int_fast64_t err_500;

    /* Inference counters */
    atomic_int_fast64_t inferences_total;
    atomic_int_fast64_t tokens_prompt_total;
    atomic_int_fast64_t tokens_completion_total;
    atomic_int_fast64_t inference_ms_total;      /* cumulative inference time */

    /* Connection counters */
    atomic_int_fast64_t connections_total;
    atomic_int_fast64_t connections_rejected;     /* over PROF_MAX_CONNECTIONS */
    int                 connections_active;        /* not atomic -- main thread only */

    /* State */
    atomic_int          generating;               /* 1 if inference in progress */

    /* Timing */
    struct timespec     start_time;               /* process start */
    struct timespec     last_report_time;          /* last periodic report */
    int32_t             report_interval_seconds;   /* 0 = disabled */
} stats_t;

void  stats_init(stats_t *st, int32_t report_interval_seconds);
void  stats_report(stats_t *st, logger_t *lg);
void  stats_check_report(stats_t *st, logger_t *lg);
char *stats_to_json(const stats_t *st);

#endif /* PROF_STATS_H */
