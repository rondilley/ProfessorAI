/*
 * logger.h -- Thread-safe logging to stderr, syslog, and/or file
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_LOGGER_H
#define PROF_LOGGER_H

#include <pthread.h>
#include <stdio.h>

typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

typedef struct {
    log_level_t      level;
    FILE            *log_file;      /* NULL = no file output */
    pthread_mutex_t  mutex;
    int              use_color;     /* 1 if stderr is a tty */
    int              use_stderr;    /* 1 = write to stderr */
    int              use_syslog;    /* 1 = write to syslog */
} logger_t;

int   logger_init(logger_t *lg, log_level_t level, const char *log_path,
                  int use_syslog);
void  logger_destroy(logger_t *lg);
void  logger_log(logger_t *lg, log_level_t level, const char *file, int line,
                 const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* Call after daemonize to switch from stderr to syslog */
void  logger_enable_syslog(logger_t *lg);

#define LOG_TRACE(lg, ...) logger_log(lg, LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_DEBUG(lg, ...) logger_log(lg, LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(lg, ...)  logger_log(lg, LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(lg, ...)  logger_log(lg, LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(lg, ...) logger_log(lg, LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(lg, ...) logger_log(lg, LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

#endif /* PROF_LOGGER_H */
