/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_tune.c - the compile-time variants of the GPU kernels, the choice of the fastest for each device (measured once, remembered
 * on disk) and the size of the kangaroo herd. The benchmark is in gpu_bench.c, the self-test of the choice in gpu_tune_selftest.c. */
#include "gpu_tune_priv.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#include "doctor.h"

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
size_t gpu_tune_kang_threads_for(const rt_dev *d, int nk, bool l2fit, int wide, bool block)
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
            const size_t tw = gpu_tune_kang_threads_for(d, nk, false, w, false);
            if (tw * 4 < gpu_tune_kang_threads_for(d, nk, false, 0, false) * 5) continue;                       /* not 25% more work-items than the default herd */
            if (w == KV_WIDE_X8 && tw * 4 < gpu_tune_kang_threads_for(d, nk, false, KV_WIDE_X4, false) * 5) continue;   /* nor than the 4x herd */
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
int gpu_tune_default_kvariant(const rt_dev *d) { return d->api == RT_CUDA ? 128 : 64; }
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
char *gpu_kvariant_name(int v, char *b, size_t n)
{
    static const char *const wide[] = {"", ", 4x herd", ", 8x herd", ", full herd"};
    static const char *const cfg[] = {"", " pairs", " quads", " block herd"};
    snprintf(b, n, "NK %d%s%s%s%s%s", KV_NK(v), cfg[KV_CFG(v) & 3], KV_META(v) ? ", tags once" : "", KV_L2(v) && (KV_CFG(v) || KV_META(v)) ? "," : "",
             KV_L2(v) ? " L2 herd" : "", wide[KV_WIDE(v)]);
    return b;
}
char *gpu_variant_name(int v, char *b, size_t n)
{
    snprintf(b, n, "%d%s", VAR_HALF(v), VAR_MINB(v) ? ", register cap" : "");
    return b;
}

int gpu_tune_kang_code(const gpu_session *s) { return s->kvar | (s->l2fit ? KV_L2FLAG : 0) | KV_WIDEFLAG(s->wide); }   /* what the tuner remembers */

/* ---------------- autotune ---------------- */

static bool g_retune;       /* 'bench --retune': ignore the saved choice, measure again and save the new one */
void gpu_set_retune(bool on) { g_retune = on; }
bool gpu_retune_requested(void) { return g_retune; }

bool gpu_tune_cache_path(char *out, size_t n) { return pz_cache_file("gpu_tune.txt", out, n); }

void gpu_tune_key(const rt_dev *d, bool kang, char *out, size_t n)
{
    snprintf(out, n, "%s%s|%s|%s|%u|%s", kang ? "kang|" : "", rt_api_name(d->api), d->name, d->version, (unsigned)d->cu, PZ_VERSION);
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '=') *c = '_';
}

/* The value of a line of gpu_tune.txt: the variant, then (since the launch length is tuned) a blank and the length of a launch in seconds;
 * a line without it, or with one out of range, keeps GPU_LAUNCH_S. The variant is 0 when the value is empty (pz_atoi skips a newline: an empty
 * value must not read the next line). */
