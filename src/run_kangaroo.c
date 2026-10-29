/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* run_kangaroo.c - kangaroo campaign runner (known public key), self-test and benchmark. */
#include "kangaroo_priv.h"
#include "platform.h"
#include "gpu.h"
#include <pzcore/hash.h>
#include "bench_tg.h"
#include "outfmt.h"
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

typedef struct { kang_state *ks; int widx, slot; u64 stream; _Atomic int *active; } kth_arg;

static void *kang_thread(void *p)
{
    kth_arg *a = p;
    kang_cpu_worker_main(a->ks, a->widx, a->slot, a->stream);
    if (atomic_fetch_sub(a->active, 1) == 1) atomic_store(&a->ks->r->stop, 1);
    return NULL;
}

static void kang_progress(run_ctx *r, progress_info *pi)
{
    kang_state *ks = r->kang;
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    long double done = (long double)(r->prev_units_total + session);
    long double c = KANG_C_MEAN / 0.8862269254527580L * ks->sqrtW;
    long double z = done / c;
    pi->done = done;
    pi->total = ks->expected_steps;
    pi->fraction = (double)(1.0L - expl(-z * z));
    pi->unit = "steps";
    pi->eta = (eta_in){ETA_RAYLEIGH, done, 0, c, false, 0};
    /* far past the expected effort the Rayleigh model only says "any moment now": more likely the key is not in the range, or the public key
     * is not the one of the address */
    pi->beyond = ks->expected_steps > 0 && done > 5.0L * ks->expected_steps;
    if (pi->beyond && !atomic_exchange(&r->effort_warned, 1))
        pz_log(PZ_LOG_WARN, "kangaroo: 5x the expected effort without a collision: check the public key (-k) and the range");
    /* the size of the table, from the copies that need no lock: a rebuild of the table holds mu for seconds (see kang_state) */
    const u64 cnt = atomic_load_explicit(&ks->count_mirror, memory_order_relaxed), cap = atomic_load_explicit(&ks->cap_mirror, memory_order_relaxed);
    const unsigned thin = atomic_load_explicit(&ks->thin_mirror, memory_order_relaxed);
    char nd[32], nk[32], nr[32], nx[32];
    int n = snprintf(pi->detail, sizeof pi->detail, "distinguished points %s (table %.1f%%%s) | kangaroos %s | dp-bits %u", pz_fmt_u64(cnt, nd, sizeof nd),
                     100.0 * (double)cnt / (double)cap, thin ? ", RAM exhausted: density reduced" : "", pz_fmt_u64((u64)ks->K_total, nk, sizeof nk), ks->P.dp_bits);
    if (pz_log_verbose() && n > 0 && (size_t)n < sizeof pi->detail)          /* internal counters: for -v */
        snprintf(pi->detail + n, sizeof pi->detail - (size_t)n, " | restarts %s | discarded %s",
                 pz_fmt_u64(atomic_load(&ks->restarts) + atomic_load(&ks->dups), nr, sizeof nr), pz_fmt_u64(atomic_load(&ks->dropped), nx, sizeof nx));
}

static int kang_checkpoint(run_ctx *r)
{
    kang_state *ks = r->kang;
    int rc = kang_flush_log(ks);
    u64 session = 0;
    for (int i = 0; i < r->nws; i++) session += atomic_load(&r->ws[i].units);
    run_session_save(r, r->elapsed_prev + run_session_elapsed(r), r->prev_units_total + session);
    return rc;
}

static void kang_cleanup(run_ctx *r)
{
    kang_destroy(r->kang);
    r->kang = NULL;
}

