/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bench_tg.h - measuring CPU workers and GPUs running together (what 'bench --together' and the automatic first-run
 * calibration do): tries CPU thread counts and switching the slower GPUs off, and reports the best configuration. */
#ifndef PZ_BENCH_TG_H
#define PZ_BENCH_TG_H

#include "run.h"
#include "gpu.h"

typedef struct tg_choice {
    bool better;             /* a configuration beats the default clearly (beyond the noise), confirmed by alternating pairs */
    int cap;                 /* CPU threads of the best configuration (what -t takes) */
    unsigned off;            /* bit j set: GPU j (order of the sessions) is better switched off */
    int ngpu, gidx[16];      /* device index (what --gpu takes) of each GPU session */
    double def_rate, best_rate, gain_pct;   /* rates in units/s: the default's and the best's, both confirmed when `better` */
    prio_mode prio;          /* IN (the callers zero the struct: PRIO_AUTO): PRIO_NO / PRIO_YES fix the priority of the GPU service threads, AUTO measures it */
    bool boost;              /* OUT: the GPU service threads are better at a higher priority (the whole scan ran with it) */
    bool boost_tested;       /* OUT: the priority was measured (a GPU, a system that lets us, mode AUTO) */
    double boost_gain_pct;   /* OUT: measured gain of the higher priority at the default configuration (may be negative) */
} tg_choice;

typedef struct {
    run_ctx *r;
    gpu_session *g;          /* the GPU sessions (already launched) */
    int ngpu;
    const int *gw;           /* worker slot (r->ws index) of each GPU */
    const int *gidx;         /* device index of each GPU */
    int total;               /* CPU workers the caller can run at most */
    int full;                /* CPU threads of a campaign without -t (what the scan starts from) */
    bool fixed;              /* try only `full` threads */
    const char *unit;        /* "steps/s" / "keys/s" */
    /* sets the number of CPU workers that run (the others idle); called before every measurement */
    void (*set_cpu)(void *ctx, int cap);
    void *ctx;
    prio_mode prio;          /* as tg_choice.prio */
} tg_scan_cfg;

/* The same measurements for the linear search (defined in run_linear.c, like kangaroo_bench_together in run_kangaroo.c). `engine`
 * is the CPU engine of the campaign (NULL: the automatic choice). */
int linear_bench_together(const cpu_engine *engine, int threads, const char *gpu_filter, tg_choice *choice);

/* The tables and messages of the scans go to stdout unless silenced (the first-run calibration of a quiet hunt). */
extern int tg_silent;
void tg_section(const char *title);                    /* a "== title" heading, unless silenced */
void tg_say(const char *fmt, ...) __attribute__((format(PZ_PRINTF, 1, 2)));      /* printf for the scan: wrapped at 128 columns, flushed */
#define tg_printf tg_say

/* Seconds scale of the measurements (environment variable PZ_BENCH_SCALE, default 1: 2 s of settling and 8 s of measurement). */
double tg_scale(void);

/* Runs the scan (prints the table) and returns the verdict. Stops early when r->stop is set. */
tg_choice tg_scan(const tg_scan_cfg *c);
/* The GPUs left on in the verdict as a --gpu list ("0,2"); empty string when all are kept. */
void tg_gpu_list(const tg_choice *ch, char *out, size_t n);
/* Prints the advice line of 'bench --together'. */
void tg_print_advice(const tg_choice *ch, const char *unit);
/* Verdict on the priority of the GPU service threads from two alternating pairs (off, on) then (on, off): the higher priority is
 * kept when it is faster by at least `min_pct` over the four runs together and in each pair. *gain gets the gain over the four runs. */
bool tg_prio_verdict(double off1, double on1, double on2, double off2, double min_pct, double *gain);

#endif
