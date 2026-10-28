/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_selftest.c - the GPU kernels against the CPU: field arithmetic, hash160 and curve arithmetic, every variant of the linear
 * kernel and of the kangaroo kernel, bit by bit. */
#include "gpu_priv.h"
#include "platform.h"
#include "outfmt.h"
#include <pzcore/hash.h>

/* The state is in the session: after the system sleeps every GPU thread recovers its device and repeats these tests at the same time. */
u64 gpu_test_rand(gpu_session *s)
{
    u64 x = s->rs ? s->rs : (0x9E3779B97F4A7C15ULL ^ ((u64)(s->info.index + 1) * 0xD1B54A32D192ED03ULL)) | 1;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    s->rs = x;
    return x;
}

/* ---------------- self-test ---------------- */

/* Runs kernel `k` (two buffer arguments) on n items. */
static bool run2(gpu_session *s, rt_kern *k, rt_buf *in, rt_buf *out, size_t n)
{
    rt_arg_buf(k, 0, in);
    rt_arg_buf(k, 1, out);
    return rt_run(s->c, k, n);
}

bool gpu_test_fe(gpu_session *s)
{
    const int N = 192;
    rt_mod *p = rt_build(s->c, RT_PROG_TEST, 0);
    if (!p) return false;
    rt_kern *kfe = rt_kernel(p, "pz_test_fe", 2), *khash = rt_kernel(p, "pz_test_hash", 2), *kec = rt_kernel(p, "pz_test_ec", 2);
    bool ok = kfe && khash && kec;
    uint32_t *in = pz_xcalloc((size_t)N * 64, 4), *out = pz_xcalloc((size_t)N * 64, 4);
    pz_fe A[192], B[192];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 4; j++) { A[i].n[j] = gpu_test_rand(s); B[i].n[j] = gpu_test_rand(s); }
        if (i % 5 == 0) { A[i].n[3] = A[i].n[2] = ~0ULL; }                 /* near p */
        if (i % 7 == 0) { B[i].n[1] = B[i].n[2] = B[i].n[3] = ~0ULL; }
        if (i == 1) { pz_fe_set_u64(&A[i], 0); }
        if (i == 2) { pz_fe_set_u64(&B[i], 0); }
        if (i == 3) { A[i] = pz_fe_p; }
        if (i == 4) { pz_fe_set_u64(&A[i], 1); pz_fe_set_u64(&B[i], 1); }
        for (int j = 0; j < 4; j++) { in[i * 16 + 2 * j] = (u32)A[i].n[j]; in[i * 16 + 2 * j + 1] = (u32)(A[i].n[j] >> 32); in[i * 16 + 8 + 2 * j] = (u32)B[i].n[j]; in[i * 16 + 8 + 2 * j + 1] = (u32)(B[i].n[j] >> 32); }
    }
    rt_buf *bin = ok ? rt_buf_new(s->c, (size_t)N * 64, in) : NULL;
    rt_buf *bout = ok ? rt_buf_new(s->c, (size_t)N * 256, NULL) : NULL;
    if (ok && bin && bout) {
        ok = run2(s, kfe, bin, bout, (size_t)N) && rt_read(s->c, bout, 0, out, (size_t)N * 256);
        for (int i = 0; ok && i < N; i++) {
            pz_fe r[8], e[8];
            for (int k = 0; k < 8; k++) unpack_fe(&r[k], out + i * 64 + k * 8);
            pz_fe_mul(&e[0], &A[i], &B[i]);
            pz_fe_sqr(&e[1], &A[i]);
            pz_fe_add(&e[2], &A[i], &B[i]);
            pz_fe_sub(&e[3], &A[i], &B[i]);
            pz_fe a0 = A[i]; pz_fe_normalize(&a0);
            if (pz_fe_is_zero(&a0)) pz_fe_zero(&e[4]); else pz_fe_inv(&e[4], &A[i]);
            e[5] = A[i];
            pz_fe_sub(&e[6], &B[i], &A[i]);
            pz_fe_add(&e[7], &A[i], &A[i]);
            for (int k = 0; k < 8; k++) {
                if (k == 4 && pz_fe_is_zero(&a0)) continue;   /* inverse of zero is undefined */
                if (!pz_fe_equiv(&r[k], &e[k])) { pz_log(PZ_LOG_ERR, "GPU self-test: operation %d differs from the CPU (item %d)", k, i); ok = false; s->mismatch = true; break; }
            }
        }
    } else ok = false;

    /* hash160 */
    if (ok) {
        const int H = 64;
        uint32_t *hin = pz_xcalloc((size_t)H * 9, 4), *hout = pz_xcalloc((size_t)H * 5, 4);
        u8 xb[64][32];
        pz_fe xs[64];
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < 32; j++) xb[i][j] = (u8)gpu_test_rand(s);
            pz_fe_from_be(&xs[i], xb[i]);
            u8 xx[32]; pz_fe_to_be(xx, &xs[i]);
            memcpy(xb[i], xx, 32);
            uint32_t pk[8]; pack_fe(pk, &xs[i]);
            memcpy(hin + i * 9, pk, 32);
            hin[i * 9 + 8] = (u32)(gpu_test_rand(s) & 1);
        }
        rt_buf *hb = rt_buf_new(s->c, (size_t)H * 36, hin);
        rt_buf *ho = rt_buf_new(s->c, (size_t)H * 20, NULL);
        ok = hb && ho && run2(s, khash, hb, ho, (size_t)H) && rt_read(s->c, ho, 0, hout, (size_t)H * 20);
        for (int i = 0; ok && i < H; i++) {
            u8 h[20], got[20];
            pz_hash160_pub33((u8)hin[i * 9 + 8], xb[i], h);
            for (int w = 0; w < 5; w++) for (int b = 0; b < 4; b++) got[4 * w + b] = (u8)(hout[i * 5 + w] >> (8 * b));
            if (memcmp(h, got, 20)) { pz_log(PZ_LOG_ERR, "GPU self-test: hash160 differs from the CPU (item %d)", i); ok = false; s->mismatch = true; }
        }
        rt_buf_free(hb);
        rt_buf_free(ho);
        free(hin); free(hout);
    }

    /* point arithmetic: a + t*D */
    if (ok) {
        const int E = 24;
        uint32_t *ein = pz_xcalloc((size_t)E * 33, 4), *eout = pz_xcalloc((size_t)E * 17, 4);
        pz_ge pa[24], pd[24];
        u32 ts[24];
        for (int i = 0; i < E; i++) {
            pz_u256 k1 = pz_u256_from_u64(gpu_test_rand(s) | 1), k2 = pz_u256_from_u64(gpu_test_rand(s) | 1);
            pz_ec_mul_g(&pa[i], &k1);
            pz_ec_mul_g(&pd[i], &k2);
            ts[i] = i == 0 ? 0 : (i == 1 ? 1 : (i == 2 ? 0xFFFFFFFFu : (u32)gpu_test_rand(s)));
            pack_ge(ein + i * 33, &pa[i]);
            pack_ge(ein + i * 33 + 16, &pd[i]);
            ein[i * 33 + 32] = ts[i];
        }
        rt_buf *eb = rt_buf_new(s->c, (size_t)E * 33 * 4, ein);
        rt_buf *eo = rt_buf_new(s->c, (size_t)E * 17 * 4, NULL);
        ok = eb && eo && run2(s, kec, eb, eo, (size_t)E) && rt_read(s->c, eo, 0, eout, (size_t)E * 17 * 4);
        for (int i = 0; ok && i < E; i++) {
            /* want = pa + ts*pd computed with the host group law */
            pz_ge want;
            pz_gej acc;
            acc.inf = true;
            for (int bit = 31; bit >= 0; bit--) {
                pz_gej_double(&acc, &acc);
                if ((ts[i] >> bit) & 1) pz_gej_add_ge(&acc, &acc, &pd[i]);
            }
            pz_gej_add_ge(&acc, &acc, &pa[i]);
            bool inf = !pz_gej_to_ge(&want, &acc);
            if (inf || eout[i * 17 + 16] == 0) { if (!(inf && eout[i * 17 + 16] == 0)) { ok = false; s->mismatch = true; } continue; }
            pz_ge got;
            unpack_fe(&got.x, eout + i * 17);
            unpack_fe(&got.y, eout + i * 17 + 8);
            if (!pz_ge_equal(&got, &want)) { pz_log(PZ_LOG_ERR, "GPU self-test: point sum differs from the CPU (item %d)", i); ok = false; s->mismatch = true; }
        }
        rt_buf_free(eb);
        rt_buf_free(eo);
        free(ein); free(eout);
    }
    rt_buf_free(bin);
    rt_buf_free(bout);
    rt_kern_free(kfe);
    rt_kern_free(khash);
    rt_kern_free(kec);
    rt_mod_free(p);
    free(in); free(out);
    return ok;
}

