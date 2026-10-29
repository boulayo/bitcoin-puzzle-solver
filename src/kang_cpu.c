/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kang_cpu.c - the CPU side of the kangaroo: the step variants and the choice of the fastest (checked against the portable one), their
 * benchmark, and the worker thread that runs a herd and feeds the table. */
#include "kangaroo_priv.h"
#include "cpu_engine.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"

/* ---------------- CPU worker ---------------- */

static bool always(void) { return true; }
static const kang_round_variant KROUNDS[] = {
    {"base", "portable", always, kang_round_base},
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    {"x86", "BMI2/ADX", cpu_has_avx2, kang_round_x86},
    {"avx2v", "AVX2 lanes", cpu_has_avx2, kang_round_avx2v},
    {"avx512v", "AVX-512 lanes", cpu_has_avx512, kang_round_avx512v},
    {"avx512i", "AVX-512 IFMA lanes", cpu_has_ifma, kang_round_avx512i},
    {"avx512i2", "AVX-512 IFMA pairs", cpu_has_ifma, kang_round_avx512i2},
#endif
};
#define NKROUNDS ((int)(sizeof KROUNDS / sizeof *KROUNDS))

/* Index of the variant with this id, -1 when this build has none. */
static int kround_find(const char *id)
{
    for (int i = 0; i < NKROUNDS; i++)
        if (!strcmp(KROUNDS[i].id, id)) return i;
    return -1;
}

const kang_round_variant *kang_round_variants(int *n)
{
    *n = NKROUNDS;
    return KROUNDS;
}

/* One comparison of `fn` with the portable round. R < KANG_R shrinks the jump table: with R = 4 the walks fall into
 * 2-, 4- and longer cycles every few steps, so the cycle detection and the escape jumps are exercised (*escapes counts
 * the escape jumps of the reference herd). */
static bool kang_round_compare(kang_round_fn fn, int K, int rounds, unsigned R, u64 dp_mask, u64 *escapes)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return false;
    ks->P.R = R;
    ks->P.dp_mask = dp_mask;
    ks->P.cap_since = (1ULL << 32) + 5;   /* restarts of walkers without a DP (tested at the checkpoint steps) */
    kang_worker *a = kang_worker_new(ks, K, 77), *b = kang_worker_new(ks, K, 77);
    kang_init_herd(ks, a);
    kang_init_herd(ks, b);
    /* half the walkers have gone 2^32 - 4 steps without a DP: past the cap after about 70 more steps, never for a 32-bit
     * counter that wraps (it would restart them at the first step) */
    for (int i = 1; i < K; i += 2) a->lastdp[i] = b->lastdp[i] = 0 - ((1ULL << 32) - 4);
    bool ok = true;
    for (int r = 0; r < rounds && ok; r++) {
        if (r == rounds / 2 && R == KANG_R) {
            /* a kangaroo on the jump point it is about to use: the batch product is zero */
            for (unsigned j = 0; j < R; j++) {
                pz_fe x = ks->P.jx[j];
                pz_fe_normalize(&x);
                const unsigned lo = (unsigned)x.n[0] & (R - 1);
                if (lo != j && ((lo + 1) & (R - 1)) != j) continue;
                pz_fe y = ks->P.jy[j];
                pz_fe_normalize(&y);
                const int i = K / 3;
                a->x[i] = b->x[i] = x;
                a->y[i] = b->y[i] = y;
                a->prev2[i] = b->prev2[i] = lo == j ? KANG_NOTAG : kang_tag(x.n[2]);   /* the 2-cycle escape moves lo to lo + 1 */
                a->ck[i] = b->ck[i] = a->mn[i] = b->mn[i] = KANG_NOTAG;
                a->esc[i] = b->esc[i] = 0;
                a->vstale[i] = b->vstale[i] = 1;
                break;
            }
        }
        fn(a);
        kang_round_base(b);
        for (int i = 0; i < K; i++) *escapes += b->jidx[i] >= R;
        if (a->ndp != b->ndp || a->nrestart != b->nrestart) { ok = false; break; }
        for (int k = 0; k < a->ndp && ok; k++)
            ok = a->dpidx[k] == b->dpidx[k] && a->dpbuf[k].tagk == b->dpbuf[k].tagk && !memcmp(&a->dpbuf[k].d, &b->dpbuf[k].d, sizeof(i192));
        for (int k = 0; k < a->nrestart && ok; k++) ok = a->restart[k] == b->restart[k];
        for (int k = 0; k < a->nrestart && ok; k++) {
            kang_restart_one(ks, a, a->restart[k]);
            kang_restart_one(ks, b, b->restart[k]);
        }
        a->ndp = b->ndp = a->nrestart = b->nrestart = 0;
    }
    if (ok) {
        kang_worker_sync(a);
        for (int i = 0; i < K && ok; i++) {
            pz_fe ax = a->x[i], ay = a->y[i], bx = b->x[i], by = b->y[i];
            pz_fe_normalize(&ax); pz_fe_normalize(&ay); pz_fe_normalize(&bx); pz_fe_normalize(&by);
            ok = pz_fe_eq(&ax, &bx) && pz_fe_eq(&ay, &by) && !memcmp(&a->d[i], &b->d[i], sizeof(i192)) && a->sigma[i] == b->sigma[i] &&
                 a->prev[i] == b->prev[i] && a->prev2[i] == b->prev2[i] && a->lastdp[i] == b->lastdp[i] && a->ck[i] == b->ck[i] &&
                 a->mn[i] == b->mn[i] && a->esc[i] == b->esc[i];
        }
    }
    kang_worker_free(a);
    kang_worker_free(b);
    kang_free_test_state(ks);
    return ok;
}