/* Starts workers; returns the number of CPU threads started. r->nthreads is at most MAX_WORKERS - 16 (main, the profile and the callers here). */
static int kang_launch_cpu(run_ctx *r, kang_state *ks, pthread_t *th, kth_arg *args, _Atomic int *active)
{
    int n = 0;
    const int nthr = r->use_cpu ? r->nthreads : 0;
    for (int i = 0; i < nthr; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "CPU%d", i);
        args[i] = (kth_arg){ks, run_alloc_worker(r, nm, 'C'), i, (u64)i, active};
        if (pthread_create(&th[n], NULL, kang_thread, &args[i]) != 0) pz_die("pthread_create failed");
        n++;
    }
    return n;
}

int run_kangaroo(run_ctx *r)
{
    pz_ec_init();
    if (!r->cfg.has_pubkey) pz_die("kangaroo mode needs the public key");
    gpu_session *gpus = NULL;
    int k_gpu = 0, ngpu = 0;
    if (r->use_gpu) ngpu = gpu_kangaroo_start(r, &gpus, &k_gpu);
    const int reserve = gpu_cores_needed(gpus, ngpu);
    if (reserve && r->use_cpu && !r->threads_explicit && r->nthreads > reserve) {
        r->nthreads -= reserve;   /* leave one core per OpenCL device to drive it */
        pz_log(PZ_LOG_INFO, "CPU: %d compute threads (one core per OpenCL GPU is reserved to drive it)", r->nthreads);
    }
    kang_state *ks = kang_create(r);
    r->kang = ks;
    if (kang_configure(ks, k_gpu) != 0) pz_die("no compute device available");
    int lr = kang_load_log(ks);
    if (lr < 0) pz_die("cannot use the distinguished-point file %s", ks->log_path);
    char nd[32];
    if (lr == 0) pz_log(PZ_LOG_INFO, "campaign resumed: %s distinguished points loaded from %s", pz_fmt_u64(ks->loaded, nd, sizeof nd), ks->log_path);
    r->progress = kang_progress;
    r->checkpoint = kang_checkpoint;

    static _Atomic int active;
    pthread_t th[MAX_WORKERS];
    kth_arg args[MAX_WORKERS];
    int nth = 0;
    atomic_store(&active, (r->use_cpu ? r->nthreads : 0) + ngpu);
    if (!atomic_load(&r->stop)) {
        nth = kang_launch_cpu(r, ks, th, args, &active);
        gpu_kangaroo_launch(r, ks, gpus, ngpu, &active);
        run_reporter_loop(r);
    }
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    gpu_kangaroo_join(gpus, ngpu);
    kang_checkpoint(r);
    r->cleanup = kang_cleanup;
    return 0;
}

/* ---------------- self-test / bench ---------------- */

static u64 st_rnd(u64 *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

/* ---------------- synthetic campaigns (self-tests and measurements) ---------------- */

/* The scratch folder of a synthetic campaign (pzkang*, in the temporary folder) holds a file "owner" whose lock the program keeps for as long as
 * the folder exists. A folder whose lock is free and that is not new was left by a program that did not get to remove it (a second Ctrl-C, a
 * crash): it can hold up to 256 MiB of distinguished points, on a RAM disk on many systems. The first synthetic campaign of a program removes
 * such folders. */
static void sweep_scratch_dirs(const char *mine)
{
    static _Atomic int swept;
    if (atomic_exchange(&swept, 1)) return;
    char base[700];
    pz_strlcpy(base, mine, sizeof base);
    char *sep = NULL;
    for (char *c = base; *c; c++)
        if (*c == '/' || *c == '\\') sep = c;
    if (!sep) return;
    *sep = 0;
    const char *own_name = mine + (sep - base) + 1;
    DIR *d = opendir(base);
    if (!d) return;
    const time_t now = time(NULL);
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "pzkang", 6) != 0 || !strcmp(e->d_name, own_name)) continue;
        char path[1400], owner[1500];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", base, e->d_name);
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || now - st.st_mtime < 60) continue;   /* a folder that is just being made is left alone */
        snprintf(owner, sizeof owner, "%s/owner", path);
        const int fd = pz_open_rw(owner, false);
        if (fd < 0) continue;                      /* not one of ours */
        const bool abandoned = pz_lock_try(fd, true) == 0;
        close(fd);
        if (abandoned) pz_remove_tree(path);
    }
    closedir(d);
}

