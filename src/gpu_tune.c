/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_tune.c - the compile-time variants of the GPU kernels, the choice of the fastest for each device (measured once, remembered
 * on disk), the size of the kangaroo herd and the GPU benchmark. */
#include "gpu_priv.h"
#include "platform.h"
#include "outfmt.h"
#include "doctor.h"
#include <math.h>

static const int CL_VARIANTS[] = {64, 128, 256, 512};
static const int CU_VARIANTS[] = {128, 256, 512, 128 | 4 << 16, 256 | 4 << 16, 512 | 4 << 16};
static const int CL_KVARIANTS[] = {32, 64, 128, 32 | 1 << 16, 64 | 1 << 16, 128 | 1 << 16};
static const int CU_KVARIANTS[] = {64, 128, 256, 64 | 1 << 16, 128 | 1 << 16, 256 | 1 << 16, 64 | 2 << 16, 128 | 2 << 16};

int gpu_variants_of(const rt_dev *d, const int **v)
{
    if (d->api == RT_CUDA) { *v = CU_VARIANTS; return (int)(sizeof CU_VARIANTS / sizeof *CU_VARIANTS); }
    *v = CL_VARIANTS;
    return (int)(sizeof CL_VARIANTS / sizeof *CL_VARIANTS);
}
int gpu_default_variant(const rt_dev *d) { return d->api == RT_CUDA ? 256 : 128; }
/* Work-items of a herd sized to take half the L2 cache with `nk` walkers each, in blocks of 128; 0 when the device has no
 * large cache. Never below 4 warps per SM (a herd smaller than that cannot hide latencies). */
static size_t kang_l2_threads_for(const rt_dev *d, int nk)
{
    if (d->l2_bytes < KANG_L2_MIN || !d->cu) return 0;
    size_t t = (size_t)(d->l2_bytes / 2 / ((size_t)nk * KANG_WALKER_BYTES));
    t &= ~(size_t)127;
    const size_t lo = (size_t)d->cu * 128;
    return t < lo ? lo : t;
}
static size_t kang_default_threads_for(const rt_dev *d)
{
    size_t t = (size_t)d->cu * (d->api == RT_CUDA ? 512 : 64);
    return t > 32768 ? 32768 : t;
}

/* The variants the tuner tries on this device (kernel | L2 flag), most plain first; returns how many (at most 32). The
 * cache-sized herds only where they can differ from the default herd and the cache can hold them. */
int gpu_kvariants_of(const rt_dev *d, int *out)
{
    const int *base = d->api == RT_CUDA ? CU_KVARIANTS : CL_KVARIANTS;
    const int nb = d->api == RT_CUDA ? (int)(sizeof CU_KVARIANTS / sizeof *CU_KVARIANTS) : (int)(sizeof CL_KVARIANTS / sizeof *CL_KVARIANTS);
    int n = 0;
    for (int i = 0; i < nb; i++) out[n++] = base[i];
    if (d->l2_bytes >= KANG_L2_MIN) {
        for (int c = 0; c < 2; c++)
            for (size_t i = 0; i < sizeof KV_L2_NK / sizeof *KV_L2_NK; i++) {
                const int nk = KV_L2_NK[i];
                const size_t tl = kang_l2_threads_for(d, nk);
                if (tl && tl * 4 < kang_default_threads_for(d) * 3) out[n++] = (nk | c << 16) | KV_L2FLAG;   /* clearly fewer work-items than the default herd */
            }
    }
    return n;
}
static int default_kvariant(const rt_dev *d) { return d->api == RT_CUDA ? 128 : 64; }
static bool valid_kvariant(const rt_dev *d, int v)
{
    int vs[32];
    int n = gpu_kvariants_of(d, vs);
    for (int i = 0; i < n; i++) if (vs[i] == v) return true;
    return false;
}
static bool valid_variant(const rt_dev *d, int v)
{
    const int *vs;
    int n = gpu_variants_of(d, &vs);
    for (int i = 0; i < n; i++) if (vs[i] == v) return true;
    return false;
}
static char *kvariant_name(int v, char *b, size_t n)
{
    snprintf(b, n, "NK %d%s%s%s", KV_NK(v), KV_CFG(v) == 1 ? " pairs" : KV_CFG(v) == 2 ? " quads" : "", KV_L2(v) && KV_CFG(v) ? "," : "", KV_L2(v) ? " L2 herd" : "");
    return b;
}
static char *variant_name(int v, char *b, size_t n)
{
    if (VAR_MINB(v)) snprintf(b, n, "%d, register cap", VAR_HALF(v));
    else snprintf(b, n, "%d", VAR_HALF(v));
    return b;
}

