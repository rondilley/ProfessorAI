/*
 * config.h -- INI + CLI configuration parsing
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_CONFIG_H
#define PROF_CONFIG_H

#include <stdint.h>

#define PROF_PATH_MAX  512
#define PROF_ADDR_MAX  64
#define PROF_NAME_MAX  128
#define PROF_KEY_MAX   128
#define PROF_ACL_MAX   16       /* max allowed IPs */

typedef struct {
    /* Model */
    char     model_path[PROF_PATH_MAX];
    char     model_alias[PROF_NAME_MAX];
    int32_t  n_ctx;
    int32_t  n_gpu_layers;
    int32_t  n_batch;
    int32_t  n_threads;          /* 0 = auto-detect */

    /* Sampling defaults */
    float    temperature;
    float    top_p;
    int32_t  top_k;
    float    repeat_penalty;
    int32_t  max_tokens;

    /* Server */
    char     listen_addr[PROF_ADDR_MAX];

    /* Authentication */
    char     api_key[PROF_KEY_MAX];

    /* IP ACL (empty = allow all) */
    char     acl[PROF_ACL_MAX][PROF_ADDR_MAX];
    int      acl_count;

    /* Timeouts */
    int32_t  max_inference_seconds;

    /* Stats */
    int32_t  stats_interval;         /* seconds between log reports, 0=off */

    /* Daemon */
    int      daemonize;
    char     pid_file[PROF_PATH_MAX];

    /* Logging */
    int      log_level;
    char     log_file[PROF_PATH_MAX];

    /* Config file path (CLI only) */
    char     config_file[PROF_PATH_MAX];
} config_t;

void  config_defaults(config_t *cfg);
int   config_load_file(config_t *cfg, const char *path);
int   config_parse_cli(config_t *cfg, int argc, char **argv);
int   config_validate(const config_t *cfg);
void  config_print_help(const char *prog);

#endif /* PROF_CONFIG_H */
