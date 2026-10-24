/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* campaigns.c - campaign lock and the 'list' command. */
#include "campaigns.h"
#include "campaign.h"
#include "platform.h"
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

int campaign_lock(const char *dir)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/lock", dir);
    int fd = pz_open_rw(p, true);
    if (fd < 0) pz_die("cannot open %s: %s", p, strerror(errno));
    int rc = -1, err = 0;
    /* campaign_is_running() of a concurrent 'list' takes a shared lock for an instant: retry briefly before concluding
     * that another instance is really running (a real one keeps the lock for its whole life) */
    for (int attempt = 0; attempt < 6; attempt++) {
        rc = pz_lock_try(fd, true);
        err = errno;
        if (rc != 1) break;
        pz_sleep(0.04);
    }
    if (rc == 1) {
        char buf[32] = {0};
        i64 n = pz_pread(fd, buf, sizeof buf - 1, 0);
        if (n < 0) buf[0] = 0;
        for (char *c = buf; *c; c++) if (*c == '\n' || *c == '\r') *c = 0;
        pz_die("the campaign in %s is already running%s%s%s: stop it first (Ctrl-C) or use another name (-c)", dir, buf[0] ? " (PID " : "", buf[0] ? buf : "", buf[0] ? ")" : "");
    }
    if (rc != 0) {
        /* e.g. a filesystem without lock support: do not refuse to work, but say that the protection is missing */
        pz_log(PZ_LOG_WARN, "cannot lock %s (%s): there is no guarantee that another instance is not working on the same campaign", p, strerror(err));
        return fd;
    }
    if (pz_fd_truncate(fd, 0) == 0) {
        char pid[32];
        int n = snprintf(pid, sizeof pid, "%d\n", pz_getpid());
        if (pz_pwrite(fd, pid, (size_t)n, 0) < 0) { /* informational only */ }
    }
    return fd;
}

bool campaign_is_running(const char *dir)
{
    char p[4200];
    snprintf(p, sizeof p, "%s/lock", dir);
    int fd = pz_open_read(p);
    if (fd < 0) return false;
    bool running = pz_lock_try(fd, false) == 1;
    close(fd);
    return running;
}

/* Extracts a scalar from status.json ("key": value); strings come back without quotes. */
static bool json_get(const char *text, const char *key, char *out, size_t n)
{
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && *p != ',' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return true;
}

typedef struct {
    char *name;
    int puzzle;
    const char *mode;
    char state[32], frac[40], rate[40], eta[64], upd[56];
    char err[220];
    bool bad;
} row_t;

static int cmp_rows(const void *a, const void *b) { return strcmp(((const row_t *)a)->name, ((const row_t *)b)->name); }

/* status.json written before 1.8.0 holds Italian state names. */
static const char *state_name(const char *s)
{
    static const char *const old[][2] = {{"IN CORSO", "RUNNING"}, {"INTERROTTA", "STOPPED"}, {"COMPLETATA", "COMPLETED"}, {"TROVATA", "FOUND"}};
    for (size_t i = 0; i < sizeof old / sizeof *old; i++)
        if (!strcmp(s, old[i][0])) return old[i][1];
    return s;
}