bool gpu_test_linear(gpu_session *s)
{
    if (!gpu_linear_prepare(s)) return false;
    const size_t tmax_production = s->T_max;      /* the test uses a handful of work-items; production keeps the full size */
    s->T_max = s->T_max > 96 ? 96 : s->T_max;
    bool ok = true;
    pz_u256 start = pz_u256_pow2(77);
    pz_u256_add_u64(&start, &start, gpu_test_rand(s) & 0xffffff);
    /* Three launches over 40 work-items (the first and last key of the range and a random one), then one small launch for each position
     * of a group where the kernel takes its own branch: the lowest keys of the window (C - HALF*G, which has no counterpart above the
     * centre), the keys next to the centre (the first denominator, i = 1), the centre itself, and the highest. They are placed in the
     * second group of the second work-item (4 groups in: the start of the group comes from the previous one and the centre from the
     * multiple of the step), which the first and last key of the range never reach. */
    const u64 H = (u64)s->half, G = 2 * H;
    const u64 wpos[] = {0, 1, H - 2, H - 1, H, H + 1, H + 2, G - 2, G - 1};
    const int nfix = (int)(sizeof wpos / sizeof *wpos);
    for (int trial = 0; trial < 3 + nfix && ok; trial++) {
        const bool fixed = trial >= 3;
        u64 span = fixed ? 6 * G : 40 * G * 3ULL;
        u64 off = fixed ? 4 * G + wpos[trial - 3] : trial == 0 ? 0 : (trial == 1 ? span - 1 : gpu_test_rand(s) % span);
        pz_u256 key = start;
        pz_u256_add_u64(&key, &key, off);
        pz_ge p;
        u8 pub[33], h[20];
        pz_ec_mul_g(&p, &key);
        pz_ge_to_pub33(pub, &p);
        pz_hash160(pub, 33, h);
        lin_launch L;
        memset(&L, 0, sizeof L);
        L.key_start = start;
        L.nkeys = span;
        L.ngroups = 3;
        if (!gpu_linear_run(s, h, &L)) { ok = false; break; }
        bool found = false;
        for (uint32_t i = 0; i < L.nhits; i++) {
            pz_u256 k = start;
            pz_u256_add_u64(&k, &k, L.hits[i]);
            if (!pz_u256_cmp(&k, &key)) found = true; else { ok = false; s->mismatch = true; }
        }
        if (!found || L.nhits != 1 || L.err) {
            pz_log(PZ_LOG_ERR, "GPU self-test: wrong linear search result (hit=%u err=%u, expected offset %llu)", L.nhits, L.err, (unsigned long long)off);
            ok = false;
            s->mismatch = true;
        }
    }
    s->T_max = tmax_production;
    return ok;
}