static int kang_code(const gpu_session *s) { return s->kvar | (s->l2fit ? KV_L2FLAG : 0); }   /* what the tuner remembers */

/* ---------------- autotune ---------------- */

static bool g_retune;       /* 'bench --retune': ignore the saved choice, measure again and save the new one */
void gpu_set_retune(bool on) { g_retune = on; }

static bool tune_cache_path(char *out, size_t n) { return pz_cache_file("gpu_tune.txt", out, n); }

static void tune_key(const rt_dev *d, bool kang, char *out, size_t n)
{
    snprintf(out, n, "%s%s|%s|%s|%u|%s", kang ? "kang|" : "", rt_api_name(d->api), d->name, d->version, (unsigned)d->cu, PZ_VERSION);
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '=') *c = '_';
}

static int tune_cache_get(const rt_dev *d, bool kang)
{
    char path[512], key[400];
    if (!tune_cache_path(path, sizeof path)) return 0;
    char *t = pz_read_file(path, NULL);
    if (!t) return 0;
    tune_key(d, kang, key, sizeof key);
    int v = 0;
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strrchr(line, '=');
        if (eq && (size_t)(eq - line) == strlen(key) && !strncmp(line, key, strlen(key))) v = pz_atoi(eq + 1);
        line = nl ? nl + 1 : NULL;
    }
    free(t);
    return (kang ? valid_kvariant(d, v) : valid_variant(d, v)) ? v : 0;
}

static void tune_cache_put(const rt_dev *d, bool kang, int variant)
{
    char path[512], key[400];
    if (!tune_cache_path(path, sizeof path)) return;
    tune_key(d, kang, key, sizeof key);
    char *old = pz_read_file(path, NULL);
    size_t cap = (old ? strlen(old) : 0) + 600;
    char *out = pz_xcalloc(cap, 1);
    size_t len = 0;
    for (char *line = old; line && *line;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        char *eq = memchr(line, '=', ll);
        bool same = eq && (size_t)(eq - line) == strlen(key) && !strncmp(line, key, strlen(key));
        if (!same && ll > 0 && len + ll + 2 < cap) { memcpy(out + len, line, ll); len += ll; out[len++] = '\n'; }
        line = nl ? nl + 1 : NULL;
    }
    len += (size_t)snprintf(out + len, cap - len, "%s=%d\n", key, variant);
    pz_write_file_atomic(path, out, len, 0644);
    free(out);
    free(old);
}

/* Keys per second of the prepared linear kernel; 0 on failure. About two seconds at most. */
static double linear_measure(gpu_session *s)
{
    u8 h[20] = {0};
    pz_u256 start = pz_u256_pow2(100);
    uint32_t ng = 1;
    double t_begin = pz_mono(), best = 0;
    int good = 0;
    while (pz_mono() - t_begin < 3.0 && good < 2) {
        lin_launch L;
        memset(&L, 0, sizeof L);
        L.key_start = start;
        L.ngroups = ng;
        L.nkeys = (u64)s->T_max * ng * (u64)(2 * s->half);
        double a = pz_mono();
        if (!gpu_linear_run(s, h, &L) || L.err) return 0;
        double dt = pz_mono() - a;
        pz_u256_add_u64(&start, &start, L.nkeys);
        if (dt < 0.12 && ng < 4096) { ng *= 2; continue; }      /* too short to time reliably */
        double rate = (double)L.nkeys / dt;
        if (rate > best) best = rate;
        good++;
    }
    return best;
}

