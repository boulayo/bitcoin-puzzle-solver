/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_tune.c - the compile-time variants of the GPU kernels, the choice of the fastest for each device (measured once, remembered
 * on disk), the size of the kangaroo herd and the GPU benchmark. */
#include "gpu_priv.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#include "doctor.h"
#include <math.h>
#include <stdarg.h>

static const int CL_VARIANTS[] = {64, 128, 256, 512};
static const int CU_VARIANTS[] = {128, 256, 512, 128 | 4 << 16, 256 | 4 << 16, 512 | 4 << 16};
/* the kangaroo variants: NK plain, pairs and quads, each in the form of 1.19.2 and with the tags written once (KV_METAFLAG, pz_kang.cl KANG_META:
 * measured side by side, RESEARCH.md section 5 after 1.19.2) */
static const int CL_KVARIANTS[] = {32, 64, 128, 32 | 1 << 16, 64 | 1 << 16, 128 | 1 << 16,
                                   32 | KV_METAFLAG, 64 | KV_METAFLAG, 128 | KV_METAFLAG, 32 | 1 << 16 | KV_METAFLAG, 64 | 1 << 16 | KV_METAFLAG, 128 | 1 << 16 | KV_METAFLAG};
static const int CU_KVARIANTS[] = {64, 128, 256, 64 | 1 << 16, 128 | 1 << 16, 256 | 1 << 16, 64 | 2 << 16, 128 | 2 << 16,
                                   64 | KV_METAFLAG, 128 | KV_METAFLAG, 64 | 1 << 16 | KV_METAFLAG, 128 | 1 << 16 | KV_METAFLAG, 64 | 2 << 16 | KV_METAFLAG, 128 | 2 << 16 | KV_METAFLAG};

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
#define KANG_ITEMS_MAX 32768                 /* work-items of the default herd at most */
#define KANG_WIDE_WALKERS (32768u * 128)     /* walkers of a wide herd at most: as many as the default herd of CUDA (the host keeps a mirror of each) */
static size_t kang_default_threads_for(const rt_dev *d)
{
    size_t t = (size_t)d->cu * (d->api == RT_CUDA ? 512 : 64);
    return t > KANG_ITEMS_MAX ? KANG_ITEMS_MAX : t;
}
/* Work-items of a herd wider than the default one, in blocks of 128. OpenCL: 4 or 8 times its 64 per compute unit (on AMD, and on NVIDIA through
 * OpenCL, a compute unit is a whole CU or SM, which 64 work-items leave with no latency hiding), but at most 128 on an Intel GPU, whose compute unit
 * is one EU of 7 or 8 hardware threads. CUDA: 512 per SM past the cap of the default herd, which binds on cards of more than 64 SMs. At most
 * KANG_WIDE_WALKERS walkers. */
static size_t kang_wide_threads_for(const rt_dev *d, int nk, int wide)
{
    size_t per = wide == KV_WIDE_X4 ? 256 : 512;
    if (d->api == RT_OPENCL && d->is_gpu && strstr(d->vendor, "Intel") && per > 128) per = 128;
    size_t t = (size_t)d->cu * per;
    if (t > KANG_WIDE_WALKERS / (size_t)nk) t = KANG_WIDE_WALKERS / (size_t)nk;
    return t & ~(size_t)127;
}
/* Work-items of the herd of a variant: the default herd, one sized to the L2 cache, a wide one or a block herd (as many work-items as stay
 * resident, KANG_BLOCK_ITEMS_CU per SM; 128 per compute unit with OpenCL), bounded by the state and the history of the least depth in a sixth of
 * the device memory. */
static size_t kang_threads_for(const rt_dev *d, int nk, bool l2fit, int wide, bool block)
{
    size_t t = l2fit ? kang_l2_threads_for(d, nk) : wide ? kang_wide_threads_for(d, nk, wide) : 0;
    if (block) t = (size_t)d->cu * (d->api == RT_CUDA ? KANG_BLOCK_ITEMS_CU : KANG_GROUP);
    if (!t) t = kang_default_threads_for(d);
    size_t by_mem = (size_t)(d->gmem / 6 / ((size_t)nk * (KSTRIDE * 4 + 2 * KANG_HIST_ROUNDS)));   /* state + history */
    if (t > by_mem) t = by_mem;
    if (t < 64) t = 64;
    if (t > KANG_ITEMS_MAX && !wide) t = KANG_ITEMS_MAX;
    return t;
}

/* The variants the tuner tries on this device (kernel | L2 flag | width), most plain first; returns how many (at most 32). The wide herds and the
 * cache-sized ones only where they differ clearly from the default herd (and the cache can hold the latter). */
int gpu_kvariants_of(const rt_dev *d, int *out)
{
    const int *base = d->api == RT_CUDA ? CU_KVARIANTS : CL_KVARIANTS;
    const int nb = d->api == RT_CUDA ? (int)(sizeof CU_KVARIANTS / sizeof *CU_KVARIANTS) : (int)(sizeof CL_KVARIANTS / sizeof *CL_KVARIANTS);
    int n = 0;
    for (int i = 0; i < nb; i++) out[n++] = base[i];
    /* OpenCL: NK 32 and 64, plain and pairs, with 4 and 8 times the work-items; CUDA: NK 64 at 512 per SM (as many walkers as the default NK 128) */
    static const int CL_WIDE[] = {32, 64, 32 | 1 << 16, 64 | 1 << 16}, CU_WIDE[] = {64};
    const int *wv = d->api == RT_CUDA ? CU_WIDE : CL_WIDE;
    const int nw = d->api == RT_CUDA ? (int)(sizeof CU_WIDE / sizeof *CU_WIDE) : (int)(sizeof CL_WIDE / sizeof *CL_WIDE);
    for (int w = d->api == RT_CUDA ? KV_WIDE_FULL : KV_WIDE_X4; w <= (d->api == RT_CUDA ? KV_WIDE_FULL : KV_WIDE_X8); w++)
        for (int i = 0; i < nw; i++) {
            const int nk = KV_NK(wv[i]);
            const size_t tw = kang_threads_for(d, nk, false, w, false);
            if (tw * 4 < kang_threads_for(d, nk, false, 0, false) * 5) continue;                       /* not 25% more work-items than the default herd */
            if (w == KV_WIDE_X8 && tw * 4 < kang_threads_for(d, nk, false, KV_WIDE_X4, false) * 5) continue;   /* nor than the 4x herd */
            out[n++] = wv[i] | KV_WIDEFLAG(w);
        }
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
/* what PZ_GPU_NK may force: a variant the tuner measures, or the block herd, which no tuner offers (self-tested all the same) */
static bool forcible_kvariant(const rt_dev *d, int v)
{
    for (size_t i = 0; i < sizeof KV_BLOCK_NK / sizeof *KV_BLOCK_NK; i++) if (v == (KV_BLOCK_NK[i] | KV_BLOCK << 16)) return true;
    return valid_kvariant(d, v);
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
    static const char *const wide[] = {"", ", 4x herd", ", 8x herd", ", full herd"};
    static const char *const cfg[] = {"", " pairs", " quads", " block herd"};
    snprintf(b, n, "NK %d%s%s%s%s%s", KV_NK(v), cfg[KV_CFG(v) & 3], KV_META(v) ? ", tags once" : "", KV_L2(v) && (KV_CFG(v) || KV_META(v)) ? "," : "",
             KV_L2(v) ? " L2 herd" : "", wide[KV_WIDE(v)]);
    return b;
}
static char *variant_name(int v, char *b, size_t n)
{
    static const char *const hash[] = {"", ", hash h1", ", hash h2", ", hash h3"};
    snprintf(b, n, "%d%s%s", VAR_HALF(v), VAR_MINB(v) ? ", register cap" : "", hash[VAR_HASH(v)]);
    return b;
}

static int kang_code(const gpu_session *s) { return s->kvar | (s->l2fit ? KV_L2FLAG : 0) | KV_WIDEFLAG(s->wide); }   /* what the tuner remembers */

/* ---------------- autotune ---------------- */

static bool g_retune;       /* 'bench --retune': ignore the saved choice, measure again and save the new one */
void gpu_set_retune(bool on) { g_retune = on; }

static bool tune_cache_path(char *out, size_t n) { return pz_cache_file("gpu_tune.txt", out, n); }

static void tune_key(const rt_dev *d, bool kang, char *out, size_t n)
{
    snprintf(out, n, "%s%s|%s|%s|%u|%s", kang ? "kang|" : "", rt_api_name(d->api), d->name, d->version, (unsigned)d->cu, PZ_VERSION);
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '=') *c = '_';
}

/* The value of a line of gpu_tune.txt: the variant, then (since the launch length is tuned) a blank and the length of a launch in seconds;
 * a line without it, or with one out of range, keeps GPU_LAUNCH_S. The variant is 0 when the value is empty (pz_atoi skips a newline: an empty
 * value must not read the next line). */
static int tune_value(const char *e, double *launch_s)
{
    if (launch_s) *launch_s = GPU_LAUNCH_S;
    if (!e || *e == '\n' || *e == '\r' || !*e) return 0;
    const int v = pz_atoi(e);
    const char *q = e;
    while (*q >= '0' && *q <= '9') q++;
    if (launch_s && *q == ' ' && q[1] >= '0' && q[1] <= '9') {
        char *end;
        const double x = strtod(q + 1, &end);
        if (end > q + 1 && x >= 0.05 && x <= 5.0) *launch_s = x;
    }
    return v;
}

/* The saved variant of this device (0: none that it offers) and, when `launch_s` is given, the saved length of a launch. */
static int tune_cache_get(const rt_dev *d, bool kang, double *launch_s)
{
    char path[512], key[400];
    if (launch_s) *launch_s = GPU_LAUNCH_S;
    if (!tune_cache_path(path, sizeof path)) return 0;
    char *t = pz_read_file(path, NULL);
    if (!t) return 0;
    tune_key(d, kang, key, sizeof key);
    const char *e = pz_kv_find(t, key, '=', PZ_KV_LAST);        /* the last line of the key wins */
    double ls;
    const int v = tune_value(e, &ls);
    free(t);
    if (!(kang ? valid_kvariant(d, v) : valid_variant(d, v))) return 0;
    if (launch_s) *launch_s = ls;
    return v;
}

