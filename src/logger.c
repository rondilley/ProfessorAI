/*
 * logger.c -- Thread-safe logging to stderr, syslog, and/or file
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "logger.h"

#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

static const char *level_colors[] = {
    "\033[90m",   /* TRACE: grey */
    "\033[36m",   /* DEBUG: cyan */
    "\033[32m",   /* INFO:  green */
    "\033[33m",   /* WARN:  yellow */
    "\033[31m",   /* ERROR: red */
    "\033[35m",   /* FATAL: magenta */
};

static const char *color_reset = "\033[0m";

/* Map our log levels to syslog priorities */
static int level_to_syslog[] = {
    LOG_DEBUG,    /* LOG_TRACE -> syslog DEBUG */
    LOG_DEBUG,    /* LOG_DEBUG -> syslog DEBUG */
    LOG_INFO,     /* LOG_INFO  -> syslog INFO */
    LOG_WARNING,  /* LOG_WARN  -> syslog WARNING */
    LOG_ERR,      /* LOG_ERROR -> syslog ERR */
    LOG_CRIT,     /* LOG_FATAL -> syslog CRIT */
};

int logger_init(logger_t *lg, log_level_t level, const char *log_path,
                int use_syslog)
{
    memset(lg, 0, sizeof(*lg));
    lg->level = level;
    lg->log_file = NULL;
    lg->use_stderr = 1;
    lg->use_syslog = use_syslog;
    lg->use_color = isatty(fileno(stderr));

    if (log_path && log_path[0] != '\0') {
        lg->log_file = fopen(log_path, "a");
        if (!lg->log_file) {
            fprintf(stderr, "logger: failed to open log file: %s\n", log_path);
            return -1;
        }
    }

    if (pthread_mutex_init(&lg->mutex, NULL) != 0) {
        if (lg->log_file) {
            fclose(lg->log_file);
            lg->log_file = NULL;
        }
        return -1;
    }

    if (use_syslog) {
        openlog("professord", LOG_PID | LOG_NDELAY, LOG_DAEMON);
    }

    return 0;
}

void logger_enable_syslog(logger_t *lg)
{
    pthread_mutex_lock(&lg->mutex);
    if (!lg->use_syslog) {
        openlog("professord", LOG_PID | LOG_NDELAY, LOG_DAEMON);
        lg->use_syslog = 1;
    }
    /* After daemonize, stderr goes to /dev/null -- disable it */
    lg->use_stderr = 0;
    lg->use_color = 0;
    pthread_mutex_unlock(&lg->mutex);
}

void logger_destroy(logger_t *lg)
{
    if (lg->log_file) {
        fclose(lg->log_file);
        lg->log_file = NULL;
    }
    if (lg->use_syslog) {
        closelog();
        lg->use_syslog = 0;
    }
    pthread_mutex_destroy(&lg->mutex);
}

void logger_log(logger_t *lg, log_level_t level, const char *file, int line,
                const char *fmt, ...)
{
    if (level < lg->level) {
        return;
    }

    /* Format the user message */
    char msgbuf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
    va_end(ap);

    /* Strip path prefix from file, keep only basename */
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    pthread_mutex_lock(&lg->mutex);

    /* Write to stderr */
    if (lg->use_stderr) {
        /* ISO-8601 timestamp */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_buf;
        gmtime_r(&ts.tv_sec, &tm_buf);

        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

        if (lg->use_color) {
            fprintf(stderr, "%s.%03ldZ %s%-5s%s %s:%d: %s\n",
                    timebuf, ts.tv_nsec / 1000000,
                    level_colors[level], level_names[level], color_reset,
                    basename, line, msgbuf);
        } else {
            fprintf(stderr, "%s.%03ldZ %-5s %s:%d: %s\n",
                    timebuf, ts.tv_nsec / 1000000,
                    level_names[level],
                    basename, line, msgbuf);
        }
    }

    /* Write to syslog */
    if (lg->use_syslog) {
        syslog(level_to_syslog[level], "%-5s %s:%d: %s",
               level_names[level], basename, line, msgbuf);
    }

    /* Write to file (no color) */
    if (lg->log_file) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm_buf;
        gmtime_r(&ts.tv_sec, &tm_buf);

        char timebuf[32];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

        fprintf(lg->log_file, "%s.%03ldZ %-5s %s:%d: %s\n",
                timebuf, ts.tv_nsec / 1000000,
                level_names[level],
                basename, line, msgbuf);
        fflush(lg->log_file);
    }

    pthread_mutex_unlock(&lg->mutex);
}
