/*
 * config.c -- INI + CLI configuration parsing
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "config.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void config_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    snprintf(cfg->model_alias, sizeof(cfg->model_alias), "%s", "local-model");
    cfg->n_ctx           = 4096;
    cfg->n_gpu_layers    = 99;
    cfg->n_batch         = 2048;
    cfg->n_threads       = 0;      /* 0 = auto-detect */

    cfg->temperature     = 0.7f;
    cfg->top_p           = 0.9f;
    cfg->top_k           = 40;
    cfg->repeat_penalty  = 1.1f;
    cfg->max_tokens      = 512;

    snprintf(cfg->listen_addr, sizeof(cfg->listen_addr), "%s", "127.0.0.1:8080");

    cfg->max_inference_seconds = 300;
    cfg->stats_interval = 60;

    snprintf(cfg->pid_file, sizeof(cfg->pid_file), "%s",
             "/run/professord/professord.pid");

    cfg->log_level = 2; /* LOG_INFO */
}

/* Trim leading/trailing whitespace in-place, return pointer into buf */
static char *trim(char *buf)
{
    while (*buf && isspace((unsigned char)*buf)) {
        buf++;
    }
    if (*buf == '\0') {
        return buf;
    }
    char *end = buf + strlen(buf) - 1;
    while (end > buf && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return buf;
}

static void set_string(char *dst, size_t dst_size, const char *value)
{
    snprintf(dst, dst_size, "%s", value);
}

static void set_config_field(config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "model_path") == 0) {
        set_string(cfg->model_path, sizeof(cfg->model_path), value);
    } else if (strcmp(key, "model_alias") == 0) {
        set_string(cfg->model_alias, sizeof(cfg->model_alias), value);
    } else if (strcmp(key, "n_ctx") == 0) {
        cfg->n_ctx = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "n_gpu_layers") == 0) {
        cfg->n_gpu_layers = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "n_batch") == 0) {
        cfg->n_batch = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "n_threads") == 0) {
        cfg->n_threads = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "temperature") == 0) {
        cfg->temperature = strtof(value, NULL);
    } else if (strcmp(key, "top_p") == 0) {
        cfg->top_p = strtof(value, NULL);
    } else if (strcmp(key, "top_k") == 0) {
        cfg->top_k = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "repeat_penalty") == 0) {
        cfg->repeat_penalty = strtof(value, NULL);
    } else if (strcmp(key, "max_tokens") == 0) {
        cfg->max_tokens = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "listen_addr") == 0) {
        set_string(cfg->listen_addr, sizeof(cfg->listen_addr), value);
    } else if (strcmp(key, "api_key") == 0) {
        set_string(cfg->api_key, sizeof(cfg->api_key), value);
    } else if (strcmp(key, "max_inference_seconds") == 0) {
        cfg->max_inference_seconds = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "stats_interval") == 0) {
        cfg->stats_interval = (int32_t)strtol(value, NULL, 10);
    } else if (strcmp(key, "daemonize") == 0) {
        cfg->daemonize = (int)strtol(value, NULL, 10);
    } else if (strcmp(key, "pid_file") == 0) {
        set_string(cfg->pid_file, sizeof(cfg->pid_file), value);
    } else if (strcmp(key, "log_level") == 0) {
        cfg->log_level = (int)strtol(value, NULL, 10);
    } else if (strcmp(key, "log_file") == 0) {
        set_string(cfg->log_file, sizeof(cfg->log_file), value);
    } else if (strcmp(key, "allow_ip") == 0) {
        /* Comma-separated list of IPs */
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", value);
        char *saveptr = NULL;
        char *tok = strtok_r(buf, ",", &saveptr);
        while (tok && cfg->acl_count < PROF_ACL_MAX) {
            char *ip = trim(tok);
            if (ip[0] != '\0') {
                snprintf(cfg->acl[cfg->acl_count], PROF_ADDR_MAX, "%s", ip);
                cfg->acl_count++;
            }
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }
    /* Unknown keys are silently ignored */
}

int config_load_file(config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }

        /* Split on first '=' */
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key   = trim(p);
        char *value = trim(eq + 1);

        set_config_field(cfg, key, value);
    }

    fclose(fp);
    return 0;
}