int gpu_tune_value(const char *e, double *launch_s)
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
int gpu_tune_cache_get(const rt_dev *d, bool kang, double *launch_s)
{
    char path[512], key[400];
    if (launch_s) *launch_s = GPU_LAUNCH_S;
    if (!gpu_tune_cache_path(path, sizeof path)) return 0;
    char *t = pz_read_file(path, NULL);
    if (!t) return 0;
    gpu_tune_key(d, kang, key, sizeof key);
    const char *e = pz_kv_find(t, key, '=', PZ_KV_LAST);        /* the last line of the key wins */
    double ls;
    const int v = gpu_tune_value(e, &ls);
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

void gpu_tune_cache_put(const rt_dev *d, bool kang, int variant, double launch_s)
{
    char path[512], key[400], val[32];
    if (!gpu_tune_cache_path(path, sizeof path)) return;
    gpu_tune_key(d, kang, key, sizeof key);
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
    gpu_tune_key(d, kang, key, sizeof key);
    int v;
    bool told = false;
    pthread_mutex_lock(&g_tune_mu);
    for (;;) {
        v = gpu_tune_cache_get(d, kang, launch_s);
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
    gpu_tune_key(d, kang, key, sizeof key);
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
bool gpu_tune_linear_probe(gpu_session *s, pz_u256 *start, uint32_t ng, lin_launch *L, double *dt)
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
        if (!gpu_tune_linear_probe(s, &start, ng, &L, &dt) || L.err) return 0;
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
void gpu_tune_linear_set_variant(gpu_session *s, int v) { s->variant = v; }
int gpu_tune_linear_code(const gpu_session *s) { return s->variant; }
char *gpu_variant_code(int v, char *b, size_t n) { snprintf(b, n, "%d", v); return b; }      /* what PZ_GPU_HALF takes */

/* Work-items of the kangaroo kernel: CUDA keeps 512 threads per SM resident (126 registers: four blocks of 128),
 * OpenCL the historical 64 per compute unit, unless the tuner chose a herd sized to the L2 cache, a wide one or a block herd (gpu_tune_kang_threads_for). */
size_t gpu_kang_threads(const gpu_session *s) { return gpu_tune_kang_threads_for(&s->info, s->NK, s->l2fit, s->wide, KV_CFG(s->kvar) == KV_BLOCK); }

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
double gpu_tune_kang_measure(gpu_session *s)
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

void gpu_tune_kang_set_variant(gpu_session *s, int v)
{
    s->kvar = KV_KERNEL(v);
    s->NK = KV_NK(v);
    s->l2fit = KV_L2(v) != 0;
    s->wide = KV_WIDE(v);
}

static double kang_measure_variant(gpu_session *s, int v)
{
    gpu_tune_kang_set_variant(s, v);
    return gpu_tune_kang_measure(s);
}

/* What PZ_GPU_NK takes for a variant ("128b", "128cm", "64l", "4g"); "-" for a herd it cannot force (the wide herds have no suffix). */
char *gpu_kvariant_code(int v, char *b, size_t n)
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
    if (!gpu_tune_linear_probe(s, &c->start, size, &L, dt) || L.err) return false;
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
int gpu_tune_launch_choose(const double *rate, int n)
{
    int best = -1;
    for (int i = 0; i < n; i++) if (rate[i] > 0 && (best < 0 || rate[i] > rate[best])) best = i;
    if (best < 0) return -1;
    for (int i = 0; i < n; i++) if (rate[i] > 0 && rate[i] >= rate[best] * GPU_LAUNCH_NEAR) return i;
    return best;
}

/* ---------------- the choice of a variant ---------------- */

/* The median of n rates, 0 when any of them is 0 (a variant that failed once is not trusted) or there is none. */
double gpu_tune_median(const double *s, int n)
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

int gpu_tune_choose(const tune_ops *op, const double *rate, int nv, int di)
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
void gpu_tune_turns(gpu_session *s, const tune_ops *op, int turns, bool cut, int di, double smp[][TUNE_TURNS_MAX], int *nsmp)
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

/* The length of a launch for the variant that is set (the rule of gpu_tune_launch_choose), measured at each of GPU_LAUNCH_TRY seconds and logged
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
    const int pick = gpu_tune_launch_choose(rate, n);
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
    gpu_tune_turns(s, op, TUNE_TURNS, true, di, smp, nsmp);
    for (int i = 0; i < op->nv; i++) rate[i] = gpu_tune_median(smp[i], nsmp[i]);
    const int best = gpu_tune_choose(op, rate, op->nv, di);
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
    gpu_tune_cache_put(&s->info, op->kang, op->code(s), s->launch_s);
}

/* The saved choice if there is one (or the one that another device of the same model is measuring), else the measurement. */
void gpu_tune_pick(gpu_session *s, int idx, const tune_ops *op)
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
    if (kang) snprintf(buf, n, "kangaroo %s", gpu_kvariant_name(gpu_tune_kang_code(s), nb, sizeof nb));
    else snprintf(buf, n, "window %s", gpu_variant_name(s->variant, nb, sizeof nb));
    return buf;
}

/* PZ_GPU_TUNE=0: no tuning; =1: also the devices that are not GPUs are tuned (for tests). */
static bool tune_off(const rt_dev *d)
{
    return g_env.gpu_tune == PZ_ENV_TUNE_OFF || !(d->is_gpu || g_env.gpu_tune == PZ_ENV_TUNE_ALL);
}

/* The tuning of each kernel on device d: its variants and how they are measured, checked, named and logged. */
void gpu_tune_linear_ops(const rt_dev *d, tune_ops *op)
{
    *op = (tune_ops){.kang = false, .def = gpu_default_variant(d), .measure = linear_measure_variant, .warm_s = d->is_gpu ? TUNE_WARM_S : 0,
                     .set = gpu_tune_linear_set_variant, .check = gpu_test_linear, .finish = gpu_linear_release_program, .code = gpu_tune_linear_code,
                     .name = gpu_variant_name, .code_text = gpu_variant_code, .at_length = linear_at_length, .size_unit = "groups per work-item",
                     .cached_fmt = "GPU[%d]: kernel with window %s (from earlier tuning)",
                     .intro_fmt = "GPU[%d]: tuning the kernel (one time only: %d variants, the driver compiles them the first time)...",
                     .head = "linear kernel tuning", .unit = "keys/s", .entry = "window ",
                     .bad_check_fmt = "GPU[%d]: variant %s failed the correctness check: using the default and not saving the choice"};
    const int *vs;
    op->nv = gpu_variants_of(d, &vs);
    memcpy(op->vs, vs, (size_t)op->nv * sizeof *vs);
}

void gpu_tune_kang_ops(const rt_dev *d, tune_ops *op)
{
    *op = (tune_ops){.kang = true, .def = gpu_tune_default_kvariant(d), .measure = kang_measure_variant, .warm_s = d->is_gpu ? TUNE_WARM_S : 0,
                     .set = gpu_tune_kang_set_variant, .check = gpu_test_kang, .code = gpu_tune_kang_code, .name = gpu_kvariant_name, .code_text = gpu_kvariant_code,
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
    gpu_tune_linear_ops(d, &op);
    gpu_tune_pick(s, idx, &op);
}

/* Chooses the kangaroo kernel variant for this device (cached on disk like the linear variant) and leaves it in s. */
void gpu_kang_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    char nb[64];
    gpu_tune_kang_set_variant(s, gpu_tune_default_kvariant(d));
    s->launch_s = GPU_LAUNCH_S;
    if (g_env.gpu_nk) {
        const int v = g_env.gpu_nk | g_env.gpu_nk_cfg << 16 | (g_env.gpu_nk_meta ? KV_METAFLAG : 0) | (g_env.gpu_nk_l2 ? KV_L2FLAG : 0);
        if (forcible_kvariant(d, v)) { gpu_tune_kang_set_variant(s, v); rt_log(PZ_LOG_INFO, "GPU[%d]: kangaroo variant set by PZ_GPU_NK: %s", idx, gpu_kvariant_name(v, nb, sizeof nb)); }
        else rt_log(PZ_LOG_WARN, "GPU[%d]: PZ_GPU_NK=%s ignored (allowed values with %s: %s)", idx, g_env.gpu_nk_text, rt_api_name(d->api),
                    d->api == RT_CUDA ? "64, 128, 256, with suffix b; 64 and 128 also with c; 64 and 128, plain, b or c, with m after it (tags once); 4g (block herd)"
                                      : "32, 64, 128, with suffix b, then m if wanted (tags once); with an L2 of at least 8 MiB also 16 and 32 and the suffix l (herd in L2); 4g (block herd)");
        return;
    }
    if (tune_off(d)) return;
    tune_ops op;
    gpu_tune_kang_ops(d, &op);
    gpu_tune_pick(s, idx, &op);
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
            /* "[kang|]API|NAME|VERSION|CU|PZVERSION=VARIANT" (gpu_tune_key) -> "NAME (API, VERSION): kangaroo NK 128" */
            const bool kang = !strncmp(line, "kang|", 5);
            char key[400], nb[64];
            pz_strlcpy(key, kang ? line + 5 : line, sizeof key);
            char *f[4] = {key, NULL, NULL, NULL};
            for (int k = 1; k < 4 && f[k - 1]; k++) {
                char *bar = strchr(f[k - 1], '|');
                if (bar) { *bar = 0; f[k] = bar + 1; }
            }
            double ls;
            const int v = gpu_tune_value(eq + 1, &ls);
            if (f[3]) {
                doctor_line('i', "saved GPU tuning: %.80s (%s, %.40s): %s %s, launches of %.2f s", f[1], f[0], f[2], kang ? "kangaroo" : "linear window",
                            kang ? gpu_kvariant_name(v, nb, sizeof nb) : gpu_variant_name(v, nb, sizeof nb), ls);
            } else doctor_line('i', "saved GPU tuning: %.150s", line);
            cnt++;
        }
    }
    free(t);
    if (!cnt) doctor_line('i', "no GPU kernel tuning saved for version %s: it is done the first time each GPU is used (20-60 s, one time only)", PZ_VERSION);
}
