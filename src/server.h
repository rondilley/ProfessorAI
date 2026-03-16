/*
 * server.h -- HTTP server: routing, auth, admission control, SSE streaming
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_SERVER_H
#define PROF_SERVER_H

#include "api_types.h"
#include "config.h"
#include "logger.h"
#include "stats.h"
#include "worker.h"

#include <mongoose.h>

typedef struct {
    config_t     *cfg;
    logger_t     *lg;
    worker_t     *worker;
    char          model_id[PROF_MODEL_MAX];
    stats_t      *stats;
    int           active_connections;
    int           ready;
    unsigned long listener_id;
} server_ctx_t;

int   server_init(struct mg_mgr *mgr, server_ctx_t *sctx);
void  server_run(struct mg_mgr *mgr, int poll_ms);
void  server_destroy(struct mg_mgr *mgr);

#endif /* PROF_SERVER_H */