/* Chooses the variant of the linear kernel for this device (cached on disk per device and program version). */
void gpu_linear_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    char nb[48];
    s->variant = gpu_default_variant(d);
    const char *forced = getenv("PZ_GPU_HALF");
    if (forced && forced[0]) {
        int h = pz_atoi(forced);
        if (valid_variant(d, h)) { s->variant = h; pz_log(PZ_LOG_INFO, "GPU[%d]: kernel window set by PZ_GPU_HALF: %d", idx, h); }
        else pz_log(PZ_LOG_WARN, "GPU[%d]: PZ_GPU_HALF=%s ignored (allowed values with %s: %s)", idx, forced, rt_api_name(d->api),
                    d->api == RT_CUDA ? "128, 256, 512" : "64, 128, 256, 512");
        return;
    }
    const char *dis = getenv("PZ_GPU_TUNE");
    if ((dis && dis[0] == '0') || !(d->is_gpu || (dis && dis[0] == '1'))) return;
    int cached = g_retune ? 0 : tune_cache_get(d, false);
    if (cached) {
        s->variant = cached;
        pz_log(PZ_LOG_INFO, "GPU[%d]: kernel with window %s (from earlier tuning)", idx, variant_name(cached, nb, sizeof nb));
        return;
    }
    const int *vs;
    const int nv = gpu_variants_of(d, &vs);
    pz_log(PZ_LOG_INFO, "GPU[%d]: tuning the kernel (one time only: %d variants, the driver compiles them the first time)...", idx, nv);
    double rate[16] = {0};
    int best = 0;
    for (int i = 0; i < nv; i++) {
        if (gpu_linear_prepare_variant(s, vs[i])) rate[i] = linear_measure(s);
        gpu_linear_release_program(s);
        if (vs[i] == gpu_default_variant(d)) best = i;
    }
    {   /* the default stays unless the fastest variant is clearly (> 3%) faster than it */
        int fastest = 0;
        for (int i = 1; i < nv; i++) if (rate[i] > rate[fastest]) fastest = i;
        if (rate[fastest] > rate[best] * 1.03) best = fastest;
    }
    if (rate[best] <= 0) { s->variant = gpu_default_variant(d); return; }
    char line[400];
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: linear kernel tuning (keys/s):", idx);
    for (int i = 0; i < nv && o < sizeof line; i++) {
        char r[32];
        o += (size_t)snprintf(line + o, sizeof line - o, "%s window %s %s", i ? ";" : "", variant_name(vs[i], nb, sizeof nb), pz_fmt_si(rate[i], r, sizeof r));
    }
    pz_log(PZ_LOG_INFO, "%s; picking window %s", line, variant_name(vs[best], nb, sizeof nb));
    s->variant = vs[best];
    /* the winner must pass the correctness test before it is remembered, otherwise a bad choice would exclude the GPU forever */
    if (!gpu_test_linear(s)) {
        pz_log(PZ_LOG_WARN, "GPU[%d]: variant %s failed the correctness check: using the default and not saving the choice", idx, variant_name(vs[best], nb, sizeof nb));
        s->variant = gpu_default_variant(d);
        gpu_linear_release_program(s);
        return;
    }
    gpu_linear_release_program(s);
    tune_cache_put(d, false, s->variant);
}

/* Work-items of the kangaroo kernel: CUDA keeps 512 threads per SM resident (126 registers: four blocks of 128),
 * OpenCL the historical 64 per compute unit; bounded by the state buffer in an eighth of the device memory. */
size_t gpu_kang_threads(const gpu_session *s)
{
    size_t t = s->l2fit ? kang_l2_threads_for(&s->info, s->NK) : 0;
    if (!t) t = kang_default_threads_for(&s->info);
    size_t by_mem = (size_t)(s->info.gmem / 6 / ((size_t)s->NK * (KSTRIDE * 4 + 2 * KANG_HIST_ROUNDS)));   /* state + history */
    if (t > by_mem) t = by_mem;
    if (t < 64) t = 64;
    if (t > 32768) t = 32768;
    return t;
}

/* Kangaroo steps per second of the kernel for s->NK; 0 if it cannot be built or run (e.g. not enough memory for
 * its private arrays). The herd is random field elements: the cost of a step does not depend on the points. */
