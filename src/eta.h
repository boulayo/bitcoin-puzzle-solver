/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* eta.h - completion-time and success-probability models for the search modes.
 *
 * Linear search: the key is uniformly distributed over the keys still to scan, so
 *   P(found within e more keys) = e / remaining.
 * Kangaroo: the number of steps n at which the key is found follows (measured, docs/en/RESEARCH.md 4.5)
 *   P(found by n) = 1 - exp(-(n/c)^2),  c = c_mean / 0.8862 * sqrt(W), c_mean the constant of the kangaroo (KANG_C_MEAN);
 * conditioning on "not found yet after n0 steps" gives the residual distribution used below. */
#ifndef PZ_ETA_H
#define PZ_ETA_H

#include <math.h>
#include <stdbool.h>

/* ETA_NONE: no completion model yet (e.g. the BSGS table is still being built); times are INFINITY, probabilities NaN. */
typedef enum { ETA_UNIFORM = 0, ETA_RAYLEIGH = 1, ETA_NONE = 2 } eta_model;

typedef struct {
    eta_model model;
    long double done;        /* units already spent (keys scanned / steps walked) */
    long double remaining;   /* ETA_UNIFORM: units still to scan */
    long double scale;       /* ETA_RAYLEIGH: c, in units */
    bool capped;             /* ETA_UNIFORM: this computer can scan only `reach` more units (its part of a range shared by several) */
    long double reach;
} eta_in;

/* Units still needed so that the probability of success from now on reaches q (0 <= q < 1;
 * q >= 1 means "certainty": remaining for the uniform model, +inf for the Rayleigh one). */
long double eta_units_for_prob(const eta_in *m, double q);
/* Probability of finding the key within `extra` more units, given that it was not found yet. */
double eta_prob_within(const eta_in *m, long double extra);
/* Seconds to reach probability q at `rate` units/s (inf when rate <= 0 or unbounded). */
double eta_seconds(const eta_in *m, double q, double rate);

#define ETA_NWIN 5
extern const double ETA_WINDOWS[ETA_NWIN];        /* 1 h, 24 h, 7 d, 30 d, 365 d (seconds) */
extern const char *const ETA_WINDOW_NAMES[ETA_NWIN];

typedef struct {
    double rate;               /* units/s used (0 = unknown) */
    double t50, t90, t_full;   /* seconds to reach 50%, 90% and "certainty" (INFINITY when not defined / rate unknown) */
    double t99;                /* seconds to reach 99% (the finite stand-in for certainty in the Rayleigh model) */
    double p_win[ETA_NWIN];    /* probability of success within each window at the current rate (NaN when undefined) */
} eta_summary;

void eta_summarize(const eta_in *m, double rate, eta_summary *out);

/* ---- the rate the completion times are computed from ----
 * The first samples of a run say little: the GPUs are not counted until their first launch ends, the clocks are still rising, and a
 * laptop slows down after a few minutes of load. So the estimate (1) does not exist until every worker has produced something,
 * (2) is the average over a sliding window (at most 2 minutes) after that moment, and (3) starts from a remembered rate when there is
 * one (the steady rate of an earlier run of the same configuration, else the calibration's) and hands over to the measure in 150 s. */
#define RATE_N 720                       /* one sample per second: 12 minutes of history */
typedef enum { RATE_MEASURING = 0, RATE_PROVISIONAL, RATE_STEADY } rate_state;
typedef struct {
    double t[RATE_N], u[RATE_N];         /* ring of (time, total units counted so far) */
    int n, head;                         /* samples held, index of the next write */
    double ready;                        /* time at which every worker had produced, < 0 until then */
    double prior;                        /* remembered rate in units/s, 0 = none */
} rate_est;

void rate_est_init(rate_est *e, double prior);
/* One sample per second (`t` increasing). `started`: every worker has produced something. */
void rate_est_add(rate_est *e, double t, double units, bool started);
/* The machine slept for `gap` seconds: earlier samples are moved so that the interval across the gap counts only time spent running. */
void rate_est_gap(rate_est *e, double gap);
/* The rate for the completion times at `now` (0 while there is nothing to go by) and how far to trust it. */
double rate_est_value(const rate_est *e, double now, rate_state *state);
/* The steady rate worth remembering: the average from 90 s after the start over at most the last 10 minutes; 0 until 120 s of it exist. */
double rate_est_sustained(const rate_est *e, double now);

#endif
