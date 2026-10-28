/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* run_bsgs.c - baby-step giant-step campaign (public key known, table in RAM): CPU threads build the baby-step table
 * (or load it from disk), then scan the giant steps in the seed-permuted order of the scheduler. */
#include "run.h"
#include "kangaroo.h"
#include "platform.h"
#include "bsgs.h"
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>

typedef enum { PH_START = 0, PH_LOAD, PH_BUILD, PH_SAVE, PH_SEARCH, PH_DONE } bs_phase;

typedef struct {
    run_ctx *r;
    bsgs_params P;
    bsgs_table tab;
    bsgs_job job;
    campaign_cfg scfg;                  /* scheduler configuration over the giant indices */
    _Atomic int phase;
    _Atomic u64 falsepos;
    int nthreads;
    u64 table_bytes;
    int widx[MAX_WORKERS];
    double table_seconds;
    bool table_from_file;
    pthread_t driver;
} bs_run;

typedef struct { bs_run *b; int slot; } bs_warg;

static void bs_progress(run_ctx *r, progress_info *pi)
{
    bs_run *b = r->bsgs;
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    bs_phase ph = (bs_phase)atomic_load(&b->phase);
    char a[48], c[48], det[160];
    if (ph == PH_SEARCH || ph == PH_DONE) {
        long double total = (long double)b->P.Ng;
        long double done = (long double)sched_keys_done(r->sched);
        if (done > total) done = total;
        pi->done = done;
        pi->total = total;
        pi->fraction = (double)(done / total);
        pi->unit = "giants";
        pi->eta = (eta_in){ETA_UNIFORM, done, total - done, 0, false, 0};
        pz_fmt_bytes((double)b->table_bytes, a, sizeof a);
        snprintf(det, sizeof det, "table: %llu baby steps (%s, %s%s) | table false positives: %llu", (unsigned long long)b->P.T, a,
                 b->tab.thp ? "huge pages" : "normal pages", b->table_from_file ? ", from file" : "", (unsigned long long)atomic_load(&b->falsepos));
        snprintf(pi->detail, sizeof pi->detail, "%s\nrandom order: blocks of 2^%u giants, %zu open intervals", det, r->sched->cbits, sched_open_intervals(r->sched));
        return;
    }
    /* table phase: progress is the number of baby steps inserted so far */
    long double total = (long double)b->P.T;
    long double done = ph == PH_BUILD ? (long double)session : ph == PH_SAVE ? total : 0.0L;
    if (done > total) done = total;
    pi->done = done;
    pi->total = total;
    pi->fraction = total > 0 ? (double)(done / total) : 0;
    pi->unit = "baby steps";
    pi->eta = (eta_in){ETA_NONE, done, total - done, 0, false, 0};   /* no estimate of the search until it starts and its speed is known */
    const char *what = ph == PH_LOAD ? "reading the table from file" : ph == PH_SAVE ? "saving the table to file" : "building the baby-step table";
    pz_fmt_si((double)b->P.Ng, c, sizeof c);
    pz_fmt_bytes((double)b->table_bytes, a, sizeof a);
    snprintf(pi->detail, sizeof pi->detail, "phase 1 of 2: %s (%s)\nafter the table: %s giant steps to check", what, a, c);
}

static int bs_checkpoint(run_ctx *r)
{
    bs_run *b = r->bsgs;
    int rc = 0;
    if (r->sched && atomic_load(&b->phase) >= PH_SEARCH) rc = sched_save(r->sched, r->path_state);
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    run_session_save(r, r->elapsed_prev + run_session_elapsed(r), r->prev_units_total + session);
    return rc;
}

static void bs_on_found(void *user, const pz_u256 *key)
{
    run_ctx *r = user;
    run_submit_key(r, key, "BSGS");
}

static void *search_worker(void *p)
{
    bs_warg *w = p;
    bs_run *b = w->b;
    run_ctx *r = b->r;
    sysinfo_pin_thread(&r->sys, w->slot);
    u64 want = (u64)1 << 18;
    const int widx = b->widx[w->slot];
    while (!atomic_load(&r->stop)) {
        claim_t c;
        claim_status st = sched_claim(r->sched, want, &c);
        if (st == CLAIM_DONE) break;
        if (st == CLAIM_WAIT) {
            pz_sleep(0.05);
            continue;
        }
        const int64_t a0 = (int64_t)c.key_start.w[0] - 1 - (int64_t)b->P.A;
        double t0 = pz_mono();
        u64 cov = bsgs_scan(&b->job, a0, c.nkeys, bs_on_found, r, &r->ws[widx].units, &b->falsepos, &r->stop);
        sched_complete(r->sched, &c, cov);
        double dt = pz_mono() - t0;
        if (dt > 0.01 && cov == c.nkeys) {
            double wnt = (double)cov / dt * 3.0;      /* aim for ~3 s per claim */
            if (wnt < 4096) wnt = 4096;
            if (wnt > 1073741824.0) wnt = 1073741824.0;
            want = (u64)wnt & ~(u64)1023;
        }
    }
    return NULL;
}

