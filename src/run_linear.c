/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* run_linear.c - linear (sequential-in-chunk) search campaign: CPU worker
 * threads plus optional GPU workers, driven by the permuted-chunk scheduler. */
#include "run.h"
#include "platform.h"
#include "gpu.h"
#include <pzcore/hash.h>
#include "bench_tg.h"
#include "outfmt.h"
#include "campaign.h"
#include <unistd.h>
#include <time.h>

typedef struct {
    run_ctx *r;
    int widx;
    int slot;
} cpu_arg;

static _Atomic int g_active_workers;
static u128 g_keys_at_start;

static void on_cpu_hit(void *user, const pz_u256 *key)
{
    run_submit_key((run_ctx *)user, key, "CPU");
}

static void *cpu_worker(void *p)
{
    cpu_arg *a = p;
    run_ctx *r = a->r;
    sysinfo_pin_thread(&r->sys, a->slot);
    scan_job job;
    scan_job_init(&job, r->cfg.target_h160);
    _Atomic int *flag = r->worker_stop ? &r->worker_stop[a->slot] : &r->stop;   /* bench: idles and interrupts this worker alone */
    job.stop = flag;
    job.progress = &r->ws[a->widx].units;
    job.on_hit = on_cpu_hit;
    job.user = r;
    u64 want = 1u << 22;
    while (!atomic_load(&r->stop)) {
        if (flag != &r->stop && atomic_load(flag)) { pz_sleep(0.01); continue; }
        claim_t c;
        claim_status st = sched_claim(r->sched, want, &c);
        if (st == CLAIM_DONE) break;
        if (st == CLAIM_WAIT) {
            pz_sleep(0.05);
            continue;
        }
        double t0 = pz_mono();
        u64 cov = r->engine->scan(&job, &c.key_start, c.nkeys);
        sched_complete(r->sched, &c, cov);
        double dt = pz_mono() - t0;
        if (dt > 0.01 && cov == c.nkeys) {
            double w = (double)cov / dt * 3.0;      /* aim for ~3 s per claim */
            if (w < 65536) w = 65536;
            if (w > 1073741824.0) w = 1073741824.0;
            const u64 gr = r->engine->granule > 1024 ? r->engine->granule : 1024;
            want = (u64)w / gr * gr;
            if (want < gr) want = gr;
        }
    }
    if (atomic_fetch_sub(&g_active_workers, 1) == 1) atomic_store(&r->stop, 1);
    return NULL;
}

static void linear_progress(run_ctx *r, progress_info *pi)
{
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    const sched_t *sc = r->sched;
    const bool parted = sc->part.total != 0;
    long double total = pz_u256_to_ld(&sc->part_keys);          /* this computer's part (the whole range without a split) */
    long double done = (long double)g_keys_at_start + (long double)session;
    u128 exact = sched_keys_done(r->sched);
    if ((long double)exact > done) done = (long double)exact;
    if (done > total) done = total;
    pi->done = done;
    pi->total = total;
    pi->fraction = (double)(done / total);
    pi->unit = "keys";
    const long double left = total - done > 0 ? total - done : 0;
    if (parted) {
        /* the key may lie anywhere in what this computer has not scanned yet, but it can scan only its own part: probabilities are
         * relative to the whole range, times beyond the part are not reachable by this computer alone */
        const long double all = pz_u256_to_ld(&sc->range_size);
        pi->eta = (eta_in){ETA_UNIFORM, done, all - done > 0 ? all - done : 0, 0, true, left};
    } else {
        pi->eta = (eta_in){ETA_UNIFORM, done, left, 0, false, 0};
    }
    int n = snprintf(pi->detail, sizeof pi->detail, "random order: blocks of 2^%u keys, open intervals: %zu", sc->cbits, sched_open_intervals(r->sched));
    if (parted && n > 0 && (size_t)n < sizeof pi->detail)
        snprintf(pi->detail + n, sizeof pi->detail - (size_t)n, "\npart %.120s, %.3Lg%% of the range: probabilities refer to the whole range; n/a = beyond this computer's part",
                 sc->part_text, total * 100.0L / pz_u256_to_ld(&sc->range_size));
}

static int linear_checkpoint(run_ctx *r)
{
    int rc = sched_save(r->sched, r->path_state);
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    run_session_save(r, r->elapsed_prev + run_session_elapsed(r), r->prev_units_total + session);
    return rc;
}