static bool scratch_dir(run_ctx *r, char *dir, size_t n)
{
    if (!pz_make_temp_dir(dir, n, "pzkang")) return false;
    char owner[700];
    snprintf(owner, sizeof owner, "%s/owner", dir);
    r->scratch_lock = pz_open_rw(owner, true);
    if (r->scratch_lock > 0) (void)pz_lock_try(r->scratch_lock, true);    /* held until synth_free() or the end of the program */
    sweep_scratch_dirs(dir);
    return true;
}

/* A synthetic kangaroo campaign on the interval [lo, lo + 2^wbits - 1] with the public key of `key`, in a scratch folder of its own: the base of the
 * self-tests and measurements. The caller sets the rest (seed, hash of the target, threads, devices) and ends it with synth_free(). NULL when
 * there is no temporary folder. */
static run_ctx *synth_new(const pz_u256 *lo, unsigned wbits, const pz_u256 *key)
{
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    char dir[600];
    if (!scratch_dir(r, dir, sizeof dir)) { free(r); return NULL; }
    pz_strlcpy(r->cfg.dir, dir, sizeof r->cfg.dir);
    run_paths(r);
    r->cfg.range_start = *lo;
    r->cfg.range_end = pz_u256_pow2(wbits);
    pz_u256 one = pz_u256_from_u64(1);
    pz_u256_add(&r->cfg.range_end, &r->cfg.range_end, lo);
    pz_u256_sub(&r->cfg.range_end, &r->cfg.range_end, &one);
    pz_ge q;
    pz_ec_mul_g(&q, key);
    r->cfg.pubkey = q;
    pz_ge_to_pub33(r->cfg.pubkey33, &q);
    r->cfg.has_pubkey = true;
    r->cfg.mode = MODE_KANGAROO;
    r->use_cpu = true;
    r->sessions = 1;
    return r;
}

static void synth_free(run_ctx *r)
{
    if (r->scratch_lock > 0) close(r->scratch_lock);
    pz_remove_tree(r->cfg.dir);
    free(r);
}

double kang_try(const pz_u256 *lo, unsigned wbits, const pz_u256 *key, int nthreads, double timeout, u64 seedn, const kang_try_opts *o)
{
    run_ctx *r = synth_new(lo, wbits, key);
    if (!r) return -1;
    pz_hash160(r->cfg.pubkey33, 33, r->cfg.target_h160);
    u64 s = seedn * 0x9E3779B97F4A7C15ULL + 1;
    for (int i = 0; i < 4; i++) memcpy(r->cfg.seed + 8 * i, &(u64){st_rnd(&s)}, 8);
    r->nthreads = nthreads;
    r->use_gpu = false;
    r->silent_setup = true;
    kang_state *ks = kang_create(r);
    r->kang = ks;
    kang_configure(ks, 0);
    if (o && o->R) ks->P.R = o->R;
    if (o && o->dp) { ks->P.dp_bits = o->dp; ks->P.dp_mask = ((u64)1 << o->dp) - 1; }
    if (o && o->no_watchdog) ks->P.cap_since = ~0ULL;
    kang_load_log(ks);
    static _Atomic int active;
    atomic_store(&active, nthreads);
    pthread_t th[MAX_WORKERS];
    kth_arg args[MAX_WORKERS];
    int nth = kang_launch_cpu(r, ks, th, args, &active);
    double t0 = pz_mono();
    while (!atomic_load(&r->found) && pz_mono() - t0 < timeout) {
        pz_sleep(0.005);
    }
    atomic_store(&r->stop, 1);
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    double steps = -1;
    if (atomic_load(&r->found) && !pz_u256_cmp(&r->found_key, key)) {
        steps = 0;
        for (int i = 0; i < r->nws; i++) steps += (double)atomic_load(&r->ws[i].units);
    }
    kang_destroy(ks);
    synth_free(r);
    return steps;
}