static double kang_measure(gpu_session *s)
{
    kang_state *ks = kang_make_test_state(40, 1);
    if (!ks) return 0;
    ks->P.dp_mask = ~0ULL >> 4;        /* no distinguished points, no restarts: pure stepping */
    ks->P.cap_since = ~0ULL;
    const int T = (int)gpu_kang_threads(s);
    double rate = 0;
    if (gpu_kang_prepare(s, &ks->P, T, KANG_HIST_ROUNDS)) {
        const size_t K = (size_t)T * s->NK;
        uint32_t *st = pz_xcalloc(K * KSTRIDE, 4);
        u64 x = 0x9E3779B97F4A7C15ULL;
        for (size_t i = 0; i < K; i++)
            for (int w = 0; w < 16; w++) {           /* x and y: random words, top word below 0xFFFFFFFF (so < p) */
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                st[i * KSTRIDE + w] = (w == 7 || w == 15) ? (uint32_t)(x % 0xFFFFFFFFu) : (uint32_t)x;
            }
        bool ok = rt_write(s->c, s->b_state, 0, st, K * KSTRIDE * 4);
        free(st);
        uint32_t rounds = 1, ndp, nrl;
        const double t_begin = pz_mono();
        while (ok && pz_mono() - t_begin < 4.0) {
            double a = pz_mono();
            if (!gpu_kang_run(s, &ks->P, rounds, &ndp, &nrl)) { rate = 0; break; }
            double dt = pz_mono() - a;
            if (dt < 0.25 && rounds < s->hist_rounds) { rounds *= 2; continue; }
            double r = (double)K * rounds / dt;
            if (r > rate) rate = r;
            if (pz_mono() - t_begin > 1.5) break;
        }
    }
    gpu_kang_release(s);
    kang_free_test_state(ks);
    return rate;
}

/* "128", "128b" (products at every other walker), "128c" (every fourth), with a final "l" for the herd sized to the L2 cache -> variant id, 0 if malformed */
static int parse_kvariant(const char *t)
{
    long n = 0;
    const char *e = t;
    while (*e >= '0' && *e <= '9' && n <= 0xFFFF) n = n * 10 + (*e++ - '0');   /* no strtol: glibc >= 2.38 would be needed */
    if (n <= 0 || n > 0xFFFF) return 0;
    int v = (int)n;
    if (*e == 'b') { v |= 1 << 16; e++; }
    else if (*e == 'c') { v |= 2 << 16; e++; }                  /* products every fourth walker */
    if (*e == 'l') { v |= KV_L2FLAG; e++; }       /* herd sized to the L2 cache */
    return *e ? 0 : v;
}

static void kang_set_variant(gpu_session *s, int v)
{
    s->kvar = KV_KERNEL(v);
    s->NK = KV_NK(v);
    s->l2fit = KV_L2(v) != 0;
}