int run_linear(run_ctx *r)
{
    pz_ec_init();
    cpu_tables_init();
    bool resumed;
    r->sched = run_load_sched(r, &r->cfg, &resumed);
    if (resumed) {
        char kd[48];
        pz_log(PZ_LOG_INFO, "campaign resumed: %s keys already covered (%.6f%%)", u128_to_dec(sched_keys_done(r->sched), kd), sched_fraction(r->sched) * 100.0);
    }
    g_keys_at_start = sched_keys_done(r->sched);
    r->progress = linear_progress;
    r->checkpoint = linear_checkpoint;

    if (sched_finished(r->sched)) {
        if (r->cfg.part[0]) pz_log(PZ_LOG_INFO, "part %s is already completed: all its shares were scanned without finding the key (the shares of the other computers remain)", r->cfg.part);
        else pz_log(PZ_LOG_INFO, "the campaign is already completed: the whole range was covered without finding the key");
        return 0;
    }

    pthread_t th[MAX_WORKERS];
    cpu_arg args[MAX_WORKERS];
    int nth = 0;
    gpu_session *gpus = NULL;
    int ngpu = 0;
    if (r->use_gpu) ngpu = gpu_linear_start(r, &gpus);
    int ncpu_threads = r->use_cpu ? r->nthreads : 0;
    const int reserve = gpu_cores_needed(gpus, ngpu);
    if (reserve && r->use_cpu && !r->threads_explicit && ncpu_threads > reserve) {
        ncpu_threads -= reserve;   /* leave one core per OpenCL device to drive it */
        pz_log(PZ_LOG_INFO, "CPU: %d compute threads (one core per OpenCL GPU is reserved to drive it)", ncpu_threads);
    }
    if (ncpu_threads > MAX_WORKERS - 16) ncpu_threads = MAX_WORKERS - 16;
    atomic_store(&g_active_workers, ncpu_threads + ngpu);
    if (ncpu_threads + ngpu == 0) pz_die("no compute device available (CPU disabled and no GPU)");

    for (int i = 0; i < ncpu_threads; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "CPU%d", i);
        args[i].r = r;
        args[i].widx = run_alloc_worker(r, nm, 'C');
        args[i].slot = i;
        if (pthread_create(&th[nth], NULL, cpu_worker, &args[i]) != 0) pz_die("pthread_create failed");
        nth++;
    }
    gpu_linear_launch(r, gpus, ngpu, &g_active_workers);

    run_reporter_loop(r);

    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    gpu_linear_join(gpus, ngpu);
    return 0;
}

/* ---------------- CPU and GPU together ('bench --together', first-run calibration) ---------------- */

static int g_bench_total;

static void lin_set_cpu(void *ctx, int cap)
{
    run_ctx *r = ctx;
    for (int i = 0; i < g_bench_total; i++) atomic_store(&r->worker_stop[i], i >= cap);
}

int linear_bench_together(const cpu_engine *engine, int threads, const char *gpu_filter, tg_choice *choice)
{
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    if (gpu_filter && gpu_filter[0]) pz_strlcpy(r->gpu_filter, gpu_filter, sizeof r->gpu_filter);
    pz_ec_init();
    cpu_tables_init();
    r->engine = engine ? engine : cpu_engine_autotune(0.25, false);
    r->cfg.range_start = pz_u256_pow2(100);
    r->cfg.range_end = pz_u256_pow2(101);
    pz_u256 one = pz_u256_from_u64(1);
    pz_u256_sub(&r->cfg.range_end, &r->cfg.range_end, &one);
    r->cfg.mode = MODE_LINEAR;
    for (int i = 0; i < 20; i++) r->cfg.target_h160[i] = (u8)(0x5A + 31 * i);
    for (int i = 0; i < 32; i++) r->cfg.seed[i] = (u8)(7 * i + 3);
    pz_u256 sz = cfg_range_size(&r->cfg);
    r->cfg.chunk_bits = cfg_default_chunk_bits(&sz);
    r->use_cpu = true;
    r->use_gpu = true;
    r->sessions = 1;
    { char t[96]; snprintf(t, sizeof t, "CPU and GPUs together: linear search, engine %s", r->engine->name); tg_section(t); }
    gpu_session *gpus = NULL;
    int ngpu = gpu_linear_start(r, &gpus);
    if (ngpu <= 0) {
        tg_printf("  no usable GPU: nothing to compare (the CPU-only benchmark is the plain 'bench')\n");
        free(r);
        return 1;
    }
    if (ngpu > 16) ngpu = 16;
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);
    const int reserve = gpu_cores_needed(gpus, ngpu);
    int total = threads > 0 ? threads : r->sys.ncpu;
    if (total > MAX_WORKERS - 16) total = MAX_WORKERS - 16;
    const int full = threads > 0 ? total : (r->sys.ncpu > reserve ? r->sys.ncpu - reserve : 1);
    r->sched = sched_new(&r->cfg);
    _Atomic int *flags = pz_xcalloc((size_t)total, sizeof *flags);
    r->worker_stop = flags;
    g_bench_total = total;
    atomic_store(&g_active_workers, 1 << 20);          /* the workers come and go: the last one leaving must not stop the run */
    for (int i = 0; i < total; i++) atomic_store(&flags[i], i >= full);
    pthread_t th[MAX_WORKERS];
    cpu_arg args[MAX_WORKERS];
    for (int i = 0; i < total; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "CPU%d", i);
        args[i] = (cpu_arg){r, run_alloc_worker(r, nm, 'C'), i};
        if (pthread_create(&th[i], NULL, cpu_worker, &args[i]) != 0) pz_die("pthread_create failed");
    }
    gpu_linear_launch(r, gpus, ngpu, &g_active_workers);
    int gw[16], gi = 0, gidx[16];
    for (int j = 0; j < ngpu; j++) gidx[j] = gpu_session_index(gpu_session_at(gpus, j));
    for (int i = 0; i < r->nws && gi < ngpu; i++)
        if (r->ws[i].kind == 'G') gw[gi++] = i;
    const tg_scan_cfg cfg = {r, gpus, ngpu, gw, gidx, total, full, threads > 0, "keys/s", lin_set_cpu, r, choice ? choice->prio : PRIO_AUTO};
    tg_choice ch = tg_scan(&cfg);

    atomic_store(&r->stop, 1);
    for (int i = 0; i < total; i++) atomic_store(&flags[i], 1);
    for (int i = 0; i < total; i++) pthread_join(th[i], NULL);
    gpu_linear_join(gpus, ngpu);
    sched_free(r->sched);
    r->sched = NULL;
    r->worker_stop = NULL;
    free(flags);
    pz_log_set_quiet(was_quiet);
    if (choice) *choice = ch;
    free(r);
    return 0;
}