/* Devices start at the same time, one thread each. g_tune_mu makes the update of the file one step (two devices that finish together would each
 * keep only their own line), and g_tuning lists the keys being measured: a second device of the same model waits for the first and takes its
 * result from the file, as it did when the devices started one after the other. */
static pthread_mutex_t g_tune_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tune_cv = PTHREAD_COND_INITIALIZER;
static char g_tuning[RT_MAX_DEVS][400];
static int g_tuning_idx[RT_MAX_DEVS];         /* the device that measures each of them */
static int g_ntuning;

/* The device that is measuring `key`, or -1. */
static int tune_busy(const char *key)
{
    for (int i = 0; i < g_ntuning; i++)
        if (!strcmp(g_tuning[i], key)) return g_tuning_idx[i];
    return -1;
}

static void tune_cache_put(const rt_dev *d, bool kang, int variant, double launch_s)
{
    char path[512], key[400], val[32];
    if (!tune_cache_path(path, sizeof path)) return;
    tune_key(d, kang, key, sizeof key);
    snprintf(val, sizeof val, "%d %.2f", variant, launch_s > 0 ? launch_s : GPU_LAUNCH_S);
    pthread_mutex_lock(&g_tune_mu);
    pz_keyed_file_put(path, key, val);
    pthread_mutex_unlock(&g_tune_mu);
}

/* The saved choice for this device if there is one (with its launch length in *launch_s), waiting for another thread that is measuring the
 * same model; 0 when this thread is to measure (and then calls tune_end). */
static int tune_begin(const rt_dev *d, bool kang, int idx, double *launch_s)
{
    char key[400];
    tune_key(d, kang, key, sizeof key);
    int v;
    bool told = false;
    pthread_mutex_lock(&g_tune_mu);
    for (;;) {
        v = tune_cache_get(d, kang, launch_s);
        if (v) break;
        const int other = tune_busy(key);
        if (other < 0) {
            if (g_ntuning < RT_MAX_DEVS) {
                g_tuning_idx[g_ntuning] = idx;
                pz_strlcpy(g_tuning[g_ntuning++], key, sizeof g_tuning[0]);
            }
            break;
        }
        if (!told) rt_log(PZ_LOG_INFO, "GPU[%d]: waiting for the tuning of GPU[%d] (same model)", idx, other);
        told = true;
        pthread_cond_wait(&g_tune_cv, &g_tune_mu);
    }
    pthread_mutex_unlock(&g_tune_mu);
    return v;
}

static void tune_end(const rt_dev *d, bool kang)
{
    char key[400];
    tune_key(d, kang, key, sizeof key);
    pthread_mutex_lock(&g_tune_mu);
    for (int i = 0; i < g_ntuning; i++)
        if (!strcmp(g_tuning[i], key)) {
            --g_ntuning;
            pz_strlcpy(g_tuning[i], g_tuning[g_ntuning], sizeof g_tuning[0]);
            g_tuning_idx[i] = g_tuning_idx[g_ntuning];
            break;
        }
    pthread_cond_broadcast(&g_tune_cv);
    pthread_mutex_unlock(&g_tune_mu);
}

/* One launch of the prepared linear kernel over T_max work-items of `ng` groups each, from *start on (which moves past the keys). False on an
 * error of the API. The tuner and the benchmark time launches like this. */
static bool linear_probe(gpu_session *s, pz_u256 *start, uint32_t ng, lin_launch *L, double *dt)
{
    static const u8 target[20] = {0};
    memset(L, 0, sizeof *L);
    L->key_start = *start;
    L->ngroups = ng;
    L->nkeys = (u64)s->T_max * ng * (u64)(2 * s->half);
    const double a = pz_mono();
    if (!gpu_linear_run(s, target, L)) return false;
    *dt = pz_mono() - a;
    pz_u256_add_u64(start, start, L->nkeys);
    return true;
}

/* Keys per second of the prepared linear kernel; 0 on failure. About two seconds at most. */
static double linear_measure(gpu_session *s)
{
    pz_u256 start = pz_u256_pow2(100);
    uint32_t ng = 1;
    double t_begin = pz_mono(), best = 0;
    int good = 0;
    while (pz_mono() - t_begin < 3.0 && good < 2) {
        lin_launch L;
        double dt;
        if (!linear_probe(s, &start, ng, &L, &dt) || L.err) return 0;
        if (dt < 0.12 && ng < 4096) { ng *= 2; continue; }      /* too short to time reliably */
        double rate = (double)L.nkeys / dt;
        if (rate > best) best = rate;
        good++;
    }
    return best;
}

static double linear_measure_variant(gpu_session *s, int v)
{
    const double rate = gpu_linear_prepare_variant(s, v) ? linear_measure(s) : 0;
    gpu_linear_release_program(s);
    return rate;
}
static void linear_set_variant(gpu_session *s, int v) { s->variant = v; }
static int linear_code(const gpu_session *s) { return s->variant; }
static char *variant_code(int v, char *b, size_t n) { snprintf(b, n, "%d", v); return b; }      /* what PZ_GPU_HALF takes */

/* Work-items of the kangaroo kernel: CUDA keeps 512 threads per SM resident (126 registers: four blocks of 128),
 * OpenCL the historical 64 per compute unit, unless the tuner chose a herd sized to the L2 cache, a wide one or a block herd (kang_threads_for). */
size_t gpu_kang_threads(const gpu_session *s) { return kang_threads_for(&s->info, s->NK, s->l2fit, s->wide, KV_CFG(s->kvar) == KV_BLOCK); }

/* The depth of the jump history of a herd of K walkers, that is the most rounds a launch can run: KANG_HIST_MAX (KANG_HIST_MAX_BLOCK for the
 * small herd of a block-herd kernel) while the history takes at most an eighth of the device memory and the state with it at most the sixth
 * that gpu_kang_threads allows, halved until it does, never below KANG_HIST_ROUNDS (the herd is sized for that). Longer launches pay the fixed
 * costs of a launch (the state read and written back, the first batch products, the transfers and the wait) less often. */
uint32_t gpu_kang_depth(const rt_dev *d, size_t K, int kvar)
{
    uint32_t depth = KV_CFG(kvar) == KV_BLOCK ? KANG_HIST_MAX_BLOCK : KANG_HIST_MAX;
    while (depth > KANG_HIST_ROUNDS && (K * 2 * depth > d->gmem / 8 || K * (KSTRIDE * 4 + 2 * (size_t)depth) > d->gmem / 6)) depth /= 2;
    return depth;
}

/* A herd of random field elements on the device for timing the kernel of s->NK (the cost of a step does not depend on the points): no
 * distinguished points, no restarts, pure stepping. kang_probe_begin builds the kernel and the buffers and writes the herd (false if it cannot
 * be built or run, e.g. not enough memory for its private arrays), kang_probe_run times one launch, kang_probe_end releases everything. */
typedef struct { kang_state *ks; size_t K; } kang_probe;

static void kang_probe_end(gpu_session *s, kang_probe *p)
{
    gpu_kang_release(s);
    if (p->ks) kang_free_test_state(p->ks);
    p->ks = NULL;
}

static bool kang_probe_begin(gpu_session *s, kang_probe *p)
{
    p->ks = kang_make_test_state(40, 1);
    p->K = 0;
    if (!p->ks) return false;
    p->ks->P.dp_mask = ~0ULL >> 4;
    p->ks->P.cap_since = ~0ULL;
    const int T = (int)gpu_kang_threads(s);
    if (!gpu_kang_prepare(s, &p->ks->P, T, gpu_kang_depth(&s->info, (size_t)T * s->NK, s->kvar))) { kang_probe_end(s, p); return false; }
    const size_t K = (size_t)T * s->NK;
    uint32_t *st = pz_xcalloc(K * KSTRIDE, 4);
    u64 x = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < K; i++)
        for (int w = 0; w < 16; w++) {           /* x and y: random words, top word below 0xFFFFFFFF (so < p) */
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            st[i * KSTRIDE + w] = (w == 7 || w == 15) ? (uint32_t)(x % 0xFFFFFFFFu) : (uint32_t)x;
        }
    const bool ok = rt_write(s->c, s->b_state, 0, st, K * KSTRIDE * 4);
    free(st);
    if (!ok) { kang_probe_end(s, p); return false; }
    p->K = K;
    return true;
}

static bool kang_probe_run(gpu_session *s, kang_probe *p, uint32_t rounds, double *dt)
{
    uint32_t ndp, nrl;
    const double a = pz_mono();
    if (!gpu_kang_run(s, &p->ks->P, rounds, &ndp, &nrl)) return false;
    *dt = pz_mono() - a;
    return true;
}

/* Kangaroo steps per second of the kernel for s->NK; 0 if it cannot be built or run. */
static double kang_measure(gpu_session *s)
{
    kang_probe p;
    if (!kang_probe_begin(s, &p)) return 0;
    uint32_t rounds = 1;
    double rate = 0;
    const double t_begin = pz_mono();
    while (pz_mono() - t_begin < 4.0) {
        double dt;
        if (!kang_probe_run(s, &p, rounds, &dt)) { rate = 0; break; }
        if (dt < 0.25 && rounds < s->hist_rounds) { rounds *= 2; continue; }
        const double r = (double)p.K * rounds / dt;
        if (r > rate) rate = r;
        if (pz_mono() - t_begin > 1.5) break;
    }
    kang_probe_end(s, &p);
    return rate;
}

static void kang_set_variant(gpu_session *s, int v)
{
    s->kvar = KV_KERNEL(v);
    s->NK = KV_NK(v);
    s->l2fit = KV_L2(v) != 0;
    s->wide = KV_WIDE(v);
}

static double kang_measure_variant(gpu_session *s, int v)
{
    kang_set_variant(s, v);
    return kang_measure(s);
}

/* What PZ_GPU_NK takes for a variant ("128b", "128cm", "64l", "4g"); "-" for a herd it cannot force (the wide herds have no suffix). */
static char *kvariant_code(int v, char *b, size_t n)
{
    static const char *const cfg[] = {"", "b", "c", "g"};
    if (KV_WIDE(v)) { pz_strlcpy(b, "-", n); return b; }
    snprintf(b, n, "%d%s%s%s", KV_NK(v), cfg[KV_CFG(v) & 3], KV_META(v) ? "m" : "", KV_L2(v) ? "l" : "");
    return b;
}

/* ---------------- the length of a launch ---------------- */