/* Chooses the kangaroo kernel variant for this device (cached on disk like the linear variant) and leaves it in s. */
void gpu_kang_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    char nb[64];
    kang_set_variant(s, default_kvariant(d));
    const char *forced = getenv("PZ_GPU_NK");
    if (forced && forced[0]) {
        const int v = parse_kvariant(forced);
        if (valid_kvariant(d, v)) { kang_set_variant(s, v); pz_log(PZ_LOG_INFO, "GPU[%d]: kangaroo variant set by PZ_GPU_NK: %s", idx, kvariant_name(v, nb, sizeof nb)); }
        else pz_log(PZ_LOG_WARN, "GPU[%d]: PZ_GPU_NK=%s ignored (allowed values with %s: %s)", idx, forced, rt_api_name(d->api),
                    d->api == RT_CUDA ? "64, 128, 256, with suffix b; 64 and 128 also with c" : "32, 64, 128, with suffix b; with an L2 of at least 8 MiB also 16 and 32 and the suffix l (herd in L2)");
        return;
    }
    const char *dis = getenv("PZ_GPU_TUNE");
    if ((dis && dis[0] == '0') || !(d->is_gpu || (dis && dis[0] == '1'))) return;
    int cached = g_retune ? 0 : tune_cache_get(d, true);
    if (cached) {
        kang_set_variant(s, cached);
        pz_log(PZ_LOG_INFO, "GPU[%d]: kangaroo with %s (from earlier tuning)", idx, kvariant_name(cached, nb, sizeof nb));
        return;
    }
    int vs[32];
    const int nv = gpu_kvariants_of(d, vs);
    pz_log(PZ_LOG_INFO, "GPU[%d]: tuning the kangaroo kernel (one time only: %d variants)...", idx, nv);
    double rate[32] = {0};
    int best = 0;
    for (int i = 0; i < nv; i++) {
        kang_set_variant(s, vs[i]);
        rate[i] = kang_measure(s);
        if (vs[i] == default_kvariant(d)) best = i;
    }
    {   /* the default stays unless the fastest variant is clearly (> 3%) faster than it (a chain of 3% steps would stop short of the best) */
        int fastest = 0;
        for (int i = 1; i < nv; i++) if (rate[i] > rate[fastest]) fastest = i;
        if (rate[fastest] > rate[best] * 1.03) best = fastest;
    }
    char line[1000];
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: kangaroo tuning (steps/s):", idx);
    for (int i = 0; i < nv && o < sizeof line; i++) {
        char r[32];
        o += (size_t)snprintf(line + o, sizeof line - o, "%s %s %s", i ? ";" : "", kvariant_name(vs[i], nb, sizeof nb), rate[i] > 0 ? pz_fmt_si(rate[i], r, sizeof r) : "cannot run");
    }
    if (rate[best] <= 0) {
        kang_set_variant(s, default_kvariant(d));
        pz_log(PZ_LOG_WARN, "%s: using the default %s", line, kvariant_name(kang_code(s), nb, sizeof nb));
        return;
    }
    pz_log(PZ_LOG_INFO, "%s; picking %s", line, kvariant_name(vs[best], nb, sizeof nb));
    kang_set_variant(s, vs[best]);
    /* remembered only once the choice has passed the bit-exact test against the CPU */
    if (!gpu_test_kang(s)) {
        pz_log(PZ_LOG_WARN, "GPU[%d]: variant %s failed the correctness check: using the default", idx, kvariant_name(vs[best], nb, sizeof nb));
        kang_set_variant(s, default_kvariant(d));
        return;
    }
    tune_cache_put(d, true, kang_code(s));
}

/* ---------------- benchmark ---------------- */

typedef struct { double copy_gbs, lo_gops, hi_gops, add3_gops; } gpu_diag;

/* per step of the CUDA kangaroo kernel (mc=0/1, NK 64..256): device memory traffic and integer instructions (SASS counts, docs/en/ALGORITHMS.md 8.4) */
#define KANG_MODEL_BYTES 215
#define KANG_MODEL_INSNS 2200

/* Throughput probes: memory copy bandwidth and 32-bit integer instruction rates (the two limits of the kangaroo kernel:
 * it is bound by both, see docs/en/ALGORITHMS.md 8.4). About two seconds. False when a probe cannot run. */
static bool gpu_diag_run(gpu_session *s, gpu_diag *g)
{
    memset(g, 0, sizeof *g);
    rt_mod *p = rt_build(s->c, RT_PROG_TEST, 0);
    if (!p) return false;
    rt_kern *kbw = rt_kernel(p, "pz_diag_bw", 3), *kalu = rt_kernel(p, "pz_diag_alu", 3);
    bool ok = kbw && kalu;
    /* a multiple of every resident-thread count (1024, 1536, 2048 per SM): no partial last wave */
    size_t n = (size_t)s->info.cu * 12288;
    if (n < 4096) n = 4096;
    if (n > ((size_t)1 << 22)) n = (size_t)1 << 22;
    size_t bytes = (size_t)384 << 20;
    if (s->info.gmem / 10 < bytes) bytes = (size_t)(s->info.gmem / 10);
    uint32_t wpi = (uint32_t)(bytes / 4 / n);
    wpi &= ~3u;
    rt_buf *src = ok && wpi >= 4 ? rt_buf_new(s->c, (size_t)wpi * n * 4, NULL) : NULL, *dst = src ? rt_buf_new(s->c, (size_t)wpi * n * 4, NULL) : NULL;
    rt_buf *out = ok ? rt_buf_new(s->c, n * 4, NULL) : NULL;
    if (ok && src && dst) {
        rt_arg_buf(kbw, 0, src);
        rt_arg_buf(kbw, 1, dst);
        rt_arg_u32(kbw, 2, wpi);
        double best = 1e9;
        for (int i = 0; i < 6 && ok; i++) {           /* the first launch is a warm-up; each is a few ms: keep the best of five */
            double a = pz_mono();
            ok = rt_run(s->c, kbw, n);
            double dt = pz_mono() - a;
            if (i && dt < best) best = dt;
        }
        if (ok) g->copy_gbs = 8.0 * (double)wpi * (double)n / best / 1e9;
    }
    if (ok && out) {
        rt_arg_buf(kalu, 0, out);
        double *res[3] = {&g->lo_gops, &g->hi_gops, &g->add3_gops};
        for (uint32_t mode = 0; mode < 3 && ok; mode++) {
            rt_arg_u32(kalu, 1, mode);
            uint32_t iters = 256;
            double best = 1e9;
            for (int i = 0; i < 8 && ok; i++) {
                rt_arg_u32(kalu, 2, iters);
                double a = pz_mono();
                ok = rt_run(s->c, kalu, n);
                double dt = pz_mono() - a;
                if (dt < 0.1) {                        /* too short: scale the work to about 0.2 s (never above ~0.3 s: display watchdogs) */
                    double f = 0.2 / (dt > 1e-4 ? dt : 1e-4);
                    iters = (uint32_t)((double)iters * (f > 16 ? 16 : f));
                    if (iters > (1u << 28)) iters = 1u << 28;
                    continue;
                }
                if (dt < best) best = dt;
                if (i >= 3) break;
            }
            if (ok && best < 1e9) *res[mode] = 8.0 * (double)n * (double)iters / best / 1e9;
        }
    }
    rt_buf_free(src); rt_buf_free(dst); rt_buf_free(out);
    rt_kern_free(kbw); rt_kern_free(kalu);
    rt_mod_free(p);
    return ok && g->copy_gbs > 0 && g->lo_gops > 0;
}

