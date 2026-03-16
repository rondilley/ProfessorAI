/*
 * daemon.h -- Signal handling, PID file, optional daemonization
 *
 * Part of Professor_AI (professord)
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PROF_DAEMON_H
#define PROF_DAEMON_H

#include <signal.h>

extern volatile sig_atomic_t g_shutdown_requested;

int   daemon_daemonize(void);
int   daemon_write_pidfile(const char *path);
void  daemon_remove_pidfile(const char *path);
void  daemon_install_signals(void);

#endif /* PROF_DAEMON_H */