struct kang_state *kang_make_test_state(unsigned wbits, int nthreads)
{
    return kang_make_test_state_at(wbits, nthreads, &(pz_u256){{123456789, 0, 0, 0}});
}

struct kang_state *kang_make_test_state_at(unsigned wbits, int nthreads, const pz_u256 *offset)
{
    return kang_make_test_state_dp(wbits, nthreads, offset, 0);
}

struct kang_state *kang_make_test_state_dp(unsigned wbits, int nthreads, const pz_u256 *offset, unsigned dp_bits)
{
    const pz_u256 lo = pz_u256_pow2(90);
    pz_u256 k = lo;
    pz_u256_add(&k, &k, offset);
    run_ctx *r = synth_new(&lo, wbits, &k);
    if (!r) return NULL;
    pz_hash160(r->cfg.pubkey33, 33, r->cfg.target_h160);
    for (int i = 0; i < 32; i++) r->cfg.seed[i] = (u8)(i * 37 + 11);
    r->nthreads = nthreads;
    r->cfg.dp_bits = dp_bits;
    r->silent_setup = true;
    r->mem_limit = 256ULL << 20;                           /* a small table: the intervals of the tests would otherwise ask for a huge one */
    kang_state *ks = kang_create(r);
    r->kang = ks;
    kang_configure(ks, 0);
    return ks;
}

void kang_free_test_state(struct kang_state *ks)
{
    run_ctx *r = ks->r;
    kang_destroy(ks);
    synth_free(r);
}

typedef struct { run_ctx *r; progress_info pi; _Atomic int done; } progress_call;

static void *progress_call_main(void *p)
{
    progress_call *c = p;
    kang_progress(c->r, &c->pi);
    atomic_store(&c->done, 1);
    return NULL;
}

/* The "discarded" of the progress line counts the distinguished points that were lost (thinned out, no room in the table or in the log
 * buffer, GPU buffer overflow), not the collisions that gave no whole solution. The line must also come out while the table is locked: a
 * rebuild of the table (growth, thinning) holds the lock for seconds when the table is large, and the reporter that waits for it makes the
 * clock-gap detector of run_reporter_loop (15 s) see a suspension of the system. */
static int progress_selftest(void)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return 1;
    atomic_store(&ks->dropped, 7);
    atomic_store(&ks->rejected, 3);
    dp_rec recs[100];
    memset(recs, 0, sizeof recs);
    for (int i = 0; i < 100; i++) recs[i].tagk = ((u64)(i + 1) << 20) | KIND_TAME;
    bool ok = !kang_insert_dps(ks, recs, 100, NULL);
    progress_call c;
    memset(&c, 0, sizeof c);
    c.r = ks->r;
    const int was_verbose = pz_log_verbose();
    pz_log_set_verbose(1);                                /* the counters are in the line with -v */
    kang_progress(ks->r, &c.pi);
    pz_log_set_verbose(was_verbose);
    ok = ok && strstr(c.pi.detail, "| discarded 7") != NULL && strstr(c.pi.detail, "distinguished points 100 (table ") != NULL;
    pthread_mutex_lock(&ks->mu);                          /* as in the middle of table_grow or table_thin */
    pthread_t th;
    const bool started = pthread_create(&th, NULL, progress_call_main, &c) == 0;
    for (int i = 0; started && i < 400 && !atomic_load(&c.done); i++) pz_sleep(0.005);
    const bool unblocked = atomic_load(&c.done);
    pthread_mutex_unlock(&ks->mu);
    if (started) pthread_join(th, NULL);
    ok = ok && started && unblocked;
    kang_free_test_state(ks);
    pz_result_line("kangaroo, progress line, table locked", "%s", ok ? "OK" : "FAILED");
    return !ok;
}

