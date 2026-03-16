/*
 * test_api_types.c -- Unit tests for api_types module
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "api_types.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_parse_chat_request(void)
{
    const char *json =
        "{"
        "  \"model\": \"test-model\","
        "  \"messages\": ["
        "    {\"role\": \"system\", \"content\": \"You are helpful.\"},"
        "    {\"role\": \"user\", \"content\": \"Hello\"}"
        "  ],"
        "  \"temperature\": 0.5,"
        "  \"top_p\": 0.8,"
        "  \"max_tokens\": 256,"
        "  \"stream\": true,"
        "  \"stop\": [\"\\n\\n\"]"
        "}";

    chat_request_t req;
    int rc = chat_request_parse(&req, json, strlen(json));
    assert(rc == 0);
    assert(strcmp(req.model, "test-model") == 0);
    assert(req.message_count == 2);
    assert(strcmp(req.messages[0].role, "system") == 0);
    assert(strcmp(req.messages[0].content, "You are helpful.") == 0);
    assert(strcmp(req.messages[1].role, "user") == 0);
    assert(strcmp(req.messages[1].content, "Hello") == 0);
    assert(fabsf(req.temperature - 0.5f) < 0.001f);
    assert(fabsf(req.top_p - 0.8f) < 0.001f);
    assert(req.max_tokens == 256);
    assert(req.stream == 1);
    assert(req.stop_count == 1);
    assert(strcmp(req.stop[0], "\n\n") == 0);

    chat_request_free(&req);
    printf("  PASS: test_parse_chat_request\n");
}

static void test_parse_chat_request_minimal(void)
{
    const char *json =
        "{\"messages\": [{\"role\": \"user\", \"content\": \"Hi\"}]}";

    chat_request_t req;
    int rc = chat_request_parse(&req, json, strlen(json));
    assert(rc == 0);
    assert(req.message_count == 1);
    assert(isnan(req.temperature));
    assert(isnan(req.top_p));
    assert(req.max_tokens == -1);
    assert(req.stream == 0);
    assert(req.stop_count == 0);

    chat_request_free(&req);
    printf("  PASS: test_parse_chat_request_minimal\n");
}

static void test_parse_chat_request_bad_role(void)
{
    const char *json =
        "{\"messages\": [{\"role\": \"villain\", \"content\": \"Muahaha\"}]}";

    chat_request_t req;
    int rc = chat_request_parse(&req, json, strlen(json));
    assert(rc == -1);
    printf("  PASS: test_parse_chat_request_bad_role\n");
}

static void test_parse_chat_request_no_messages(void)
{
    const char *json = "{\"model\": \"test\"}";

    chat_request_t req;
    int rc = chat_request_parse(&req, json, strlen(json));
    assert(rc == -1);
    printf("  PASS: test_parse_chat_request_no_messages\n");
}

static void test_parse_chat_request_invalid_json(void)
{
    const char *json = "this is not json at all";

    chat_request_t req;
    int rc = chat_request_parse(&req, json, strlen(json));
    assert(rc == -1);
    printf("  PASS: test_parse_chat_request_invalid_json\n");
}

static void test_chat_response_to_json(void)
{
    chat_response_t resp;
    memset(&resp, 0, sizeof(resp));
    snprintf(resp.id, sizeof(resp.id), "chatcmpl-test123");
    snprintf(resp.model, sizeof(resp.model), "test-model");
    resp.created = 1710000000;
    snprintf(resp.content, sizeof(resp.content), "Hello world!");
    snprintf(resp.finish_reason, sizeof(resp.finish_reason), "stop");
    resp.usage.prompt_tokens = 10;
    resp.usage.completion_tokens = 5;
    resp.usage.total_tokens = 15;

    char *json = chat_response_to_json(&resp);
    assert(json != NULL);
    assert(strstr(json, "chatcmpl-test123") != NULL);
    assert(strstr(json, "chat.completion") != NULL);
    assert(strstr(json, "Hello world!") != NULL);
    assert(strstr(json, "\"stop\"") != NULL);

    free(json);
    printf("  PASS: test_chat_response_to_json\n");
}

static void test_stream_chunk_to_json(void)
{
    stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    snprintf(chunk.id, sizeof(chunk.id), "chatcmpl-abc");
    snprintf(chunk.model, sizeof(chunk.model), "test");
    chunk.created = 1710000000;
    snprintf(chunk.delta_content, sizeof(chunk.delta_content), "Hello");
    chunk.is_chat = 1;

    char *json = stream_chunk_to_json(&chunk);
    assert(json != NULL);
    assert(strstr(json, "chat.completion.chunk") != NULL);
    assert(strstr(json, "\"delta\"") != NULL);
    assert(strstr(json, "Hello") != NULL);
    assert(strstr(json, "\"finish_reason\":null") != NULL);

    free(json);
    printf("  PASS: test_stream_chunk_to_json\n");
}

static void test_generate_request_id(void)
{
    char buf[PROF_ID_MAX];
    generate_request_id(buf, sizeof(buf), "chatcmpl");

    assert(strncmp(buf, "chatcmpl-", 9) == 0);
    assert(strlen(buf) > 9);

    /* Generate another and confirm they're different */
    char buf2[PROF_ID_MAX];
    generate_request_id(buf2, sizeof(buf2), "chatcmpl");
    assert(strcmp(buf, buf2) != 0);

    printf("  PASS: test_generate_request_id\n");
}

static void test_error_to_json(void)
{
    char *json = error_to_json(400, "Bad request", "invalid_json");
    assert(json != NULL);
    assert(strstr(json, "Bad request") != NULL);
    assert(strstr(json, "invalid_json") != NULL);
    assert(strstr(json, "400") != NULL);

    free(json);
    printf("  PASS: test_error_to_json\n");
}

static void test_models_list_to_json(void)
{
    char *json = models_list_to_json("my-model");
    assert(json != NULL);
    assert(strstr(json, "\"list\"") != NULL);
    assert(strstr(json, "my-model") != NULL);
    assert(strstr(json, "\"data\"") != NULL);

    free(json);
    printf("  PASS: test_models_list_to_json\n");
}

static void test_completion_request_parse(void)
{
    const char *json =
        "{\"prompt\": \"Once upon a time\", \"max_tokens\": 100, \"stream\": false}";

    completion_request_t req;
    int rc = completion_request_parse(&req, json, strlen(json));
    assert(rc == 0);
    assert(strcmp(req.prompt, "Once upon a time") == 0);
    assert(req.max_tokens == 100);
    assert(req.stream == 0);

    printf("  PASS: test_completion_request_parse\n");
}

int main(void)
{
    printf("test_api_types:\n");
    test_parse_chat_request();
    test_parse_chat_request_minimal();
    test_parse_chat_request_bad_role();
    test_parse_chat_request_no_messages();
    test_parse_chat_request_invalid_json();
    test_chat_response_to_json();
    test_stream_chunk_to_json();
    test_generate_request_id();
    test_error_to_json();
    test_models_list_to_json();
    test_completion_request_parse();
    printf("All api_types tests passed.\n");
    return 0;
}