/* one decimal below 10, none above */
static const char *fmt_dec(double v, char *buf, size_t n)
{
    snprintf(buf, n, v < 10 ? "%.1f" : "%.0f", v);
    return buf;
}

static void gpu_diag_print(const gpu_session *s, const gpu_diag *g, size_t herd_bytes)
{
    const rt_dev *d = &s->info;
    char b1[32], b2[32], b3[32];
    if (d->peak_gbs > 0) pz_prop_line("copy bandwidth", "%s GB/s of %.0f GB/s theoretical (%.0f%%)", fmt_dec(g->copy_gbs, b1, sizeof b1), d->peak_gbs, 100.0 * g->copy_gbs / d->peak_gbs);
    else pz_prop_line("copy bandwidth", "%s GB/s", fmt_dec(g->copy_gbs, b1, sizeof b1));
    pz_prop_line("32-bit integer ops", "mad.lo %s/s, mad.hi %s/s, 3-input add %s/s", pz_fmt_si(g->lo_gops * 1e9, b1, sizeof b1),
                 pz_fmt_si(g->hi_gops * 1e9, b2, sizeof b2), pz_fmt_si(g->add3_gops * 1e9, b3, sizeof b3));
    if (d->cu && d->mhz) {
        char unit[40];
        snprintf(unit, sizeof unit, "per %s per cycle", d->api == RT_CUDA ? "SM" : "CU");
        pz_prop_line(unit, "%s / %s / %s at the rated clock of %u MHz (under load the real clock is usually lower)",
                     fmt_dec(g->lo_gops * 1e3 / d->cu / d->mhz, b1, sizeof b1), fmt_dec(g->hi_gops * 1e3 / d->cu / d->mhz, b2, sizeof b2),
                     fmt_dec(g->add3_gops * 1e3 / d->cu / d->mhz, b3, sizeof b3), d->mhz);
    }
    if (d->l2_bytes) {
        char l2[32], sm[32], hb[32], t[160] = "";
        snprintf(t, sizeof t, "L2 cache %s", pz_fmt_bytes((double)d->l2_bytes, l2, sizeof l2));
        if (d->smem_bytes) snprintf(t + strlen(t), sizeof t - strlen(t), ", shared memory %s per %s", pz_fmt_bytes((double)d->smem_bytes, sm, sizeof sm), d->api == RT_CUDA ? "SM" : "CU");
        pz_prop_line("caches", "%s", t);
        if (herd_bytes) pz_prop_line("kangaroo herd", "%s, %.0f times the L2: every step goes through the card's memory", pz_fmt_bytes((double)herd_bytes, hb, sizeof hb),
                                     (double)herd_bytes / (double)d->l2_bytes);
    }
}