int kangaroo_selftest(void)
{
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);
    int fails = 0;
    u64 s = 0xC0FFEE1234ULL;
    struct { unsigned wbits; int n; } cases[] = {{24, 4}, {34, 3}};
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        const kang_try_opts so = {0, 0, false};
        char vals[160] = "";
        int lost = 0;
        for (int t = 0; t < cases[c].n; t++) {
            pz_u256 lo = pz_u256_pow2(60 + (unsigned)t), key;
            pz_u256_add_u64(&lo, &lo, st_rnd(&s) & 0xffffff);
            pz_u256 off = pz_u256_from_u64(st_rnd(&s) & ((1ULL << cases[c].wbits) - 1));
            if (t == 1) off = pz_u256_zero();                                              /* first key of the interval */
            if (t == 2) { off = pz_u256_pow2(cases[c].wbits); pz_u256_sub(&off, &off, &(pz_u256){{1, 0, 0, 0}}); }   /* last key */
            pz_u256_add(&key, &lo, &off);
            double steps = kang_try(&lo, cases[c].wbits, &key, 2, 60.0, st_rnd(&s), &so);
            if (steps < 0) { fails++; lost++; }
            else snprintf(vals + strlen(vals), sizeof vals - strlen(vals), "%s%.2f", vals[0] ? " " : "", steps / sqrt(ldexp(1.0, (int)cases[c].wbits)));
        }
        char lb[64];
        snprintf(lb, sizeof lb, "kangaroo, W=2^%u", cases[c].wbits);
        if (lost) pz_result_line(lb, "%d of %d keys NOT found", lost, cases[c].n);
        else pz_result_line(lb, "%d key%s found, steps / sqrt(W): %s", cases[c].n, cases[c].n == 1 ? "" : "s", vals);
    }
    /* Fruitless cycles on real curve points: 8 jumps instead of 256 make a walker fall into a cycle about every 250
     * steps, distinguished points are about as rare and the watchdog is off, so the search ends only if the walkers
     * leave their cycles, and leave them in the same way (otherwise walkers that met would part again). */
    char cyc[80] = "";
    int cyc_lost = 0;
    for (int t = 0; t < 2; t++) {
        pz_u256 lo = pz_u256_pow2(80 + (unsigned)t), key;
        pz_u256 off = pz_u256_from_u64(st_rnd(&s) & ((1ULL << 30) - 1));
        pz_u256_add(&key, &lo, &off);
        const kang_try_opts o = {8, 7, true};
        double steps = kang_try(&lo, 30, &key, 2, 60.0, st_rnd(&s), &o);
        if (steps < 0) { fails++; cyc_lost++; }
        else snprintf(cyc + strlen(cyc), sizeof cyc - strlen(cyc), "%s%.2f", cyc[0] ? " " : "", steps / sqrt(ldexp(1.0, 30)));
    }
    if (cyc_lost) pz_result_line("kangaroo, 8 jumps (cycles), W=2^30", "%d of 2 keys NOT found", cyc_lost);
    else pz_result_line("kangaroo, 8 jumps (cycles), W=2^30", "2 keys found, steps / sqrt(W): %s", cyc);
    fails += progress_selftest();
    pz_log_set_quiet(was_quiet);
    return fails;
}