static int cmp_dp(const void *a, const void *b)
{
    const uint32_t *x = a, *y = b;
    if (x[8] != y[8]) return x[8] < y[8] ? -1 : 1;
    for (int i = 0; i < 8; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

/* Runs the device kangaroo kernel and the CPU round on identical herds and compares everything. With R = 4 jumps
 * instead of 256 the walks fall into cycles every few steps (detection and escape jumps, counted in *escapes). */
/* `start`: the step number the launch begins at (the ring of tags in the kernel depends on its parity) */
static bool test_kang_one(gpu_session *s, unsigned R, u64 dp_mask, int ROUNDS, u64 start, u64 *escapes)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return false;
    ks->P.R = R;
    ks->P.dp_mask = dp_mask;
    ks->P.cap_since = (1ULL << 32) + 5;   /* half the walkers have gone 2^32 - 4 steps without a DP (see kang_round_compare) */
    const int T = 4, K = T * s->NK;
    bool ok = gpu_kang_prepare(s, &ks->P, T, (uint32_t)ROUNDS);
    kang_worker *w1 = kang_worker_new(ks, K, 77), *w2 = kang_worker_new(ks, K, 77);
    kang_init_herd(ks, w1);
    kang_init_herd(ks, w2);
    w1->round = w2->round = start;
    if (ok) s->gstep = start;
    for (int i = 1; i < K; i += 2) w1->lastdp[i] = w2->lastdp[i] = 0 - ((1ULL << 32) - 4);
    uint32_t *st = pz_xcalloc((size_t)K * KSTRIDE, 4), *back = pz_xcalloc((size_t)K * KSTRIDE, 4);
    for (int i = 0; i < K; i++) pack_state(st + (size_t)i * KSTRIDE, w1, i);
    uint32_t ndp = 0, nrl = 0;
    if (ok) ok = rt_write(s->c, s->b_state, 0, st, (size_t)K * KSTRIDE * 4);
    if (ok) ok = gpu_kang_run(s, &ks->P, ROUNDS, &ndp, &nrl);
    if (ok) ok = rt_read(s->c, s->b_state, 0, back, (size_t)K * KSTRIDE * 4);
    if (ok && (ndp > s->dps_cap || nrl > s->rl_cap)) ok = false, s->mismatch = true;
    if (!ok) ndp = nrl = 0;                                  /* a device that answers nonsense must not decide how much we allocate */
    uint32_t *dpd = pz_xcalloc(ndp + 1, 40), *rld = pz_xcalloc(nrl + 1, 4);
    if (ok && ndp) ok = rt_read(s->c, s->b_dps, 0, dpd, (size_t)ndp * 40);
    if (ok && nrl) ok = rt_read(s->c, s->b_rl, 0, rld, (size_t)nrl * 4);
    if (ok) {
        for (int r = 0; r < ROUNDS; r++) {
            kang_round_base(w2);
            for (int i = 0; i < K; i++) *escapes += w2->jidx[i] >= R;
        }
        for (int i = 0; i < K && ok; i++) {
            uint32_t ref[KSTRIDE];
            pack_state(ref, w2, i);
            if (memcmp(ref, back + (size_t)i * KSTRIDE, KSTRIDE * 4) != 0) {
                pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: state of kangaroo %d differs from the CPU", i);
                ok = false;
                s->mismatch = true;
            }
        }
        if (ok && (int)ndp != w2->ndp) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: %u distinguished points against %d on the CPU", ndp, w2->ndp); ok = false; s->mismatch = true; }
        if (ok && (int)nrl != w2->nrestart) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: %u restarts against %d on the CPU", nrl, w2->nrestart); ok = false; s->mismatch = true; }
        if (ok) {
            uint32_t *cpu = pz_xcalloc((size_t)w2->ndp + 1, 40);
            for (int i = 0; i < w2->ndp; i++) {
                uint32_t *o = cpu + (size_t)i * 10;
                o[0] = (uint32_t)w2->dpbuf[i].tagk; o[1] = (uint32_t)(w2->dpbuf[i].tagk >> 32);
                for (int j = 0; j < 3; j++) { o[2 + 2 * j] = (uint32_t)w2->dpbuf[i].d.w[j]; o[3 + 2 * j] = (uint32_t)(w2->dpbuf[i].d.w[j] >> 32); }
                o[8] = (uint32_t)w2->dpidx[i];
                o[9] = 0;
            }
            for (uint32_t i = 0; i < ndp; i++) dpd[i * 10 + 9] = 0;
            qsort(dpd, ndp, 40, cmp_dp);
            qsort(cpu, (size_t)w2->ndp, 40, cmp_dp);
            if (memcmp(dpd, cpu, (size_t)ndp * 40) != 0) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: distinguished points differ from the CPU"); ok = false; s->mismatch = true; }
            free(cpu);
            uint32_t *rc = pz_xcalloc((size_t)w2->nrestart + 1, 4);
            for (int i = 0; i < w2->nrestart; i++) rc[i] = (uint32_t)w2->restart[i];
            qsort(rc, (size_t)w2->nrestart, 4, cmp_u32);
            qsort(rld, nrl, 4, cmp_u32);
            if (nrl && memcmp(rc, rld, nrl * 4) != 0) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: restarts differ from the CPU"); ok = false; s->mismatch = true; }
            free(rc);
        }
        if (ok && R == KANG_R && (w2->ndp == 0 || w2->nrestart == 0)) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: the test did not produce both distinguished points and restarts"); ok = false; s->mismatch = true; }
    }
    free(st); free(back); free(dpd); free(rld);
    kang_worker_free(w1);
    kang_worker_free(w2);
    kang_free_test_state(ks);
    gpu_kang_release(s);
    return ok;
}

bool gpu_test_kang(gpu_session *s)
{
    u64 esc = 0;
    /* a DP every 64 steps on average and past the first checkpoint step after 64 (restarts of the walkers that have been
     * without a DP for long) ... */
    if (!test_kang_one(s, KANG_R, 63, 72, 0, &esc)) return false;
    /* ... launches that begin and end on odd step numbers (the tags of the two previous points sit in a ring indexed by the parity) */
    if (!test_kang_one(s, KANG_R, 63, 74, 1, &esc) || !test_kang_one(s, KANG_R, 63, 75, 0, &esc) || !test_kang_one(s, KANG_R, 63, 75, 1, &esc)) return false;
    esc = 0;
    /* ... then 4 jumps: cycles and escapes over several checkpoint periods */
    if (!test_kang_one(s, 4, ~0ULL >> 12, 160, 0, &esc)) return false;
    if (!test_kang_one(s, 4, ~0ULL >> 12, 99, 5, &esc)) return false;
    if (esc == 0) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: the test produced no cycle escapes"); s->mismatch = true; }
    return esc > 0;
}

typedef struct { run_ctx *r; double after; } stop_job;

static void *stop_thread(void *p)
{
    stop_job *j = p;
    pz_sleep(j->after);
    atomic_store(&j->r->stop, 1);
    return NULL;
}

typedef struct { gpu_session *s; u64 v[4000]; } rnd_job;

static void *rnd_thread(void *p)
{
    rnd_job *j = p;
    for (size_t k = 0; k < sizeof j->v / sizeof *j->v; k++) j->v[k] = gpu_test_rand(j->s);
    return NULL;
}

/* The rules by which the worker threads recover from a failed launch. */
int gpu_policy_selftest(void)
{
    int fails = 0;
#define PCHK(cond) do { if (!(cond)) { fails++; printf("  GPU recovery: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    /* a quarter of the launch, never less than one */
    PCHK(gpu_launch_after_failure(4096) == 1024 && gpu_launch_after_failure(8) == 2 && gpu_launch_after_failure(4) == 1);
    PCHK(gpu_launch_after_failure(3) == 1 && gpu_launch_after_failure(2) == 1 && gpu_launch_after_failure(1) == 1 && gpu_launch_after_failure(0) == 1);
    uint32_t n = 4096;
    int steps = 0;
    while (n > 1 && steps < 20) { n = gpu_launch_after_failure(n); steps++; }
    PCHK(n == 1 && steps == 6);                               /* 4096 to 1 in six failures */
    /* the budget of failures: 3 in a row are recovered from, the 4th gives up the device */
    gpu_health h = {0};
    PCHK(!gpu_health_failed(&h) && !gpu_health_failed(&h) && !gpu_health_failed(&h) && gpu_health_failed(&h));
    h = (gpu_health){0};
    for (int i = 0; i < 3; i++) { PCHK(!gpu_health_failed(&h)); gpu_health_ok(&h, 0.0); }      /* a success in between: the streak starts again */
    PCHK(!gpu_health_failed(&h));
    /* 40 failures in all are recovered from, whatever the spacing, if good launches never last 10 minutes; the 41st gives up */
    h = (gpu_health){0};
    double t = 100.0;
    int given_up = 0;
    for (int i = 1; i <= 45 && !given_up; i++) {
        t += 1.0;
        gpu_health_ok(&h, t);
        t += GPU_FAIL_FORGET - 1.0;                                 /* 599 s of good launches after the first, just short of the limit */
        gpu_health_ok(&h, t);
        if (gpu_health_failed(&h)) given_up = i;
    }
    PCHK(given_up == 41);
    /* a failure every night: 10 minutes of good launches between two failures wipe the count, so the device is never given up */
    h = (gpu_health){0};
    t = 100.0;
    given_up = 0;
    for (int i = 1; i <= 400 && !given_up; i++) {
        t += 1.0;
        gpu_health_ok(&h, t);
        t += GPU_FAIL_FORGET;
        gpu_health_ok(&h, t);
        if (gpu_health_failed(&h)) given_up = i;
    }
    PCHK(!given_up && h.total == 1);
    /* the random numbers of the tests belong to the session: two threads drawing at once each get the sequence they would get alone */
    {
        enum { NR = 4000 };
        rnd_job job[2];
        pthread_t th[2];
        memset(job, 0, sizeof job);
        for (int i = 0; i < 2; i++) {
            job[i].s = pz_xcalloc(1, sizeof(gpu_session));
            job[i].s->info.index = i;
            if (pthread_create(&th[i], NULL, rnd_thread, &job[i]) != 0) { fails++; th[i] = 0; }
        }
        for (int i = 0; i < 2; i++) {
            if (th[i]) pthread_join(th[i], NULL);
            gpu_session *alone = pz_xcalloc(1, sizeof *alone);
            alone->info.index = i;
            bool same = true;
            for (int k = 0; k < NR; k++) same &= job[i].v[k] == gpu_test_rand(alone);
            PCHK(same);
            free(alone);
            free(job[i].s);
        }
        PCHK(job[0].v[0] != job[1].v[0]);                       /* and the sessions do not draw the same numbers */
    }
    /* the wait of a recovery (up to 30 s) ends when the campaign is stopped, and lasts its time when it is not */
    {
        run_ctx *r = pz_xcalloc(1, sizeof *r);
        double t0 = pz_mono();
        gpu_sleep_unless_stopped(r, 0.3);
        const double full = pz_mono() - t0;
        PCHK(full >= 0.29 && full < 5.0);
        stop_job sj = {r, 0.2};
        pthread_t th;
        if (pthread_create(&th, NULL, stop_thread, &sj) == 0) {
            t0 = pz_mono();
            gpu_sleep_unless_stopped(r, 30.0);
            const double cut = pz_mono() - t0;
            pthread_join(th, NULL);
            PCHK(cut >= 0.15 && cut < 5.0);
            t0 = pz_mono();
            gpu_sleep_unless_stopped(r, 30.0);                   /* already stopped: no wait at all */
            PCHK(pz_mono() - t0 < 1.0);
        } else fails++;
        free(r);
    }
    pz_result_line("GPU recovery rules", "%s", fails ? "FAILED" : "OK");
    return fails;
}

int gpu_selftest(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    int fails = gpu_policy_selftest(), tested = 0;
    for (int i = 0; i < n; i++) {
        if (!gpu_selftest_allowed(&devs[i])) continue;
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        char lb[96];
        snprintf(lb, sizeof lb, "GPU[%d] %.30s via %s", i, devs[i].name, rt_api_name(devs[i].api));
        pz_result_begin(lb);
        bool ok = gpu_session_open(s, &devs[i]) && gpu_test_fe(s);
        const int *vs;
        const int nv = gpu_variants_of(&devs[i], &vs);
        for (int k = 0; k < nv && ok; k++) {          /* every compile-time variant of the linear kernel */
            s->variant = vs[k];
            ok = gpu_test_linear(s);
            gpu_linear_release_program(s);
        }
        if (ok) { gpu_session_release(s); memset(s, 0, sizeof *s); ok = gpu_session_open(s, &devs[i]); }
        int ks[32];
        const int nk = gpu_kvariants_of(&devs[i], ks);
        for (int k = 0; k < nk && ok; k++) {          /* every variant of the kangaroo kernel (the cache-sized herds use the same kernels) */
            if (KV_L2(ks[k])) continue;
            s->kvar = ks[k];
            s->NK = KV_NK(ks[k]);
            ok = gpu_test_kang(s);
        }
        for (size_t k = 0; k < sizeof KV_MC4_NK / sizeof *KV_MC4_NK && ok && devs[i].api == RT_OPENCL; k++) {   /* CUDA tests them as list entries */
            s->kvar = KV_MC4_NK[k] | 2 << 16;
            s->NK = KV_MC4_NK[k];
            ok = gpu_test_kang(s);
        }
        for (size_t k = 0; k < sizeof KV_L2_NK / sizeof *KV_L2_NK && ok && devs[i].l2_bytes >= KANG_L2_MIN; k++)
            for (int c = 0; c < 2 && ok; c++) {       /* the kernels only the cache-sized herds use */
                s->kvar = KV_L2_NK[k] | c << 16;
                s->NK = KV_L2_NK[k];
                ok = gpu_test_kang(s);
            }
        printf(ok ? "kernels identical to the CPU\n" : "FAILED\n");
        if (!ok) fails++;
        tested++;
        gpu_session_release(s);
        free(s);
    }
    if (!tested) pz_result_line("GPU", "none found%s%s. To try an OpenCL CPU device set PZ_OPENCL_ALL=1", note[0] ? ": " : "", note);
    return fails;
}
