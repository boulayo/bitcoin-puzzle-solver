/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* cpu_common.c - tables, engine registry and ISA detection for the CPU engines. */
#include "cpu_engine.h"
#include "envopts.h"
#include <pzcore/cpu.h>
#include <pzcore/hash.h>
#include <pthread.h>

pz_ge cpu_gtab[CPU_HALF];
pz_ge cpu_gstep;
static pthread_once_t g_tab_once = PTHREAD_ONCE_INIT;
static atomic_bool g_tab_built;
static campaign_cfg g_basis;            /* has_step, step and step_base of the campaign (cpu_basis_set) */
static pz_ge g_move;                    /* the move of one index: G, or H = step * G */

void cpu_basis_set(const campaign_cfg *c)
{
    if (!c->has_step) return;
    if (atomic_load(&g_tab_built)) pz_die("internal error: the steps of the search are set after the tables of the engines");
    pz_ec_init();
    g_basis.has_step = true, g_basis.step = c->step, g_basis.step_base = c->step_base;
}

bool lin_point(pz_ge *r, const pz_u256 *i)
{
    pz_u256 k;
    cfg_key_of(&g_basis, i, &k);                 /* i itself without a step */
    return pz_ec_mul_g(r, &k);
}

bool lin_mul(pz_ge *r, const pz_u256 *k)
{
    if (!g_basis.has_step) return pz_ec_mul_g(r, k);
    pz_u256 t = *k;
    if (pz_u256_cmp(&t, &pz_secp256k1_n) >= 0) pz_u256_sub(&t, &t, &pz_secp256k1_n);
    pz_u256_mulmod_n(&t, &t, &g_basis.step);
    return pz_ec_mul_g(r, &t);
}

static void tables_build(void)
{
    pz_ec_init();
    atomic_store(&g_tab_built, true);
    if (!lin_mul(&g_move, &(pz_u256){{1, 0, 0, 0}})) pz_die("CPU tables: the step is a multiple of the group order");
    /* i*G (i*H) for i = 1..HALF by repeated affine addition from Jacobian points */
    pz_gej *pts = pz_xmalloc(sizeof(pz_gej) * (CPU_HALF + 1));
    pz_gej cur;
    pz_gej_set_ge(&cur, &g_move);
    pts[0] = cur;
    for (int i = 1; i < CPU_HALF; i++) {
        pz_gej_add_ge(&cur, &cur, &g_move);
        pts[i] = cur;
    }
    pz_u256 k = pz_u256_from_u64(CPU_GRP);
    pz_gej_double(&cur, &pts[CPU_HALF - 1]);       /* 2*HALF*G */
    pts[CPU_HALF] = cur;
    pz_ge *aff = pz_xmalloc(sizeof(pz_ge) * (CPU_HALF + 1));
    if (!pz_gej_to_ge_batch(aff, pts, CPU_HALF + 1)) pz_die("CPU tables: unexpected point at infinity");
    memcpy(cpu_gtab, aff, sizeof(pz_ge) * CPU_HALF);
    cpu_gstep = aff[CPU_HALF];
    pz_ge chk;
    if (!lin_mul(&chk, &k) || !pz_ge_equal(&chk, &cpu_gstep)) pz_die("CPU tables: check failed");
    free(pts);
    free(aff);
}

void cpu_tables_init(void) { pthread_once(&g_tab_once, tables_build); }

void cpu_gmultiples(pz_ge *out, int n)
{
    cpu_tables_init();
    const int m = n < CPU_HALF ? n : CPU_HALF;
    memcpy(out, cpu_gtab, sizeof(pz_ge) * (size_t)m);
    if (n <= CPU_HALF) return;
    /* the rest by repeated addition of G from (CPU_HALF)*G, converted to affine in one batch */
    pz_gej *pts = pz_xmalloc(sizeof(pz_gej) * (size_t)(n - CPU_HALF));
    pz_gej cur;
    pz_gej_set_ge(&cur, &cpu_gtab[CPU_HALF - 1]);
    for (int i = 0; i < n - CPU_HALF; i++) {
        pz_gej_add_ge(&cur, &cur, &g_move);
        pts[i] = cur;
    }
    if (!pz_gej_to_ge_batch(out + CPU_HALF, pts, (size_t)(n - CPU_HALF))) pz_die("CPU tables: unexpected point at infinity");
    free(pts);
}

void scan_job_init(scan_job *j, const u8 h160[20])
{
    memset(j, 0, sizeof *j);
    memcpy(j->target_h160, h160, 20);
    for (int i = 0; i < 5; i++)
        j->target_w[i] = (u32)h160[4 * i] | (u32)h160[4 * i + 1] << 8 | (u32)h160[4 * i + 2] << 16 | (u32)h160[4 * i + 3] << 24;
}