double kangaroo_bench(double secs, bool print)
{
    const pz_u256 lo = pz_u256_pow2(120);
    pz_u256 k = lo;
    pz_u256_add_u64(&k, &k, 987654321);
    run_ctx *r = synth_new(&lo, 120, &k);
    if (!r) return 0;
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);
    r->nthreads = r->sys.ncpu > MAX_WORKERS - 16 ? MAX_WORKERS - 16 : r->sys.ncpu;
    kang_state *ks = kang_create(r);
    r->kang = ks;
    kang_configure(ks, 0);
    kang_load_log(ks);
    kang_pick_round();                 /* the one-off choice of the step variant is not part of the measurement */
    static _Atomic int active;
    atomic_store(&active, r->nthreads);
    pthread_t th[MAX_WORKERS];
    kth_arg args[MAX_WORKERS];
    int nth = kang_launch_cpu(r, ks, th, args, &active);
    pz_sleep(0.5);                     /* herds created */
    u64 steps0 = 0, steps = 0;
    for (int i = 0; i < r->nws; i++) steps0 += atomic_load(&r->ws[i].units);
    double t0 = pz_mono();
    pz_sleep(secs);
    for (int i = 0; i < r->nws; i++) steps += atomic_load(&r->ws[i].units);
    steps -= steps0;
    double dt = pz_mono() - t0;
    atomic_store(&r->stop, 1);
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    pz_log_set_quiet(was_quiet);
    char b[32];
    double rate = (double)steps / dt;
    if (print) printf("  in a campaign (%d threads, step %s): %s steps/s\n", r->nthreads, kang_round_name(), pz_fmt_si(rate, b, sizeof b));
    kang_destroy(ks);
    synth_free(r);
    return rate;
}