/* The rate of the kernel that is prepared with launches of about `target` seconds, as the controller of a campaign would size them (size:
 * groups per work-item of the linear kernel, rounds of the kangaroo, 1 to `max`): from a launch long enough to time, scaled to the target and
 * corrected once by the duration that gives (that launch is the warm-up of the size), then at least two launches and about a second of them,
 * all counted: the fixed costs of a launch are what differs between the lengths. *size holds the size of the length tried before (0: none):
 * when this length gets the same size (the bounds, or a device too slow for the shorter ones), -1 and nothing is measured. 0 on an error. */
typedef bool (*launch_fn)(gpu_session *s, void *ctx, uint32_t size, double *dt, double *units);

static double tune_at_length(gpu_session *s, launch_fn launch, void *ctx, uint32_t max, double target, double *got, uint32_t *size)
{
    uint32_t n = 1;
    double dt, units;
    for (;;) {
        if (!launch(s, ctx, n, &dt, &units)) return 0;
        if (dt >= 0.05 || n >= max) break;
        n *= 2;
    }
    for (int k = 0; k < 2; k++) {
        double want = (double)n * target / (dt > 1e-4 ? dt : 1e-4);
        n = (uint32_t)(want < 1 ? 1 : want > max ? max : want + 0.5);
        if (n == *size) return -1;
        if (!launch(s, ctx, n, &dt, &units)) return 0;
    }
    *size = n;
    double secs = 0, sum = 0;
    for (int i = 0; i < 2 || secs < 1.0; i++) {
        if (!launch(s, ctx, n, &dt, &units)) return 0;
        secs += dt;
        sum += units;
        if (i >= 1) *got = secs / (i + 1);
    }
    return sum / secs;
}

typedef struct { pz_u256 start; } linear_len;
static bool linear_len_launch(gpu_session *s, void *ctx, uint32_t size, double *dt, double *units)
{
    linear_len *c = ctx;
    lin_launch L;
    if (!linear_probe(s, &c->start, size, &L, dt) || L.err) return false;
    *units = (double)L.nkeys;
    return true;
}
/* the linear kernel of s->variant: the controller counts groups per work-item up to 4096 (gpu_linear_thread) */
static double linear_at_length(gpu_session *s, double target, double *got, uint32_t *size)
{
    linear_len c = {pz_u256_pow2(100)};
    const double r = gpu_linear_prepare_variant(s, s->variant) ? tune_at_length(s, linear_len_launch, &c, 4096, target, got, size) : 0;
    gpu_linear_release_program(s);
    return r;
}

static bool kang_len_launch(gpu_session *s, void *ctx, uint32_t size, double *dt, double *units)
{
    kang_probe *p = ctx;
    if (!kang_probe_run(s, p, size, dt)) return false;
    *units = (double)p->K * size;
    return true;
}
/* the kangaroo kernel of s->kvar: rounds per launch, at most the depth of the history (what the device memory allows) */
static double kang_at_length(gpu_session *s, double target, double *got, uint32_t *size)
{
    kang_probe p;
    if (!kang_probe_begin(s, &p)) return 0;
    const double r = tune_at_length(s, kang_len_launch, &p, s->hist_rounds, target, got, size);
    kang_probe_end(s, &p);
    return r;
}

/* The rule for the length: the shortest of the lengths tried whose rate is within GPU_LAUNCH_NEAR of the best (a rate of 0 or less: not
 * measured); -1 when none was. */
static int tune_launch_choose(const double *rate, int n)
{
    int best = -1;
    for (int i = 0; i < n; i++) if (rate[i] > 0 && (best < 0 || rate[i] > rate[best])) best = i;
    if (best < 0) return -1;
    for (int i = 0; i < n; i++) if (rate[i] > 0 && rate[i] >= rate[best] * GPU_LAUNCH_NEAR) return i;
    return best;
}

/* ---------------- the choice of a variant ---------------- */

/* How the choice of a variant differs between the two kernels. tune_pick does the rest, which is the same for both. */
typedef struct {
    bool kang;                    /* the kangaroo kernel (else the linear one): the key of the saved choice */
    int nv, vs[32], def;          /* the variants to try, and the default one */
    double (*measure)(gpu_session *s, int variant);     /* sets the variant, measures it: units per second, 0 when it cannot run */
    double warm_s;                                      /* seconds of the default variant before the measurements (0: none) */
    void (*set)(gpu_session *s, int variant);
    bool (*check)(gpu_session *s);                      /* the bit-exact test of the variant that is set */
    void (*finish)(gpu_session *s);                     /* what the check leaves behind to release (may be NULL) */
    int (*code)(const gpu_session *s);                  /* the variant that is set, as it is saved */
    char *(*name)(int variant, char *buf, size_t n);
    char *(*code_text)(int variant, char *buf, size_t n);         /* the code as PZ_GPU_HALF or PZ_GPU_NK takes it ("-": none takes it) */
    /* the length of a launch (tune_at_length) of the variant that is set; NULL: not tuned, GPU_LAUNCH_S */
    double (*at_length)(gpu_session *s, double target, double *got, uint32_t *size);
    const char *size_unit;                              /* what the size of a launch counts: "groups per work-item", "rounds" */
    /* the log lines: GPU[%d] and the name of the variant that was saved; GPU[%d] and the number of variants; the title of the table of rates
     * (and of the line of the choice) and the unit of the rates; the word before each variant in them and after "picking"; what the table says
     * of a variant that cannot run (NULL: its rate, 0 included) */
    const char *cached_fmt, *intro_fmt, *head, *unit, *entry, *no_rate;
    const char *no_data_fmt;      /* when no variant could run: the table and the default; NULL: no line */
    const char *bad_check_fmt;    /* when the choice fails the check: GPU[%d] and its name */
} tune_ops;

/* The choice of a variant. Each one is measured for a second or so, which on a laptop GPU held back by its power limit or its temperature is
 * noisy by 2 to 3 % (the same variant measured twice), and the fastest of eight noisy rates is lucky by about as much: with one measurement
 * each and a margin of 3 % (1.19.0 to 1.19.2) the choice between two variants 2.8 % apart, window 256 and window 512 with the register cap on
 * the RTX A2000 Laptop, depended on the run. So: the device is warmed up first (TUNE_WARM_S of the default variant, not counted: the clocks of
 * a card at rest rise during the first seconds), the variants are measured in TUNE_TURNS turns (all of them in order, again in the reverse
 * order, then in order: a drift of the clock weighs the same on every variant), only those within TUNE_NEAR of the fastest of the first turn
 * (and the default) go through all the turns (a variant 5 % behind does not come back when the noise is 3 %), and each one gets the median of
 * its rates, which one lucky or unlucky turn does not move. The default stays unless another is faster by more than TUNE_MARGIN: the median
 * of three is still off by about 1 %, so a smaller margin would pick by luck, and a larger one misses the 2.8 % above. More turns would narrow
 * the margin by the square root of their number only, for a tuning that already takes about a minute per kernel on a laptop. */
#define TUNE_TURNS 3
#define TUNE_TURNS_MAX 9          /* 'bench --variants --turns N' at most */
#define TUNE_NEAR 0.95
#define TUNE_MARGIN 1.01
#define TUNE_WARM_S 3.0