bool kang_round_check(kang_round_fn fn, int K, int rounds)
{
    u64 esc = 0;
    /* distinguished points every 64 steps on average (so that some walkers reach the watchdog at the second checkpoint,
     * step 64), then a tiny jump table: cycles and escapes. `rounds` should be at least 70. */
    if (!kang_round_compare(fn, K, rounds, KANG_R, 63, &esc)) return false;
    esc = 0;
    if (!kang_round_compare(fn, K, rounds * 2, 4, ~0ULL >> 12, &esc)) return false;
    return esc > 0;
}

static pthread_once_t g_kround_once = PTHREAD_ONCE_INIT;
static int g_kround;
static bool g_kround_ok[NKROUNDS];      /* supported by this CPU and equal to the portable round (set by kround_pick) */
static bool g_kmix;                     /* the threads after the first of a core run the scalar step (kround_choose_mix) */
static double g_kmix_gain;              /* the mixed layout against the uniform one at the start-up measurement, 0 when not measured */

/* Steps per second of `fn` on one thread (a short run on a test herd). */
static double kround_rate(kang_round_fn fn)
{
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) return 0;
    ks->P.dp_mask = ~0ULL >> 8;
    ks->P.cap_since = ~0ULL;
    kang_worker *w = kang_worker_new(ks, 1024, 5);
    kang_init_herd(ks, w);
    fn(w);
    u64 steps = 0;
    double t0 = pz_mono(), t;
    do {
        fn(w);
        steps += (u64)w->K;
        w->ndp = w->nrestart = 0;
        t = pz_mono();
    } while (t - t0 < 0.12);
    kang_worker_free(w);
    kang_free_test_state(ks);
    return (double)steps / (t - t0);
}

static double kbench_run(kang_state *ks, const sysinfo_t *s, int n, const kang_round_fn *fn, const int *pos, double secs);
static void kround_choose_pair(void);
static void kround_choose_mix(bool measured);