/* 'pzsolve kstat BITS RUNS [THREADS]': measures the mean cost of the kangaroo in units of sqrt(W). */
static int cmp_double(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int kangaroo_stats(int argc, char **argv)
{
    if (argc < 4) pz_die("usage: pzsolve kstat BITS TRIALS [THREADS [R [DP]]]  (see 'pzsolve help kstat')");
    const unsigned bits = (unsigned)pz_arg_int("kstat BITS", argv[2], 4, 64);
    const int runs = (int)pz_arg_int("kstat TRIALS", argv[3], 1, 1000000);
    const int thr = argc > 4 ? (int)pz_arg_int("kstat THREADS", argv[4], 1, MAX_WORKERS - 16) : 1;
    kang_try_opts o = {argc > 5 ? (unsigned)pz_arg_int("kstat R", argv[5], 0, KANG_R) : 0, argc > 6 ? (unsigned)pz_arg_int("kstat DP", argv[6], 0, 40) : 0, argc > 5};
    if (o.R && (o.R < 2 || (o.R & (o.R - 1)))) pz_die("kstat R: invalid value '%s' (expected a power of 2 between 2 and %d)", argv[5], KANG_R);
    pz_log_set_quiet(1);
    u64 s = 0xABCDEF987ULL;
    double sum = 0, *v = pz_xmalloc(sizeof(double) * (size_t)runs);
    int fails = 0, n = 0;
    for (int i = 0; i < runs; i++) {
        pz_u256 lo = pz_u256_pow2(70);
        pz_u256_add_u64(&lo, &lo, st_rnd(&s));
        pz_u256 off = pz_u256_zero(), key;
        for (int j = 0; j < 4; j++) off.w[j] = st_rnd(&s);
        pz_u256 m = pz_u256_pow2(bits), one = pz_u256_from_u64(1);
        pz_u256_sub(&m, &m, &one);
        for (int j = 0; j < 4; j++) off.w[j] &= m.w[j];
        pz_u256_add(&key, &lo, &off);
        double steps = kang_try(&lo, bits, &key, thr, 600.0, st_rnd(&s), &o);
        if (steps < 0) { fails++; continue; }
        v[n++] = steps / sqrt(ldexp(1.0, (int)bits));
        sum += v[n - 1];
    }
    if (n == 0) { free(v); printf("W = 2^%u: none of the %d trials succeeded\n", bits, runs); return 1; }
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    printf("kangaroo cost over %d trial%s on W = 2^%u (%d thread%s), in steps / sqrt(W)\n", n, n == 1 ? "" : "s", bits, thr, thr == 1 ? "" : "s");
    pz_prop_line("mean", "%.3f", sum / n);
    pz_prop_line("median", "%.3f", v[n / 2]);
    pz_prop_line("90th percentile", "%.3f", v[n * 9 / 10]);
    if (fails) pz_prop_line("failed trials", "%d", fails);
    free(v);
    return 0;
}

/* ---------------- CPU and GPU together ('bench --together', first-run calibration) ---------------- */

static void kang_set_cpu(void *ctx, int cap) { atomic_store(&((run_ctx *)ctx)->cpu_cap, cap); }

int kangaroo_bench_together(int threads, const char *gpu_filter, tg_choice *choice)
{
    const pz_u256 lo = pz_u256_pow2(139);                      /* the shape of the puzzle 140 campaign */
    pz_u256 k = lo;
    pz_u256_add_u64(&k, &k, 987654321);
    run_ctx *r = synth_new(&lo, 139, &k);
    if (!r) return 1;
    if (gpu_filter && gpu_filter[0]) pz_strlcpy(r->gpu_filter, gpu_filter, sizeof r->gpu_filter);
    r->use_gpu = true;
    r->mem_limit = 256ULL << 20;                           /* a small table of distinguished points: the walk is what is measured */
    pz_ec_init();
    tg_section("CPU and GPUs together: kangaroo, as in a campaign on puzzle 140");
    gpu_session *gpus = NULL;
    int k_gpu = 0;
    int ngpu = gpu_kangaroo_start(r, &gpus, &k_gpu);
    if (ngpu <= 0) {
        tg_printf("  no usable GPU: nothing to compare (the CPU-only benchmark is the plain 'bench')\n");
        synth_free(r);
        return 1;
    }
    if (ngpu > 16) ngpu = 16;
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);
    const int reserve = gpu_cores_needed(gpus, ngpu);
    int total = threads > 0 ? threads : r->sys.ncpu;
    if (total > MAX_WORKERS - 16) total = MAX_WORKERS - 16;
    const int full = threads > 0 ? total : (r->sys.ncpu > reserve ? r->sys.ncpu - reserve : 1);   /* what a campaign without -t runs */
    r->nthreads = total;
    kang_state *ks = kang_create(r);
    r->kang = ks;
    kang_configure(ks, k_gpu);
    kang_load_log(ks);
    kang_pick_round();
    static _Atomic int active;
    atomic_store(&active, total + ngpu);
    pthread_t th[MAX_WORKERS];
    kth_arg args[MAX_WORKERS];
    atomic_store(&r->cpu_cap, full);
    int nth = kang_launch_cpu(r, ks, th, args, &active);
    gpu_kangaroo_launch(r, ks, gpus, ngpu, &active);
    int gw[16], gi = 0, gidx[16];                  /* worker slot and device index of each GPU (the sessions are freed by the join) */
    for (int j = 0; j < ngpu; j++) gidx[j] = gpu_session_index(gpu_session_at(gpus, j));
    for (int i = 0; i < r->nws && gi < ngpu; i++)
        if (r->ws[i].kind == 'G') gw[gi++] = i;

    tg_printf("  preparing: %d CPU thread%s and %d GPU%s (the kangaroos of the GPUs are built on the CPU)...\n", total, total == 1 ? "" : "s", ngpu, ngpu == 1 ? "" : "s");
    const double tw = pz_mono();
    for (;;) {
        bool ready = true;
        for (int i = 0; i < r->nws; i++) {
            if (r->ws[i].kind == 'C' && i >= full) continue;        /* parked from the start */
            if (!atomic_load(&r->ws[i].units)) ready = false;
        }
        if (ready || pz_mono() - tw > 300 || atomic_load(&r->stop) || run_interrupted()) break;
        pz_sleep(0.2);
    }
    const tg_scan_cfg cfg = {r, gpus, ngpu, gw, gidx, total, full, threads > 0, "steps/s", kang_set_cpu, r, choice ? choice->prio : PRIO_AUTO};
    tg_choice ch = tg_scan(&cfg);

    atomic_store(&r->stop, 1);
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    gpu_kangaroo_join(gpus, ngpu);
    kang_destroy(ks);
    pz_log_set_quiet(was_quiet);
    if (choice) *choice = ch;
    synth_free(r);
    return 0;
}