static void fill_row(row_t *w, const char *base, time_t now)
{
    char dir[1100], p[1200];
    snprintf(dir, sizeof dir, "%s/%s", base, w->name);
    snprintf(p, sizeof p, "%s/campaign.conf", dir);
    campaign_cfg c;
    if (cfg_load(&c, p, w->err, sizeof w->err) != 0) { w->bad = true; return; }
    w->puzzle = c.puzzle;
    w->mode = mode_name(c.mode);
    snprintf(p, sizeof p, "%s/status.json", dir);
    char *js = pz_read_file(p, NULL);
    char v[64];
    pz_strlcpy(w->state, "NEW", sizeof w->state);
    pz_strlcpy(w->frac, "-", sizeof w->frac);
    pz_strlcpy(w->rate, "-", sizeof w->rate);
    pz_strlcpy(w->eta, "-", sizeof w->eta);
    pz_strlcpy(w->upd, "-", sizeof w->upd);
    snprintf(p, sizeof p, "%s/FOUND.txt", dir);
    bool found = access(p, F_OK) == 0;
    bool running = campaign_is_running(dir);
    if (js) {
        if (json_get(js, "state", v, sizeof v)) pz_strlcpy(w->state, state_name(v), sizeof w->state);
        if (json_get(js, "fraction", v, sizeof v)) {
            double f = atof(v) * 100.0;
            if (f >= 0.001 || f == 0) snprintf(w->frac, sizeof w->frac, "%.4f%%", f);
            else snprintf(w->frac, sizeof w->frac, "%.2e%%", f);
        }
        if (json_get(js, "rate_per_second", v, sizeof v)) {
            double rt = atof(v);
            if (rt > 0) { char b[32]; pz_fmt_si(rt, b, sizeof b); snprintf(w->rate, sizeof w->rate, "%s/s", b); }
        }
        if (json_get(js, "eta_seconds_50", v, sizeof v) && strcmp(v, "null") != 0) pz_fmt_duration(atof(v), w->eta, sizeof w->eta);
        if (json_get(js, "updated_unix", v, sizeof v)) {
            double age = (double)now - atof(v);
            if (age < 0) age = 0;
            char b[48];
            if (age < 2) pz_strlcpy(w->upd, "just now", sizeof w->upd);   /* updated_unix has a resolution of one second */
            else snprintf(w->upd, sizeof w->upd, "%s ago", pz_fmt_duration(age, b, sizeof b));
        }
        free(js);
    }
    /* the file says "RUNNING" until the next write: if nobody holds the lock the process died without a final report */
    if (found) pz_strlcpy(w->state, "FOUND", sizeof w->state);
    else if (running) pz_strlcpy(w->state, "RUNNING", sizeof w->state);
    else if (!strcmp(w->state, "RUNNING")) pz_strlcpy(w->state, "STOPPED", sizeof w->state);
    if (!running || found) pz_strlcpy(w->rate, "-", sizeof w->rate);
    if (found || !strcmp(w->state, "COMPLETED")) pz_strlcpy(w->eta, "-", sizeof w->eta);
}

int campaigns_list(const char *base)
{
    DIR *d = opendir(base);
    if (!d) {
        printf("No campaigns: the folder %s does not exist.\n", base);
        return 0;
    }
    row_t *rows = NULL;
    int nc = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1500];
        snprintf(p, sizeof p, "%s/%s/campaign.conf", base, e->d_name);
        if (access(p, F_OK) != 0) continue;
        if (nc == cap) {
            cap = cap ? cap * 2 : 32;
            rows = pz_xrealloc(rows, (size_t)cap * sizeof *rows);
        }
        memset(&rows[nc], 0, sizeof rows[nc]);
        rows[nc].name = pz_xstrdup(e->d_name);
        nc++;
    }
    closedir(d);
    if (nc == 0) {
        printf("No campaigns in %s.\n", base);
        free(rows);
        return 0;
    }
    qsort(rows, (size_t)nc, sizeof *rows, cmp_rows);
    time_t now = time(NULL);
    int wn = 9, ws = 5, wf = 8, wr = 5, we = 13;   /* column widths: header text, grown to the longest cell */
    for (int i = 0; i < nc; i++) {
        fill_row(&rows[i], base, now);
        if (rows[i].bad) continue;
        int l;
        if ((l = (int)strlen(rows[i].name)) > wn) wn = l;
        if ((l = (int)strlen(rows[i].state)) > ws) ws = l;
        if ((l = (int)strlen(rows[i].frac)) > wf) wf = l;
        if ((l = (int)strlen(rows[i].rate)) > wr) wr = l;
        if ((l = (int)strlen(rows[i].eta)) > we) we = l;
    }
    printf("%-*s %6s %-9s %-*s %-*s %-*s %-*s %s\n", wn, "CAMPAIGN", "PUZZLE", "MODE", ws, "STATE", wf, "PROGRESS", wr, "SPEED", we, "TIME LEFT 50%", "UPDATED");
    for (int i = 0; i < nc; i++) {
        const row_t *w = &rows[i];
        if (w->bad) printf("%-*s  (unreadable configuration: %s)\n", wn, w->name, w->err);
        else printf("%-*s %6d %-9s %-*s %-*s %-*s %-*s %s\n", wn, w->name, w->puzzle, w->mode, ws, w->state, wf, w->frac, wr, w->rate, we, w->eta, w->upd);
        free(w->name);
    }
    free(rows);
    printf("\nResume a campaign with:  pzsolve hunt -c NAME   (or  hunt -p N  for the puzzles of the database)\n");
    return nc;
}