/* Prepares the table (load or build+save), then runs the search workers; sets r->stop when it ends. */
static void *driver_main(void *p)
{
    bs_run *b = p;
    run_ctx *r = b->r;
    char err[256];
    double t0 = pz_mono();
    bool have = false;
    if (r->table_path[0] && access(r->table_path, F_OK) == 0) {
        u64 tf = 0;
        if (!bsgs_table_peek(r->table_path, &tf, err, sizeof err)) pz_die("table %s is not usable: %s", r->table_path, err);
        if (tf != b->P.T) pz_die("table %s has %llu baby steps but the campaign uses %llu (this cannot be changed after creation)", r->table_path,
                                 (unsigned long long)tf, (unsigned long long)b->P.T);
        atomic_store(&b->phase, PH_LOAD);
        pz_log(PZ_LOG_INFO, "BSGS: reading the table from %s", r->table_path);
        if (!bsgs_table_load(&b->tab, r->table_path, b->nthreads, err, sizeof err)) pz_die("reading the table failed: %s", err);
        if (b->tab.T != b->P.T) pz_die("table %s changed while it was being read (%llu baby steps instead of %llu)", r->table_path, (unsigned long long)b->tab.T, (unsigned long long)b->P.T);
        have = true;
        b->table_from_file = true;
    }
    if (!have) {
        if (!bsgs_table_alloc(&b->tab, b->P.T, err, sizeof err)) pz_die("%s", err);
        atomic_store(&b->phase, PH_BUILD);
        _Atomic u64 *units[MAX_WORKERS];
        for (int i = 0; i < b->nthreads; i++) units[i] = &r->ws[b->widx[i]].units;
        pz_log(PZ_LOG_INFO, "BSGS: building the table of %llu baby steps with %d threads", (unsigned long long)b->P.T, b->nthreads);
        if (!bsgs_table_build(&b->tab, b->nthreads, units, &r->stop)) { atomic_store(&b->phase, PH_DONE); return NULL; }
        if (r->table_path[0]) {      /* saved even if a stop was requested meanwhile: the build can take hours */
            atomic_store(&b->phase, PH_SAVE);
            pz_log(PZ_LOG_INFO, "BSGS: saving the table to %s", r->table_path);
            if (!bsgs_table_save(&b->tab, r->table_path, b->nthreads, err, sizeof err)) pz_log(PZ_LOG_WARN, "table not saved: %s", err);
        }
    }
    b->table_seconds = pz_mono() - t0;
    if (atomic_load(&r->stop)) { atomic_store(&b->phase, PH_DONE); return NULL; }
    char tb[48];
    pz_log(PZ_LOG_INFO, "BSGS: table ready in %s, starting the giant-step search", pz_fmt_duration(b->table_seconds, tb, sizeof tb));

    bsgs_job_init(&b->job, &b->tab, &b->P);
    atomic_store(&b->phase, PH_SEARCH);
    pthread_t th[MAX_WORKERS];
    bs_warg args[MAX_WORKERS];
    int n = 0;
    for (int i = 0; i < b->nthreads; i++) {
        args[i] = (bs_warg){b, i};
        if (pthread_create(&th[n], NULL, search_worker, &args[i]) != 0) pz_die("pthread_create failed");
        n++;
    }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    atomic_store(&b->phase, PH_DONE);
    atomic_store(&r->stop, 1);
    return NULL;
}

static void bs_cleanup(run_ctx *r)
{
    bs_run *b = r->bsgs;
    if (b) {
        bsgs_table_free(&b->tab);
        free(b);
        r->bsgs = NULL;
    }
}

/* What decides whether a BSGS campaign can run at all: the CPU, the size of the range for the table, the RAM, and a folder that the
 * table can be saved to. 'hunt' asks it before it creates the folder of a new campaign, so that a refused command leaves nothing
 * behind, and run_bsgs() asks it again (a resumed campaign, another machine). Exits with a message when the campaign cannot run;
 * *out (optional) gets the parameters of the search. */