/* The median of n rates, 0 when any of them is 0 (a variant that failed once is not trusted) or there is none. */
static double tune_median(const double *s, int n)
{
    double v[TUNE_TURNS_MAX];
    if (n <= 0 || n > TUNE_TURNS_MAX) return 0;
    for (int i = 0; i < n; i++) {
        if (s[i] <= 0) return 0;
        v[i] = s[i];
        for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { const double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    }
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* The rule for a variant: the index of the one to keep from the medians, the default (index di) unless another is faster by more than
 * TUNE_MARGIN; when the default cannot run (0), the fastest. */
/* The size of a variant that decides a tie: the window of the linear kernel, the walkers per work-item of the kangaroo. A larger one does the
 * same keys or steps with fewer inversions and fewer launches, so when two variants are within the margin of each other the larger wins
 * (on the RTX A2000 window 256 and 512 with the register cap measured 875.4 and 875.1 M keys/s: the card prefers 512 in a campaign). */
static int tune_size(const tune_ops *op, int v) { return op->kang ? KV_NK(v) : VAR_HALF(v); }

static int tune_choose(const tune_ops *op, const double *rate, int nv, int di)
{
    int fastest = 0;
    for (int i = 1; i < nv; i++) if (rate[i] > rate[fastest]) fastest = i;
    if (!(rate[fastest] > rate[di] * TUNE_MARGIN)) return di;
    int pick = fastest;                /* the largest of the variants within the margin of the fastest; the fastest among equal sizes */
    for (int i = 0; i < nv; i++)
        if (rate[i] * TUNE_MARGIN >= rate[fastest] && (tune_size(op, op->vs[i]) > tune_size(op, op->vs[pick]) ||
                                                        (tune_size(op, op->vs[i]) == tune_size(op, op->vs[pick]) && rate[i] > rate[pick]))) pick = i;
    return pick;
}

/* The measurements of the variants in `turns` turns (in order, in the reverse order, in order, ...), after op->warm_s seconds of the default
 * variant: smp[i][0..nsmp[i]-1] for variant i. A variant that could not run in the first turn is not tried again; with `cut`, after the first
 * turn only those within TUNE_NEAR of its fastest and the default (index di) are measured again. */
static void tune_turns(gpu_session *s, const tune_ops *op, int turns, bool cut, int di, double smp[][TUNE_TURNS_MAX], int *nsmp)
{
    for (int i = 0; i < op->nv; i++) nsmp[i] = 0;
    if (op->warm_s > 0) {
        const double t0 = pz_mono();
        while (op->measure(s, op->def) > 0 && pz_mono() - t0 < op->warm_s) {}
    }
    for (int r = 0; r < turns; r++) {
        double top = 0;
        for (int i = 0; i < op->nv; i++) if (nsmp[i] && smp[i][0] > top) top = smp[i][0];
        for (int k = 0; k < op->nv; k++) {
            const int i = r % 2 ? op->nv - 1 - k : k;
            if (r > 0 && (smp[i][0] <= 0 || (cut && i != di && smp[i][0] < top * TUNE_NEAR))) continue;
            smp[i][nsmp[i]++] = op->measure(s, op->vs[i]);
        }
    }
}

/* The length of a launch for the variant that is set (the rule of tune_launch_choose), measured at each of GPU_LAUNCH_TRY seconds and logged
 * like the variants: the table in the log file, the choice on the terminal. GPU_LAUNCH_S when nothing could be measured. */
static double tune_launch(gpu_session *s, int idx, const tune_ops *op)
{
    static const double try_s[] = GPU_LAUNCH_TRY;
    const int n = (int)(sizeof try_s / sizeof *try_s);
    double rate[8] = {0}, got[8] = {0};
    uint32_t size[8] = {0}, prev = 0;
    for (int i = 0; i < n; i++) {
        size[i] = prev;
        got[i] = try_s[i];
        rate[i] = op->at_length(s, try_s[i], &got[i], &size[i]);
        if (rate[i] > 0) prev = size[i];
    }
    const int pick = tune_launch_choose(rate, n);
    char line[600], r[32];
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: launch length (%s; the size in %s):", idx, op->unit, op->size_unit);
    for (int i = 0; i < n && o < sizeof line; i++) {
        if (rate[i] < 0) o += (size_t)snprintf(line + o, sizeof line - o, "%s %.2f s not tried (the same size)", i ? ";" : "", try_s[i]);
        else if (rate[i] == 0) o += (size_t)snprintf(line + o, sizeof line - o, "%s %.2f s cannot run", i ? ";" : "", try_s[i]);
        else o += (size_t)snprintf(line + o, sizeof line - o, "%s %.2f s %s (size %u, %.2f s measured)", i ? ";" : "", try_s[i],
                                   pz_fmt_si(rate[i], r, sizeof r), size[i], got[i]);
    }
    if (pick < 0) {
        rt_log(PZ_LOG_WARN, "%s: none could be measured, launches of %.2f s", line, GPU_LAUNCH_S);
        return GPU_LAUNCH_S;
    }
    int best = pick;
    for (int i = 0; i < n; i++) if (rate[i] > rate[best]) best = i;
    rt_log(PZ_LOG_DEBUG, "%s; picking %.2f s", line, try_s[pick]);
    rt_log(PZ_LOG_INFO, "GPU[%d]: launches of %.2f s (%s %s; %s)", idx, try_s[pick], pz_fmt_si(rate[pick], r, sizeof r), op->unit,
           best == pick ? "the fastest" : "the shortest within 1% of the fastest");
    return try_s[pick];
}

/* Measures the variants and leaves the chosen one in s (the rule above), and the choice is saved only once it has passed the bit-exact test
 * against the CPU, otherwise a bad choice would exclude the GPU for good; then the length of a launch of that variant is measured and saved
 * with it. The terminal shows the choice in one line; the rates of all the variants go to the log file (and to the terminal with -v). */
static void tune_measure(gpu_session *s, int idx, const tune_ops *op)
{
    char nb[64], cb[24];
    rt_log(PZ_LOG_INFO, op->intro_fmt, idx, op->nv);
    double rate[32] = {0}, smp[32][TUNE_TURNS_MAX];
    int nsmp[32], di = 0;
    for (int i = 0; i < op->nv; i++) if (op->vs[i] == op->def) di = i;
    tune_turns(s, op, TUNE_TURNS, true, di, smp, nsmp);
    for (int i = 0; i < op->nv; i++) rate[i] = tune_median(smp[i], nsmp[i]);
    const int best = tune_choose(op, rate, op->nv, di);
    const double margin_pct = 100.0 * (TUNE_MARGIN - 1);
    char line[1000];
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: %s (%s):", idx, op->head, op->unit);
    for (int i = 0; i < op->nv && o < sizeof line; i++) {
        char r[32];
        o += (size_t)snprintf(line + o, sizeof line - o, "%s %s%s %s", i ? ";" : "", op->entry, op->name(op->vs[i], nb, sizeof nb),
                              rate[i] > 0 || !op->no_rate ? pz_fmt_si(rate[i], r, sizeof r) : op->no_rate);
    }
    if (rate[best] <= 0) {
        op->set(s, op->def);
        if (op->no_data_fmt) rt_log(PZ_LOG_WARN, op->no_data_fmt, line, op->name(op->code(s), nb, sizeof nb));
        return;
    }
    char why[160], db[64];
    if (best == di) snprintf(why, sizeof why, " (default; none >%.0f%% faster)", margin_pct);
    else why[0] = 0;
    rt_log(PZ_LOG_DEBUG, "%s; picking %s%s%s", line, op->entry, op->name(op->vs[best], nb, sizeof nb), why);
    {
        char r[32];
        const char *dname = op->name(op->def, db, sizeof db);
        if (best == di) snprintf(why, sizeof why, "the default: no other variant >%.0f%% faster", margin_pct);
        else if (rate[di] > 0) snprintf(why, sizeof why, "+%.1f%% over %s%s", 100.0 * (rate[best] / rate[di] - 1), op->entry, dname);
        else snprintf(why, sizeof why, "the default %s%s cannot run", op->entry, dname);
        rt_log(PZ_LOG_INFO, "GPU[%d]: %s: %s%s, code %s (%s %s, %s; margin %.0f%%)", idx, op->head, op->entry, op->name(op->vs[best], nb, sizeof nb),
               op->code_text(op->vs[best], cb, sizeof cb), pz_fmt_si(rate[best], r, sizeof r), op->unit, why, margin_pct);
    }
    op->set(s, op->vs[best]);
    const bool ok = op->check(s);
    if (op->finish) op->finish(s);
    if (!ok) {
        rt_log(PZ_LOG_WARN, op->bad_check_fmt, idx, op->name(op->vs[best], nb, sizeof nb));
        op->set(s, op->def);
        return;
    }
    s->launch_s = op->at_length ? tune_launch(s, idx, op) : GPU_LAUNCH_S;
    tune_cache_put(&s->info, op->kang, op->code(s), s->launch_s);
}

/* The saved choice if there is one (or the one that another device of the same model is measuring), else the measurement. */
static void tune_pick(gpu_session *s, int idx, const tune_ops *op)
{
    char nb[64];
    double launch_s = GPU_LAUNCH_S;
    const int cached = g_retune ? 0 : tune_begin(&s->info, op->kang, idx, &launch_s);
    if (cached) {
        op->set(s, cached);
        s->launch_s = launch_s;
        rt_log(PZ_LOG_DEBUG, op->cached_fmt, idx, op->name(cached, nb, sizeof nb));   /* the start-up line of the device names the variant */
        return;
    }
    tune_measure(s, idx, op);
    tune_end(&s->info, op->kang);
}

char *gpu_variant_text(const gpu_session *s, bool kang, char *buf, size_t n)
{
    char nb[64];
    if (kang) snprintf(buf, n, "kangaroo %s", kvariant_name(kang_code(s), nb, sizeof nb));
    else snprintf(buf, n, "window %s", variant_name(s->variant, nb, sizeof nb));
    return buf;
}

/* PZ_GPU_TUNE=0: no tuning; =1: also the devices that are not GPUs are tuned (for tests). */
static bool tune_off(const rt_dev *d)
{
    return g_env.gpu_tune == PZ_ENV_TUNE_OFF || !(d->is_gpu || g_env.gpu_tune == PZ_ENV_TUNE_ALL);
}

/* The tuning of each kernel on device d: its variants and how they are measured, checked, named and logged. */
static void linear_tune_ops(const rt_dev *d, tune_ops *op)
{
    *op = (tune_ops){.kang = false, .def = gpu_default_variant(d), .measure = linear_measure_variant, .warm_s = d->is_gpu ? TUNE_WARM_S : 0,
                     .set = linear_set_variant, .check = gpu_test_linear, .finish = gpu_linear_release_program, .code = linear_code,
                     .name = variant_name, .code_text = variant_code, .at_length = linear_at_length, .size_unit = "groups per work-item",
                     .cached_fmt = "GPU[%d]: kernel with window %s (from earlier tuning)",
                     .intro_fmt = "GPU[%d]: tuning the kernel (one time only: %d variants, the driver compiles them the first time)...",
                     .head = "linear kernel tuning", .unit = "keys/s", .entry = "window ",
                     .bad_check_fmt = "GPU[%d]: variant %s failed the correctness check: using the default and not saving the choice"};
    const int *vs;
    op->nv = gpu_variants_of(d, &vs);
    memcpy(op->vs, vs, (size_t)op->nv * sizeof *vs);
}

static void kang_tune_ops(const rt_dev *d, tune_ops *op)
{
    *op = (tune_ops){.kang = true, .def = default_kvariant(d), .measure = kang_measure_variant, .warm_s = d->is_gpu ? TUNE_WARM_S : 0,
                     .set = kang_set_variant, .check = gpu_test_kang, .code = kang_code, .name = kvariant_name, .code_text = kvariant_code,
                     .at_length = kang_at_length, .size_unit = "rounds",
                     .cached_fmt = "GPU[%d]: kangaroo with %s (from earlier tuning)",
                     .intro_fmt = "GPU[%d]: tuning the kangaroo kernel (one time only: %d variants)...",
                     .head = "kangaroo tuning", .unit = "steps/s", .entry = "", .no_rate = "cannot run", .no_data_fmt = "%s: using the default %s",
                     .bad_check_fmt = "GPU[%d]: variant %s failed the correctness check: using the default"};
    op->nv = gpu_kvariants_of(d, op->vs);
}

/* Chooses the variant of the linear kernel for this device (cached on disk per device and program version). */
void gpu_linear_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    s->variant = gpu_default_variant(d);
    s->launch_s = GPU_LAUNCH_S;
    if (g_env.gpu_half) {
        const int h = g_env.gpu_half;
        if (valid_variant(d, h)) { s->variant = h; rt_log(PZ_LOG_INFO, "GPU[%d]: kernel window set by PZ_GPU_HALF: %d", idx, h); }
        else rt_log(PZ_LOG_WARN, "GPU[%d]: PZ_GPU_HALF=%d ignored (allowed values with %s: %s)", idx, h, rt_api_name(d->api),
                    d->api == RT_CUDA ? "128, 256, 512" : "64, 128, 256, 512");
        return;
    }
    if (tune_off(d)) return;
    tune_ops op;
    linear_tune_ops(d, &op);
    tune_pick(s, idx, &op);
}

/* Chooses the kangaroo kernel variant for this device (cached on disk like the linear variant) and leaves it in s. */
void gpu_kang_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    char nb[64];
    kang_set_variant(s, default_kvariant(d));
    s->launch_s = GPU_LAUNCH_S;
    if (g_env.gpu_nk) {
        const int v = g_env.gpu_nk | g_env.gpu_nk_cfg << 16 | (g_env.gpu_nk_meta ? KV_METAFLAG : 0) | (g_env.gpu_nk_l2 ? KV_L2FLAG : 0);
        if (forcible_kvariant(d, v)) { kang_set_variant(s, v); rt_log(PZ_LOG_INFO, "GPU[%d]: kangaroo variant set by PZ_GPU_NK: %s", idx, kvariant_name(v, nb, sizeof nb)); }
        else rt_log(PZ_LOG_WARN, "GPU[%d]: PZ_GPU_NK=%s ignored (allowed values with %s: %s)", idx, g_env.gpu_nk_text, rt_api_name(d->api),
                    d->api == RT_CUDA ? "64, 128, 256, with suffix b; 64 and 128 also with c; 64 and 128, plain, b or c, with m after it (tags once); 4g (block herd)"
                                      : "32, 64, 128, with suffix b, then m if wanted (tags once); with an L2 of at least 8 MiB also 16 and 32 and the suffix l (herd in L2); 4g (block herd)");
        return;
    }
    if (tune_off(d)) return;
    tune_ops op;
    kang_tune_ops(d, &op);
    tune_pick(s, idx, &op);
}