/* ---------------- merge of distinguished-point logs ('pzsolve merge') ---------------- */

/* printf wrapped at the terminal width, with the continuation lines indented. */
static void say_wrapped(const char *fmt, ...) __attribute__((format(PZ_PRINTF, 1, 2)));
static void say_wrapped(const char *fmt, ...)
{
    char buf[1600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pz_fputs_wrap(stdout, buf, PZ_LINE_MAX, 4);
}

static char *count_text(u64 v, char *out, size_t n) { return pz_fmt_u64(v, out, n); }

int kangaroo_merge(run_ctx *r, const char *const *files, int nfiles)
{
    pz_ec_init();
    r->nthreads = 1;
    r->use_cpu = true;
    r->use_gpu = false;
    r->silent_setup = true;
    kang_state *ks = kang_create(r);
    r->kang = ks;
    if (kang_configure(ks, 0) != 0) pz_die("kangaroo setup failed");
    const int lr = kang_load_log(ks);
    if (lr < 0) pz_die("cannot use the distinguished-point file %s", ks->log_path);
    char c1[32], c2[32], c3[32];
    say_wrapped("campaign '%s': %s distinguished points already in %s\n", r->cfg.name, count_text(ks->loaded, c1, sizeof c1), ks->log_path);
    int bad = 0;
    for (int i = 0; i < nfiles && !atomic_load(&r->found); i++) {
        char path[1300];
        snprintf(path, sizeof path, "%s", files[i]);
        if (pz_is_dir(path)) snprintf(path, sizeof path, "%s/dp.bin", files[i]);   /* a campaign folder stands for its log */
        kang_merge_stats st;
        memset(&st, 0, sizeof st);
        const int rc = kang_merge_file(ks, path, &st);
        if (rc == -1) { say_wrapped("  %s: %s\n", path, access(path, F_OK) != 0 ? "does not exist" : "cannot read the file"); bad++; continue; }
        if (rc == -3) {
            say_wrapped("  %s: written with another jump table (id %s, this campaign: %s): another seed or another computer setup, the points cannot meet\n",
                        path, st.jump_id, ks->jump_id);
            bad++;
            continue;
        }
        if (rc == -2) {
            char hdr[6] = {0};
            FILE *f = fopen(path, "rb");
            const bool got = f && fread(hdr, 1, 6, f) == 6;
            if (f) fclose(f);
            const int cls = got ? kang_log_class((const u8 *)hdr) : KANG_LOG_FOREIGN;
            if (cls == KANG_LOG_OLDER)
                say_wrapped("  %s: log of an older layout of the kangaroo (versions up to 1.9.0, small intervals): its points cannot be combined with those of this version\n", path);
            else if (cls == KANG_LOG_NEWER)
                say_wrapped("  %s: log written by a newer version of pzsolve: its points cannot be combined with those of this version (the file is not touched)\n", path);
            else
                say_wrapped("  %s: not the log of this campaign: the range or the public key differ, or it comes from an earlier version (open it with 'hunt' first)\n", path);
            bad++;
            continue;
        }
        say_wrapped("  %s: %s points read, %s new, %s already known\n", path, count_text(st.read, c1, sizeof c1), count_text(st.added, c2, sizeof c2), count_text(st.dups, c3, sizeof c3));
        if (st.skipped) say_wrapped("  (%s points discarded because of the RAM limit: raise --mem)\n", count_text(st.skipped, c1, sizeof c1));
    }
    if (kang_flush_log(ks) != 0) { pz_log(PZ_LOG_ERR, "writing %s failed (disk full?)", ks->log_path); bad++; }
    kang_destroy(ks);
    r->kang = NULL;
    return atomic_load(&r->found) ? 10 : (bad ? 1 : 0);
}