void gpu_bench(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    bool titled = false;
    for (int i = 0; i < n; i++) {
        if (!gpu_selftest_allowed(&devs[i])) continue;
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        s->r = r;
        if (!gpu_session_open(s, &devs[i])) { gpu_session_release(s); free(s); continue; }
        if (!titled) { pz_section("GPUs"); titled = true; }
        printf("GPU[%d] %s via %s\n", devs[i].index, devs[i].name, rt_api_name(devs[i].api));
        fflush(stdout);
        gpu_linear_autotune(s, i);
        if (!gpu_linear_prepare(s)) { gpu_session_release(s); free(s); continue; }
        u8 h[20] = {0};
        lin_launch L;
        uint32_t ng = 2;
        u64 keys = 0;
        pz_u256 start = pz_u256_pow2(100);
        double t0 = pz_mono(), el;
        for (;;) {
            memset(&L, 0, sizeof L);
            L.key_start = start;
            L.ngroups = ng;
            L.nkeys = (u64)s->T_max * ng * (u64)(2 * s->half);
            double a = pz_mono();
            if (!gpu_linear_run(s, h, &L)) break;
            double dt = pz_mono() - a;
            keys += L.nkeys;
            pz_u256_add_u64(&start, &start, L.nkeys);
            el = pz_mono() - t0;
            if (el > 4.0) break;
            if (dt < 0.3 && ng < 4096) ng *= 2;
        }
        char b[32];
        el = pz_mono() - t0;
        char nb[48];
        pz_prop_line("linear search", "%s keys/s, window %s", pz_fmt_si((double)keys / el, b, sizeof b), variant_name(s->variant, nb, sizeof nb));
        fflush(stdout);
        gpu_linear_release_program(s);
        gpu_kang_autotune(s, i);
        double kr = kang_measure(s);
        if (kr > 0) {
            char mu[80];
            pz_prop_line("kangaroo", "%s steps/s, %s; memory used %s", pz_fmt_si(kr, b, sizeof b), kvariant_name(kang_code(s), nb, sizeof nb),
                         gpu_fmt_devused(&devs[i], s->mem_used, mu, sizeof mu));
        }
        gpu_diag dg;
        if (gpu_diag_run(s, &dg)) {
            gpu_diag_print(s, &dg, kr > 0 ? s->mem_used : 0);
            if (kr > 0 && devs[i].api == RT_CUDA) {   /* the static counts of the CUDA kernel (docs/en/ALGORITHMS.md 8.4) */
                char c1[32], c2[32], c3[32];
                pz_prop_line("kangaroo ceilings", "%s steps/s by bandwidth, %s by integer work; measured %s",
                             pz_fmt_si(dg.copy_gbs * 1e9 / KANG_MODEL_BYTES, c2, sizeof c2), pz_fmt_si(dg.lo_gops * 1e9 / KANG_MODEL_INSNS, c3, sizeof c3),
                             pz_fmt_si(kr, c1, sizeof c1));
                pz_prop_line("", "model: ~%d bytes and ~%d integer instructions per step", KANG_MODEL_BYTES, KANG_MODEL_INSNS);
            }
        } else {
            pz_prop_line("probes", "the bandwidth and integer throughput probes cannot run");
        }
        fflush(stdout);
        gpu_session_release(s);
        free(s);
    }
    free(r);
}

void gpu_doctor_cache(void)
{
    char path[512];
    if (!pz_cache_file("gpu_tune.txt", path, sizeof path)) return;
    char *t = pz_read_file(path, NULL);
    int cnt = 0;
    const size_t pz_vl = strlen(PZ_VERSION);
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strrchr(line, '=');
        if (eq && (size_t)(eq - line) > pz_vl && !strncmp(eq - pz_vl - 1, "|" PZ_VERSION, pz_vl + 1)) { doctor_line('i', "saved GPU tuning: %.150s", line); cnt++; }
        line = nl ? nl + 1 : NULL;
    }
    free(t);
    if (!cnt) doctor_line('i', "no GPU kernel tuning saved for version %s: it is done the first time each GPU is used (20-60 s, one time only)", PZ_VERSION);
}
