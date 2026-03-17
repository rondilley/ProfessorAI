/*
 * main.c -- Entry point for professord
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "config.h"
#include "daemon.h"
#include "inference.h"
#include "logger.h"
#include "recommend.h"
#include "server.h"
#include "stats.h"
#include "worker.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void run_recommend(void)
{
    hw_info_t hw;
    hw_detect(&hw);
    printf("\n");
    hw_print_summary(&hw);
    hw_print_recommendations(&hw);
}

#ifdef PROF_USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

int main(int argc, char **argv)
{
    config_t           cfg;
    logger_t           lg;
    inference_engine_t engine;
    worker_t           worker;
    struct mg_mgr      mgr;
    server_ctx_t       sctx;
    stats_t            stats;

    /* 1. Config defaults */
    config_defaults(&cfg);

    /* 2. Check for early-exit flags (before config validation) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--recommend") == 0) {
            run_recommend();
            return 0;
        }
        if (strcmp(argv[i], "--gen-api-key") == 0) {
            unsigned char buf[32];
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd < 0 || read(fd, buf, sizeof(buf)) != sizeof(buf)) {
                fprintf(stderr, "Error: failed to read /dev/urandom\n");
                if (fd >= 0) close(fd);
                return 1;
            }
            close(fd);
            for (int j = 0; j < 32; j++) {
                printf("%02x", buf[j]);
            }
            printf("\n");
            return 0;
        }
    }

    /* 3. Extract --config path before full parse */
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            snprintf(cfg.config_file, sizeof(cfg.config_file), "%s", argv[i + 1]);
            break;
        }
    }

    /* 3b. Load config file if specified */
    if (cfg.config_file[0] != '\0') {
        if (config_load_file(&cfg, cfg.config_file) != 0) {
            fprintf(stderr, "Error: failed to load config: %s\n", cfg.config_file);
            return 1;
        }
    }

    /* 4. Second CLI pass: CLI overrides INI */
    if (config_parse_cli(&cfg, argc, argv) != 0) {
        return 1;
    }

    /* 5. Validate */
    if (config_validate(&cfg) != 0) {
        return 1;
    }

    /* 6. Init logger */
    if (logger_init(&lg, (log_level_t)cfg.log_level,
                    cfg.log_file[0] ? cfg.log_file : NULL, 0) != 0) {
        fprintf(stderr, "Error: failed to init logger\n");
        return 1;
    }

    LOG_INFO(&lg, "professord starting");

    /* 7. Install signal handlers */
    daemon_install_signals();

    /* 8. Load model BEFORE daemonize so errors are visible */
    if (inference_init(&engine, &cfg, &lg) != 0) {
        LOG_FATAL(&lg, "failed to initialize inference engine");
        logger_destroy(&lg);
        return 1;
    }

    /* 9. Daemonize (optional, after model load) */
    if (cfg.daemonize) {
        LOG_INFO(&lg, "daemonizing -- switching to syslog");
        if (daemon_daemonize() != 0) {
            LOG_FATAL(&lg, "failed to daemonize");
            inference_destroy(&engine);
            logger_destroy(&lg);
            return 1;
        }
        /* stderr is now /dev/null -- switch to syslog */
        logger_enable_syslog(&lg);
        if (daemon_write_pidfile(cfg.pid_file) != 0) {
            LOG_WARN(&lg, "failed to write PID file: %s", cfg.pid_file);
        }
    }

    /* 10. Start worker thread */
    if (worker_init(&worker, &engine, &lg) != 0) {
        LOG_FATAL(&lg, "failed to start worker thread");
        inference_destroy(&engine);
        if (cfg.daemonize) daemon_remove_pidfile(cfg.pid_file);
        logger_destroy(&lg);
        return 1;
    }

    /* 11. Init stats */
    stats_init(&stats, cfg.stats_interval);

    /* 12. Init HTTP server */
    memset(&sctx, 0, sizeof(sctx));
    sctx.cfg    = &cfg;
    sctx.lg     = &lg;
    sctx.worker = &worker;
    sctx.stats  = &stats;
    snprintf(sctx.model_id, sizeof(sctx.model_id), "%s", cfg.model_alias);

    if (server_init(&mgr, &sctx) != 0) {
        LOG_FATAL(&lg, "failed to start HTTP server");
        worker_destroy(&worker);
        inference_destroy(&engine);
        if (cfg.daemonize) daemon_remove_pidfile(cfg.pid_file);
        logger_destroy(&lg);
        return 1;
    }

    /* 12. Signal readiness */
#ifdef PROF_USE_SYSTEMD
    sd_notify(0, "READY=1");
    LOG_INFO(&lg, "sd_notify: READY=1");
#endif

    /* Always announce readiness regardless of log level */
    logger_announce(&lg, "professord ready");

    /* 13. Event loop */
    server_run(&mgr, 100);

    /* 14. Cleanup */
    LOG_INFO(&lg, "shutting down");

    server_destroy(&mgr);
    worker_cancel(&worker);
    worker_destroy(&worker);
    inference_destroy(&engine);

    if (cfg.daemonize) {
        daemon_remove_pidfile(cfg.pid_file);
    }

    LOG_INFO(&lg, "shutdown complete");
    logger_destroy(&lg);

    return 0;
}