/* ---------------- benchmark ---------------- */

typedef struct { double copy_gbs, lo_gops, hi_gops, add3_gops; } gpu_diag;

/* per step of the CUDA kangaroo kernel (NK 128 quads in the form of 1.19.2, the default; the variant the tuner picked on the card measured):
 * device memory traffic counted on the executed path of the SASS for sm_86 with the per-launch share at 128 rounds, and integer instructions,
 * the step and its share of the inversion (docs/en/RESEARCH.md 5 "After 1.19.2: the bytes of a kangaroo step" and 8.4) */
#define KANG_MODEL_BYTES 181
#define KANG_MODEL_INSNS 1300
/* the block herd (NK 4): the walkers stay in registers; per step the history (written, then read by the replay at the end of the launch) and
 * 941 instructions of the step, plus the 15,680 that the inverting warp adds (the column products and fe_inv) shared by the 16 warp-steps of a
 * block and round */
#define KANG_BLOCK_MODEL_BYTES 4
#define KANG_BLOCK_MODEL_INSNS 1920

/* Throughput probes: memory copy bandwidth and 32-bit integer instruction rates (the two limits of the kangaroo kernel:
 * it is bound by both, see docs/en/RESEARCH.md 8.4). About two seconds. False when a probe cannot run. */
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
        lin_launch L;
        uint32_t ng = 2;
        u64 keys = 0;
        pz_u256 start = pz_u256_pow2(100);
        double t0 = pz_mono(), el;
        for (;;) {
            double dt;
            if (!linear_probe(s, &start, ng, &L, &dt)) break;
            keys += L.nkeys;
            el = pz_mono() - t0;
            if (el > 4.0) break;
            if (dt < 0.3 && ng < 4096) ng *= 2;
        }
        char b[32];
        el = pz_mono() - t0;
        char nb[48];
        pz_prop_line("linear search", "%s keys/s, window %s, launches of %.2f s", pz_fmt_si((double)keys / el, b, sizeof b), variant_name(s->variant, nb, sizeof nb), s->launch_s);
        fflush(stdout);
        gpu_linear_release_program(s);
        gpu_kang_autotune(s, i);
        double kr = kang_measure(s);
        if (kr > 0) {
            char mu[80];
            pz_prop_line("kangaroo", "%s steps/s, %s, launches of %.2f s; memory used %s", pz_fmt_si(kr, b, sizeof b), kvariant_name(kang_code(s), nb, sizeof nb),
                         s->launch_s, gpu_fmt_devused(&devs[i], s->mem_used, mu, sizeof mu));
        }
        gpu_diag dg;
        if (gpu_diag_run(s, &dg)) {
            const bool blk = KV_CFG(s->kvar) == KV_BLOCK;   /* the herd of a block-herd kernel is in the registers, not in the device memory */
            gpu_diag_print(s, &dg, kr > 0 && !blk ? s->mem_used : 0);
            if (kr > 0 && devs[i].api == RT_CUDA && blk) {
                char c1[32], c3[32];
                pz_prop_line("kangaroo ceilings", "%s steps/s by integer work; measured %s", pz_fmt_si(dg.lo_gops * 1e9 / KANG_BLOCK_MODEL_INSNS, c3, sizeof c3),
                             pz_fmt_si(kr, c1, sizeof c1));
                pz_prop_line("", "model: ~%d bytes and ~%d integer instructions per step, one inversion per block and round", KANG_BLOCK_MODEL_BYTES,
                             KANG_BLOCK_MODEL_INSNS);
            } else if (kr > 0 && devs[i].api == RT_CUDA) {   /* the static counts of the CUDA kernel (docs/en/RESEARCH.md 8.4) */
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

/* ---------------- 'bench --variants' ---------------- */

/* The kangaroo variants that no tuner offers and PZ_GPU_NK can force (the block herds): measured by 'bench --variants' where they can run,
 * marked "forced only". */
static int kvariants_forced_only(const rt_dev *d, int *out)
{
    (void)d;
    int n = 0;
    for (size_t i = 0; i < sizeof KV_BLOCK_NK / sizeof *KV_BLOCK_NK; i++) out[n++] = KV_BLOCK_NK[i] | KV_BLOCK << 16;
    return n;
}

/* A line of text of the benchmark, wrapped at the width of the terminal (PZ_LINE_MAX at most) under its indentation. */
static void bench_say(const char *fmt, ...) __attribute__((format(PZ_PRINTF, 1, 2)));
static void bench_say(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pz_fputs_wrap(stdout, buf, PZ_LINE_MAX, 5);
    putchar('\n');
    fflush(stdout);
}

/* A row of a table of the benchmark: never wrapped (its columns would no longer line up), without blanks at the end. */
static void bench_row(const char *line)
{
    char b[400];
    pz_strlcpy(b, line, sizeof b);
    size_t l = strlen(b);
    while (l && b[l - 1] == ' ') b[--l] = 0;
    puts(b);
    fflush(stdout);
}

static void bench_marks(char *out, size_t n, const char *const *marks, int nm)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < nm; i++) if (marks[i]) o += (size_t)snprintf(out + o, n - o, "%s%s", o ? ", " : "  ", marks[i]);
}

/* The table of one kernel: for every variant its code (what PZ_GPU_HALF or PZ_GPU_NK take), its name, the rates of the turns (up to 3; beyond
 * that the least and the greatest), the median, the spread of the rates (max/min - 1) and the median against the default's; the default,
 * the tuner's saved choice, the fastest, the variants that cannot run and those only forced are marked. Rows stay within PZ_LINE_MAX
 * columns. Returns the index of the fastest, -1 when none could run. */
static int bench_variants_table(const tune_ops *op, const bool *forced, int turns, double smp[][TUNE_TURNS_MAX], const int *nsmp, int saved)
{
    char line[400], nb[64], cb[24], r[32], marks[80];
    double rate[32];
    int di = -1, fastest = -1;
    for (int i = 0; i < op->nv; i++) {
        rate[i] = tune_median(smp[i], nsmp[i]);
        if (op->vs[i] == op->def) di = i;
        if (rate[i] > 0 && (fastest < 0 || rate[i] > rate[fastest])) fastest = i;
    }
    const bool per_turn = turns <= 3;
    size_t o = (size_t)snprintf(line, sizeof line, "  %-9s%-25s", "code", "variant");
    if (per_turn)
        for (int t = 0; t < turns; t++) {
            char h[16];
            snprintf(h, sizeof h, "turn %d", t + 1);
            o += (size_t)snprintf(line + o, sizeof line - o, "%11s", h);
        }
    else o += (size_t)snprintf(line + o, sizeof line - o, "%11s%11s", "least", "greatest");
    snprintf(line + o, sizeof line - o, "%11s%8s%12s", "median", "spread", "vs default");
    bench_row(line);
    for (int i = 0; i < op->nv; i++) {
        o = (size_t)snprintf(line, sizeof line, "  %-9s%-25s", op->code_text(op->vs[i], cb, sizeof cb), op->name(op->vs[i], nb, sizeof nb));
        double lo = 0, hi = 0;
        for (int t = 0; t < nsmp[i]; t++) {
            if (smp[i][t] <= 0) continue;
            if (lo == 0 || smp[i][t] < lo) lo = smp[i][t];
            if (smp[i][t] > hi) hi = smp[i][t];
        }
        if (per_turn)
            for (int t = 0; t < turns; t++)
                o += (size_t)snprintf(line + o, sizeof line - o, "%11s", t < nsmp[i] && smp[i][t] > 0 ? pz_fmt_cell(smp[i][t], r, sizeof r) : "");
        else o += (size_t)snprintf(line + o, sizeof line - o, "%11s%11s", lo > 0 ? pz_fmt_cell(lo, r, sizeof r) : "", hi > 0 ? pz_fmt_cell(hi, r, sizeof r) : "");
        if (rate[i] > 0) {
            char sp[16] = "-", vd[16] = "-";
            if (nsmp[i] >= 2 && lo > 0) snprintf(sp, sizeof sp, "%.1f%%", 100.0 * (hi / lo - 1));
            if (di >= 0 && rate[di] > 0) snprintf(vd, sizeof vd, "%+.1f%%", 100.0 * (rate[i] / rate[di] - 1));
            o += (size_t)snprintf(line + o, sizeof line - o, "%11s%8s%12s", pz_fmt_cell(rate[i], r, sizeof r), sp, vd);
        } else o += (size_t)snprintf(line + o, sizeof line - o, "%11s%8s%12s", "", "", "");
        const char *const m[] = {rate[i] <= 0 ? "cannot run" : NULL, i == di ? "default" : NULL, op->vs[i] == saved ? "saved" : NULL,
                                 i == fastest ? "fastest" : NULL, forced[i] ? "forced only" : NULL};
        bench_marks(marks, sizeof marks, m, 5);
        snprintf(line + o, sizeof line - o, "%s", marks);
        bench_row(line);
    }
    return fastest;
}