static void kround_pick(void)
{
    g_kround = 0;
    double best = 0;
    const char *forced = g_env.kang_round;                 /* diagnostics: base, x86, avx2v, avx512v, avx512i */
    if (forced[0]) {
        const int f = kround_find(forced);
        if (f >= 0 && KROUNDS[f].supported()) { g_kround = f; kround_choose_mix(false); return; }
        pz_log(PZ_LOG_WARN, "PZ_KANG_ROUND=%s is not available on this CPU: choosing automatically", forced);
    }
    for (int i = 0; i < NKROUNDS; i++) {
        if (!KROUNDS[i].supported()) continue;
        if (i > 0 && !kang_round_check(KROUNDS[i].fn, 96, 75)) {
            pz_log(PZ_LOG_WARN, "kangaroo CPU: variant %s does not match the reference, excluded", KROUNDS[i].name);
            continue;
        }
        g_kround_ok[i] = true;
        double r = kround_rate(KROUNDS[i].fn);
        if (g_env.kang_verbose) fprintf(stderr, "kangaroo CPU: %-22s %.2f M steps/s (1 thread)\n", KROUNDS[i].name, r / 1e6);
        if (r > best * 1.03) { best = r; g_kround = i; }
    }
    kround_choose_pair();
    kround_choose_mix(true);
}

kang_round_fn kang_pick_round(void)
{
    pthread_once(&g_kround_once, kround_pick);
    return KROUNDS[g_kround].fn;
}

const char *kang_round_name(void)
{
    pthread_once(&g_kround_once, kround_pick);
    return KROUNDS[g_kround].name;
}

/* The widest instruction set among the step variants this CPU supports: unlike the measured choice of kang_round_name,
 * which may differ from one process to the next when two variants are about as fast, it never changes on a machine. */
const char *kang_round_isa(void)
{
    int best = 0;
    for (int v = 0; v < NKROUNDS; v++)
        if (KROUNDS[v].supported()) best = v;
    return best == kround_find("avx512i2") ? "avx512i" : KROUNDS[best].id;
}

/* ---------------- CPU benchmark of the step variants ---------------- */

typedef struct {
    kang_state *ks;
    const sysinfo_t *s;
    kang_round_fn fn;
    int pos;                     /* position in s->cpu_ids */
    double secs, rate;
    _Atomic int *ready;
    int nthreads;
} kbench_arg;

static void *kbench_thread(void *p)
{
    kbench_arg *a = p;
    sysinfo_pin_thread(a->s, a->pos);
    kang_worker *w = kang_worker_new(a->ks, KANG_K_THREAD_MAX, 900 + (u64)a->pos);
    kang_init_herd(a->ks, w);
    a->fn(w);
    w->ndp = w->nrestart = 0;
    atomic_fetch_add(a->ready, 1);
    while (atomic_load(a->ready) < a->nthreads) { /* all herds ready: measure together */ }
    u64 steps = 0;
    double t0 = pz_mono(), t;
    do {
        a->fn(w);
        steps += (u64)w->K;
        w->ndp = w->nrestart = 0;
        t = pz_mono();
    } while (t - t0 < a->secs);
    a->rate = (double)steps / (t - t0);
    kang_worker_free(w);
    return NULL;
}

/* Steps per second of n threads, thread i running fn[i] on CPU position pos[i]. */
static double kbench_run(kang_state *ks, const sysinfo_t *s, int n, const kang_round_fn *fn, const int *pos, double secs)
{
    pthread_t th[256];
    kbench_arg a[256];
    _Atomic int ready = 0;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        a[i] = (kbench_arg){ks, s, fn[i], pos[i], secs, 0, &ready, n};
        pthread_create(&th[i], NULL, kbench_thread, &a[i]);
    }
    double total = 0;
    for (int i = 0; i < n; i++) { pthread_join(th[i], NULL); total += a[i].rate; }
    return total;
}

/* The IFMA step comes in two forms that do the same (kv_bwd_step): one vector at a time, or two, which keeps the units busier
 * when a core runs a single thread but gains nothing when the sibling thread fills the gaps. So when one of the two has won, they
 * are timed again with every logical CPU busy, and the pair is kept only if it is faster there too. Only variants that kround_pick
 * found equal to the portable round take part (the winner is one of them). */
