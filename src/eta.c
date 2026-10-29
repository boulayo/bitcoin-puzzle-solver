/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eta.c - see eta.h */
#include "eta.h"
#include <string.h>

long double eta_units_for_prob(const eta_in *m, double q)
{
    if (q <= 0) return 0;
    if (m->model == ETA_NONE) return INFINITY;
    if (m->model == ETA_UNIFORM) {
        const long double u = q >= 1 ? m->remaining : (long double)q * m->remaining;
        if (m->capped && u > m->reach * (1 + 1e-12L)) return INFINITY;     /* more than this computer's part: it cannot get there alone */
        return u > 0 ? u : 0;
    }
    if (q >= 1 || m->scale <= 0) return INFINITY;
    /* total steps n_q with ((n_q/c)^2 - (n/c)^2) = -ln(1-q)  =>  n_q = sqrt(n^2 + c^2 L) */
    long double L = -log1pl(-(long double)q);
    long double c = m->scale, n = m->done;
    long double nq = sqrtl(n * n + c * c * L);
    long double extra = nq - n;
    /* c^2 L / (n_q + n) is the cancellation-free form of n_q - n */
    if (n > 0) extra = c * c * L / (nq + n);
    return extra > 0 ? extra : 0;
}

double eta_prob_within(const eta_in *m, long double extra)
{
    if (m->model == ETA_NONE) return NAN;
    if (extra <= 0) return 0;
    if (m->model == ETA_UNIFORM) {
        if (m->remaining <= 0) return 0;
        if (m->capped && extra > m->reach) extra = m->reach > 0 ? m->reach : 0;
        long double p = extra / m->remaining;
        return p > 1 ? 1.0 : (double)p;
    }
    if (m->scale <= 0) return 0;
    long double c2 = m->scale * m->scale;
    long double x = extra * (2 * m->done + extra) / c2;
    return (double)(-expm1l(-x));
}

double eta_seconds(const eta_in *m, double q, double rate)
{
    if (!(rate > 0)) return INFINITY;
    long double u = eta_units_for_prob(m, q);
    if (isinf((double)u)) return INFINITY;
    return (double)(u / rate);
}

static const double ETA_WINDOWS[ETA_NWIN] = {          /* 1 h, 24 h, 7 d, 30 d, 365 d */3600.0, 86400.0, 7 * 86400.0, 30 * 86400.0, 365.25 * 86400.0};
const char *const ETA_WINDOW_NAMES[ETA_NWIN] = {"1h", "24h", "7d", "30d", "365d"};

void eta_summarize(const eta_in *m, double rate, eta_summary *o)
{
    o->rate = rate > 0 ? rate : 0;
    o->t50 = eta_seconds(m, 0.5, rate);
    o->t90 = eta_seconds(m, 0.9, rate);
    o->t99 = eta_seconds(m, 0.99, rate);
    o->t_full = m->model == ETA_UNIFORM ? eta_seconds(m, 1.0, rate) : INFINITY;   /* ETA_NONE: INFINITY as well */
    for (int i = 0; i < ETA_NWIN; i++) o->p_win[i] = m->model == ETA_NONE || !(rate > 0) ? NAN : eta_prob_within(m, (long double)(rate * ETA_WINDOWS[i]));
}

/* ---------------- rate estimator ---------------- */

void rate_est_init(rate_est *e, double prior)
{
    memset(e, 0, sizeof *e);
    e->ready = -1;
    e->prior = prior > 0 && isfinite(prior) ? prior : 0;
}

void rate_est_add(rate_est *e, double t, double units, bool started)
{
    if (e->n > 0 && !(t > e->t[(e->head + RATE_N - 1) % RATE_N])) return;     /* times must increase */
    if (started && e->ready < 0) e->ready = t;
    e->t[e->head] = t;
    e->u[e->head] = units;
    e->head = (e->head + 1) % RATE_N;
    if (e->n < RATE_N) e->n++;
}

void rate_est_gap(rate_est *e, double gap)
{
    if (!(gap > 0)) return;
    for (int i = 0; i < e->n; i++) e->t[(e->head + RATE_N - 1 - i) % RATE_N] += gap;
    if (e->ready >= 0) e->ready += gap;
}

/* Average rate between the newest sample and the oldest one not older than `from`; 0 when they are closer than `min_span` seconds. */
static double rate_since(const rate_est *e, double from, double min_span)
{
    if (e->n < 2) return 0;
    const int newest = (e->head + RATE_N - 1) % RATE_N;
    int old = newest;
    for (int i = 1; i < e->n; i++) {
        const int k = (e->head + RATE_N - 1 - i) % RATE_N;
        if (e->t[k] < from) break;
        old = k;
    }
    const double dt = e->t[newest] - e->t[old];
    if (!(dt >= min_span)) return 0;
    const double du = e->u[newest] - e->u[old];
    return du > 0 ? du / dt : 0;
}

/* Seconds after `ready`: the remembered rate counts alone until RATE_BLEND_FROM and has handed over to the measure at RATE_HANDOVER, which is
 * when the estimate is steady; without a remembered rate the measure is steady at RATE_SETTLE. */
#define RATE_BLEND_FROM 10.0
#define RATE_HANDOVER 150.0
#define RATE_SETTLE 60.0

/* Seconds after `ready` at which the estimate becomes steady. */
static double settle_delay(const rate_est *e) { return e->prior > 0 ? RATE_HANDOVER : RATE_SETTLE; }

double rate_est_settle_time(const rate_est *e) { return e->ready >= 0 ? e->ready + settle_delay(e) : 0; }

double rate_est_value(const rate_est *e, double now, rate_state *state)
{
    rate_state st = RATE_MEASURING;
    double v = 0;
    const double since = e->ready >= 0 ? now - e->ready : -1;
    if (e->ready < 0 && e->n > 0 && now - e->t[(e->head + RATE_N - e->n) % RATE_N] > 300) {
        if (state) *state = RATE_MEASURING;                      /* nothing has run for five minutes: the remembered rate is not a promise any more */
        return 0;
    }
    const double meas = since >= 3 ? rate_since(e, now - (since < 120 ? since : 120), 3) : 0;
    if (e->prior > 0 && since < RATE_HANDOVER) {             /* the remembered rate hands over to the measure */
        const double w = since < RATE_BLEND_FROM ? 0 : (since - RATE_BLEND_FROM) / (RATE_HANDOVER - RATE_BLEND_FROM);   /* weight of the measure */
        v = meas > 0 ? w * meas + (1 - w) * e->prior : e->prior;
        st = RATE_PROVISIONAL;
    } else if (meas > 0) {
        v = meas;
        st = since >= settle_delay(e) ? RATE_STEADY : RATE_PROVISIONAL;
    }
    if (state) *state = st;
    return v;
}

double rate_est_sustained(const rate_est *e, double now)
{
    if (e->ready < 0 || now - e->ready < 210) return 0;
    const double from = fmax(e->ready + 90, now - 600);
    return rate_since(e, from, 120);
}
