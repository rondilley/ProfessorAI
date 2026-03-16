/*
 * api_types.c -- OpenAI-compatible request/response types + JSON conversion
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "api_types.h"

#include "cJSON.h"
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int valid_role(const char *role)
{
    return (strcmp(role, "system") == 0 ||
            strcmp(role, "user") == 0 ||
            strcmp(role, "assistant") == 0);
}

int chat_request_parse(chat_request_t *req, const char *json, size_t len)
{
    memset(req, 0, sizeof(*req));
    req->temperature      = NAN;
    req->top_p            = NAN;
    req->max_tokens       = -1;
    req->frequency_penalty = NAN;
    req->presence_penalty  = NAN;
    req->repeat_penalty    = NAN;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return -1;
    }

    /* model (optional) */
    cJSON *jmodel = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(jmodel)) {
        snprintf(req->model, sizeof(req->model), "%s", jmodel->valuestring);
    }

    /* messages (required) */
    cJSON *jmsgs = cJSON_GetObjectItem(root, "messages");
    if (!cJSON_IsArray(jmsgs)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = cJSON_GetArraySize(jmsgs);
    if (count <= 0) {
        cJSON_Delete(root);
        return -1;
    }
    if (count > PROF_MAX_MESSAGES) {
        cJSON_Delete(root);
        return -1;
    }

    req->messages = calloc((size_t)count, sizeof(chat_message_t));
    if (!req->messages) {
        cJSON_Delete(root);
        return -1;
    }
    req->message_count = (int32_t)count;

    for (int i = 0; i < count; i++) {
        cJSON *msg = cJSON_GetArrayItem(jmsgs, i);
        cJSON *jrole = cJSON_GetObjectItem(msg, "role");
        cJSON *jcontent = cJSON_GetObjectItem(msg, "content");

        if (!cJSON_IsString(jrole)) {
            chat_request_free(req);
            cJSON_Delete(root);
            return -1;
        }

        if (!valid_role(jrole->valuestring)) {
            chat_request_free(req);
            cJSON_Delete(root);
            return -1;
        }

        snprintf(req->messages[i].role, sizeof(req->messages[i].role),
                 "%s", jrole->valuestring);

        if (cJSON_IsString(jcontent)) {
            snprintf(req->messages[i].content, sizeof(req->messages[i].content),
                     "%s", jcontent->valuestring);
        }
        /* null/missing content -> empty string (assistant prefill) */
    }

    /* Optional fields */
    cJSON *jtemp = cJSON_GetObjectItem(root, "temperature");
    if (cJSON_IsNumber(jtemp)) {
        req->temperature = (float)jtemp->valuedouble;
    }

    cJSON *jtopp = cJSON_GetObjectItem(root, "top_p");
    if (cJSON_IsNumber(jtopp)) {
        req->top_p = (float)jtopp->valuedouble;
    }

    cJSON *jmax = cJSON_GetObjectItem(root, "max_tokens");
    if (cJSON_IsNumber(jmax)) {
        req->max_tokens = (int32_t)jmax->valuedouble;
    }

    cJSON *jfreq = cJSON_GetObjectItem(root, "frequency_penalty");
    if (cJSON_IsNumber(jfreq)) {
        req->frequency_penalty = (float)jfreq->valuedouble;
    }

    cJSON *jpres = cJSON_GetObjectItem(root, "presence_penalty");
    if (cJSON_IsNumber(jpres)) {
        req->presence_penalty = (float)jpres->valuedouble;
    }

    cJSON *jrep = cJSON_GetObjectItem(root, "repeat_penalty");
    if (cJSON_IsNumber(jrep)) {
        req->repeat_penalty = (float)jrep->valuedouble;
    }

    cJSON *jstream = cJSON_GetObjectItem(root, "stream");
    if (cJSON_IsBool(jstream)) {
        req->stream = cJSON_IsTrue(jstream);
    }

    cJSON *jstop = cJSON_GetObjectItem(root, "stop");
    if (cJSON_IsArray(jstop)) {
        int sc = cJSON_GetArraySize(jstop);
        if (sc > PROF_STOP_MAX) sc = PROF_STOP_MAX;
        req->stop_count = (int32_t)sc;
        for (int i = 0; i < sc; i++) {
            cJSON *s = cJSON_GetArrayItem(jstop, i);
            if (cJSON_IsString(s)) {
                snprintf(req->stop[i], sizeof(req->stop[i]), "%s", s->valuestring);
            }
        }
    } else if (cJSON_IsString(jstop)) {
        req->stop_count = 1;
        snprintf(req->stop[0], sizeof(req->stop[0]), "%s", jstop->valuestring);
    }

    cJSON_Delete(root);
    return 0;
}

