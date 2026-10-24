/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu_common.c - tables, engine registry and ISA detection for the CPU engines. */
#include "cpu_engine.h"
#include "hash.h"
#include <pthread.h>

ge cpu_gtab[CPU_HALF];
ge cpu_gstep;
static pthread_once_t g_tab_once = PTHREAD_ONCE_INIT;

static void tables_build(void)
{
    ec_init();
    /* i*G for i = 1..HALF by repeated affine addition from Jacobian points */
    gej *pts = pz_xmalloc(sizeof(gej) * (CPU_HALF + 1));
    gej cur;
    gej_set_ge(&cur, &EC_G);
    pts[0] = cur;
    for (int i = 1; i < CPU_HALF; i++) {
        gej_add_ge(&cur, &cur, &EC_G);
        pts[i] = cur;
    }
    u256 k = u256_from_u64(CPU_GRP);
    gej_double(&cur, &pts[CPU_HALF - 1]);       /* 2*HALF*G */
    pts[CPU_HALF] = cur;
    ge *aff = pz_xmalloc(sizeof(ge) * (CPU_HALF + 1));
    gej_to_ge_batch(aff, pts, CPU_HALF + 1);
    memcpy(cpu_gtab, aff, sizeof(ge) * CPU_HALF);
    cpu_gstep = aff[CPU_HALF];
    ge chk;
    if (!ec_mul_g(&chk, &k) || !ge_equal(&chk, &cpu_gstep)) pz_die("CPU tables: check failed");
    free(pts);
    free(aff);
}

void cpu_tables_init(void) { pthread_once(&g_tab_once, tables_build); }

void cpu_gmultiples(ge *out, int n)
{
    cpu_tables_init();
    const int m = n < CPU_HALF ? n : CPU_HALF;
    memcpy(out, cpu_gtab, sizeof(ge) * (size_t)m);
    if (n <= CPU_HALF) return;
    /* the rest by repeated addition of G from (CPU_HALF)*G, converted to affine in one batch */
    gej *pts = pz_xmalloc(sizeof(gej) * (size_t)(n - CPU_HALF));
    gej cur;
    gej_set_ge(&cur, &cpu_gtab[CPU_HALF - 1]);
    for (int i = 0; i < n - CPU_HALF; i++) {
        gej_add_ge(&cur, &cur, &EC_G);
        pts[i] = cur;
    }
    gej_to_ge_batch(out + CPU_HALF, pts, (size_t)(n - CPU_HALF));
    free(pts);
}

void scan_job_init(scan_job *j, const u8 h160[20])
{
    memset(j, 0, sizeof *j);
    memcpy(j->target_h160, h160, 20);
    for (int i = 0; i < 5; i++)
        j->target_w[i] = (u32)h160[4 * i] | (u32)h160[4 * i + 1] << 8 | (u32)h160[4 * i + 2] << 16 | (u32)h160[4 * i + 3] << 24;
}