/* The second table: the rate of the variant that is set with launches of each of GPU_LAUNCH_TRY seconds, as the controller would size them;
 * the saved length and the one the rule picks are marked. Returns the length the rule picks (GPU_LAUNCH_S when nothing could be measured). */
static double bench_launch_table(gpu_session *s, const tune_ops *op, double saved_s)
{
    static const double try_s[] = GPU_LAUNCH_TRY;
    const int n = (int)(sizeof try_s / sizeof *try_s);
    char line[400], r[32], nb[64], marks[80];
    double rate[8] = {0}, got[8] = {0};
    uint32_t size[8] = {0}, prev = 0;
    bench_say("  launch length, %s%s: the rate with launches of each length, sized as the controller would (%s)", op->entry,
              op->name(op->code(s), nb, sizeof nb), op->size_unit);
    for (int i = 0; i < n; i++) {
        size[i] = prev;
        got[i] = try_s[i];
        rate[i] = op->at_length(s, try_s[i], &got[i], &size[i]);
        if (rate[i] > 0) prev = size[i];
    }
    const int pick = tune_launch_choose(rate, n);
    int best = pick;
    for (int i = 0; i < n; i++) if (pick >= 0 && rate[i] > rate[best]) best = i;
    snprintf(line, sizeof line, "  %-9s%11s%9s%11s%12s", "target", "measured", "size", op->unit, "vs best");
    bench_row(line);
    for (int i = 0; i < n; i++) {
        char tg[16], ms[16], sz[16], vb[16];
        snprintf(tg, sizeof tg, "%.2f s", try_s[i]);
        if (rate[i] > 0) {
            snprintf(ms, sizeof ms, "%.2f s", got[i]);
            snprintf(sz, sizeof sz, "%u", size[i]);
            snprintf(vb, sizeof vb, "%+.1f%%", 100.0 * (rate[i] / rate[best] - 1));
        } else { ms[0] = sz[0] = vb[0] = 0; }
        const char *const m[] = {rate[i] < 0 ? "not tried: the same size as the length before it" : rate[i] == 0 ? "cannot run" : NULL,
                                 fabs(try_s[i] - saved_s) < 1e-9 ? "saved" : NULL, i == pick ? (pick == best ? "chosen: the fastest" : "chosen: the shortest within 1% of the fastest") : NULL};
        bench_marks(marks, sizeof marks, m, 3);
        snprintf(line, sizeof line, "  %-9s%11s%9s%11s%12s%s", tg, ms, sz, rate[i] > 0 ? pz_fmt_cell(rate[i], r, sizeof r) : "", vb, marks);
        bench_row(line);
    }
    return pick < 0 ? GPU_LAUNCH_S : try_s[pick];
}

/* Both kernels of one device: every variant in `turns` turns (the tuner's procedure without its cut after the first turn, and the variants
 * only forced too), one table each; then the lengths of a launch of the variant in use (the saved one, else the default). With --retune the
 * rule of the tuner decides from the medians, checks the choice bit for bit and saves it with the length, as the first run would. */
static void bench_variants_kernel(gpu_session *s, int idx, const tune_ops *base, int turns, bool retune, int nforced, const int *forced_v)
{
    tune_ops op = *base;
    bool forced[32] = {false};
    for (int i = 0; i < nforced && op.nv < 32; i++) { forced[op.nv] = true; op.vs[op.nv++] = forced_v[i]; }
    op.warm_s = TUNE_WARM_S;
    double smp[32][TUNE_TURNS_MAX];
    int nsmp[32], di = 0;
    for (int i = 0; i < op.nv; i++) if (op.vs[i] == op.def) di = i;
    bench_say("  %s, %s: %d variants in %d turn%s (in order, reversed, in order, ...), after %.0f s of the default variant not counted",
              op.kang ? "kangaroo kernel" : "linear kernel", op.unit, op.nv, turns, turns == 1 ? "" : "s", op.warm_s);
    tune_turns(s, &op, turns, false, di, smp, nsmp);
    double saved_s = GPU_LAUNCH_S;
    const int saved = tune_cache_get(&s->info, op.kang, &saved_s);
    bench_variants_table(&op, forced, turns, smp, nsmp, saved);
    /* the rule of the tuner on these medians: only the variants it measures (not those only forced) */
    double rate[32] = {0};
    for (int i = 0; i < op.nv; i++) rate[i] = forced[i] ? 0 : tune_median(smp[i], nsmp[i]);
    const int pick = tune_choose(&op, rate, op.nv, di);
    char nb[64], cb[24], r[32], db[64];
    if (rate[pick] <= 0) {
        bench_say("  -> no variant the tuner measures could run: the default %s%s stays", op.entry, op.name(op.def, nb, sizeof nb));
        op.set(s, op.def);
        return;
    }
    bench_say("  -> the rule (the median of the turns, the default unless another is >%.0f%% faster, the largest of those within %.0f%% of the"
              " fastest) picks %s%s, code %s (%s %s%s%+.1f%%)",
           100.0 * (TUNE_MARGIN - 1), 100.0 * (TUNE_MARGIN - 1), op.entry, op.name(op.vs[pick], nb, sizeof nb), op.code_text(op.vs[pick], cb, sizeof cb),
           pz_fmt_si(rate[pick], r, sizeof r), op.unit, rate[di] > 0 ? ", vs the default " : ", the default cannot run ",
           rate[di] > 0 ? 100.0 * (rate[pick] / rate[di] - 1) : 0.0);
    int use = saved ? saved : op.def;
    if (retune) {
        op.set(s, op.vs[pick]);
        const bool ok = op.check(s);
        if (op.finish) op.finish(s);
        if (!ok) {
            bench_say("  -> %s%s failed the correctness check: not saved, the default %s%s stays", op.entry, op.name(op.vs[pick], nb, sizeof nb), op.entry,
                      op.name(op.def, db, sizeof db));
            op.set(s, op.def);
            return;
        }
        use = op.vs[pick];
    }
    op.set(s, use);
    const double len = bench_launch_table(s, &op, saved ? saved_s : GPU_LAUNCH_S);
    if (retune) {
        s->launch_s = len;
        tune_cache_put(&s->info, op.kang, op.code(s), len);
        bench_say("  -> saved for this device: %s%s (code %s) with launches of %.2f s", op.entry, op.name(use, nb, sizeof nb), op.code_text(use, cb, sizeof cb), len);
    } else bench_say("  -> nothing saved (add --retune to save what the tables decide); the tuner's choice stays %s%s with launches of %.2f s", op.entry,
                     op.name(use, nb, sizeof nb), saved ? saved_s : GPU_LAUNCH_S);
    (void)idx;
}

void gpu_bench_variants(int turns)
{
    if (turns < 1) turns = 1;
    if (turns > TUNE_TURNS_MAX) turns = TUNE_TURNS_MAX;
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    const int n = enumerate(devs, note, sizeof note);
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    bool titled = false;
    for (int i = 0; i < n; i++) {
        if (!gpu_selftest_allowed(&devs[i])) continue;
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        s->r = r;
        if (!gpu_session_open(s, &devs[i])) { gpu_session_release(s); free(s); continue; }
        if (!titled) { pz_section("GPUs: every variant of the kernels"); titled = true; }
        printf("GPU[%d] %s via %s\n", devs[i].index, devs[i].name, rt_api_name(devs[i].api));
        fflush(stdout);
        tune_ops op;
        s->variant = gpu_default_variant(&devs[i]);
        s->launch_s = GPU_LAUNCH_S;
        linear_tune_ops(&devs[i], &op);
        bench_variants_kernel(s, i, &op, turns, g_retune, 0, NULL);
        gpu_linear_release_program(s);
        int fv[8];
        const int nf = kvariants_forced_only(&devs[i], fv);
        kang_set_variant(s, default_kvariant(&devs[i]));
        kang_tune_ops(&devs[i], &op);
        bench_variants_kernel(s, i, &op, turns, g_retune, nf, fv);
        gpu_kang_release(s);
        gpu_session_release(s);
        free(s);
    }
    if (!titled) printf("no GPU to measure%s%s\n", note[0] ? ": " : "", note);
    free(r);
}

/* ---------------- self-test of the choice of a variant ---------------- */

/* tune_pick with invented rates: when the default stays, when another wins, what is saved and what is logged. No device is needed. */
static struct {
    double rate[3]; int measured; bool check_ok; int set_to; const double (*seq)[3]; int nth[3];
    double len_rate[4]; uint32_t len_size[4]; int len_calls;      /* the rates at the four lengths of a launch and the controller's sizes for them */
} g_fake;
static const int FAKE_V[3] = {128, 256, 512};

static double fake_measure(gpu_session *s, int v)
{
    g_fake.measured++;
    linear_set_variant(s, v);
    for (int i = 0; i < 3; i++) if (FAKE_V[i] == v) return g_fake.seq ? g_fake.seq[i][g_fake.nth[i]++ % 3] : g_fake.rate[i];
    return 0;
}
static bool fake_check(gpu_session *s) { (void)s; return g_fake.check_ok; }
/* tune_at_length with invented rates: a length whose size is that of the length before is not tried (-1), as the real one does */
static double fake_at_length(gpu_session *s, double target, double *got, uint32_t *size)
{
    static const double try_s[] = GPU_LAUNCH_TRY;
    (void)s;
    g_fake.len_calls++;
    for (int i = 0; i < 4; i++) {
        if (fabs(try_s[i] - target) > 1e-9) continue;
        if (g_fake.len_rate[i] <= 0) return 0;
        if (g_fake.len_size[i] == *size) return -1;
        *size = g_fake.len_size[i];
        *got = target;
        return g_fake.len_rate[i];
    }
    return 0;
}

typedef struct { char line[12][240]; int n; } fake_log;
static void fake_sink(void *ctx, pz_loglevel lvl, const char *msg)
{
    fake_log *l = ctx;
    if (l->n < 12) snprintf(l->line[l->n++], sizeof l->line[0], "%s%s", lvl == PZ_LOG_WARN ? "WARN " : "", msg);
}

static gpu_session *fake_session(const char *name)
{
    gpu_session *s = pz_xcalloc(1, sizeof *s);
    s->info.api = RT_CUDA;
    s->info.is_gpu = true;
    s->info.cu = 8;
    snprintf(s->info.name, sizeof s->info.name, "%s", name);          /* its own key in the saved choices */
    snprintf(s->info.version, sizeof s->info.version, "sm_86");
    return s;
}

