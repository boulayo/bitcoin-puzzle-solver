/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_eta.c - checks of the completion-time models. */
#include "../src/eta.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static int near(double a, double b, double rel) { return fabs(a - b) <= rel * fmax(fabs(a), fabs(b)) + 1e-300; }

/* Cumulative units at time t for a synthetic run: CPU from 0, GPU from t_gpu, a rate that falls linearly by `drop` over `ramp` seconds
 * after the GPU joined (a laptop warming up), steady afterwards. */
static double sim_units(double t, double cpu, double gpu, double t_gpu, double drop, double ramp)
{
    double u = cpu * t;
    if (t > t_gpu) {
        const double x = t - t_gpu, r = x < ramp ? x : ramp;
        u += gpu * x - gpu * drop * ((x - r) + r * r / (2 * ramp));     /* integral of gpu * (1 - drop * min(x, ramp) / ramp) */
    }
    return u;
}
static double sim_rate(double t, double cpu, double gpu, double t_gpu, double drop, double ramp)
{
    if (t <= t_gpu) return cpu;
    const double x = t - t_gpu;
    return cpu + gpu * (1 - drop * (x < ramp ? x : ramp) / ramp);
}

static void t_rate_est(void)
{
    rate_state st;
    /* 190 M/s of CPU from the start, 800 M/s of GPU from second 8; sampled once a second */
    rate_est e;
    rate_est_init(&e, 0);
    for (int t = 1; t <= 100; t++) {
        rate_est_add(&e, t, sim_units(t, 190e6, 800e6, 8, 0, 1), t >= 9);
        double v = rate_est_value(&e, t, &st);
        if (t < 9) CHECK(v == 0 && st == RATE_MEASURING);                 /* nothing to go by until every worker has produced */
        if (t >= 9 && t < 12) CHECK(v == 0 && st == RATE_MEASURING);      /* and 3 s after that */
        if (t >= 20 && t < 60) CHECK(near(v, 990e6, 0.04) && st == RATE_PROVISIONAL);
        if (t >= 70) CHECK(near(v, 990e6, 0.015) && st == RATE_STEADY);
    }
    CHECK(rate_est_sustained(&e, 100) == 0);                              /* not enough steady time yet */
    /* the old way of seeding (the first one-second sample, CPU only) would have said 190 M/s: the estimate never does */
    /* a remembered rate: the first estimate is the memory, the measure takes over by 150 s */
    rate_est_init(&e, 800e6);
    double first = -1, at20 = -1, at200 = -1;
    for (int t = 1; t <= 200; t++) {
        rate_est_add(&e, t, sim_units(t, 190e6, 800e6, 8, 0, 1), t >= 9);
        double v = rate_est_value(&e, t, &st);
        if (t == 1) { first = v; CHECK(st == RATE_PROVISIONAL); }
        if (t == 20) at20 = v;
        if (t == 200) { at200 = v; CHECK(st == RATE_STEADY); }
    }
    CHECK(near(first, 800e6, 1e-12));
    CHECK(at20 > 800e6 && at20 < 990e6 * 1.01);                           /* moving from the memory to the measure */
    CHECK(near(at200, 990e6, 0.01));
    /* a laptop that slows down by 20% in the first 3 minutes: the estimate follows the window, the steady rate is what is remembered */
    rate_est_init(&e, 0);
    double sus = 0;
    for (int t = 1; t <= 700; t++) {
        rate_est_add(&e, t, sim_units(t, 190e6, 800e6, 8, 0.25, 180), t >= 9);
        if (t == 700) {
            double v = rate_est_value(&e, t, &st);
            double final_rate = sim_rate(t, 190e6, 800e6, 8, 0.25, 180);
            CHECK(near(v, final_rate, 0.005) && st == RATE_STEADY);
            sus = rate_est_sustained(&e, t);
            CHECK(near(sus, final_rate, 0.02));
        }
    }
    /* the machine sleeps for ten minutes: the interval across the gap counts only running time */
    rate_est_init(&e, 0);
    for (int t = 1; t <= 100; t++) rate_est_add(&e, t, 1e9 * t, t >= 2);
    rate_est_gap(&e, 600);
    for (int t = 101; t <= 110; t++) {
        rate_est_add(&e, t + 600, 1e9 * t, true);
        CHECK(near(rate_est_value(&e, t + 600, &st), 1e9, 0.01));
    }
    /* the ring wraps without losing track; samples that do not advance in time are ignored */
    rate_est_init(&e, 0);
    for (int t = 1; t <= 2000; t++) rate_est_add(&e, t, 5e8 * t, true);
    rate_est_add(&e, 2000, 9e18, true);
    CHECK(e.n == RATE_N && near(rate_est_value(&e, 2000, &st), 5e8, 1e-9) && near(rate_est_sustained(&e, 2000), 5e8, 1e-9));
    /* workers that never start, or a stall after a good start */
    rate_est_init(&e, 0);
    for (int t = 1; t <= 50; t++) rate_est_add(&e, t, 0, false);
    CHECK(rate_est_value(&e, 50, &st) == 0 && st == RATE_MEASURING);
    rate_est_init(&e, 700e6);
    for (int t = 1; t <= 300; t++) rate_est_add(&e, t, t < 20 ? 7e8 * t : 7e8 * 20, true);
    CHECK(rate_est_value(&e, 300, &st) == 0 && st == RATE_MEASURING);     /* a prior is not kept alive once the run has clearly stalled */
    rate_est_init(&e, 800e6);
    for (int t = 1; t <= 400; t++) rate_est_add(&e, t, 0, false);         /* and when no worker ever produced */
    CHECK(rate_est_value(&e, 100, &st) == 800e6 && st == RATE_PROVISIONAL);
    CHECK(rate_est_value(&e, 400, &st) == 0 && st == RATE_MEASURING);
}