static void kround_choose_pair(void)
{
    const int one = kround_find("avx512i"), two = kround_find("avx512i2");
    if (one < 0 || two < 0 || (g_kround != one && g_kround != two)) return;
    if (!g_kround_ok[one] || !g_kround_ok[two]) return;
    sysinfo_t s;
    sysinfo_detect(&s);
    const int n = s.ncpu > 64 ? 64 : s.ncpu;
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) { g_kround = one; return; }
    ks->P.dp_mask = ~0ULL >> 8;
    ks->P.cap_since = ~0ULL;
    int pos[64] = {0};
    kang_round_fn fns[64] = {0};
    for (int i = 0; i < n; i++) pos[i] = i;
    double r[2];
    for (int v = 0; v < 2; v++) {
        for (int i = 0; i < n; i++) fns[i] = KROUNDS[v ? two : one].fn;
        r[v] = kbench_run(ks, &s, n, fns, pos, 0.15);
    }
    kang_free_test_state(ks);
    if (g_env.kang_verbose)
        fprintf(stderr, "kangaroo CPU: IFMA with %d threads: one vector %.2f, two vectors %.2f M steps/s\n", n, r[0] / 1e6, r[1] / 1e6);
    g_kround = r[1] > r[0] * 1.03 ? two : one;
}

bool kang_mix_wanted(int env, double uniform, double mixed)
{
    if (env >= 0) return env > 0;
    return uniform > 0 && mixed > uniform * 1.03;              /* the margin of the choice of the step */
}

bool kang_slot_is_sibling(const sysinfo_t *s, int slot)
{
    if (s->ncpu <= 0) return false;
    const int pos = slot % s->ncpu;                            /* as sysinfo_pin_thread */
    int p[1];
    return sysinfo_core_cpus(s, s->core_of[pos], p, 1) && p[0] != pos;
}

/* On cores with SMT the first thread can run the chosen (vector) step and the others the scalar BMI2/ADX one, which uses the
 * integer multiplier instead of the vector unit: +14 % on a Zen runner without AVX-512, a loss elsewhere. PZ_KANG_MIX=0 or 1
 * decides; unset, the two layouts are timed with every thread of whole cores busy (at most 64), twice and interleaved, and the
 * mixed one is kept when it is faster by the margin of kang_mix_wanted. `measured`: kround_pick has checked the variants (the
 * scalar step is checked here otherwise, when it is wanted). */
static void kround_choose_mix(bool measured)
{
    const int vx86 = kround_find("x86");
    if (g_env.kang_mix == 0 || vx86 < 0 || g_kround == vx86 || !KROUNDS[vx86].supported()) return;
    if (g_env.kang_mix < 0 && !measured) return;               /* PZ_KANG_ROUND imposes the step: no measured layout either */
    sysinfo_t s;
    sysinfo_detect(&s);
    if (s.ncpu <= s.ncore) return;                             /* no SMT */
    if (measured ? !g_kround_ok[vx86] : !kang_round_check(KROUNDS[vx86].fn, 96, 75)) return;
    double ru = 0, rm = 0;
    if (g_env.kang_mix < 0) {
        int pos[64], n = 0;
        kang_round_fn uni[64], mix[64];
        for (int c = 0; c < s.ncore && n < 64; c++) {
            int p[8];
            const int k = sysinfo_core_cpus(&s, c, p, 8);
            if (n + k > 64) break;                             /* whole cores only */
            for (int j = 0; j < k; j++) {
                pos[n] = p[j];
                uni[n] = KROUNDS[g_kround].fn;
                mix[n++] = j == 0 ? KROUNDS[g_kround].fn : KROUNDS[vx86].fn;
            }
        }
        kang_state *ks = kang_make_test_state(60, 1);
        if (!ks) return;
        ks->P.dp_mask = ~0ULL >> 8;
        ks->P.cap_since = ~0ULL;
        for (int rep = 0; rep < 2; rep++) {
            const double u = kbench_run(ks, &s, n, uni, pos, 0.15), m = kbench_run(ks, &s, n, mix, pos, 0.15);
            if (u > ru) ru = u;
            if (m > rm) rm = m;
        }
        kang_free_test_state(ks);
        if (g_env.kang_verbose)
            fprintf(stderr, "kangaroo CPU: %d threads: %s on all %.2f, mixed with %s %.2f M steps/s\n", n, KROUNDS[g_kround].name, ru / 1e6,
                    KROUNDS[vx86].name, rm / 1e6);
    }
    g_kmix = kang_mix_wanted(g_env.kang_mix, ru, rm);
    g_kmix_gain = ru > 0 ? rm / ru - 1.0 : 0;
    if (g_kmix && g_env.kang_mix > 0)
        pz_log(PZ_LOG_INFO, "kangaroo CPU: the other threads of each core run the %s step (PZ_KANG_MIX=1)", KROUNDS[vx86].name);
    else if (g_kmix)
        pz_log(PZ_LOG_INFO, "kangaroo CPU: the other threads of each core run the %s step, %+.0f%% measured against %s on every thread "
               "(PZ_KANG_MIX=0 turns it off)", KROUNDS[vx86].name, 100.0 * g_kmix_gain, KROUNDS[g_kround].name);
    else
        pz_log_file(PZ_LOG_INFO, "kangaroo CPU: %s on every thread; with %s on the other threads of each core: %+.0f%%, not used",
                    KROUNDS[g_kround].name, KROUNDS[vx86].name, 100.0 * g_kmix_gain);
}