static tune_ops fake_ops(void)
{
    tune_ops op = {.kang = false, .nv = 3, .def = 256, .measure = fake_measure, .set = linear_set_variant, .check = fake_check, .code = linear_code,
                   .name = variant_name, .code_text = variant_code, .size_unit = "groups", .cached_fmt = "GPU[%d]: cached %s", .intro_fmt = "GPU[%d]: tuning %d variants", .head = "table", .unit = "u/s", .entry = "window ",
                   .bad_check_fmt = "GPU[%d]: variant %s failed"};
    memcpy(op.vs, FAKE_V, sizeof FAKE_V);
    return op;
}

int gpu_tune_selftest(void)
{
    int fails = 0;
#define TCHK(cond) do { if (!(cond)) { fails++; printf("  GPU tuning: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)
    char tmp[512];
    if (!pz_make_temp_dir(tmp, sizeof tmp, "pztune")) return 0;       /* nothing to test with: the other checks of the self-test say more */
    char old[PZ_ENV_PATH];
    pz_strlcpy(old, g_env.xdg_cache_home, sizeof old);
    pz_strlcpy(g_env.xdg_cache_home, tmp, sizeof g_env.xdg_cache_home);       /* the saved choices of the test go to a folder of its own */
    /* rates of the windows 128, 256 (the default) and 512; the window that is picked; the lines logged (the table, when one is made, for the log
     * file, and the choice for the terminal) */
    static const struct { double rate[3]; int picked; const char *table, *choice; } cases[] = {
        {{100e6, 102e6, 90e6}, 256, "window 128 100.00 M; window 256 102.00 M; window 512 90.00 M; picking window 256 (default; none >1% faster)",
         "window 256, code 256 (102.00 M u/s, the default: no other variant >1% faster; margin 1%)"},             /* the default is the fastest */
        {{100e6, 102e6, 107e6}, 512, "window 128 100.00 M; window 256 102.00 M; window 512 107.00 M; picking window 512",
         "window 512, code 512 (107.00 M u/s, +4.9% over window 256; margin 1%)"},                                /* 4.9% faster: it wins */
        {{100e6, 102e6, 103e6}, 256, "window 128 100.00 M; window 256 102.00 M; window 512 103.00 M; picking window 256 (default; none >1% faster)",
         "window 256, code 256 (102.00 M u/s, the default: no other variant >1% faster; margin 1%)"},             /* under 1% faster: the default stays */
        {{100e6, 102e6, 104e6}, 512, "window 128 100.00 M; window 256 102.00 M; window 512 104.00 M; picking window 512",
         "window 512, code 512 (104.00 M u/s, +2.0% over window 256; margin 1%)"},                                /* 2% faster: it wins */
        {{108e6, 102e6, 107.5e6}, 512, "window 128 108.00 M; window 256 102.00 M; window 512 107.50 M; picking window 512",
         "window 512, code 512 (107.50 M u/s, +5.4% over window 256; margin 1%)"},                                /* 128 and 512 within 1%: the larger window */
        {{0, 0, 0}, 256, NULL, NULL},                                                                             /* nothing runs: the default, no table */
        {{100e6, 0, 0}, 128, "window 128 100.00 M; window 256 0.00; window 512 0.00; picking window 128",
         "window 128, code 128 (100.00 M u/s, the default window 256 cannot run; margin 1%)"},                    /* the default cannot run */
    };
    for (int c = 0; c < (int)(sizeof cases / sizeof *cases); c++) {
        gpu_session *s = fake_session("Test GPU");
        char nm[24];
        snprintf(nm, sizeof nm, "Test GPU %d", c);
        snprintf(s->info.name, sizeof s->info.name, "%s", nm);
        for (int i = 0; i < 3; i++) g_fake.rate[i] = cases[c].rate[i];
        g_fake.measured = 0;
        g_fake.check_ok = true;
        tune_ops op = fake_ops();
        fake_log lg;
        memset(&lg, 0, sizeof lg);
        rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
        rt_logseq_enter(q, 0);
        tune_pick(s, 3, &op);
        TCHK(g_fake.measured >= 3 && g_fake.measured <= 3 * TUNE_TURNS && s->variant == cases[c].picked);
        TCHK(lg.n >= 1 && !strcmp(lg.line[0], "GPU[3]: tuning 3 variants"));
        if (cases[c].table) {
            char want[300], want2[240];
            snprintf(want, sizeof want, "GPU[3]: table (u/s): %s", cases[c].table);
            snprintf(want2, sizeof want2, "GPU[3]: table: %s", cases[c].choice);
            const bool same = lg.n == 3 && !strcmp(lg.line[1], want) && !strcmp(lg.line[2], want2);
            TCHK(same);
            for (int i = 1; !same && i < lg.n; i++) printf("  GPU tuning: line %d is \"%s\"\n", i, lg.line[i]);
        } else {
            TCHK(lg.n == 1);                                              /* the linear kernel says nothing more when no window could run */
        }
        /* the next device of the same model takes the saved choice, and when nothing was saved it measures again */
        g_fake.measured = 0;
        s->variant = 0;
        lg.n = 0;
        tune_pick(s, 3, &op);
        if (cases[c].table) {
            char want[24];
            snprintf(want, sizeof want, "GPU[3]: cached %d", cases[c].picked);
            TCHK(g_fake.measured == 0 && s->variant == cases[c].picked && lg.n == 1 && !strcmp(lg.line[0], want));
        } else {
            TCHK(g_fake.measured == 3 && s->variant == 256);     /* nothing runs: one turn only */
        }
        rt_logseq_done(q, 0);
        rt_logseq_free(q);
        free(s);
    }
    {   /* a choice that fails the bit-exact test is not kept and not saved: the default, a warning, and the next device measures again */
        gpu_session *s = fake_session("Test GPU bad");
        g_fake.rate[0] = 100e6; g_fake.rate[1] = 100e6; g_fake.rate[2] = 120e6;
        g_fake.check_ok = false;
        g_fake.measured = 0;
        tune_ops op = fake_ops();
        fake_log lg;
        memset(&lg, 0, sizeof lg);
        rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
        rt_logseq_enter(q, 0);
        tune_pick(s, 0, &op);
        TCHK(s->variant == 256 && lg.n == 4 && !strcmp(lg.line[3], "WARN GPU[0]: variant 512 failed"));
        g_fake.measured = 0;
        tune_pick(s, 0, &op);
        TCHK(g_fake.measured >= 3);
        rt_logseq_done(q, 0);
        rt_logseq_free(q);
        free(s);
    }
    {   /* the turns and the median: window 512 is lucky in the first turn only and loses, window 128 is far behind and is measured once,
         * the default and window 512 three times each */
        static const double seq[3][3] = {{80e6, 80e6, 80e6}, {100e6, 101e6, 99e6}, {110e6, 99e6, 100e6}};
        gpu_session *s = fake_session("Test GPU turns");
        g_fake.seq = seq;
        memset(g_fake.nth, 0, sizeof g_fake.nth);
        g_fake.check_ok = true;
        g_fake.measured = 0;
        tune_ops op = fake_ops();
        fake_log lg;
        memset(&lg, 0, sizeof lg);
        rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
        rt_logseq_enter(q, 0);
        tune_pick(s, 2, &op);
        TCHK(s->variant == 256 && g_fake.measured == 7 && g_fake.nth[0] == 1 && g_fake.nth[1] == 3 && g_fake.nth[2] == 3);
        TCHK(lg.n == 3 && !strcmp(lg.line[1], "GPU[2]: table (u/s): window 128 80.00 M; window 256 100.00 M; window 512 100.00 M; picking window 256 (default; none >1% faster)"));
        g_fake.seq = NULL;
        rt_logseq_done(q, 0);
        rt_logseq_free(q);
        free(s);
    }
    {   /* with the warning the kangaroo's table has for a variant that cannot run, and for none that could */
        gpu_session *s = fake_session("Test GPU kang");
        g_fake.rate[0] = 0; g_fake.rate[1] = 0; g_fake.rate[2] = 0;
        g_fake.check_ok = true;
        tune_ops op = fake_ops();
        op.kang = true;
        op.no_rate = "cannot run";
        op.no_data_fmt = "%s: using the default %s";
        op.entry = "";
        fake_log lg;
        memset(&lg, 0, sizeof lg);
        rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
        rt_logseq_enter(q, 0);
        tune_pick(s, 1, &op);
        TCHK(s->variant == 256 && lg.n == 2 && !strcmp(lg.line[1], "WARN GPU[1]: table (u/s): 128 cannot run; 256 cannot run; 512 cannot run: using the default 256"));
        rt_logseq_done(q, 0);
        rt_logseq_free(q);
        free(s);
    }
    {   /* the wide kangaroo herds: offered where they add work-items, with at most 128 per compute unit on an Intel GPU and at most as many
         * walkers as the default CUDA herd, and only as the variants the tuner measures */
        static const struct { rt_api api; unsigned cu; unsigned gib; const char *vendor; bool gpu; int nwide; int probe; size_t threads; } wc[] = {
            {RT_CUDA, 128, 24, "NVIDIA", true, 1, 64 | KV_WIDEFLAG(KV_WIDE_FULL), 65536},       /* 128 SMs: 512 per SM past the cap of 32768 */
            {RT_CUDA, 20, 4, "NVIDIA", true, 0, 0, 0},                                            /* the cap does not bind */
            {RT_OPENCL, 96, 24, "Advanced Micro Devices, Inc.", true, 8, 64 | KV_WIDEFLAG(KV_WIDE_X8), 49152},
            {RT_OPENCL, 96, 8, "Intel(R) Corporation", true, 4, 64 | KV_WIDEFLAG(KV_WIDE_X4), 12288},   /* 128 per EU: no 8x herd */
            {RT_OPENCL, 4, 4, "GenuineIntel", false, 8, 32 | 1 << 16 | KV_WIDEFLAG(KV_WIDE_X8), 2048},   /* a CPU device is not bounded like an Intel GPU */
            {RT_OPENCL, 96, 1, "Advanced Micro Devices, Inc.", true, 4, 32 | KV_WIDEFLAG(KV_WIDE_X4), 21845},   /* 1 GiB: the memory bounds the herds */
        };
        for (size_t c = 0; c < sizeof wc / sizeof *wc; c++) {
            rt_dev d;
            memset(&d, 0, sizeof d);
            d.api = wc[c].api;
            d.cu = wc[c].cu;
            d.gmem = (u64)wc[c].gib << 30;
            d.is_gpu = wc[c].gpu;
            pz_strlcpy(d.vendor, wc[c].vendor, sizeof d.vendor);
            int vs[32], nwide = 0;
            bool probe = false;
            const int n = gpu_kvariants_of(&d, vs);
            for (int i = 0; i < n; i++) { nwide += KV_WIDE(vs[i]) != 0; probe |= vs[i] == wc[c].probe; }
            TCHK(nwide == wc[c].nwide && (!wc[c].probe || probe));
            if (wc[c].probe) TCHK(kang_threads_for(&d, KV_NK(wc[c].probe), false, KV_WIDE(wc[c].probe), false) == wc[c].threads);
            if (nwide != wc[c].nwide) printf("  GPU tuning: case %zu offers %d wide herds\n", c, nwide);
        }
        char nb[64];
        TCHK(!strcmp(kvariant_name(64 | 1 << 16 | KV_WIDEFLAG(KV_WIDE_X4), nb, sizeof nb), "NK 64 pairs, 4x herd"));
        TCHK(!strcmp(kvariant_name(64 | KV_WIDEFLAG(KV_WIDE_FULL), nb, sizeof nb), "NK 64, full herd"));
        TCHK(!strcmp(kvariant_name(4 | KV_BLOCK << 16, nb, sizeof nb), "NK 4 block herd"));
        TCHK(!strcmp(kvariant_name(128 | 2 << 16 | KV_METAFLAG, nb, sizeof nb), "NK 128 quads, tags once"));
        TCHK(!strcmp(kvariant_name(64 | KV_METAFLAG | KV_L2FLAG, nb, sizeof nb), "NK 64, tags once, L2 herd"));
        TCHK(!strcmp(kvariant_code(128 | 2 << 16 | KV_METAFLAG, nb, sizeof nb), "128cm") && !strcmp(kvariant_code(64 | 1 << 16 | KV_L2FLAG, nb, sizeof nb), "64bl"));
        TCHK(!strcmp(kvariant_code(64 | KV_METAFLAG | KV_L2FLAG, nb, sizeof nb), "64ml") && !strcmp(kvariant_code(64 | KV_WIDEFLAG(KV_WIDE_FULL), nb, sizeof nb), "-"));
        /* the block herd: never tuned; two blocks of 128 work-items per SM with CUDA */
        for (int api = 0; api < 2; api++) {
            rt_dev d;
            memset(&d, 0, sizeof d);
            d.api = api ? RT_OPENCL : RT_CUDA;
            d.cu = 20;
            d.gmem = (u64)4 << 30;
            d.is_gpu = true;
            pz_strlcpy(d.vendor, "NVIDIA", sizeof d.vendor);
            int vs[32], nblock = 0;
            const int n = gpu_kvariants_of(&d, vs);
            for (int i = 0; i < n; i++) nblock += KV_CFG(vs[i]) == KV_BLOCK;
            TCHK(nblock == 0);                   /* the block herd is not tuned (slower on the A2000), only forced */
            TCHK(kang_threads_for(&d, 4, false, 0, true) == (api ? 20u * KANG_GROUP : 20u * KANG_BLOCK_ITEMS_CU));
        }
    }
    {   /* reading the saved choices: the last line of the key counts, other keys and an empty value do not disturb it */
        gpu_session *s = fake_session("Test GPU file");
        char path[512], key[400], text[1700];
        TCHK(tune_cache_path(path, sizeof path));
        tune_key(&s->info, false, key, sizeof key);
        snprintf(text, sizeof text, "%sx=128\n%s=128\nother=64\n%s=512\n", key, key, key);
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, NULL) == 512);
        snprintf(text, sizeof text, "%s=512\n%s=\n128\n", key, key);           /* the last line has no value: the digits below it are another line */
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, NULL) == 0);
        snprintf(text, sizeof text, "%s=300\n", key);                            /* a window this device does not offer */
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, NULL) == 0);
        snprintf(text, sizeof text, "%s=256", key);                               /* no newline at the end */
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, NULL) == 256);
        /* the length of a launch after the variant; a line without it (the files of 1.19.x), or with one out of range, means 0.35 s */
        double ls = 0;
        snprintf(text, sizeof text, "%s=512 0.70\n", key);
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - 0.7) < 1e-9);
        snprintf(text, sizeof text, "%s=512\n", key);
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
        snprintf(text, sizeof text, "%s=512 7\n%s=512 0.0\n", key, key);
        TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
        TCHK(tune_value("", &ls) == 0 && tune_value("\n512 0.7", &ls) == 0 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
        free(s);
    }
    {   /* the length of a launch: the shortest within 1% of the best; a length the device cannot reach (the same size) is not tried; saved
         * with the variant and read back by the next device of the model */
        static const struct { double rate[4]; uint32_t size[4]; double picked; int calls; } lc[] = {
            {{95e6, 100e6, 100.5e6, 101e6}, {1, 2, 4, 8}, 0.35, 4},            /* 0.35 s is within 1% of the best 1.4 s */
            {{97e6, 99e6, 101e6, 101e6}, {1, 2, 4, 8}, 0.7, 4},                /* 0.7 s: the first within 1% */
            {{100e6, 100e6, 100e6, 100e6}, {1, 1, 1, 1}, 0.15, 4},             /* a launch of one group lasts longer than every target: only the first is measured */
            {{0, 0, 0, 0}, {1, 2, 4, 8}, GPU_LAUNCH_S, 4},                     /* nothing could be measured: the default */
            {{90e6, 100e6, 100e6, 110e6}, {1, 2, 2, 8}, 1.4, 4},               /* 0.7 s gets the size of 0.35 s and is not tried; 1.4 s is 10% better */
        };
        for (size_t c = 0; c < sizeof lc / sizeof *lc; c++) {
            gpu_session *s = fake_session("Test GPU length");
            snprintf(s->info.name, sizeof s->info.name, "Test GPU length %zu", c);
            g_fake.rate[0] = 100e6; g_fake.rate[1] = 100e6; g_fake.rate[2] = 120e6;
            g_fake.check_ok = true;
            g_fake.len_calls = 0;
            for (int i = 0; i < 4; i++) { g_fake.len_rate[i] = lc[c].rate[i]; g_fake.len_size[i] = lc[c].size[i]; }
            tune_ops op = fake_ops();
            op.at_length = fake_at_length;
            fake_log lg;
            memset(&lg, 0, sizeof lg);
            rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
            rt_logseq_enter(q, 0);
            tune_pick(s, 4, &op);
            TCHK(s->variant == 512 && fabs(s->launch_s - lc[c].picked) < 1e-9 && g_fake.len_calls == lc[c].calls);
            if (fabs(s->launch_s - lc[c].picked) >= 1e-9) printf("  GPU tuning: length case %zu picked %.2f s\n", c, s->launch_s);
            char want[80];
            snprintf(want, sizeof want, "GPU[4]: launches of %.2f s (", lc[c].picked);
            TCHK(lc[c].rate[0] <= 0 || (lg.n == 5 && !strncmp(lg.line[4], want, strlen(want))));
            if (lc[c].rate[0] <= 0) TCHK(lg.n == 4 && !strncmp(lg.line[3], "WARN GPU[4]: launch length (u/s; the size in groups): 0.15 s cannot run;", 72));
            s->launch_s = 0;
            s->variant = 0;
            g_fake.measured = 0;
            tune_pick(s, 4, &op);                                     /* the next device of the model: the saved variant and length */
            TCHK(g_fake.measured == 0 && s->variant == 512 && fabs(s->launch_s - lc[c].picked) < 1e-9);
            rt_logseq_done(q, 0);
            rt_logseq_free(q);
            free(s);
        }
        const double a[4] = {100, 100.5, 101, 101.5}, b[4] = {-1, 0, 100, 99.5}, z[4] = {0, 0, -1, 0};
        TCHK(tune_launch_choose(a, 4) == 1 && tune_launch_choose(b, 4) == 2 && tune_launch_choose(z, 4) == -1);   /* 100.5 is within 1% of 101.5, 100 is not */
    }
    pz_strlcpy(g_env.xdg_cache_home, old, sizeof g_env.xdg_cache_home);
    pz_remove_tree(tmp);
    return fails;
}

