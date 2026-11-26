/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* canary.c - the canary of the silent misses of the linear search (canary.h). */
#include "canary.h"
#include <math.h>

void canary_init(canary *c, const char *name)
{
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->mu, NULL);
    pz_strlcpy(c->name, name, sizeof c->name);
}

void canary_free(canary *c)
{
    free(c->claim);
    pthread_mutex_destroy(&c->mu);
}

double canary_log_tail(double lambda, u64 n)
{
    if ((double)n >= lambda) return 0;
    const double a = (double)n / lambda;
    return n ? -lambda * (1 - a + a * log(a)) : -lambda;
}

/* One count against its expectation: true when it condemns. The line says how far it is, for the log. */
static bool judge(const char *what, u64 keys, u64 near, char *line, size_t n)
{
    const double lambda = ldexp((double)keys, -32), lp = canary_log_tail(lambda, near);
    snprintf(line, n, "%s: %llu near misses in 2^%.1f keys, %.1f expected (probability below %.1e)", what, (unsigned long long)near,
             log2((double)keys), lambda, exp(lp));
    return lp < CANARY_LOG_P;
}

bool canary_complete(canary *c, sched_t *s, const claim_t *cl, u64 covered, u64 near)
{
    pthread_mutex_lock(&c->mu);
    if (atomic_load(&c->condemned)) {
        pthread_mutex_unlock(&c->mu);
        sched_complete(s, cl, 0);
        return false;
    }
    sched_complete(s, cl, covered);
    if (covered) {
        if (c->nclaim == c->capclaim) c->claim = pz_xrealloc(c->claim, (c->capclaim = c->capclaim ? 2 * c->capclaim : 256) * sizeof *c->claim);
        c->claim[c->nclaim].v = cl->v_start;
        c->claim[c->nclaim++].n = covered;
        c->wkeys += covered, c->wnear += near;
        c->keys += covered, c->near += near;
    }
    if (c->wkeys >= CANARY_WINDOW) {
        char lw[160], lt[160];
        const bool bad_w = judge("window", c->wkeys, c->wnear, lw, sizeof lw), bad_t = judge("session", c->keys, c->near, lt, sizeof lt);
        if (bad_w || bad_t) {
            atomic_store(&c->condemned, true);
            char k[32];
            if (bad_t) {
                sched_revert(s);
                pz_log(PZ_LOG_ERR, "%s misses keys (%s; %s): it is no longer used, and every key marked as scanned in this session goes back "
                       "to be scanned again (its own cannot be told apart from those of the other workers)", c->name, lw, lt);
            } else {
                for (size_t i = 0; i < c->nclaim; i++) sched_reopen(s, c->claim[i].v, c->claim[i].n);
                pz_log(PZ_LOG_ERR, "%s misses keys (%s; %s): it is no longer used, and the %s keys of its window go back to be scanned again",
                       c->name, lw, lt, pz_fmt_u64(c->wkeys, k, sizeof k));
            }
        }
        c->wkeys = c->wnear = 0;
        c->nclaim = 0;
    }
    const bool ok = !atomic_load(&c->condemned);
    pthread_mutex_unlock(&c->mu);
    return ok;
}