/* ISA probes: the detection itself is in pzcore; PZ_ASSUME_IFMA=1 skips the CPUID check of the IFMA extension, for tests on
 * hosts whose (virtual) CPUID hides an extension the processor does execute. */
bool cpu_has_avx2(void) { return pz_cpu_has_avx2(); }
bool cpu_has_avx512(void) { return pz_cpu_has_avx512(); }
bool cpu_has_ifma(void)
{
    if (g_env.assume_ifma) return pz_cpu_has_avx512();
    return pz_cpu_has_ifma();
}
bool cpu_has_shani(void) { return pz_cpu_has_shani(); }
bool cpu_has_sha2(void) { return pz_cpu_has_sha2(); }

extern const cpu_engine ENG_BASE;
#if defined(__aarch64__)
extern const cpu_engine ENG_NEON;
extern const cpu_engine ENG_NEON2;
#endif
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
extern const cpu_engine ENG_AVX2;
extern const cpu_engine ENG_SHANI;
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
#if defined(__aarch64__)
    g_engines[g_nengines++] = &ENG_NEON;
    g_engines[g_nengines++] = &ENG_NEON2;
#endif
#if defined(__x86_64__) && !defined(PZ_NO_X86_VARIANTS)
    g_engines[g_nengines++] = &ENG_AVX2;
    g_engines[g_nengines++] = &ENG_SHANI;       /* after avx2: the autotune keeps the first of two that measure alike */
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

typedef struct { int hits; pz_u256 key; } qc_t;
static void qc_hit(void *user, const pz_u256 *k)
{
    qc_t *q = user;
    q->hits++;
    q->key = *k;
}

/* The hash of every lane against the scalar code, on keys of a fixed pattern: a fault in one lane or one word position would show in the
 * scan below only if its planted key happened to fall there. */
static bool lanes_check(const cpu_engine *e)
{
    const int L = e->lanes;
    u32 X[8 * 64], pfx[64], out[5 * 64];
    u8 xb[64][32];
    u64 st = 0x9E3779B97F4A7C15ULL;
    for (int round = 0; round < 4; round++) {
        for (int l = 0; l < L; l++) {
            for (int i = 0; i < 32; i++) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                xb[l][i] = round == 3 ? (u8)(l * 31 + i) : (u8)st;
            }
            pfx[l] = 2 + (u32)((l + round) & 1);
            for (int i = 0; i < 8; i++) X[i * L + l] = (u32)xb[l][4 * i] << 24 | (u32)xb[l][4 * i + 1] << 16 | (u32)xb[l][4 * i + 2] << 8 | xb[l][4 * i + 3];
        }
        e->hash160_lanes(X, pfx, out);
        for (int l = 0; l < L; l++) {
            u8 want[20], got[20];
            pz_hash160_pub33((u8)pfx[l], xb[l], want);
            for (int w = 0; w < 5; w++) for (int b = 0; b < 4; b++) got[4 * w + b] = (u8)(out[w * L + l] >> (8 * b));
            if (memcmp(want, got, 20) != 0) return false;
        }
    }
    return true;
}

bool cpu_engine_quickcheck(const cpu_engine *e)
{
    cpu_tables_init();
    if (!lanes_check(e)) return false;
    /* one granule starting at an arbitrary large key, with the target planted at an odd offset in its second half */
    pz_u256 start = pz_u256_pow2(97);
    pz_u256_add_u64(&start, &start, 0x5A5A5ULL);
    pz_u256 want = start;
    pz_u256_add_u64(&want, &want, e->granule / 2 + 37);
    pz_ge p;
    u8 pub[33], h[20];
    if (!lin_point(&p, &want)) return false;
    pz_ge_to_pub33(pub, &p);
    pz_hash160(pub, 33, h);
    scan_job job;
    scan_job_init(&job, h);
    qc_t q = {0};
    job.on_hit = qc_hit;
    job.user = &q;
    u64 done = e->scan(&job, &start, e->granule);
    return done == e->granule && q.hits == 1 && pz_u256_cmp(&q.key, &want) == 0;
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
        pz_u256 start = pz_u256_pow2(100);
        u64 keys = 0;
        double t0 = pz_mono(), t1;
        do {
            keys += e->scan(&job, &start, 1u << 18);
            pz_u256_add_u64(&start, &start, 1u << 18);
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

u64 cpu_scan_reference(const scan_job *job, const pz_u256 *start, u64 nkeys)
{
    pz_ec_init();
    pz_u256 k = *start;
    for (u64 i = 0; i < nkeys; i++) {
        pz_ge p;
        u8 pub[33], h[20];
        if (lin_point(&p, &k)) {
            pz_ge_to_pub33(pub, &p);
            pz_hash160(pub, 33, h);
            if (!memcmp(h, job->target_h160, 20) && job->on_hit) job->on_hit(job->user, &k);
        }
        pz_u256_add_u64(&k, &k, 1);
    }
    return nkeys;
}
