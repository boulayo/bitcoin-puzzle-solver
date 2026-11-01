/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* onfound.c - see onfound.h. */
#include "onfound.h"
#include "platform.h"
#include <pthread.h>

static struct {
    bool used, started;         /* used: a command was given (only the first counts); started: its thread is there to be joined */
    pthread_t th;
    char *cmd, *env[6];
} g;

static void *onfound_thread(void *arg)
{
    (void)arg;
    int code = 0;
    char why[200] = "";
    const int rc = pz_run_shell(g.cmd, (const char *const *)g.env, ONFOUND_TIMEOUT, &code, why, sizeof why);
    if (rc == 0 && code == 0) pz_log(PZ_LOG_INFO, "--on-found: the command ended with exit status 0");
    else if (rc == 0) pz_log(PZ_LOG_WARN, "--on-found: the command ended with exit status %d", code);
    else if (rc == 1) pz_log(PZ_LOG_WARN, "--on-found: the command did not end within %.0f seconds and was stopped", ONFOUND_TIMEOUT);
    else pz_log(PZ_LOG_ERR, "--on-found: the command could not be run: %s", why);
    return NULL;
}

static char *kv(const char *name, const char *val)
{
    const size_t n = strlen(name) + strlen(val) + 2;
    char *s = pz_xmalloc(n);
    snprintf(s, n, "%s=%s", name, val);
    return s;
}

void onfound_start(const char *cmd, const char *sealed, const char *address, int puzzle, const char *campaign, const char *found_file)
{
    if (g.used || !cmd || !cmd[0]) return;
    g.used = true;
    char pz[16];
    snprintf(pz, sizeof pz, "%d", puzzle);
    g.cmd = pz_xstrdup(cmd);
    g.env[0] = kv("PZ_FOUND_SEALED", sealed);
    g.env[1] = kv("PZ_FOUND_ADDRESS", address);
    g.env[2] = kv("PZ_FOUND_PUZZLE", pz);
    g.env[3] = kv("PZ_FOUND_CAMPAIGN", campaign);
    g.env[4] = kv("PZ_FOUND_FILE", found_file);
    g.env[5] = NULL;
    g.started = true;
    pz_log(PZ_LOG_INFO, "--on-found: running the command (at most %.0f seconds)", ONFOUND_TIMEOUT);
    if (pthread_create(&g.th, NULL, onfound_thread, NULL) != 0) {
        pz_log(PZ_LOG_ERR, "--on-found: cannot start the thread that runs the command");
        g.started = false;
    }
}

void onfound_wait(void)
{
    if (!g.started) return;
    pthread_join(g.th, NULL);
    g.started = false;
    for (int i = 0; g.env[i]; i++) { free(g.env[i]); g.env[i] = NULL; }
    free(g.cmd);
    g.cmd = NULL;
}
