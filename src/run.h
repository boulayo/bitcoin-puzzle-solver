/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* run.h - runtime context shared by workers, reporter and the CLI. */
#ifndef PZ_RUN_H
#define PZ_RUN_H

#include "campaign.h"
#include "jobq.h"
#include "cpu_engine.h"
#include "sysinfo.h"
#include "eta.h"
#include <stdatomic.h>
#include <pthread.h>

#define MAX_WORKERS 288   /* CPU threads + GPU controllers; run modes keep 16 spare entries */

typedef struct {
    char name[40];
    char kind;                       /* 'C' CPU thread, 'G' GPU */
    _Atomic u64 units;               /* keys scanned (linear) / group steps (kangaroo) */
    double rate;                     /* smoothed units/s, maintained by the reporter */
    u64 last_units;
} worker_stat;

typedef struct {
    double rate;                     /* fast smoothed total rate (display) */
    double rate_eta;                 /* the rate the completion times are computed from (see rate_est in eta.h) */
    rate_state est_state;            /* how far to trust rate_eta */
    long double done, total;         /* in units; total == 0 when unbounded */
    double fraction;                 /* 0..1 */
    eta_in eta;                      /* completion-time model inputs (done / remaining / scale) */
    const char *unit;                /* "keys", "steps" */
    char detail[320];
} progress_info;

/* Priority of the threads serving the GPUs (--prio-gpu): auto = measured by the calibration and remembered in the machine profile. */
typedef enum { PRIO_AUTO = 0, PRIO_NO, PRIO_YES } prio_mode;
typedef enum { UI_AUTO = 0, UI_FULL, UI_LINE, UI_LOG } ui_mode;   /* auto = full on a terminal, log otherwise */

typedef struct run_ctx {
    campaign_cfg cfg;
    sysinfo_t sys;
    sched_t *sched;                  /* linear mode */
    void *kang;                      /* kangaroo mode (kang_state*) */
    _Atomic int stop;
    _Atomic int *worker_stop;        /* linear CPU workers: per-slot flag that idles the worker and interrupts its scan (bench only; NULL: r->stop) */
    _Atomic int cpu_cap;             /* kangaroo CPU workers with a slot >= cpu_cap sleep (run_init: no limit; 'bench --together' varies it) */
    _Atomic int found;
    pthread_mutex_t found_mu;
    pz_u256 found_key;
    bool found_valid;
    double found_time;

    worker_stat ws[MAX_WORKERS];
    int nws;

    /* options */
    int nthreads;
    const cpu_engine *engine;
    bool use_cpu, use_gpu;
    int report_interval;             /* seconds between terminal updates */
    int checkpoint_interval;         /* seconds */
    ui_mode ui;
    double peak_rate;                /* highest smoothed total rate seen in this session */
    double rate_eta;                 /* the rate the completion times are computed from: a window average once every worker produces,
                                      * started from a remembered rate when there is one (BSGS: slowly smoothed) */
    rate_state est_state;            /* how far to trust rate_eta */
    bool all_started;                /* every worker has produced something (a GPU is counted only when its first launch ends) */
    double rate_prior;               /* remembered rate for the completion times at the start (0 = none) */
    int rate_prior_kind;             /* 0 none, 1 the calibration's (measured cold), 2 the steady rate of an earlier run */
    double rate_cold;                /* the calibration's rate of the profile entry in use (0 = none) */
    char rate_key[1100];             /* where the steady rate of this run is remembered (profile_rate_setup) */
    void (*remember_rate)(struct run_ctx *, double);   /* given the steady rate every few minutes and at the end (NULL: not remembered) */
    bool silent_setup;               /* no informational logs from setup code (self-tests) */
    bool threads_explicit;           /* --threads given */
    int limit_pct;                   /* --limit: use only this percentage (1..99) of the CPU threads and of the GPU work-items, all of them
                                      * running flat out all the time (0 = off). Set by run_limit_setup */
    prio_mode prio_gpu;              /* --prio-gpu */
    bool gpu_boost;                  /* the threads serving the GPUs run above the CPU workers (from --prio-gpu or the profile) */
    int kang_schema;                 /* kangaroo: layout of the walkers to use (0 = the campaign's own, see kang_create) */
    u32 node;                        /* kangaroo: this computer's number in a campaign shared by several (--node): different starting points */
    char gpu_filter[128];
    u64 mem_limit;                   /* bytes for DP tables / the BSGS table (0 = automatic) */
    char table_path[600];            /* bsgs: file to load the baby-step table from / save it to (empty = RAM only) */

    /* timing / persistence */
    double t_start;                  /* monotonic start of this session */
    double elapsed_prev;             /* seconds spent in previous sessions */
    double paused_total;             /* seconds of this session the machine slept or stood still (detected by the reporter): not computing time */
    int sessions;
    u64 prev_units_total;            /* units from previous sessions */
    char path_state[600], path_status[600], path_report[600], path_stats[600], path_log[600], path_found[600], path_session[600];

    /* mode hooks */
    void (*progress)(struct run_ctx *, progress_info *);
    int (*checkpoint)(struct run_ctx *);
    void (*cleanup)(struct run_ctx *);   /* called after the final report */

    /* statistics of hits */
    _Atomic u64 hits_bad;
} run_ctx;