#if defined(__x86_64__)
#include <cpuid.h>
/* ADX is CPUID leaf 7, EBX bit 19. Not __builtin_cpu_supports("adx"): clang (Apple clang too) rejects that feature name. */
static bool cpu_has_adx(void)
{
    unsigned a, b, c, d;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b >> 19 & 1);
}
bool cpu_has_avx2(void) { __builtin_cpu_init(); return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2") && cpu_has_adx(); }
bool cpu_has_avx512(void)
{
    __builtin_cpu_init();
    return cpu_has_avx2() && __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl") &&
           __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq");
}
/* AVX-512 IFMA (Ice Lake, Tiger Lake, Rocket Lake, Sapphire Rapids, Zen 4 and later). PZ_ASSUME_IFMA=1 skips the
 * CPUID check: for tests on hosts whose (virtual) CPUID hides an extension the processor does execute. */
bool cpu_has_ifma(void)
{
    if (!cpu_has_avx512()) return false;
    const char *f = getenv("PZ_ASSUME_IFMA");
    if (f && !strcmp(f, "1")) return true;
    return __builtin_cpu_supports("avx512ifma");
}
#else
bool cpu_has_avx2(void) { return false; }
bool cpu_has_avx512(void) { return false; }
bool cpu_has_ifma(void) { return false; }
#endif

extern const cpu_engine ENG_BASE;
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
extern const cpu_engine ENG_AVX2;
extern const cpu_engine ENG_AVX512;
extern const cpu_engine ENG_AVX2V;
extern const cpu_engine ENG_AVX512V;
extern const cpu_engine ENG_AVX512I;
#endif

static const cpu_engine *g_engines[8];
static int g_nengines;
static pthread_once_t g_engines_once = PTHREAD_ONCE_INIT;

static void engines_build(void)
{
    g_engines[g_nengines++] = &ENG_BASE;
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    g_engines[g_nengines++] = &ENG_AVX2;
    g_engines[g_nengines++] = &ENG_AVX512;
    g_engines[g_nengines++] = &ENG_AVX2V;
    g_engines[g_nengines++] = &ENG_AVX512V;
    g_engines[g_nengines++] = &ENG_AVX512I;
#endif
}

const cpu_engine *const *cpu_engine_all(int *n)
{
    pthread_once(&g_engines_once, engines_build);
    *n = g_nengines;
    return g_engines;
}

const cpu_engine *cpu_engine_by_name(const char *name)
{
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    for (int i = 0; i < n; i++)
        if (!strcmp(all[i]->name, name)) return all[i];
    return NULL;
}

typedef struct { int hits; u256 key; } qc_t;
static void qc_hit(void *user, const u256 *k)
{
    qc_t *q = user;
    q->hits++;
    q->key = *k;
}

bool cpu_engine_quickcheck(const cpu_engine *e)
{
    cpu_tables_init();
    /* one granule starting at an arbitrary large key, with the target planted at an odd offset in its second half */
    u256 start = u256_pow2(97);
    u256_add_u64(&start, &start, 0x5A5A5ULL);
    u256 want = start;
    u256_add_u64(&want, &want, e->granule / 2 + 37);
    ge p;
    u8 pub[33], h[20];
    if (!ec_mul_g(&p, &want)) return false;
    ge_to_pub33(pub, &p);
    hash160(pub, 33, h);
    scan_job job;
    scan_job_init(&job, h);
    qc_t q = {0};
    job.on_hit = qc_hit;
    job.user = &q;
    u64 done = e->scan(&job, &start, e->granule);
    return done == e->granule && q.hits == 1 && u256_cmp(&q.key, &want) == 0;
}

const cpu_engine *cpu_engine_autotune(double secs, bool verbose)
{
    cpu_tables_init();
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    const cpu_engine *best = &ENG_BASE;
    double best_rate = 0;
    u8 h[20] = {0};
    scan_job job;
    scan_job_init(&job, h);
    for (int i = 0; i < n; i++) {
        const cpu_engine *e = all[i];
        if (!e->supported()) continue;
        if (!cpu_engine_quickcheck(e)) {
            pz_log(PZ_LOG_WARN, "CPU engine %s: wrong result in the quick check, excluded", e->name);
            continue;
        }
        u256 start = u256_pow2(100);
        u64 keys = 0;
        double t0 = pz_mono(), t1;
        do {
            keys += e->scan(&job, &start, 1u << 18);
            u256_add_u64(&start, &start, 1u << 18);
            t1 = pz_mono();
        } while (t1 - t0 < secs);
        double rate = (double)keys / (t1 - t0);
        if (verbose) {
            char b[32];
            pz_log(PZ_LOG_INFO, "CPU autotune: %-9s %s keys/s (1 thread)", e->name, pz_fmt_si(rate, b, sizeof b));
        }
        if (rate > best_rate * 1.03) { best_rate = rate; best = e; }
    }
    return best;
}

u64 cpu_scan_reference(const scan_job *job, const u256 *start, u64 nkeys)
{
    ec_init();
    u256 k = *start;
    for (u64 i = 0; i < nkeys; i++) {
        ge p;
        u8 pub[33], h[20];
        if (ec_mul_g(&p, &k)) {
            ge_to_pub33(pub, &p);
            hash160(pub, 33, h);
            if (!memcmp(h, job->target_h160, 20) && job->on_hit) job->on_hit(job->user, &k);
        }
        u256_add_u64(&k, &k, 1);
    }
    return nkeys;
}
