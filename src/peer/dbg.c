/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dbg.c - see dbg.h. */
#include "dbg.h"
#include "sys.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static FILE *g_log;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

bool dbg_open(const char *path)
{
    const int fd = sys_open_rw(path);                           /* made for this user alone: the log names partners and their addresses */
    if (fd >= 0) close(fd);
    g_log = sys_fopen(path, "a");
    if (g_log) dbg("---- pzpeer %s ----", PZ_VERSION);
    return g_log != NULL;
}

void dbg(const char *fmt, ...)
{
    if (!g_log) return;
    struct timespec ts;
    struct tm tm;
    va_list ap;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    pthread_mutex_lock(&g_mu);
    fprintf(g_log, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_mu);
}

const char *dbg_hex(const uint8_t *p, size_t n)
{
    static _Thread_local char out[17];
    if (n > 8) n = 8;
    for (size_t i = 0; i < n; i++) snprintf(out + 2 * i, 3, "%02x", p[i]);
    out[2 * n] = 0;
    return out;
}