void chat_request_free(chat_request_t *req)
{
    if (req->messages) {
        free(req->messages);
        req->messages = NULL;
    }
    req->message_count = 0;
}

int completion_request_parse(completion_request_t *req, const char *json, size_t len)
{
    memset(req, 0, sizeof(*req));
    req->temperature   = NAN;
    req->top_p         = NAN;
    req->max_tokens    = -1;
    req->repeat_penalty = NAN;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return -1;
    }

    cJSON *jmodel = cJSON_GetObjectItem(root, "model");
    if (cJSON_IsString(jmodel)) {
        snprintf(req->model, sizeof(req->model), "%s", jmodel->valuestring);
    }

    cJSON *jprompt = cJSON_GetObjectItem(root, "prompt");
    if (!cJSON_IsString(jprompt)) {
        cJSON_Delete(root);
        return -1;
    }
    snprintf(req->prompt, sizeof(req->prompt), "%s", jprompt->valuestring);

    cJSON *jtemp = cJSON_GetObjectItem(root, "temperature");
    if (cJSON_IsNumber(jtemp)) req->temperature = (float)jtemp->valuedouble;

    cJSON *jtopp = cJSON_GetObjectItem(root, "top_p");
    if (cJSON_IsNumber(jtopp)) req->top_p = (float)jtopp->valuedouble;

    cJSON *jmax = cJSON_GetObjectItem(root, "max_tokens");
    if (cJSON_IsNumber(jmax)) req->max_tokens = (int32_t)jmax->valuedouble;

    cJSON *jrep = cJSON_GetObjectItem(root, "repeat_penalty");
    if (cJSON_IsNumber(jrep)) req->repeat_penalty = (float)jrep->valuedouble;

    cJSON *jstream = cJSON_GetObjectItem(root, "stream");
    if (cJSON_IsBool(jstream)) req->stream = cJSON_IsTrue(jstream);

    cJSON *jstop = cJSON_GetObjectItem(root, "stop");
    if (cJSON_IsArray(jstop)) {
        int sc = cJSON_GetArraySize(jstop);
        if (sc > PROF_STOP_MAX) sc = PROF_STOP_MAX;
        req->stop_count = (int32_t)sc;
        for (int i = 0; i < sc; i++) {
            cJSON *s = cJSON_GetArrayItem(jstop, i);
            if (cJSON_IsString(s)) {
                snprintf(req->stop[i], sizeof(req->stop[i]), "%s", s->valuestring);
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

char *chat_response_to_json(const chat_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "id", resp->id);
    cJSON_AddStringToObject(root, "object", "chat.completion");
    cJSON_AddNumberToObject(root, "created", (double)resp->created);
    cJSON_AddStringToObject(root, "model", resp->model);

    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);

    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");
    cJSON_AddStringToObject(message, "content", resp->content);
    cJSON_AddItemToObject(choice, "message", message);

    cJSON_AddStringToObject(choice, "finish_reason", resp->finish_reason);
    cJSON_AddItemToArray(choices, choice);

    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "prompt_tokens", resp->usage.prompt_tokens);
    cJSON_AddNumberToObject(usage, "completion_tokens", resp->usage.completion_tokens);
    cJSON_AddNumberToObject(usage, "total_tokens", resp->usage.total_tokens);
    cJSON_AddItemToObject(root, "usage", usage);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *completion_response_to_json(const completion_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "id", resp->id);
    cJSON_AddStringToObject(root, "object", "text_completion");
    cJSON_AddNumberToObject(root, "created", (double)resp->created);
    cJSON_AddStringToObject(root, "model", resp->model);

    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);
    cJSON_AddStringToObject(choice, "text", resp->text);
    cJSON_AddStringToObject(choice, "finish_reason", resp->finish_reason);
    cJSON_AddItemToArray(choices, choice);

    cJSON *usage = cJSON_CreateObject();
    cJSON_AddNumberToObject(usage, "prompt_tokens", resp->usage.prompt_tokens);
    cJSON_AddNumberToObject(usage, "completion_tokens", resp->usage.completion_tokens);
    cJSON_AddNumberToObject(usage, "total_tokens", resp->usage.total_tokens);
    cJSON_AddItemToObject(root, "usage", usage);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *stream_chunk_to_json(const stream_chunk_t *chunk)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "id", chunk->id);
    cJSON_AddStringToObject(root, "object",
                            chunk->is_chat ? "chat.completion.chunk" : "text_completion.chunk");
    cJSON_AddNumberToObject(root, "created", (double)chunk->created);
    cJSON_AddStringToObject(root, "model", chunk->model);

    cJSON *choices = cJSON_AddArrayToObject(root, "choices");
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);

    if (chunk->is_chat) {
        cJSON *delta = cJSON_CreateObject();
        if (chunk->delta_content[0] != '\0') {
            cJSON_AddStringToObject(delta, "content", chunk->delta_content);
        }
        cJSON_AddItemToObject(choice, "delta", delta);
    } else {
        if (chunk->delta_content[0] != '\0') {
            cJSON_AddStringToObject(choice, "text", chunk->delta_content);
        } else {
            cJSON_AddStringToObject(choice, "text", "");
        }
    }

    if (chunk->finish_reason[0] != '\0') {
        cJSON_AddStringToObject(choice, "finish_reason", chunk->finish_reason);
    } else {
        cJSON_AddNullToObject(choice, "finish_reason");
    }

    cJSON_AddItemToArray(choices, choice);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *models_list_to_json(const char *model_id)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "object", "list");

    cJSON *data = cJSON_AddArrayToObject(root, "data");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "id", model_id);
    cJSON_AddStringToObject(m, "object", "model");
    cJSON_AddNumberToObject(m, "created", 0);
    cJSON_AddStringToObject(m, "owned_by", "local");
    cJSON_AddItemToArray(data, m);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char *error_to_json(int code, const char *message, const char *type)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *err = cJSON_CreateObject();
    if (!err) { cJSON_Delete(root); return NULL; }
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddStringToObject(err, "type", type);
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddItemToObject(root, "error", err);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void generate_request_id(char *buf, size_t len, const char *prefix)
{
    unsigned char rand_bytes[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, rand_bytes, sizeof(rand_bytes));
        close(fd);
        if (n < (ssize_t)sizeof(rand_bytes)) {
            /* Fallback: use time-based */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            memcpy(rand_bytes, &ts, sizeof(rand_bytes) < sizeof(ts) ? sizeof(rand_bytes) : sizeof(ts));
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        memcpy(rand_bytes, &ts, sizeof(rand_bytes) < sizeof(ts) ? sizeof(rand_bytes) : sizeof(ts));
    }

    char hex[33];
    for (int i = 0; i < 12; i++) {
        snprintf(hex + i * 2, 3, "%02x", rand_bytes[i]);
    }
    hex[24] = '\0';

    snprintf(buf, len, "%s-%s", prefix, hex);
}