void kang_bench_cpu(const sysinfo_t *s, double secs)
{
    kang_state *ks = kang_make_test_state(60, 1);
    if (!ks) return;
    ks->P.dp_mask = ~0ULL >> 8;                /* pure stepping */
    ks->P.cap_since = ~0ULL;
    static int pos_all[1024], pos_core[1024];
    static kang_round_fn fns[1024];
    const int nall = s->ncpu > 256 ? 256 : s->ncpu;
    int ncore = 0;
    for (int c = 0; c < s->ncore && ncore < 256; c++) {
        int p[1];
        if (sysinfo_core_cpus(s, c, p, 1)) pos_core[ncore++] = p[0];
    }
    for (int i = 0; i < nall; i++) pos_all[i] = i;
    {
        char h[32];
        printf("  %-24s%12s", "step variant", "1 thread");
        if (nall > ncore) snprintf(h, sizeof h, "%d cores", ncore);                       /* one thread on each core */
        else snprintf(h, sizeof h, "%d thread%s", ncore, ncore == 1 ? "" : "s");
        printf("%12s", h);
        if (nall > ncore) { snprintf(h, sizeof h, "%d threads", nall); printf("%12s", h); }
        printf("\n");
    }
    double best = 0;
    int best_v = -1;
    char b1[32], b2[32], b3[32];
    for (int v = 0; v < NKROUNDS; v++) {
        if (!KROUNDS[v].supported()) continue;
        for (int i = 0; i < nall; i++) fns[i] = KROUNDS[v].fn;
        const double r1 = kbench_run(ks, s, 1, fns, pos_all, secs);
        const double rc = kbench_run(ks, s, ncore, fns, pos_core, secs);
        const double ra = nall > ncore ? kbench_run(ks, s, nall, fns, pos_all, secs) : rc;
        printf("  %-24s%12s%12s", KROUNDS[v].name, pz_fmt_cell(r1, b1, sizeof b1), pz_fmt_cell(rc, b2, sizeof b2));
        if (nall > ncore) printf("%12s", pz_fmt_cell(ra, b3, sizeof b3));
        printf("\n");
        fflush(stdout);
        const double r = ra > rc ? ra : rc;
        if (r > best) { best = r; best_v = v; }
    }
    /* SMT: the vector variant on the first thread of each core, the scalar one (integer multiplier, not the vector
     * unit) on its sibling */
    const int vx86 = kround_find("x86");
    if (nall > ncore && vx86 >= 0 && KROUNDS[vx86].supported() && best_v >= 0 && best_v != vx86) {
        int n = 0;
        for (int c = 0; c < s->ncore && n < 256; c++) {
            int p[8];
            const int k = sysinfo_core_cpus(s, c, p, 8);
            for (int j = 0; j < k && n < 256; j++) { pos_all[n] = p[j]; fns[n] = j == 0 ? KROUNDS[best_v].fn : KROUNDS[vx86].fn; n++; }
        }
        const double rm = kbench_run(ks, s, n, fns, pos_all, secs);
        const int was_quiet = pz_log_quiet();
        pz_log_set_quiet(1);
        kang_pick_round();                                     /* the layout the campaigns use */
        pz_log_set_quiet(was_quiet);
        char ln[480], use[160];
        if (g_kmix && g_env.kang_mix > 0) snprintf(use, sizeof use, "in use (PZ_KANG_MIX=1)");
        else if (g_kmix) snprintf(use, sizeof use, "in use: %+.0f%% at the start-up measurement (PZ_KANG_MIX=0 turns it off)", 100.0 * g_kmix_gain);
        else if (g_env.kang_mix == 0) snprintf(use, sizeof use, "not used (PZ_KANG_MIX=0)");
        else snprintf(use, sizeof use, "not used (PZ_KANG_MIX=1 imposes it)");
        snprintf(ln, sizeof ln, "  mixed: %s on the first thread of each core and %s on the other: %s steps/s, %+.0f%% against the best uniform setup; %s",
                 KROUNDS[best_v].name, KROUNDS[vx86].name, pz_fmt_si(rm, b1, sizeof b1), 100.0 * (rm / best - 1.0), use);
        pz_fputs_wrap(stdout, ln, PZ_LINE_MAX, 4);
        putchar('\n');
    }
    kang_free_test_state(ks);
}