void run_bsgs_check(const campaign_cfg *cfg, const sysinfo_t *sys, bool use_cpu, u64 mem_limit, const char *table_path, bsgs_params *out)
{
    bsgs_params local;
    bsgs_params *P = out ? out : &local;
    char err[256], mb[48], ma[48];
    if (!cfg->has_pubkey) pz_die("bsgs mode needs the public key");
    if (!use_cpu) pz_die("bsgs mode uses the CPU and RAM (GPUs do not take part): drop --no-cpu");
    const u64 T = cfg->bsgs_t;
    if (!T) pz_die("bsgs campaign without a table size (bsgs_entries)");
    if (!bsgs_params_init(P, &cfg->range_start, &cfg->range_end, &cfg->pubkey, T, err, sizeof err)) pz_die("%s", err);

    const u64 bytes = bsgs_bytes_for(T);
    pz_fmt_bytes((double)bytes, mb, sizeof mb);
    pz_fmt_bytes((double)sys->mem_avail, ma, sizeof ma);
    const bool have_table = table_path[0] && access(table_path, F_OK) == 0;
    if (!have_table) {
        if ((double)bytes > 0.95 * (double)sys->mem_avail)
            pz_die("the BSGS table needs %s but only %s of RAM are available: free some memory, or create a new campaign with fewer baby steps (--bsgs-entries) or with the kangaroo (--mode kangaroo): the number of baby steps of this campaign cannot be changed", mb, ma);
        if (mem_limit && bytes > mem_limit)
            pz_die("the BSGS table needs %s, above the --mem limit given", mb);
    } else if ((double)bytes > 0.95 * (double)sys->mem_avail) {
        pz_die("the BSGS table needs %s but only %s of RAM are available", mb, ma);
    }
    if (table_path[0] && !have_table) {
        /* the table will be saved after it is built (minutes or hours): find out now whether that is possible */
        char probe[700];
        snprintf(probe, sizeof probe, "%s.probeXXXXXX", table_path);
        int fd = pz_create_temp(probe);
        if (fd < 0) pz_die("--table %s: cannot write to that folder (%s)", table_path, strerror(errno));
        close(fd);
        unlink(probe);
    }
}

int run_bsgs(run_ctx *r)
{
    pz_ec_init();
    bs_run *b = pz_xcalloc(1, sizeof *b);
    r->bsgs = b;
    b->r = r;
    run_bsgs_check(&r->cfg, &r->sys, r->use_cpu, r->mem_limit, r->table_path, &b->P);
    bsgs_sched_cfg(&b->P, &r->cfg, &b->scfg);

    const u64 T = r->cfg.bsgs_t;
    const u64 bytes = bsgs_bytes_for(T);
    char mb[48], ma[48], c1[48], c2[48];
    pz_fmt_bytes((double)bytes, mb, sizeof mb);
    pz_fmt_bytes((double)r->sys.mem_avail, ma, sizeof ma);
    pz_fmt_si((double)bsgs_mean_cost(&b->P.W, T), c1, sizeof c1);
    pz_fmt_si(KANG_C_MEAN * (double)sqrtl(pz_u256_to_ld(&b->P.W)), c2, sizeof c2);
    pz_log(PZ_LOG_INFO, "BSGS: %llu baby steps (table %s, available RAM %s), %llu giant steps; expected mean cost %s operations (kangaroo: %s)",
           (unsigned long long)T, mb, ma, (unsigned long long)b->P.Ng, c1, c2);
    if (r->use_gpu) pz_log(PZ_LOG_INFO, "BSGS: GPUs do not take part in this mode (use --mode kangaroo to put them to work)");
    if (bsgs_mean_cost(&b->P.W, T) > KANG_C_MEAN * sqrtl(pz_u256_to_ld(&b->P.W)))
        pz_log(PZ_LOG_WARN, "BSGS: with %llu baby steps it costs more than the kangaroo (table too small for this range)", (unsigned long long)T);

    b->table_bytes = bytes;
    r->progress = bs_progress;
    r->checkpoint = bs_checkpoint;
    r->cleanup = bs_cleanup;

    bool resumed;
    r->sched = run_load_sched(r, &b->scfg, &resumed);   /* before any thread starts: the reporter reads it */
    if (resumed) {
        char kd[48];
        pz_log(PZ_LOG_INFO, "campaign resumed: %s giant steps already checked (%.6f%%)", u128_to_dec(sched_keys_done(r->sched), kd), sched_fraction(r->sched) * 100.0);
    }

    if (b->P.Pp_inf) {   /* the public key is c*G: the key is the centre of the range */
        pz_log(PZ_LOG_INFO, "BSGS: the public key coincides with the centre of the range");
        run_submit_key(r, &b->P.c, "BSGS");
        atomic_store(&b->phase, PH_DONE);
        return 0;
    }

    if (sched_finished(r->sched)) {
        pz_log(PZ_LOG_INFO, "the campaign is already complete: all giant steps were checked without finding the key");
        atomic_store(&b->phase, PH_DONE);
        return 0;
    }
    b->nthreads = r->nthreads > 0 ? r->nthreads : 1;
    if (b->nthreads > MAX_WORKERS - 16) b->nthreads = MAX_WORKERS - 16;
    for (int i = 0; i < b->nthreads; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "CPU%d", i);
        b->widx[i] = run_alloc_worker(r, nm, 'C');
    }
    if (pthread_create(&b->driver, NULL, driver_main, b) != 0) pz_die("pthread_create failed");
    run_reporter_loop(r);
    pthread_join(b->driver, NULL);
    return 0;
}
