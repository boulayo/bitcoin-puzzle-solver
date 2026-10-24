/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* selftest.c - 'pzsolve selftest' and 'pzsolve bench'. */
#include "selftest.h"
#include "run.h"
#include "hash.h"
#include "perm.h"
#include "kangaroo.h"
#include "gpu.h"
#include "bench_tg.h"
#include "platform.h"
#include "profile.h"
#include "puzzles.h"
#include "bsgs.h"
#include "outfmt.h"
#include <unistd.h>

int pz_sections_printed;
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("  FAILED: %s (%s:%d)\n", #c, __FILE__, __LINE__); } } while (0)

static u64 rs_ = 0x2545F4914F6CDD1DULL;
static u64 rnd64(void) { rs_ ^= rs_ << 13; rs_ ^= rs_ >> 7; rs_ ^= rs_ << 17; return rs_; }

typedef struct { int hits; u256 key; } hitrec;
static void on_hit(void *u, const u256 *k) { hitrec *h = u; h->hits++; h->key = *k; }

static void t_primitives(void)
{
    u8 d[32], h[20];
    char hx[80];
    sha256((const u8 *)"abc", 3, d); pz_hex_encode(d, 32, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ripemd160((const u8 *)"abc", 3, h); pz_hex_encode(h, 20, hx);
    CHECK(!strcmp(hx, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"));
    struct { const char *k, *h160, *addr; } v[] = {
        {"1", "751e76e8199196d454941c45d1b3a323f1433bd6", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"},
        {"7", "5dedfbf9ea599dd4e3ca6a80b333c472fd0b3f69", "19ZewH8Kk1PDbSNdJ97FP4EiCjTRaZMZQA"},
        {"1a838b13505b26867", "52e763a7ddc1aa4fa811578c491c1bc7fd570137", "18ZMbwUFLMHoZBbfpCjUJQTCMCbktshgpe"},
    };
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        u256 k; ge p; u8 pub[33]; char addr[64];
        u256_from_hex(&k, v[i].k);
        CHECK(ec_mul_g(&p, &k) && ge_on_curve(&p));
        ge_to_pub33(pub, &p);
        hash160(pub, 33, h); pz_hex_encode(h, 20, hx);
        CHECK(!strcmp(hx, v[i].h160));
        hash160_to_address(h, addr, sizeof addr);
        CHECK(!strcmp(addr, v[i].addr));
    }
    for (int it = 0; it < 500; it++) {   /* field consistency */
        fe a, b, ab, bi, c;
        for (int j = 0; j < 4; j++) { a.n[j] = rnd64(); b.n[j] = rnd64(); }
        fe_mul(&ab, &a, &b); fe_inv(&bi, &b); fe_mul(&c, &ab, &bi);
        CHECK(fe_equiv(&c, &a));
    }
}

static void t_perm(void)
{
    u8 seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (u8)rnd64();
    for (u64 m = 2; m < 3000; m += 997) {
        perm_t p; u256 M = u256_from_u64(m);
        perm_init(&p, seed, &M);
        u8 *seen = pz_xcalloc(m, 1);
        bool ok = true;
        for (u64 i = 0; i < m; i++) {
            u256 x = u256_from_u64(i), y = perm_apply(&p, &x), z = perm_invert(&p, &y);
            if (y.w[0] >= m || seen[y.w[0]] || z.w[0] != i) ok = false;
            else seen[y.w[0]] = 1;
        }
        CHECK(ok);
        free(seen);
    }
}

static void t_puzzle_db(void)
{
    char err[200];
    int bad = puzzles_verify(err, sizeof err);
    if (bad) printf("  %s\n", err);
    CHECK(bad == 0);
    int solved = 0, withpub = 0;
    for (int i = 0; i < PZ_NUM_PUZZLES; i++) {
        solved += PZ_PUZZLES[i].privkey[0] != 0;
        withpub += PZ_PUZZLES[i].pubkey[0] != 0;
    }
    pz_result_line("puzzle database", "%d puzzles (%d solved, %d with a known public key) + %d emptied in 2017", PZ_NUM_PUZZLES, solved, withpub, PZ_NUM_RETIRED);
}

static void t_bsgs(void)
{
    struct { unsigned wbits; u64 T; } cases[] = {{24, 2048}, {30, 1 << 14}, {33, 50000}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        u256 lo = u256_pow2(cases[i].wbits - 1), hi = u256_pow2(cases[i].wbits), one = u256_from_u64(1);
        u256_sub(&hi, &hi, &one);
        for (int kind = 0; kind < 3; kind++) {
            u256 off = u256_zero(), key;
            for (int j = 0; j < 2; j++) off.w[j] = rnd64();
            u256 m = u256_pow2(cases[i].wbits - 1);
            u256_sub(&m, &m, &one);
            for (int j = 0; j < 4; j++) off.w[j] &= m.w[j];
            if (kind == 1) off = u256_zero();            /* first key of the range */
            if (kind == 2) off = m;                      /* last key of the range */
            u256_add(&key, &lo, &off);
            ge P;
            ec_mul_g(&P, &key);
            u256 got = u256_zero();
            u64 giants;
            bool ok = bsgs_solve(&lo, &hi, &P, cases[i].T, 2, &got, &giants);
            CHECK(ok && !u256_cmp(&got, &key));
        }
        char lb[64];
        snprintf(lb, sizeof lb, "BSGS W=2^%u, %llu baby steps", cases[i].wbits - 1, (unsigned long long)cases[i].T);
        pz_result_line(lb, "random keys and both ends of the range found");
    }
}

static void t_engines(void)
{
    cpu_tables_init();
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    for (int e = 0; e < n; e++) {
        const cpu_engine *E = all[e];
        char lb[64];
        snprintf(lb, sizeof lb, "CPU engine %s", E->name);
        if (!E->supported()) { pz_result_line(lb, "not supported by this CPU: skipped"); continue; }
        const int L = E->lanes;
        u32 X[8 * 64], pfx[64], out[5 * 64];
        u8 xb[64][32];
        for (int l = 0; l < L; l++) {
            for (int i = 0; i < 32; i++) xb[l][i] = (u8)rnd64();
            pfx[l] = 2 + (rnd64() & 1);
            for (int i = 0; i < 8; i++) X[i * L + l] = (u32)xb[l][4*i] << 24 | (u32)xb[l][4*i+1] << 16 | (u32)xb[l][4*i+2] << 8 | xb[l][4*i+3];
        }
        E->hash160_lanes(X, pfx, out);
        bool hok = true;
        for (int l = 0; l < L; l++) {
            u8 h[20], g[20];
            hash160_pub33((u8)pfx[l], xb[l], h);
            for (int w = 0; w < 5; w++) for (int b = 0; b < 4; b++) g[4*w+b] = (u8)(out[w * L + l] >> (8*b));
            if (memcmp(h, g, 20)) hok = false;
        }
        CHECK(hok);
        /* planted key somewhere inside a few groups */
        u256 st = u256_pow2(60); u256_add_u64(&st, &st, 12345);
        for (u64 off = 5; off < 5 * CPU_GRP; off += 1999) {
            u256 key = st; u256_add_u64(&key, &key, off);
            ge p; u8 pub[33], h[20];
            ec_mul_g(&p, &key); ge_to_pub33(pub, &p); hash160(pub, 33, h);
            scan_job job; scan_job_init(&job, h);
            hitrec r = {0, u256_zero()}; job.on_hit = on_hit; job.user = &r;
            u64 cov = E->scan(&job, &st, 5 * CPU_GRP);
            CHECK(cov == 5 * CPU_GRP && r.hits == 1 && !u256_cmp(&r.key, &key));
        }
        pz_result_line(lb, "OK");
    }
}

/* Priority of the threads that serve the GPUs: the verdict on the measurements and the effect of the call on a thread. */
static void *prio_thread(void *p)
{
    int *lv = p;                                 /* before, boosted, restored, success of the boost */
    lv[0] = pz_thread_prio_level();
    lv[3] = pz_thread_boost(true);
    lv[1] = pz_thread_prio_level();
    pz_thread_boost(false);
    lv[2] = pz_thread_prio_level();
    return NULL;
}

static void t_clock_gap(void)
{
    double g;
    CHECK(run_clock_gap(0.1, 0.1, &g) == 0 && g == 0);                                  /* a normal tick */
    CHECK(run_clock_gap(0.1, 0.2, &g) == 0);                                            /* clocks that differ a little */
    CHECK(run_clock_gap(20.1, 20.1, &g) == RUN_GAP_COUNTED && g > 19.9 && g < 20.1);     /* a frozen process: both clocks ran */
    CHECK(run_clock_gap(3600.0, 3600.0, &g) == RUN_GAP_COUNTED);                        /* hibernation counted by the monotonic clock */
    CHECK(run_clock_gap(0.1, 600.1, &g) == RUN_GAP_SKIPPED && g > 599.9 && g < 600.1);  /* suspend that the monotonic clock did not see: not subtracted */
    CHECK(run_clock_gap(0.1, -50.0, &g) == 0);                                          /* the wall clock set back */
    CHECK(run_clock_gap(14.0, 14.0, &g) == 0);                                          /* a long stall below the threshold */
}

static void t_priority(void)
{
    double g;
    CHECK(tg_prio_verdict(100, 106, 106, 100, 2.0, &g) && g > 5.9 && g < 6.1);       /* clear, in both pairs */
    CHECK(!tg_prio_verdict(100, 101, 101, 100, 2.0, &g) && g > 0.9 && g < 1.1);      /* 1%: below the threshold */
    CHECK(!tg_prio_verdict(100, 110, 100, 101, 2.0, &g) && g > 4.4 && g < 4.6);      /* 4.5% overall but the second pair is against it */
    CHECK(!tg_prio_verdict(100, 90, 90, 100, 2.0, &g) && g < -9.9 && g > -10.1);     /* slower with it */
    CHECK(!tg_prio_verdict(0, 100, 100, 100, 2.0, &g));                               /* a run that measured nothing */
    int lv[4] = {0, 0, 0, 0};
    pthread_t th;
    CHECK(pthread_create(&th, NULL, prio_thread, lv) == 0);
    pthread_join(th, NULL);
    const bool ok = lv[3] != 0;
    CHECK(lv[2] == lv[0]);                                                            /* restored */
    CHECK(ok ? lv[1] != lv[0] : lv[1] == lv[0]);                                      /* changed exactly when the system allowed it */
    CHECK(pz_boost_possible() == ok);
    pz_result_line("GPU thread priority", "%s", ok ? "raised and restored" : "not allowed for this user: the tuning skips it");
}

/* The rate remembered for the completion times: stored per configuration in the machine profile, found again by the next run. The cache
 * folder is redirected to a temporary one for the duration of the test. */
static run_ctx *rate_ctx(int threads, bool kang)
{
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    r->cfg.mode = kang ? MODE_KANGAROO : MODE_LINEAR;
    r->use_cpu = r->use_gpu = true;
    r->nthreads = threads;
    sysinfo_detect(&r->sys);
    return r;
}

static void t_rate_memory(void)
{
    char tmp[512];
    if (!pz_make_temp_dir(tmp, sizeof tmp, "pzrate")) { pz_result_line("speed remembered per configuration", "not tested: no temporary folder"); return; }
    static char env_new[640], env_old[640];
    const char *old = getenv("XDG_CACHE_HOME");
    snprintf(env_old, sizeof env_old, "XDG_CACHE_HOME=%s", old ? old : "");
    snprintf(env_new, sizeof env_new, "XDG_CACHE_HOME=%s", tmp);
    putenv(env_new);
    run_ctx *a = rate_ctx(4, false);
    profile_rate_setup(a);
    CHECK(a->rate_prior == 0 && a->rate_prior_kind == 0 && a->remember_rate != NULL);       /* nothing known yet */
    a->rate_cold = 900e6;                                                                    /* the calibration's rate of the profile entry */
    profile_rate_setup(a);
    CHECK(a->rate_prior == 900e6 && a->rate_prior_kind == 1);
    a->remember_rate(a, 812345678.0);                                                        /* the steady rate of a run */
    run_ctx *b = rate_ctx(4, false);
    b->rate_cold = 900e6;
    profile_rate_setup(b);
    CHECK(b->rate_prior == 812345678.0 && b->rate_prior_kind == 2);                          /* beats the cold one */
    run_ctx *c = rate_ctx(5, false);                                                         /* another thread count: another configuration */
    profile_rate_setup(c);
    CHECK(c->rate_prior == 0);
    run_ctx *d = rate_ctx(4, true);                                                          /* the other mode */
    profile_rate_setup(d);
    CHECK(d->rate_prior == 0);
    run_ctx *e = rate_ctx(4, false);                                                         /* the priority is part of the configuration */
    e->gpu_boost = true;
    profile_rate_setup(e);
    CHECK(e->rate_prior == 0);
    a->remember_rate(a, 700e6);                                                              /* a later run replaces the value */
    profile_rate_setup(b);
    CHECK(b->rate_prior == 700e6);
    run_ctx *f = rate_ctx(4, false);                                                         /* BSGS has no memory */
    f->cfg.mode = MODE_BSGS;
    profile_rate_setup(f);
    CHECK(f->remember_rate == NULL && f->rate_prior == 0);
    run_ctx *g = rate_ctx(4, false);                                                         /* a limited run has a speed of its own */
    g->limit_pct = 50;
    profile_rate_setup(g);
    CHECK(g->rate_prior == 0);
    g->remember_rate(g, 300e6);
    run_ctx *h = rate_ctx(4, false);
    h->limit_pct = 50;
    profile_rate_setup(h);
    CHECK(h->rate_prior == 300e6 && h->rate_prior_kind == 2);
    profile_rate_setup(b);
    CHECK(b->rate_prior == 700e6);                                                           /* and does not touch the unlimited one */
    free(g); free(h);
    free(a); free(b); free(c); free(d); free(e); free(f);
    putenv(env_old);
    pz_remove_tree(tmp);
    pz_result_line("speed remembered per configuration", "OK");
}

/* --limit: the share of threads / work-items, and the order that puts the threads on distinct physical cores first. */
static void t_limit(void)
{
    CHECK(run_limit_threads(16, 50) == 8 && run_limit_threads(16, 25) == 4 && run_limit_threads(4, 40) == 2 && run_limit_threads(3, 50) == 2);
    CHECK(run_limit_threads(16, 1) == 1 && run_limit_threads(1, 50) == 1);            /* never less than one thread */
    CHECK(run_limit_threads(16, 99) == 16 && run_limit_threads(16, 100) == 16 && run_limit_threads(16, 0) == 16 && run_limit_threads(16, 150) == 16);   /* off */
    CHECK(run_limit_items(6656, 50, 128) == 3328);
    CHECK(run_limit_items(1000, 5, 128) == 128);                                      /* at least one block */
    CHECK(run_limit_items(1000, 99, 128) == 1000);                                    /* never more than there is */
    CHECK(run_limit_items(64, 30, 128) == 64 && run_limit_items(1000, 100, 128) == 1000 && run_limit_items(1000, 0, 128) == 1000);
    sysinfo_t s;
    memset(&s, 0, sizeof s);
    s.ncpu = 8;
    s.ncore = 4;
    for (int i = 0; i < 8; i++) { s.cpu_ids[i] = i; s.core_of[i] = i / 2; }          /* Windows numbering: the siblings are neighbours */
    sysinfo_spread_order(&s);
    const int want_w[8] = {0, 2, 4, 6, 1, 3, 5, 7};
    int ok = 1, pos[4];
    for (int i = 0; i < 8; i++) ok &= s.cpu_ids[i] == want_w[i] && s.core_of[i] == (i < 4 ? i : i - 4);
    CHECK(ok);
    CHECK(sysinfo_core_cpus(&s, 1, pos, 4) == 2 && pos[0] == 1 && pos[1] == 5);
    for (int i = 0; i < 8; i++) { s.cpu_ids[i] = i; s.core_of[i] = i % 4; }          /* Linux numbering: the siblings come second */
    sysinfo_spread_order(&s);
    ok = 1;
    for (int i = 0; i < 8; i++) ok &= s.cpu_ids[i] == i && s.core_of[i] == i % 4;
    CHECK(ok);
    s.ncpu = 3; s.ncore = 3;                                                         /* no SMT: nothing moves */
    for (int i = 0; i < 3; i++) { s.cpu_ids[i] = 10 + i; s.core_of[i] = i; }
    sysinfo_spread_order(&s);
    CHECK(s.cpu_ids[0] == 10 && s.cpu_ids[2] == 12 && s.core_of[2] == 2);
}

int selftest_run(int argc, char **argv)
{
    (void)argc; (void)argv;
    ec_init();
    const int was_quiet = pz_log_quiet();
    pz_log_set_quiet(1);          /* the small campaigns run by the checks must not print their own log lines (paths, "KEY FOUND") */
    printf("pzsolve %s - selftest\n", PZ_VERSION);
    int f0;
#define SECTION(title) printf("\n[%d/7] %s\n", ++sec, title); f0 = g_fail
    int sec = 0;
    SECTION("Primitives");
    t_primitives();
    pz_result_line("SHA-256, RIPEMD-160, secp256k1, Base58", "%s", g_fail == f0 ? "OK" : "FAILED");
    SECTION("Puzzle database");
    t_puzzle_db();
    SECTION("Search order");
    t_perm();
    pz_result_line("permutation of the blocks (bijective)", "%s", g_fail == f0 ? "OK" : "FAILED");
    SECTION("CPU engines");
    t_engines();
    SECTION("Kangaroo");
    {
        int rc = kang_round_selftest();
        CHECK(rc == 0);
        rc = kangaroo_selftest();
        CHECK(rc == 0);
        rc = kang_log_selftest();
        CHECK(rc == 0);
        rc = kang_merge_selftest();
        CHECK(rc == 0);
        rc = kang_log_fault_selftest();
        CHECK(rc == 0);
    }
    SECTION("Baby-step giant-step");
    {
        int rc = bsgs_vengine_selftest();
        CHECK(rc == 0);
    }
    t_bsgs();
    SECTION("Machine and GPUs");
    t_priority();
    f0 = g_fail;
    t_clock_gap();
    pz_result_line("suspend detection (clock gaps)", "%s", g_fail == f0 ? "OK" : "FAILED");
    f0 = g_fail;
    t_limit();
    pz_result_line("--limit shares and thread order", "%s", g_fail == f0 ? "OK" : "FAILED");
    {
        int rc = gpu_selftest();
        CHECK(rc == 0);
    }
    t_rate_memory();
#undef SECTION
    printf(g_fail ? "\nSELFTEST FAILED: %d of %d checks\n" : "\nSELFTEST PASSED: %d checks, no errors\n", g_fail ? g_fail : g_checks, g_checks);
    pz_log_set_quiet(was_quiet);
    return g_fail != 0;
}

typedef struct { const cpu_engine *e; double secs; u64 keys; int slot; sysinfo_t *s; } bench_arg;

static void *bench_thread(void *p)
{
    bench_arg *a = p;
    sysinfo_pin_thread(a->s, a->slot);
    u8 h[20] = {0};
    scan_job job; scan_job_init(&job, h);
    u256 st = u256_pow2(100);
    u256_add_u64(&st, &st, (u64)a->slot << 32);
    double t0 = pz_mono();
    while (pz_mono() - t0 < a->secs) { a->keys += a->e->scan(&job, &st, 1u << 18); u256_add_u64(&st, &st, 1u << 18); }
    a->secs = pz_mono() - t0;
    return NULL;
}

double bench_cpu_rate(double secs)
{
    sysinfo_t s;
    sysinfo_detect(&s);
    const cpu_engine *e = cpu_engine_autotune(0.15, false);
    int nt = s.ncpu > 256 ? 256 : s.ncpu;
    pthread_t th[256];
    bench_arg ba[256];
    for (int i = 0; i < nt; i++) { ba[i] = (bench_arg){e, secs, 0, i, &s}; pthread_create(&th[i], NULL, bench_thread, &ba[i]); }
    double total = 0;
    for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); total += (double)ba[i].keys / ba[i].secs; }
    return total;
}

static void bsgs_nofound(void *u, const u256 *k) { (void)u; (void)k; }

typedef struct { const bsgs_job *j; int64_t a0; u64 n; _Atomic int *stop; } bsgs_bench_arg;
static void *bsgs_bench_thread(void *p)
{
    bsgs_bench_arg *a = p;
    bsgs_scan(a->j, a->a0, a->n, bsgs_nofound, NULL, NULL, NULL, a->stop);
    return NULL;
}

static void bsgs_bench(int nthreads)
{
    char err[200];
    const u64 T = (u64)1 << 22;
    bsgs_table t;
    if (!bsgs_table_alloc(&t, T, err, sizeof err)) return;
    double t0 = pz_mono();
    bsgs_table_build(&t, nthreads, NULL, NULL);
    double tb = pz_mono() - t0;
    u256 lo = u256_pow2(60), hi = u256_pow2(61), one = u256_from_u64(1), k = u256_pow2(60);
    u256_sub(&hi, &hi, &one);
    u256_add_u64(&k, &k, 123456789ULL);
    ge P;
    ec_mul_g(&P, &k);
    bsgs_params p;
    if (!bsgs_params_init(&p, &lo, &hi, &P, T, err, sizeof err)) return;
    bsgs_job j;
    bsgs_job_init(&j, &t, &p);
    pthread_t th[256];
    bsgs_bench_arg ba[256];
    _Atomic int stop = 0;
    int nt = nthreads > 256 ? 256 : nthreads;
    const u64 per = (u64)1 << 20;
    t0 = pz_mono();
    for (int i = 0; i < nt; i++) { ba[i] = (bsgs_bench_arg){&j, -(int64_t)p.A + (int64_t)i * (int64_t)per, per, &stop}; pthread_create(&th[i], NULL, bsgs_bench_thread, &ba[i]); }
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    double ts = pz_mono() - t0;
    char sec[96], c1[32], c2[32];
    snprintf(sec, sizeof sec, "CPU: baby-step giant-step (table of 2^22 entries, %d thread%s)", nt, nt == 1 ? "" : "s");
    pz_section(sec);
    pz_prop_line("table build", "%s entries/s", pz_fmt_cell((double)T / tb, c1, sizeof c1));
    pz_prop_line("giant steps", "%s steps/s", pz_fmt_cell((double)per * nt / ts, c2, sizeof c2));
    bsgs_table_free(&t);
}

int bench_run(int argc, char **argv)
{
    bool together = false, gpu_only = false, linear = false;
    int threads = 0;
    const char *gpus = NULL;
    prio_mode prio = PRIO_AUTO;
    bool prio_given = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { pz_help("bench", stdout); return 0; }
        else if (!strcmp(a, "--together")) together = true;
        else if (!strcmp(a, "--gpu-only")) gpu_only = true;
        else if (!strcmp(a, "--linear")) linear = true;
        else if (!strcmp(a, "--retune")) gpu_set_retune(true);
        else if ((!strcmp(a, "-t") || !strcmp(a, "--threads")) && i + 1 < argc) threads = pz_atoi(argv[++i]);
        else if (!strcmp(a, "--gpu") && i + 1 < argc) gpus = argv[++i];
        else if (!strcmp(a, "--prio-gpu") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "auto")) prio = PRIO_AUTO;
            else if (!strcmp(v, "yes") || !strcmp(v, "si")) prio = PRIO_YES;
            else if (!strcmp(v, "no")) prio = PRIO_NO;
            else pz_die("--prio-gpu must be auto, yes or no (see 'pzsolve help bench')");
            prio_given = true;
        }
        else pz_die("bench: unrecognized option '%s' (see 'pzsolve help bench')", a);
    }
    if ((threads || gpus || linear || prio_given) && !together) pz_die("bench: -t, --gpu, --prio-gpu and --linear are used together with --together (see 'pzsolve help bench')");
    if (gpus) gpu_check_filter(gpus);
    if (threads < 0 || threads > 512) pz_die("bench: -t must be between 1 and 512 (see 'pzsolve help bench')");
    if (together) {
        tg_choice ch;
        memset(&ch, 0, sizeof ch);
        ch.prio = prio;
        int rc = linear ? linear_bench_together(NULL, threads, gpus, &ch) : kangaroo_bench_together(threads, gpus, &ch);
        if (!rc) tg_print_advice(&ch, linear ? "keys/s" : "steps/s");
        return rc;
    }
    sysinfo_t s;
    sysinfo_detect(&s);
    if (gpu_only) {
        gpu_bench();
        return 0;
    }
    cpu_tables_init();
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    const int maxt = s.ncpu > 256 ? 256 : s.ncpu;                     /* the arrays below hold 256 threads */
    pz_section("CPU: linear search, keys/s (hash160 of compressed keys, 3 s per measurement)");
    printf("  %-10s", "engine");
    for (int nt = 1; nt <= maxt; nt = nt == maxt ? nt + 1 : (nt * 2 > maxt ? maxt : nt * 2)) {
        char h[24];
        snprintf(h, sizeof h, "%d thread%s", nt, nt == 1 ? "" : "s");
        printf("%12s", h);
    }
    printf("\n");
    for (int e = 0; e < n; e++) {
        if (!all[e]->supported()) continue;
        printf("  %-10s", all[e]->name);
        fflush(stdout);
        for (int nt = 1; nt <= maxt; nt = nt == maxt ? nt + 1 : (nt * 2 > maxt ? maxt : nt * 2)) {
            pthread_t th[256];
            bench_arg ba[256];
            for (int i = 0; i < nt; i++) { ba[i] = (bench_arg){all[e], 3.0, 0, i, &s}; pthread_create(&th[i], NULL, bench_thread, &ba[i]); }
            double total = 0;
            for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); total += (double)ba[i].keys / ba[i].secs; }
            char b[32];
            printf("%12s", pz_fmt_cell(total, b, sizeof b));
            fflush(stdout);
        }
        printf("\n");
    }
    pz_section("CPU: kangaroo, steps/s");
    kang_bench_cpu(&s, 1.5);
    kangaroo_bench(3.0, true);
    bsgs_bench(s.ncpu);
    gpu_bench();
    return 0;
}
