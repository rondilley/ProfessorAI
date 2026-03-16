/*
 * daemon.c -- Signal handling, PID file, optional daemonization
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "daemon.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

volatile sig_atomic_t g_shutdown_requested = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

void daemon_install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Ignore SIGPIPE -- mongoose handles broken connections */
    signal(SIGPIPE, SIG_IGN);
}

int daemon_daemonize(void)
{
    /* First fork */
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(0); /* parent exits */
    }

    /* New session */
    if (setsid() < 0) {
        return -1;
    }

    /* Second fork */
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        _exit(0); /* first child exits */
    }

    /* Redirect stdin/stdout/stderr to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }

    umask(0027);
    return 0;
}

int daemon_write_pidfile(const char *path)
{
    if (!path || path[0] == '\0') {
        return 0;
    }

    /* O_EXCL prevents symlink attacks */
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        /* File may exist from a previous crash -- try unlink and retry */
        unlink(path);
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) {
            return -1;
        }
    }

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, (size_t)len) != len) {
        close(fd);
        unlink(path);
        return -1;
    }

    close(fd);
    return 0;
}

void daemon_remove_pidfile(const char *path)
{
    if (path && path[0] != '\0') {
        unlink(path);
    }
}