/* Seconds of this session spent computing: the monotonic time since the start minus the suspensions. */
static inline double run_session_elapsed(const run_ctx *r) { return pz_mono() - r->t_start - r->paused_total; }

/* common */
void run_init(run_ctx *r);
int run_alloc_worker(run_ctx *r, const char *name, char kind);
void run_install_signals(run_ctx *r);
/* Verifies a candidate private key against the campaign target and, if valid,
 * records it and stops everything. Returns true if the key is correct. */
bool run_submit_key(run_ctx *r, const pz_u256 *key, const char *source);
void run_write_found(run_ctx *r);

/* --limit: how many of n units (CPU threads, GPU work-items) pct percent is. Threads: whole, at least 1, rounded to the nearest. */
int run_limit_threads(int n, int pct);
/* GPU work-items: pct percent of n, rounded to a multiple of `quantum`, never below `quantum` (and never above n). pct outside 1..99: n. */
size_t run_limit_items(size_t n, int pct, size_t quantum);
/* After the thread count is known (before any worker starts): with r->limit_pct set, cuts r->nthreads to that share and puts the
 * threads on distinct physical cores first. Does nothing when the limit is off (0, or 100 and above which is turned into off). */
void run_limit_setup(run_ctx *r);

/* campaign directory handling */
void run_paths(run_ctx *r);
int run_session_load(run_ctx *r);
int run_session_save(run_ctx *r, double elapsed_total, u64 units_total);

/* reporter: runs in the calling thread until stop/found, then writes the final report */
void run_reporter_loop(run_ctx *r);
/* The reporter wakes up every 0.1 s: a much longer step of a clock means the machine slept or hibernated, or the process was frozen.
 * dm, dw: how far the monotonic and the wall clock moved since the last wake-up. Returns 0 (nothing happened), RUN_GAP_COUNTED (the monotonic
 * clock ran through the gap, as when a process is frozen or on Windows: it must be taken off the time of the session) or RUN_GAP_SKIPPED
 * (only the wall clock moved: the monotonic clock stood still, as on a Linux suspend, or the wall clock was set forward: nothing to take off).
 * *gap gets the length. */
enum { RUN_GAP_COUNTED = 1, RUN_GAP_SKIPPED = 2 };
int run_clock_gap(double dm, double dw, double *gap);
void run_write_report_files(run_ctx *r, const progress_info *pi, bool final);
void run_final_progress(run_ctx *r, progress_info *pi);   /* fills pi with the current totals */
void run_ui_final(run_ctx *r, const progress_info *pi);   /* last frame of the live display, left on screen */

/* modes */
int run_linear(run_ctx *r);       /* defined in run_linear.c */
int run_kangaroo(run_ctx *r);     /* defined in run_kangaroo.c */
int run_bsgs(run_ctx *r);         /* defined in run_bsgs.c */
/* Loads the scheduler state of a campaign (main file, then backup); *resumed tells whether progress was restored. */
sched_t *run_load_sched(run_ctx *r, const campaign_cfg *scfg, bool *resumed);

#endif