int config_parse_cli(config_t *cfg, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            config_print_help(argv[0]);
            exit(0);
        }

        if (strcmp(arg, "--recommend") == 0) {
            continue; /* handled in main.c */
        }

        if (strcmp(arg, "--gen-api-key") == 0) {
            continue; /* handled in main.c */
        }

        /* All remaining options require a value */
        if (i + 1 >= argc) {
            if (strcmp(arg, "--daemonize") == 0) {
                cfg->daemonize = 1;
                continue;
            }
            fprintf(stderr, "Missing value for %s\n", arg);
            return -1;
        }

        const char *val = argv[i + 1];

        if (strcmp(arg, "--config") == 0) {
            set_string(cfg->config_file, sizeof(cfg->config_file), val);
        } else if (strcmp(arg, "--model") == 0) {
            set_string(cfg->model_path, sizeof(cfg->model_path), val);
        } else if (strcmp(arg, "--model-alias") == 0) {
            set_string(cfg->model_alias, sizeof(cfg->model_alias), val);
        } else if (strcmp(arg, "--n-ctx") == 0 ||
                   strcmp(arg, "--fing-longer") == 0) {
            cfg->n_ctx = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--n-gpu-layers") == 0) {
            cfg->n_gpu_layers = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--n-batch") == 0) {
            cfg->n_batch = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--n-threads") == 0) {
            cfg->n_threads = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--temperature") == 0) {
            cfg->temperature = strtof(val, NULL);
        } else if (strcmp(arg, "--top-p") == 0) {
            cfg->top_p = strtof(val, NULL);
        } else if (strcmp(arg, "--top-k") == 0) {
            cfg->top_k = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--repeat-penalty") == 0) {
            cfg->repeat_penalty = strtof(val, NULL);
        } else if (strcmp(arg, "--max-tokens") == 0) {
            cfg->max_tokens = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--listen-addr") == 0) {
            set_string(cfg->listen_addr, sizeof(cfg->listen_addr), val);
        } else if (strcmp(arg, "--api-key") == 0) {
            set_string(cfg->api_key, sizeof(cfg->api_key), val);
        } else if (strcmp(arg, "--max-inference-seconds") == 0) {
            cfg->max_inference_seconds = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--stats-interval") == 0) {
            cfg->stats_interval = (int32_t)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--daemonize") == 0) {
            cfg->daemonize = 1;
            continue; /* don't skip next arg */
        } else if (strcmp(arg, "--pid-file") == 0) {
            set_string(cfg->pid_file, sizeof(cfg->pid_file), val);
        } else if (strcmp(arg, "--log-level") == 0) {
            cfg->log_level = (int)strtol(val, NULL, 10);
        } else if (strcmp(arg, "--log-file") == 0) {
            set_string(cfg->log_file, sizeof(cfg->log_file), val);
        } else if (strcmp(arg, "--allow-ip") == 0) {
            /* Comma-separated list of IPs, same as INI parser */
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s", val);
            char *saveptr = NULL;
            char *tok = strtok_r(buf, ",", &saveptr);
            while (tok && cfg->acl_count < PROF_ACL_MAX) {
                char *ip = trim(tok);
                if (ip[0] != '\0') {
                    snprintf(cfg->acl[cfg->acl_count], PROF_ADDR_MAX, "%s", ip);
                    cfg->acl_count++;
                }
                tok = strtok_r(NULL, ",", &saveptr);
            }
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return -1;
        }

        i++; /* skip value */
    }

    return 0;
}

int config_validate(const config_t *cfg)
{
    if (cfg->model_path[0] == '\0') {
        fprintf(stderr, "Error: model_path is required (--model or config file)\n");
        return -1;
    }
    if (cfg->n_ctx <= 0) {
        fprintf(stderr, "Error: n_ctx must be > 0\n");
        return -1;
    }
    if (cfg->max_tokens <= 0 || cfg->max_tokens > cfg->n_ctx) {
        fprintf(stderr, "Error: max_tokens must be > 0 and <= n_ctx\n");
        return -1;
    }
    if (strchr(cfg->listen_addr, ':') == NULL) {
        fprintf(stderr, "Error: listen_addr must contain ':' (e.g., 127.0.0.1:8080)\n");
        return -1;
    }
    if (cfg->max_inference_seconds <= 0) {
        fprintf(stderr, "Error: max_inference_seconds must be > 0\n");
        return -1;
    }
    return 0;
}

void config_print_help(const char *prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  --config PATH              Path to INI config file\n");
    printf("  --model PATH               Path to GGUF model file (required)\n");
    printf("  --model-alias NAME         Model name in API responses [local-model]\n");
    printf("  --n-ctx N                  Context window size [4096]\n");
    printf("  --n-gpu-layers N           GPU layers to offload [99]\n");
    printf("  --n-batch N                Batch size [2048]\n");
    printf("  --n-threads N              CPU threads [0=auto]\n");
    printf("  --temperature F            Sampling temperature [0.7]\n");
    printf("  --top-p F                  Nucleus sampling threshold [0.9]\n");
    printf("  --top-k N                  Top-k sampling [40]\n");
    printf("  --repeat-penalty F         Repetition penalty [1.1]\n");
    printf("  --max-tokens N             Default max tokens [512]\n");
    printf("  --listen-addr ADDR         HTTP listen address [127.0.0.1:8080]\n");
    printf("  --api-key KEY              Bearer token for auth (empty=no auth)\n");
    printf("  --max-inference-seconds N  Generation timeout [300]\n");
    printf("  --daemonize                Run as background daemon\n");
    printf("  --pid-file PATH            PID file path [/run/professord/professord.pid]\n");
    printf("  --log-level N              0=TRACE..5=FATAL [2=INFO]\n");
    printf("  --log-file PATH            Log to file (in addition to stderr)\n");
    printf("  --allow-ip IP              Add IP to ACL (repeatable, empty=allow all)\n");
    printf("  --stats-interval N         Seconds between stats log [60] (0=off)\n");
    printf("  --gen-api-key              Generate a random API key and exit\n");
    printf("  --recommend                Detect hardware, recommend models\n");
    printf("  --version                  Show version and exit\n");
    printf("  --help                     Show this help\n");
}
