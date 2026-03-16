/*
 * api_types.h -- OpenAI-compatible request/response types + JSON conversion
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_API_TYPES_H
#define PROF_API_TYPES_H

#include <stddef.h>
#include <stdint.h>

#define PROF_MODEL_MAX   128
#define PROF_ROLE_MAX    32
#define PROF_CONTENT_MAX 16384   /* 16 KiB per message */
#define PROF_STOP_MAX    4
#define PROF_STOP_LEN    64
#define PROF_ID_MAX      64

/* Resource budget constants */
#define PROF_MAX_BODY_BYTES   (1 * 1024 * 1024)
#define PROF_MAX_MESSAGES     64
#define PROF_MAX_PROMPT_BYTES (256 * 1024)
#define PROF_MAX_CONNECTIONS  32

typedef struct {
    char role[PROF_ROLE_MAX];
    char content[PROF_CONTENT_MAX];
} chat_message_t;

typedef struct {
    char            model[PROF_MODEL_MAX];
    chat_message_t *messages;
    int32_t         message_count;
    float           temperature;        /* NAN = use default */
    float           top_p;
    int32_t         max_tokens;         /* -1 = use default */
    float           frequency_penalty;
    float           presence_penalty;
    float           repeat_penalty;
    char            stop[PROF_STOP_MAX][PROF_STOP_LEN];
    int32_t         stop_count;
    int             stream;
} chat_request_t;

typedef struct {
    char    model[PROF_MODEL_MAX];
    char    prompt[PROF_CONTENT_MAX];
    float   temperature;
    float   top_p;
    int32_t max_tokens;
    float   repeat_penalty;
    char    stop[PROF_STOP_MAX][PROF_STOP_LEN];
    int32_t stop_count;
    int     stream;
} completion_request_t;

typedef struct {
    int32_t prompt_tokens;
    int32_t completion_tokens;
    int32_t total_tokens;
} usage_t;

typedef struct {
    char    id[PROF_ID_MAX];
    char    model[PROF_MODEL_MAX];
    int64_t created;
    char    content[PROF_CONTENT_MAX];
    char    finish_reason[32];
    usage_t usage;
} chat_response_t;

typedef struct {
    char    id[PROF_ID_MAX];
    char    model[PROF_MODEL_MAX];
    int64_t created;
    char    text[PROF_CONTENT_MAX];
    char    finish_reason[32];
    usage_t usage;
} completion_response_t;

typedef struct {
    char    id[PROF_ID_MAX];
    char    model[PROF_MODEL_MAX];
    int64_t created;
    char    delta_content[256];
    char    finish_reason[32];
    int     is_chat;
} stream_chunk_t;

/* Parsing */
int   chat_request_parse(chat_request_t *req, const char *json, size_t len);
void  chat_request_free(chat_request_t *req);
int   completion_request_parse(completion_request_t *req, const char *json, size_t len);

/* Serialization (caller frees returned string) */
char *chat_response_to_json(const chat_response_t *resp);
char *completion_response_to_json(const completion_response_t *resp);
char *stream_chunk_to_json(const stream_chunk_t *chunk);
char *models_list_to_json(const char *model_id);
char *error_to_json(int code, const char *message, const char *type);

/* Utility */
void  generate_request_id(char *buf, size_t len, const char *prefix);

#endif /* PROF_API_TYPES_H */