void gpu_doctor_cache(void)
{
    char path[512];
    if (!pz_cache_file("gpu_tune.txt", path, sizeof path)) return;
    char *t = pz_read_file(path, NULL);
    int cnt = 0;
    const size_t pz_vl = strlen(PZ_VERSION);
    char *eq;
    for (char *p = t, *line; (line = pz_keyed_line(&p, &eq));) {
        if (eq && (size_t)(eq - line) > pz_vl && !strncmp(eq - pz_vl - 1, "|" PZ_VERSION, pz_vl + 1)) {
            /* "[kang|]API|NAME|VERSION|CU|PZVERSION=VARIANT" (tune_key) -> "NAME (API, VERSION): kangaroo NK 128" */
            const bool kang = !strncmp(line, "kang|", 5);
            char key[400], nb[64];
            pz_strlcpy(key, kang ? line + 5 : line, sizeof key);
            char *f[4] = {key, NULL, NULL, NULL};
            for (int k = 1; k < 4 && f[k - 1]; k++) {
                char *bar = strchr(f[k - 1], '|');
                if (bar) { *bar = 0; f[k] = bar + 1; }
            }
            double ls;
            const int v = tune_value(eq + 1, &ls);
            if (f[3]) {
                doctor_line('i', "saved GPU tuning: %.80s (%s, %.40s): %s %s, launches of %.2f s", f[1], f[0], f[2], kang ? "kangaroo" : "linear window",
                            kang ? kvariant_name(v, nb, sizeof nb) : variant_name(v, nb, sizeof nb), ls);
            } else doctor_line('i', "saved GPU tuning: %.150s", line);
            cnt++;
        }
    }
    free(t);
    if (!cnt) doctor_line('i', "no GPU kernel tuning saved for version %s: it is done the first time each GPU is used (20-60 s, one time only)", PZ_VERSION);
}
