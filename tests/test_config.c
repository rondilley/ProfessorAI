/*
 * test_config.c -- Unit tests for config module
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "config.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_defaults(void)
{
    config_t cfg;
    config_defaults(&cfg);

    assert(cfg.model_path[0] == '\0');
    assert(strcmp(cfg.model_alias, "local-model") == 0);
    assert(cfg.n_ctx == 4096);
    assert(cfg.n_gpu_layers == 99);
    assert(cfg.n_batch == 512);
    assert(fabsf(cfg.temperature - 0.7f) < 0.001f);
    assert(fabsf(cfg.top_p - 0.9f) < 0.001f);
    assert(cfg.top_k == 40);
    assert(fabsf(cfg.repeat_penalty - 1.1f) < 0.001f);
    assert(cfg.max_tokens == 512);
    assert(strcmp(cfg.listen_addr, "127.0.0.1:8080") == 0);
    assert(cfg.api_key[0] == '\0');
    assert(cfg.max_inference_seconds == 300);
    assert(cfg.daemonize == 0);
    assert(cfg.log_level == 2);

    printf("  PASS: test_defaults\n");
}

static void test_parse_cli(void)
{
    config_t cfg;
    config_defaults(&cfg);

    char *argv[] = {
        "professord",
        "--model", "/tmp/test.gguf",
        "--n-ctx", "8192",
        "--temperature", "0.5",
        "--api-key", "testkey123",
        "--max-inference-seconds", "60",
        "--listen-addr", "0.0.0.0:9090",
        NULL
    };
    int argc = 13;

    int rc = config_parse_cli(&cfg, argc, argv);
    assert(rc == 0);
    assert(strcmp(cfg.model_path, "/tmp/test.gguf") == 0);
    assert(cfg.n_ctx == 8192);
    assert(fabsf(cfg.temperature - 0.5f) < 0.001f);
    assert(strcmp(cfg.api_key, "testkey123") == 0);
    assert(cfg.max_inference_seconds == 60);
    assert(strcmp(cfg.listen_addr, "0.0.0.0:9090") == 0);

    printf("  PASS: test_parse_cli\n");
}

static void test_load_file(void)
{
    /* Write temp INI */
    char tmppath[] = "/tmp/test_professord_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *ini =
        "model_path = /tmp/model.gguf\n"
        "model_alias = test-model\n"
        "n_ctx = 2048\n"
        "temperature = 0.3\n"
        "api_key = filekey\n"
        "max_inference_seconds = 120\n"
        "listen_addr = 192.168.1.1:7070\n";

    write(fd, ini, strlen(ini));
    close(fd);

    config_t cfg;
    config_defaults(&cfg);

    int rc = config_load_file(&cfg, tmppath);
    assert(rc == 0);
    assert(strcmp(cfg.model_path, "/tmp/model.gguf") == 0);
    assert(strcmp(cfg.model_alias, "test-model") == 0);
    assert(cfg.n_ctx == 2048);
    assert(fabsf(cfg.temperature - 0.3f) < 0.001f);
    assert(strcmp(cfg.api_key, "filekey") == 0);
    assert(cfg.max_inference_seconds == 120);
    assert(strcmp(cfg.listen_addr, "192.168.1.1:7070") == 0);

    unlink(tmppath);
    printf("  PASS: test_load_file\n");
}

static void test_cli_overrides_file(void)
{
    /* Write temp INI */
    char tmppath[] = "/tmp/test_professord_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *ini = "model_path = /from/file.gguf\nn_ctx = 2048\n";
    write(fd, ini, strlen(ini));
    close(fd);

    config_t cfg;
    config_defaults(&cfg);
    config_load_file(&cfg, tmppath);

    /* CLI overrides */
    char *argv[] = { "professord", "--n-ctx", "4096", NULL };
    config_parse_cli(&cfg, 3, argv);

    assert(strcmp(cfg.model_path, "/from/file.gguf") == 0); /* from file */
    assert(cfg.n_ctx == 4096); /* CLI wins */

    unlink(tmppath);
    printf("  PASS: test_cli_overrides_file\n");
}

static void test_validate_missing_model(void)
{
    config_t cfg;
    config_defaults(&cfg);

    assert(config_validate(&cfg) == -1);
    printf("  PASS: test_validate_missing_model\n");
}

static void test_validate_bad_ctx(void)
{
    config_t cfg;
    config_defaults(&cfg);
    snprintf(cfg.model_path, sizeof(cfg.model_path), "/tmp/model.gguf");
    cfg.n_ctx = 0;

    assert(config_validate(&cfg) == -1);
    printf("  PASS: test_validate_bad_ctx\n");
}

static void test_validate_good(void)
{
    config_t cfg;
    config_defaults(&cfg);
    snprintf(cfg.model_path, sizeof(cfg.model_path), "/tmp/model.gguf");

    assert(config_validate(&cfg) == 0);
    printf("  PASS: test_validate_good\n");
}

int main(void)
{
    printf("test_config:\n");
    test_defaults();
    test_parse_cli();
    test_load_file();
    test_cli_overrides_file();
    test_validate_missing_model();
    test_validate_bad_ctx();
    test_validate_good();
    printf("All config tests passed.\n");
    return 0;
}
