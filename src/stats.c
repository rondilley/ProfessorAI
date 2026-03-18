/*
 * stats.c -- Runtime statistics tracking and reporting
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void stats_init(stats_t *st, int32_t report_interval_seconds)
{
    memset(st, 0, sizeof(*st));
    clock_gettime(CLOCK_MONOTONIC, &st->start_time);
    st->last_report_time = st->start_time;
    st->report_interval_seconds = report_interval_seconds;
}

static double elapsed_since(const struct timespec *ref)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - ref->tv_sec) +
           (double)(now.tv_nsec - ref->tv_nsec) / 1e9;
}

void stats_report(stats_t *st, logger_t *lg)
{
    double uptime = elapsed_since(&st->start_time);

    int64_t req_total = atomic_load(&st->req_chat) +
                        atomic_load(&st->req_completion) +
                        atomic_load(&st->req_stream);
    int64_t inferences = atomic_load(&st->inferences_total);
    int64_t tok_in     = atomic_load(&st->tokens_prompt_total);
    int64_t tok_out    = atomic_load(&st->tokens_completion_total);
    int64_t inf_ms     = atomic_load(&st->inference_ms_total);
    int64_t err_total  = atomic_load(&st->err_400) +
                         atomic_load(&st->err_401) +
                         atomic_load(&st->err_405) +
                         atomic_load(&st->err_503) +
                         atomic_load(&st->err_500);
    int64_t conn_total = atomic_load(&st->connections_total);
    int64_t conn_rej   = atomic_load(&st->connections_rejected);
    int     active     = st->connections_active;
    int     generating = atomic_load(&st->generating);

    double avg_inf_ms = (inferences > 0) ? (double)inf_ms / (double)inferences : 0.0;
    double avg_tps = (inf_ms > 0) ? (double)tok_out / ((double)inf_ms / 1000.0) : 0.0;

    int uptime_h = (int)(uptime / 3600);
    int uptime_m = (int)((uptime - uptime_h * 3600) / 60);
    int uptime_s = (int)(uptime) % 60;

    LOG_INFO(lg,
        "[Planet Express status report] uptime=%dh%02dm%02ds "
        "conns=%lld/%d/%lld "
        "reqs=%lld errs=%lld "
        "inferences=%lld tok_in=%lld tok_out=%lld "
        "avg_ms=%.0f avg_tps=%.1f %s",
        uptime_h, uptime_m, uptime_s,
        (long long)conn_total, active, (long long)conn_rej,
        (long long)req_total, (long long)err_total,
        (long long)inferences, (long long)tok_in, (long long)tok_out,
        avg_inf_ms, avg_tps,
        generating ? "GENERATING" : "idle");

    clock_gettime(CLOCK_MONOTONIC, &st->last_report_time);
}

void stats_check_report(stats_t *st, logger_t *lg)
{
    if (st->report_interval_seconds <= 0) return;

    double since_last = elapsed_since(&st->last_report_time);
    if (since_last >= (double)st->report_interval_seconds) {
        stats_report(st, lg);
    }
}

char *stats_to_json(const stats_t *st)
{
    double uptime = elapsed_since(&st->start_time);

    int64_t inferences = atomic_load(&st->inferences_total);
    int64_t tok_in     = atomic_load(&st->tokens_prompt_total);
    int64_t tok_out    = atomic_load(&st->tokens_completion_total);
    int64_t inf_ms     = atomic_load(&st->inference_ms_total);

    double avg_inf_ms = (inferences > 0) ? (double)inf_ms / (double)inferences : 0.0;
    double avg_tps = (inf_ms > 0) ? (double)tok_out / ((double)inf_ms / 1000.0) : 0.0;

    size_t bufsz = 4096;
    char *buf = malloc(bufsz);
    if (!buf) return NULL;

    snprintf(buf, bufsz,
        "{"
        "\"uptime_seconds\":%.0f,"
        "\"connections\":{\"total\":%lld,\"active\":%d,\"rejected\":%lld},"
        "\"requests\":{\"chat\":%lld,\"completion\":%lld,\"stream\":%lld,"
        "\"health\":%lld,\"models\":%lld},"
        "\"errors\":{\"bad_request\":%lld,\"unauthorized\":%lld,"
        "\"method_not_allowed\":%lld,\"server_busy\":%lld,\"backend_error\":%lld},"
        "\"inference\":{\"total\":%lld,\"tokens_prompt\":%lld,"
        "\"tokens_completion\":%lld,\"avg_latency_ms\":%.0f,"
        "\"avg_tokens_per_second\":%.1f,\"generating\":%s}"
        "}",
        uptime,
        (long long)atomic_load(&st->connections_total),
        st->connections_active,
        (long long)atomic_load(&st->connections_rejected),
        (long long)atomic_load(&st->req_chat),
        (long long)atomic_load(&st->req_completion),
        (long long)atomic_load(&st->req_stream),
        (long long)atomic_load(&st->req_health),
        (long long)atomic_load(&st->req_models),
        (long long)atomic_load(&st->err_400),
        (long long)atomic_load(&st->err_401),
        (long long)atomic_load(&st->err_405),
        (long long)atomic_load(&st->err_503),
        (long long)atomic_load(&st->err_500),
        (long long)inferences,
        (long long)tok_in,
        (long long)tok_out,
        avg_inf_ms, avg_tps,
        atomic_load(&st->generating) ? "true" : "false");

    return buf;
}