int main(void)
{
    /* uniform: half of the remaining keys for 50%, all of them for certainty */
    eta_in u = {ETA_UNIFORM, 1000, 4000, 0, false, 0};
    CHECK(near((double)eta_units_for_prob(&u, 0.5), 2000, 1e-12));
    CHECK(near((double)eta_units_for_prob(&u, 0.9), 3600, 1e-12));
    CHECK(near((double)eta_units_for_prob(&u, 1.0), 4000, 1e-12));
    CHECK(near(eta_prob_within(&u, 1000), 0.25, 1e-12));
    CHECK(eta_prob_within(&u, 1e9) == 1.0 && eta_prob_within(&u, 0) == 0.0);
    CHECK(near(eta_seconds(&u, 0.5, 100), 20, 1e-12));
    CHECK(isinf(eta_seconds(&u, 0.5, 0)));

    /* rayleigh from scratch: median 0.8326 c, 90% 1.5174 c */
    eta_in r = {ETA_RAYLEIGH, 0, 0, 1000, false, 0};
    CHECK(near((double)eta_units_for_prob(&r, 0.5), 832.5546111576977, 1e-9));
    CHECK(near((double)eta_units_for_prob(&r, 0.9), 1517.4271293851465, 1e-9));
    CHECK(near(eta_prob_within(&r, 832.5546111576977L), 0.5, 1e-9));
    CHECK(isinf((double)eta_units_for_prob(&r, 1.0)));
    /* consistency: prob_within(units_for_prob(q)) == q after any amount of work already done */
    for (int i = 0; i < 6; i++) {
        eta_in m = {ETA_RAYLEIGH, 10.0L + 500.0L * (i * i), 0, 1000, false, 0};
        for (double q = 0.05; q < 0.99; q += 0.1) CHECK(near(eta_prob_within(&m, eta_units_for_prob(&m, q)), q, 1e-9));
    }
    /* far into the tail the residual time must stay finite and positive (no catastrophic cancellation) */
    eta_in t = {ETA_RAYLEIGH, 1e12L, 0, 1e9L, false, 0};
    long double e = eta_units_for_prob(&t, 0.5);
    CHECK(e > 0 && e < 1e6L);
    CHECK(near(eta_prob_within(&t, e), 0.5, 1e-6));
    /* uniform with nothing left */
    eta_in z = {ETA_UNIFORM, 5, 0, 0, false, 0};
    CHECK(eta_units_for_prob(&z, 0.5) == 0 && eta_prob_within(&z, 10) == 0);

    /* a computer that holds half of a range shared by two can reach only its half, whatever the odds say */
    eta_in h = {ETA_UNIFORM, 0, 1000, 0, true, 500};
    CHECK(near((double)eta_units_for_prob(&h, 0.5), 500, 1e-12));              /* exactly its part */
    CHECK(isinf((double)eta_units_for_prob(&h, 0.6)) && isinf((double)eta_units_for_prob(&h, 1.0)));
    CHECK(near(eta_prob_within(&h, 250), 0.25, 1e-12));                        /* relative to the whole range */
    CHECK(near(eta_prob_within(&h, 1e9), 0.5, 1e-12));                         /* never more than the part's share */
    CHECK(near(eta_seconds(&h, 0.5, 100), 5, 1e-12) && isinf(eta_seconds(&h, 0.9, 100)));
    eta_in hd = {ETA_UNIFORM, 500, 500, 0, true, 0};                           /* its part is finished */
    CHECK(eta_prob_within(&hd, 10) == 0 && isinf((double)eta_units_for_prob(&hd, 0.5)));
    eta_in nc = {ETA_UNIFORM, 0, 1000, 0, false, 0};                           /* not capped: as before */
    CHECK(near((double)eta_units_for_prob(&nc, 1.0), 1000, 1e-12) && near(eta_prob_within(&nc, 1e9), 1.0, 1e-12));

    /* no model (BSGS table phase): nothing is promised */
    eta_in nn = {ETA_NONE, 5, 100, 0, false, 0};
    eta_summary es;
    eta_summarize(&nn, 1e6, &es);
    CHECK(isinf(es.t50) && isinf(es.t90) && isinf(es.t99) && isinf(es.t_full));
    for (int i = 0; i < ETA_NWIN; i++) CHECK(isnan(es.p_win[i]));
    CHECK(isinf((double)eta_units_for_prob(&nn, 0.5)) && eta_units_for_prob(&nn, 0) == 0);

    t_rate_est();

    puts(fails ? "t_eta: FAILED" : "t_eta: ok");
    return fails != 0;
}