/* The mixed layout (kround_choose_mix): on cores with SMT, the threads after the first run the scalar step. */
static kang_round_fn kang_round_for_slot(const sysinfo_t *s, int slot)
{
    kang_round_fn fn = kang_pick_round();
    return g_kmix && kang_slot_is_sibling(s, slot) ? KROUNDS[kround_find("x86")].fn : fn;
}

void kang_cpu_worker_main(kang_state *ks, int widx, int slot, u64 stream)
{
    run_ctx *r = ks->r;
    sysinfo_pin_thread(&r->sys, slot);
    kang_worker *w = kang_worker_new(ks, ks->K_thread, stream);
    kang_init_herd(ks, w);
    kang_round_fn round = kang_round_for_slot(&r->sys, slot);
    int dupcap = w->dpcap;
    bool *dups = pz_xcalloc((size_t)dupcap, sizeof(bool));
    while (!atomic_load(&r->stop)) {
        if (UNLIKELY(slot >= atomic_load_explicit(&r->cpu_cap, memory_order_relaxed))) { pz_sleep(0.01); continue; }
        round(w);
        for (int i = 0; i < w->nrestart; i++) {
            kang_restart_one(ks, w, w->restart[i]);
            atomic_fetch_add(&ks->restarts, 1);
        }
        w->nrestart = 0;
        atomic_fetch_add_explicit(&r->ws[widx].units, (u64)w->K, memory_order_relaxed);
        if (w->ndp > 0) {
            if (UNLIKELY(w->ndp > dupcap)) { dupcap = w->dpcap; dups = pz_xrealloc(dups, (size_t)dupcap * sizeof(bool)); }
            for (int k = 0; k < w->ndp; k++) kang_dp_recode(w, &w->dpbuf[k], w->dpidx[k]);
            if (kang_insert_dps(ks, w->dpbuf, w->ndp, dups)) { w->ndp = 0; break; }
            for (int k = 0; k < w->ndp; k++)
                if (dups[k]) {
                    kang_restart_one(ks, w, w->dpidx[k]);
                    atomic_fetch_add(&ks->dups, 1);
                }
            w->ndp = 0;
        }
    }
    free(dups);
    kang_worker_free(w);
}
