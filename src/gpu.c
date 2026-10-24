/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu.c - GPU support: device selection, self-test, autotuning and the worker threads for linear search and the
 * kangaroo. Written against the runtime interface of gpu_rt.h, so the same code drives CUDA and OpenCL devices. */
#include "gpu.h"
#include "doctor.h"
#include "gpu_rt.h"
#include "platform.h"
#include "cpu_engine.h"
#include "hash.h"
#include "kangaroo_priv.h"
#include <unistd.h>
#include <time.h>
#include <math.h>

/* Compile-time variants of the linear kernel: half-window (group = 2*half keys) | minimum resident blocks per SM << 16
 * (CUDA only: 4 caps the kernel at 128 registers, trading a few spills for more resident warps). The autotuner measures
 * them on the device and keeps the fastest. */
#define VAR_HALF(v) ((v) & 0xFFFF)
#define VAR_MINB(v) ((v) >> 16)
static const int CL_VARIANTS[] = {64, 128, 256, 512};
static const int CU_VARIANTS[] = {128, 256, 512, 128 | 4 << 16, 256 | 4 << 16, 512 | 4 << 16};
/* Kangaroo kernel variants: kangaroos per work-item (NK: one field inversion is shared by NK steps, so larger is cheaper
 * in arithmetic but needs more private memory) | configuration << 16: 0 keeps the running product of the batch inversion
 * for every walker, 1 only for every other one (half the traffic for those products, half a field multiplication more per
 * step). Measured on the device like the linear variants. */
#define KV_NK(v) ((v) & 0xFFFF)
#define KV_CFG(v) (((v) >> 16) & 0xF)
#define KV_L2(v) (((v) >> 24) & 1)            /* the herd is sized to fit the L2 cache (fewer work-items), see kang_l2_threads */
#define KV_KERNEL(v) ((v) & 0xFFFFF)          /* NK | cfg << 16: what the kernel build takes */
#define KV_L2FLAG (1 << 24)
static const int CL_KVARIANTS[] = {32, 64, 128, 32 | 1 << 16, 64 | 1 << 16, 128 | 1 << 16};
static const int CU_KVARIANTS[] = {64, 128, 256, 64 | 1 << 16, 128 | 1 << 16, 256 | 1 << 16, 64 | 2 << 16, 128 | 2 << 16};
/* GPUs with a large last-level cache (RTX 30/40 series: 6-72 MiB; some laptop GPUs have 2) can keep a herd of some
 * hundred thousand walkers (about 130 bytes each in the work-items' private arrays) on the chip, where the traffic that
 * limits the kernel stays in the cache instead of going to the video memory. */
#define KANG_L2_MIN (8u << 20)
#define KANG_WALKER_BYTES 128
static const int KV_L2_NK[] = {16, 32, 64};
/* kernels built with products every fourth walker that the self-test checks on every device (the OpenCL tuner does not offer them) */
static const int KV_MC4_NK[] = {64, 128};

static int variants_of(const rt_dev *d, const int **v)
{
    if (d->api == RT_CUDA) { *v = CU_VARIANTS; return (int)(sizeof CU_VARIANTS / sizeof *CU_VARIANTS); }
    *v = CL_VARIANTS;
    return (int)(sizeof CL_VARIANTS / sizeof *CL_VARIANTS);
}
static int default_variant(const rt_dev *d) { return d->api == RT_CUDA ? 256 : 128; }
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
static int kvariants_of(const rt_dev *d, int *out)
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
    int n = kvariants_of(d, vs);
    for (int i = 0; i < n; i++) if (vs[i] == v) return true;
    return false;
}
static bool valid_variant(const rt_dev *d, int v)
{
    const int *vs;
    int n = variants_of(d, &vs);
    for (int i = 0; i < n; i++) if (vs[i] == v) return true;
    return false;
}
static char *kvariant_name(int v, char *b, size_t n)
{
    snprintf(b, n, "NK %d%s%s", KV_NK(v), KV_CFG(v) == 1 ? " (pair products)" : KV_CFG(v) == 2 ? " (quad products)" : "", KV_L2(v) ? " (herd in L2)" : "");
    return b;
}
static char *variant_name(int v, char *b, size_t n)
{
    if (VAR_MINB(v)) snprintf(b, n, "%d (max 128 registers)", VAR_HALF(v));
    else snprintf(b, n, "%d", VAR_HALF(v));
    return b;
}

struct gpu_session {
    run_ctx *r;
    rt_dev info;
    /* owned by the launcher and the thread, before `c`: session_release() clears everything after it and a recovery keeps these */
    int widx;
    pthread_t th;
    _Atomic int *active;
    bool started;
    kang_state *ks;
    int limit_pct;               /* --limit, set by the launcher once the kernels are tuned: the linear kernel runs with that share of its work-items */
    rt_ctx *c;
    rt_mod *prog;
    rt_kern *kern;
    rt_buf *b_prm, *b_gtab, *b_hits;
    size_t T_max;
    int half;                    /* compile-time HALF of the linear kernel (group = 2*half keys) */
    int variant;                 /* VAR_* code of the prepared / chosen linear kernel (0 = not chosen yet) */
    /* kangaroo */
    rt_mod *kprog;
    rt_kern *kstep;
    rt_buf *b_state, *b_jump, *b_dps, *b_flags, *b_rc, *b_rl, *b_hist;
    int T, NK, kvar;             /* kangaroo kernel: walkers per work-item, kernel variant (NK | cfg << 16) */
    bool l2fit;                  /* the herd is sized to the L2 cache (see kang_l2_threads_for) */
    u64 dps_cap, rl_cap;
    uint32_t hist_rounds;        /* depth of the jump history: the most rounds a launch can run */
    u64 gstep;                   /* steps taken by every walker so far (the step number of the next round) */
    size_t mem_used;             /* bytes of device buffers of the last kangaroo preparation */
    _Atomic int paused;          /* the kangaroo thread idles ('bench --together' switches GPUs off to measure them) */
};

static int kang_code(const gpu_session *s) { return s->kvar | (s->l2fit ? KV_L2FLAG : 0); }   /* what the tuner remembers */

/* ---------------- device list and selection ---------------- */

/* "4.00 GiB", or for a GPU that shares the system RAM "59.58 GiB, shared system RAM (limit reported by the driver)". */
static const char *fmt_devmem(const rt_dev *d, char *out, size_t n)
{
    char t[32];
    pz_fmt_bytes((double)d->gmem, t, sizeof t);
    if (d->unified) snprintf(out, n, "%s, shared system RAM (limit reported by the driver)", t);
    else snprintf(out, n, "%s", t);
    return out;
}

/* "44.08 MiB of 4.00 GiB", or "44.08 MiB of system RAM". */
static const char *fmt_devused(const rt_dev *d, size_t used, char *out, size_t n)
{
    char u[32], t[32];
    pz_fmt_bytes((double)used, u, sizeof u);
    if (d->unified) snprintf(out, n, "%s of system RAM", u);
    else snprintf(out, n, "%s of %s", u, pz_fmt_bytes((double)d->gmem, t, sizeof t));
    return out;
}

static gpu_api_pref g_pref = GPU_API_AUTO;
static bool g_pref_from_env;

void gpu_set_api_pref(gpu_api_pref p) { g_pref = p; g_pref_from_env = true; }

static gpu_api_pref api_pref(void)
{
    if (!g_pref_from_env) {
        const char *e = getenv("PZ_GPU_API");
        if (e && !strcmp(e, "cuda")) g_pref = GPU_API_CUDA;
        else if (e && !strcmp(e, "opencl")) g_pref = GPU_API_OPENCL;
        g_pref_from_env = true;
    }
    return g_pref;
}

static int enumerate(rt_dev *devs, char *note, size_t nl) { return rt_enumerate(devs, RT_MAX_DEVS, note, nl); }

/* Devices used when --gpu is not given: every GPU once (through CUDA when both runtimes see it). */
static bool default_choice(const rt_dev *d)
{
    if (!d->is_gpu) return false;
    switch (api_pref()) {
    case GPU_API_CUDA: return d->api == RT_CUDA;
    case GPU_API_OPENCL: return d->api == RT_OPENCL;
    default: return d->api == RT_CUDA || d->dup_of < 0;
    }
}

static bool in_filter(const char *f, int idx)
{
    char tmp[128];
    pz_strlcpy(tmp, f, sizeof tmp);
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ","))
        if (pz_atoi(t) == idx) return true;
    return false;
}

int gpu_list_devices(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    if (!n) { printf("  no device (%s): CPU only\n", note[0] ? note : "no GPU runtime"); return 0; }
    for (int i = 0; i < n; i++) {
        const rt_dev *d = &devs[i];
        char mem[160], extra[96] = "";
        if (d->api == RT_CUDA) snprintf(extra, sizeof extra, ", compute %d.%d", d->cc / 10, d->cc % 10);
        const char *use = default_choice(d) ? "used by default"
                        : d->dup_of >= 0 ? "same GPU as [%d] via OpenCL: fallback if CUDA fails, or with --gpu N"
                                         : "select with --gpu N";
        char usebuf[120];
        snprintf(usebuf, sizeof usebuf, use, d->dup_of);
        printf("  [%d] %-6s | %s | %s | %u %s @ %u MHz%s | %s  (%s)\n", i, rt_api_name(d->api), d->name, d->is_gpu ? "GPU" : "CPU", d->cu,
               d->api == RT_CUDA ? "SM" : "CU", d->mhz, extra, fmt_devmem(d, mem, sizeof mem), usebuf);
    }
    if (note[0]) printf("  (%s)\n", note);
    return n;
}

void gpu_signature(char *out, size_t n)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int cnt = enumerate(devs, note, sizeof note);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < cnt && o + 8 < n; i++) {
        if (!default_choice(&devs[i])) continue;
        o += (size_t)snprintf(out + o, n - o, "%s%s:%s:%u:%s", o ? ";" : "", rt_api_name(devs[i].api), devs[i].name, devs[i].cu, devs[i].plat_name);
    }
}

int gpu_count_usable(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note), g = 0;
    for (int i = 0; i < n; i++) g += default_choice(&devs[i]);
    return g;
}

/* ---------------- helpers ---------------- */

static void pack_fe(uint32_t out[8], const fe *f)
{
    fe t = *f;
    fe_normalize(&t);
    for (int i = 0; i < 4; i++) { out[2 * i] = (uint32_t)t.n[i]; out[2 * i + 1] = (uint32_t)(t.n[i] >> 32); }
}
static void unpack_fe(fe *f, const uint32_t in[8])
{
    for (int i = 0; i < 4; i++) f->n[i] = (u64)in[2 * i] | (u64)in[2 * i + 1] << 32;
}
static void pack_ge(uint32_t out[16], const ge *p) { pack_fe(out, &p->x); pack_fe(out + 8, &p->y); }

static u64 g_rs = 0x9E3779B97F4A7C15ULL;
static u64 grnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return g_rs; }

static void session_release(gpu_session *s)
{
    rt_buf *bufs[] = {s->b_prm, s->b_gtab, s->b_hits, s->b_state, s->b_jump, s->b_dps, s->b_flags, s->b_rc, s->b_rl, s->b_hist};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i++) rt_buf_free(bufs[i]);
    rt_kern_free(s->kern);
    rt_kern_free(s->kstep);
    rt_mod_free(s->prog);
    rt_mod_free(s->kprog);
    rt_close(s->c);
    memset(&s->c, 0, sizeof *s - offsetof(gpu_session, c));
}

static bool session_open(gpu_session *s, const rt_dev *d)
{
    s->info = *d;
    s->c = rt_open(d);
    return s->c != NULL;
}

static void make_gtab(uint32_t *tab, int half)
{
    ge *g = pz_xmalloc(sizeof(ge) * (size_t)half);
    cpu_gmultiples(g, half);
    for (int i = 0; i < half; i++) pack_ge(tab + i * 16, &g[i]);
    free(g);
}

/* ---------------- linear program ---------------- */

static void linear_release_program(gpu_session *s)
{
    rt_buf_free(s->b_prm);
    rt_buf_free(s->b_gtab);
    rt_buf_free(s->b_hits);
    rt_kern_free(s->kern);
    rt_mod_free(s->prog);
    s->b_prm = s->b_gtab = s->b_hits = NULL;
    s->kern = NULL;
    s->prog = NULL;
}

static bool linear_prepare_variant(gpu_session *s, int variant)
{
    linear_release_program(s);
    const int half = VAR_HALF(variant);
    s->variant = variant;
    s->half = half;
    s->prog = rt_build(s->c, RT_PROG_LINEAR, variant);
    if (!s->prog) return false;
    s->kern = rt_kernel(s->prog, "pz_search", 4);
    if (!s->kern) return false;
    uint32_t *tab = pz_xcalloc((size_t)half * 16, 4);
    make_gtab(tab, half);
    s->b_gtab = rt_buf_new(s->c, (size_t)half * 16 * 4, tab);
    free(tab);
    s->b_prm = rt_buf_new(s->c, 64 * 4, NULL);
    s->b_hits = rt_buf_new(s->c, (2 + 2 * 64) * 4, NULL);
    if (!s->b_gtab || !s->b_prm || !s->b_hits) return false;
    /* thread budget: compute units x 256, bounded by the private scratch of a work-item (two arrays of half+1 field
     * elements plus slack) in a quarter of the device memory */
    size_t item_bytes = (size_t)(half + 1) * 64 + 4096;
    size_t t = (size_t)s->info.cu * 256;
    size_t by_mem = (size_t)(s->info.gmem / 4 / item_bytes);
    if (t > by_mem) t = by_mem;
    if (t < 64) t = 64;
    if (t > (1u << 20)) t = 1u << 20;
    s->T_max = run_limit_items(t, s->limit_pct, 128);     /* a recovery after a device error keeps the limit */
    return true;
}

static bool linear_prepare(gpu_session *s) { return linear_prepare_variant(s, s->variant ? s->variant : default_variant(&s->info)); }

typedef struct {
    u256 key_start;
    u64 nkeys;
    size_t T;
    uint32_t ngroups;
    uint32_t nhits, err;
    u64 hits[64];
} lin_launch;

/* Runs one launch covering at least nkeys keys; fills hits. Returns false on API errors. */
static bool linear_run(gpu_session *s, const u8 target[20], lin_launch *L)
{
    const int HALFK = s->half;
    const int GRPK = 2 * HALFK;
    u64 total_groups = (L->nkeys + GRPK - 1) / GRPK;
    if (L->ngroups == 0) L->ngroups = 1;
    if (L->ngroups > total_groups) L->ngroups = (uint32_t)total_groups;
    u64 T = (total_groups + L->ngroups - 1) / L->ngroups;
    if (T > s->T_max) { L->ngroups = (uint32_t)((total_groups + s->T_max - 1) / s->T_max); T = (total_groups + L->ngroups - 1) / L->ngroups; }
    L->T = (size_t)T;

    uint32_t prm[64];
    memset(prm, 0, sizeof prm);
    u256 center = L->key_start;
    u256_add_u64(&center, &center, HALFK);
    ge P0, D, GS;
    if (!ec_mul_g(&P0, &center)) return false;
    u256 step = u256_from_u64((u64)L->ngroups * GRPK);
    if (!ec_mul_g(&D, &step)) return false;
    u256 gs = u256_from_u64(GRPK);
    if (!ec_mul_g(&GS, &gs)) return false;
    pack_ge(prm, &P0);
    pack_ge(prm + 16, &D);
    pack_ge(prm + 32, &GS);
    for (int i = 0; i < 5; i++)
        prm[48 + i] = (u32)target[4 * i] | (u32)target[4 * i + 1] << 8 | (u32)target[4 * i + 2] << 16 | (u32)target[4 * i + 3] << 24;
    prm[53] = L->ngroups;
    uint32_t zero[2] = {0, 0};
    if (!rt_write(s->c, s->b_prm, 0, prm, sizeof prm) || !rt_write(s->c, s->b_hits, 0, zero, 8)) return false;
    rt_arg_buf(s->kern, 0, s->b_prm);
    rt_arg_buf(s->kern, 1, s->b_gtab);
    rt_arg_buf(s->kern, 2, s->b_hits);
    rt_arg_u32(s->kern, 3, 64);
    if (!rt_run(s->c, s->kern, L->T)) return false;
    uint32_t hdr[2];
    if (!rt_read(s->c, s->b_hits, 0, hdr, 8)) return false;
    L->nhits = hdr[0] > 64 ? 64 : hdr[0];
    L->err = hdr[1];
    if (L->nhits) {
        uint32_t pairs[128];
        if (!rt_read(s->c, s->b_hits, 8, pairs, L->nhits * 8u)) return false;
        for (uint32_t i = 0; i < L->nhits; i++) L->hits[i] = (u64)pairs[2 * i] | (u64)pairs[2 * i + 1] << 32;
    }
    return true;
}

/* ---------------- autotune of the linear kernel ---------------- */

static _Atomic int g_boost;   /* service threads above the CPU workers' priority (gpu_set_boost) */
/* Called by a service thread before every launch: applies the requested priority when it changed. A refusal is not retried. */
static void boost_sync(int *cur)
{
    const int want = atomic_load_explicit(&g_boost, memory_order_relaxed);
    if (want == *cur) return;
    *cur = want;
    (void)pz_thread_boost(want != 0);
}

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
    u256 start = u256_pow2(100);
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
        if (!linear_run(s, h, &L) || L.err) return 0;
        double dt = pz_mono() - a;
        u256_add_u64(&start, &start, L.nkeys);
        if (dt < 0.12 && ng < 4096) { ng *= 2; continue; }      /* too short to time reliably */
        double rate = (double)L.nkeys / dt;
        if (rate > best) best = rate;
        good++;
    }
    return best;
}

static bool test_linear(gpu_session *s);

/* Chooses the variant of the linear kernel for this device (cached on disk per device and program version). */
static void linear_autotune(gpu_session *s, int idx)
{
    const rt_dev *d = &s->info;
    char nb[48];
    s->variant = default_variant(d);
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
    const int nv = variants_of(d, &vs);
    pz_log(PZ_LOG_INFO, "GPU[%d]: tuning the kernel (one time only: %d variants, the driver compiles them the first time)...", idx, nv);
    double rate[16] = {0};
    int best = 0;
    for (int i = 0; i < nv; i++) {
        if (linear_prepare_variant(s, vs[i])) rate[i] = linear_measure(s);
        linear_release_program(s);
        if (vs[i] == default_variant(d)) best = i;
    }
    {   /* the default stays unless the fastest variant is clearly (> 3%) faster than it */
        int fastest = 0;
        for (int i = 1; i < nv; i++) if (rate[i] > rate[fastest]) fastest = i;
        if (rate[fastest] > rate[best] * 1.03) best = fastest;
    }
    if (rate[best] <= 0) { s->variant = default_variant(d); return; }
    char line[400];
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: tuning:", idx);
    for (int i = 0; i < nv && o < sizeof line; i++) {
        char r[32];
        o += (size_t)snprintf(line + o, sizeof line - o, "%s window %s -> %s", i ? "," : "", variant_name(vs[i], nb, sizeof nb), pz_fmt_si(rate[i], r, sizeof r));
    }
    pz_log(PZ_LOG_INFO, "%s keys/s: picking %s", line, variant_name(vs[best], nb, sizeof nb));
    s->variant = vs[best];
    /* the winner must pass the correctness test before it is remembered, otherwise a bad choice would exclude the GPU forever */
    if (!test_linear(s)) {
        pz_log(PZ_LOG_WARN, "GPU[%d]: variant %s failed the correctness check: using the default and not saving the choice", idx, variant_name(vs[best], nb, sizeof nb));
        s->variant = default_variant(d);
        linear_release_program(s);
        return;
    }
    linear_release_program(s);
    tune_cache_put(d, false, s->variant);
}

/* ---------------- self-test ---------------- */

/* Runs kernel `k` (two buffer arguments) on n items. */
static bool run2(gpu_session *s, rt_kern *k, rt_buf *in, rt_buf *out, size_t n)
{
    rt_arg_buf(k, 0, in);
    rt_arg_buf(k, 1, out);
    return rt_run(s->c, k, n);
}

static bool test_fe(gpu_session *s)
{
    const int N = 192;
    rt_mod *p = rt_build(s->c, RT_PROG_TEST, 0);
    if (!p) return false;
    rt_kern *kfe = rt_kernel(p, "pz_test_fe", 2), *khash = rt_kernel(p, "pz_test_hash", 2), *kec = rt_kernel(p, "pz_test_ec", 2);
    bool ok = kfe && khash && kec;
    uint32_t *in = pz_xcalloc((size_t)N * 64, 4), *out = pz_xcalloc((size_t)N * 64, 4);
    fe A[192], B[192];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 4; j++) { A[i].n[j] = grnd(); B[i].n[j] = grnd(); }
        if (i % 5 == 0) { A[i].n[3] = A[i].n[2] = ~0ULL; }                 /* near p */
        if (i % 7 == 0) { B[i].n[1] = B[i].n[2] = B[i].n[3] = ~0ULL; }
        if (i == 1) { fe_set_u64(&A[i], 0); }
        if (i == 2) { fe_set_u64(&B[i], 0); }
        if (i == 3) { A[i] = FE_P; }
        if (i == 4) { fe_set_u64(&A[i], 1); fe_set_u64(&B[i], 1); }
        for (int j = 0; j < 4; j++) { in[i * 16 + 2 * j] = (u32)A[i].n[j]; in[i * 16 + 2 * j + 1] = (u32)(A[i].n[j] >> 32); in[i * 16 + 8 + 2 * j] = (u32)B[i].n[j]; in[i * 16 + 8 + 2 * j + 1] = (u32)(B[i].n[j] >> 32); }
    }
    rt_buf *bin = ok ? rt_buf_new(s->c, (size_t)N * 64, in) : NULL;
    rt_buf *bout = ok ? rt_buf_new(s->c, (size_t)N * 256, NULL) : NULL;
    if (ok && bin && bout) {
        ok = run2(s, kfe, bin, bout, (size_t)N) && rt_read(s->c, bout, 0, out, (size_t)N * 256);
        for (int i = 0; ok && i < N; i++) {
            fe r[8], e[8];
            for (int k = 0; k < 8; k++) unpack_fe(&r[k], out + i * 64 + k * 8);
            fe_mul(&e[0], &A[i], &B[i]);
            fe_sqr(&e[1], &A[i]);
            fe_add(&e[2], &A[i], &B[i]);
            fe_sub(&e[3], &A[i], &B[i]);
            fe a0 = A[i]; fe_normalize(&a0);
            if (fe_is_zero(&a0)) fe_zero(&e[4]); else fe_inv(&e[4], &A[i]);
            e[5] = A[i];
            fe_sub(&e[6], &B[i], &A[i]);
            fe_add(&e[7], &A[i], &A[i]);
            for (int k = 0; k < 8; k++) {
                if (k == 4 && fe_is_zero(&a0)) continue;   /* inverse of zero is undefined */
                if (!fe_equiv(&r[k], &e[k])) { pz_log(PZ_LOG_ERR, "GPU self-test: operation %d differs from the CPU (item %d)", k, i); ok = false; break; }
            }
        }
    } else ok = false;

    /* hash160 */
    if (ok) {
        const int H = 64;
        uint32_t *hin = pz_xcalloc((size_t)H * 9, 4), *hout = pz_xcalloc((size_t)H * 5, 4);
        u8 xb[64][32];
        fe xs[64];
        for (int i = 0; i < H; i++) {
            for (int j = 0; j < 32; j++) xb[i][j] = (u8)grnd();
            fe_from_be(&xs[i], xb[i]);
            u8 xx[32]; fe_to_be(xx, &xs[i]);
            memcpy(xb[i], xx, 32);
            uint32_t pk[8]; pack_fe(pk, &xs[i]);
            memcpy(hin + i * 9, pk, 32);
            hin[i * 9 + 8] = (u32)(grnd() & 1);
        }
        rt_buf *hb = rt_buf_new(s->c, (size_t)H * 36, hin);
        rt_buf *ho = rt_buf_new(s->c, (size_t)H * 20, NULL);
        ok = hb && ho && run2(s, khash, hb, ho, (size_t)H) && rt_read(s->c, ho, 0, hout, (size_t)H * 20);
        for (int i = 0; ok && i < H; i++) {
            u8 h[20], got[20];
            hash160_pub33((u8)hin[i * 9 + 8], xb[i], h);
            for (int w = 0; w < 5; w++) for (int b = 0; b < 4; b++) got[4 * w + b] = (u8)(hout[i * 5 + w] >> (8 * b));
            if (memcmp(h, got, 20)) { pz_log(PZ_LOG_ERR, "GPU self-test: hash160 differs from the CPU (item %d)", i); ok = false; }
        }
        rt_buf_free(hb);
        rt_buf_free(ho);
        free(hin); free(hout);
    }

    /* point arithmetic: a + t*D */
    if (ok) {
        const int E = 24;
        uint32_t *ein = pz_xcalloc((size_t)E * 33, 4), *eout = pz_xcalloc((size_t)E * 17, 4);
        ge pa[24], pd[24];
        u32 ts[24];
        for (int i = 0; i < E; i++) {
            u256 k1 = u256_from_u64(grnd() | 1), k2 = u256_from_u64(grnd() | 1);
            ec_mul_g(&pa[i], &k1);
            ec_mul_g(&pd[i], &k2);
            ts[i] = i == 0 ? 0 : (i == 1 ? 1 : (i == 2 ? 0xFFFFFFFFu : (u32)grnd()));
            pack_ge(ein + i * 33, &pa[i]);
            pack_ge(ein + i * 33 + 16, &pd[i]);
            ein[i * 33 + 32] = ts[i];
        }
        rt_buf *eb = rt_buf_new(s->c, (size_t)E * 33 * 4, ein);
        rt_buf *eo = rt_buf_new(s->c, (size_t)E * 17 * 4, NULL);
        ok = eb && eo && run2(s, kec, eb, eo, (size_t)E) && rt_read(s->c, eo, 0, eout, (size_t)E * 17 * 4);
        for (int i = 0; ok && i < E; i++) {
            /* want = pa + ts*pd computed with the host group law */
            ge want;
            gej acc;
            acc.inf = true;
            for (int bit = 31; bit >= 0; bit--) {
                gej_double(&acc, &acc);
                if ((ts[i] >> bit) & 1) gej_add_ge(&acc, &acc, &pd[i]);
            }
            gej_add_ge(&acc, &acc, &pa[i]);
            bool inf = !gej_to_ge(&want, &acc);
            if (inf || eout[i * 17 + 16] == 0) { if (!(inf && eout[i * 17 + 16] == 0)) ok = false; continue; }
            ge got;
            unpack_fe(&got.x, eout + i * 17);
            unpack_fe(&got.y, eout + i * 17 + 8);
            if (!ge_equal(&got, &want)) { pz_log(PZ_LOG_ERR, "GPU self-test: point sum differs from the CPU (item %d)", i); ok = false; }
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

static bool test_linear(gpu_session *s)
{
    if (!linear_prepare(s)) return false;
    const size_t tmax_production = s->T_max;      /* the test uses a handful of work-items; production keeps the full size */
    s->T_max = s->T_max > 96 ? 96 : s->T_max;
    bool ok = true;
    u256 start = u256_pow2(77);
    u256_add_u64(&start, &start, grnd() & 0xffffff);
    for (int trial = 0; trial < 3 && ok; trial++) {
        u64 span = 40 * (u64)(2 * s->half) * 3ULL;
        u64 off = trial == 0 ? 0 : (trial == 1 ? span - 1 : grnd() % span);
        u256 key = start;
        u256_add_u64(&key, &key, off);
        ge p;
        u8 pub[33], h[20];
        ec_mul_g(&p, &key);
        ge_to_pub33(pub, &p);
        hash160(pub, 33, h);
        lin_launch L;
        memset(&L, 0, sizeof L);
        L.key_start = start;
        L.nkeys = span;
        L.ngroups = 3;
        if (!linear_run(s, h, &L)) { ok = false; break; }
        bool found = false;
        for (uint32_t i = 0; i < L.nhits; i++) {
            u256 k = start;
            u256_add_u64(&k, &k, L.hits[i]);
            if (!u256_cmp(&k, &key)) found = true; else ok = false;
        }
        if (!found || L.nhits != 1 || L.err) {
            pz_log(PZ_LOG_ERR, "GPU self-test: wrong linear search result (hit=%u err=%u, expected offset %llu)", L.nhits, L.err, (unsigned long long)off);
            ok = false;
        }
    }
    s->T_max = tmax_production;
    return ok;
}

static bool test_kang(gpu_session *s);
static void kang_autotune(gpu_session *s, int idx);
static double kang_measure(gpu_session *s);

static bool selftest_allowed(const rt_dev *d) { return default_choice(d) || (!d->is_gpu && pz_env_on("PZ_OPENCL_ALL")) || (d->dup_of >= 0 && pz_env_on("PZ_OPENCL_ALL")); }

int gpu_selftest(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    int fails = 0, tested = 0;
    for (int i = 0; i < n; i++) {
        if (!selftest_allowed(&devs[i])) continue;
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        printf("  [%d] %s %s: ", i, rt_api_name(devs[i].api), devs[i].name);
        fflush(stdout);
        bool ok = session_open(s, &devs[i]) && test_fe(s);
        const int *vs;
        const int nv = variants_of(&devs[i], &vs);
        for (int k = 0; k < nv && ok; k++) {          /* every compile-time variant of the linear kernel */
            s->variant = vs[k];
            ok = test_linear(s);
            linear_release_program(s);
        }
        if (ok) { session_release(s); memset(s, 0, sizeof *s); ok = session_open(s, &devs[i]); }
        int ks[32];
        const int nk = kvariants_of(&devs[i], ks);
        for (int k = 0; k < nk && ok; k++) {          /* every variant of the kangaroo kernel (the cache-sized herds use the same kernels) */
            if (KV_L2(ks[k])) continue;
            s->kvar = ks[k];
            s->NK = KV_NK(ks[k]);
            ok = test_kang(s);
        }
        for (size_t k = 0; k < sizeof KV_MC4_NK / sizeof *KV_MC4_NK && ok && devs[i].api == RT_OPENCL; k++) {   /* CUDA tests them as list entries */
            s->kvar = KV_MC4_NK[k] | 2 << 16;
            s->NK = KV_MC4_NK[k];
            ok = test_kang(s);
        }
        for (size_t k = 0; k < sizeof KV_L2_NK / sizeof *KV_L2_NK && ok && devs[i].l2_bytes >= KANG_L2_MIN; k++)
            for (int c = 0; c < 2 && ok; c++) {       /* the kernels only the cache-sized herds use */
                s->kvar = KV_L2_NK[k] | c << 16;
                s->NK = KV_L2_NK[k];
                ok = test_kang(s);
            }
        printf(ok ? "OK\n" : "FAILED\n");
        if (!ok) fails++;
        tested++;
        session_release(s);
        free(s);
    }
    if (!tested) printf("  no GPU found%s%s%s (to try an OpenCL CPU device: PZ_OPENCL_ALL=1)\n", note[0] ? " (" : "", note, note[0] ? ")" : "");
    return fails;
}

/* ---------------- sessions ---------------- */

/* Opens and self-tests one device; false (logged, session released) when it cannot be used. */
static bool try_device(run_ctx *r, gpu_session *s, const rt_dev *d, bool for_kangaroo)
{
    memset(s, 0, sizeof *s);
    s->r = r;
    char mem[160];
    pz_log(PZ_LOG_INFO, "GPU[%d]: %s via %s, %u %s, %s - checking correctness...", d->index, d->name, rt_api_name(d->api), d->cu,
           d->api == RT_CUDA ? "SM" : "CU", fmt_devmem(d, mem, sizeof mem));
    bool ok = session_open(s, d) && test_fe(s);
    if (ok && !for_kangaroo) linear_autotune(s, d->index);
    if (ok && for_kangaroo) kang_autotune(s, d->index);
    ok = ok && (for_kangaroo ? test_kang(s) : test_linear(s));
    if (!ok) {
        pz_log(PZ_LOG_ERR, "GPU[%d] %s (%s): self-test failed, device excluded", d->index, d->name, rt_api_name(d->api));
        session_release(s);
        return false;
    }
    pz_log(PZ_LOG_INFO, "GPU[%d]: self-test passed", d->index);
    return true;
}

void gpu_check_filter(const char *filter)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    char tmp[128];
    pz_strlcpy(tmp, filter, sizeof tmp);
    for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
        if (pz_atoi(t) < n) continue;
        if (n) pz_die("--gpu %s: device %d does not exist (valid indices 0 to %d, see 'pzsolve devices')", filter, pz_atoi(t), n - 1);
        pz_die("--gpu %s: no GPU device found (%s)", filter, note[0] ? note : "no GPU runtime");
    }
}

static int open_sessions(run_ctx *r, gpu_session **out, bool for_kangaroo)
{
    *out = NULL;
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    const bool explicit_sel = r->gpu_filter[0] != 0;
    if (explicit_sel) gpu_check_filter(r->gpu_filter);
    gpu_session *arr = pz_xcalloc(RT_MAX_DEVS, sizeof *arr);
    int cnt = 0;
    for (int i = 0; i < n && cnt < RT_MAX_DEVS; i++) {
        if (explicit_sel ? !in_filter(r->gpu_filter, i) : !default_choice(&devs[i])) continue;
        if (try_device(r, &arr[cnt], &devs[i], for_kangaroo)) { cnt++; continue; }
        /* a GPU that fails through CUDA may still work through its OpenCL driver */
        if (explicit_sel || devs[i].api != RT_CUDA) continue;
        for (int j = 0; j < n; j++) {
            if (devs[j].dup_of != i) continue;
            pz_log(PZ_LOG_WARN, "GPU[%d]: trying the same card through OpenCL ([%d])", i, j);
            if (try_device(r, &arr[cnt], &devs[j], for_kangaroo)) cnt++;
            break;
        }
    }
    if (!cnt) {
        free(arr);
        if (!explicit_sel) pz_log(PZ_LOG_INFO, "GPU: no usable GPU%s%s%s, using the CPU only", note[0] ? " (" : "", note, note[0] ? ")" : "");
        else pz_log(PZ_LOG_WARN, "GPU: none of the devices given with --gpu %s is usable", r->gpu_filter);
        return 0;
    }
    *out = arr;
    return cnt;
}

/* ---------------- linear worker ---------------- */

static void cpu_support_hit(void *user, const u256 *key) { run_submit_key((run_ctx *)user, key, "CPU (GPU support)"); }

static void cpu_scan_claim(run_ctx *r, const claim_t *c, u64 *covered)
{
    scan_job job;
    scan_job_init(&job, r->cfg.target_h160);
    job.stop = &r->stop;
    job.user = r;
    job.on_hit = cpu_support_hit;
    const cpu_engine *e = r->engine ? r->engine : cpu_engine_by_name("baseline");
    *covered = e->scan(&job, &c->key_start, c->nkeys);
}

/* ---------------- recovery after a device error ---------------- */

static void kang_release(gpu_session *s);

/* A failed launch after the machine slept or hibernated usually means that the driver reset the device or its context.
 * Drops the session, waits, reopens and rebuilds it (about two and a half minutes of attempts) and repeats the start-up
 * self-test; false if the device never comes back or fails the test. The caller then restores its own buffers. */
static bool gpu_recover(gpu_session *s, bool kangaroo)
{
    run_ctx *r = s->r;
    const rt_dev info = s->info;
    const struct { int T, NK, kvar, variant; bool l2fit; } keep = {s->T, s->NK, s->kvar, s->variant, s->l2fit};
    const int paused = atomic_load(&s->paused);
    double wait = 1.0;
    for (int attempt = 0; attempt < 9 && !atomic_load(&r->stop); attempt++) {
        if (kangaroo) kang_release(s); else linear_release_program(s);
        session_release(s);                           /* clears everything after the context */
        s->T = keep.T; s->NK = keep.NK; s->kvar = keep.kvar; s->variant = keep.variant; s->l2fit = keep.l2fit;
        atomic_store(&s->paused, paused);
        pz_sleep(wait);
        wait = wait < 30 ? wait * 2 : 30;
        if (!session_open(s, &info)) continue;
        bool ok = test_fe(s) && (kangaroo ? test_kang(s) : test_linear(s));
        if (ok && !kangaroo) { linear_release_program(s); ok = linear_prepare_variant(s, keep.variant); }
        s->T = keep.T;                                /* the kangaroo self-test works with 4 work-items */
        if (ok) return true;
        pz_log(PZ_LOG_WARN, "GPU[%d] %s: the device came back but fails the start-up check (attempt %d)", info.index, info.name, attempt + 1);
    }
    return false;
}

static void *gpu_linear_thread(void *p)
{
    gpu_session *s = p;
    run_ctx *r = s->r;
    uint32_t ngroups = 2;
    const double target_s = 0.35;
    int consecutive = 0, total_fails = 0;                  /* failed launches in a row / in all */
    int boosted = 0;
    while (!atomic_load(&r->stop)) {
        boost_sync(&boosted);
        if (atomic_load(&s->paused)) { pz_sleep(0.01); continue; }
        u64 want = (u64)s->T_max * ngroups * (u64)(2 * s->half);
        claim_t c;
        claim_status st = sched_claim(r->sched, want, &c);
        if (st == CLAIM_DONE) break;
        if (st == CLAIM_WAIT) {
            pz_sleep(0.05);
            continue;
        }
        bool small = !(c.key_start.w[1] | c.key_start.w[2] | c.key_start.w[3]) && c.key_start.w[0] < (1u << 22);
        if (small) {
            u64 cov = 0;
            cpu_scan_claim(r, &c, &cov);
            sched_complete(r->sched, &c, cov);
            atomic_fetch_add(&r->ws[s->widx].units, cov);
            continue;
        }
        lin_launch L;
        memset(&L, 0, sizeof L);
        L.key_start = c.key_start;
        L.nkeys = c.nkeys;
        L.ngroups = ngroups;
        double t0 = pz_mono();
        if (!linear_run(s, r->cfg.target_h160, &L)) {
            sched_complete(r->sched, &c, 0);                  /* the block goes back to the pool */
            if (atomic_load(&r->stop)) break;
            pz_log(PZ_LOG_WARN, "GPU[%d] %s: execution error (it happens after the system sleeps): reopening the device", s->info.index, s->info.name);
            if (++consecutive > 3 || ++total_fails > 40 || !gpu_recover(s, false)) {
                pz_log(PZ_LOG_ERR, "GPU %s: the device does not respond, excluding it", s->info.name);
                break;
            }
            pz_log(PZ_LOG_INFO, "GPU[%d]: device resumed", s->info.index);
            continue;
        }
        consecutive = 0;
        double dt = pz_mono() - t0;
        u64 covered = c.nkeys;
        if (L.err) {
            pz_log(PZ_LOG_WARN, "GPU %s: numeric anomaly in the block, rescanning on the CPU", s->info.name);
            cpu_scan_claim(r, &c, &covered);
        }
        for (uint32_t i = 0; i < L.nhits; i++) {
            u256 k = c.key_start;
            u256_add_u64(&k, &k, L.hits[i]);
            run_submit_key(r, &k, "GPU");
        }
        sched_complete(r->sched, &c, covered);
        atomic_fetch_add(&r->ws[s->widx].units, c.nkeys);
        if (dt > 0.005 && c.nkeys == want) {
            double f = target_s / dt;
            double ng = (double)ngroups * (f < 0.5 ? 0.5 : (f > 2 ? 2 : f));
            ngroups = (uint32_t)(ng < 1 ? 1 : (ng > 4096 ? 4096 : ng));
        }
    }
    if (atomic_fetch_sub(s->active, 1) == 1) atomic_store(&r->stop, 1);
    return NULL;
}

int gpu_linear_start(run_ctx *r, gpu_session **out) { return open_sessions(r, out, false); }

int gpu_cores_needed(const gpu_session *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += s[i].info.api != RT_CUDA;
    return c;
}

void gpu_linear_launch(run_ctx *r, gpu_session *s, int n, _Atomic int *active)
{
    for (int i = 0; i < n; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "GPU%d", s[i].info.index);
        s[i].widx = run_alloc_worker(r, nm, 'G');
        s[i].active = active;
        s[i].limit_pct = r->limit_pct;
        s[i].T_max = run_limit_items(s[i].T_max, r->limit_pct, 128);
        if (r->limit_pct) pz_log(PZ_LOG_INFO, "GPU[%d]: limit %d%%: %zu work-items per launch", s[i].info.index, r->limit_pct, s[i].T_max);
        if (pthread_create(&s[i].th, NULL, gpu_linear_thread, &s[i]) != 0) pz_die("pthread_create failed");
        s[i].started = true;
    }
}

void gpu_linear_join(gpu_session *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i].started) pthread_join(s[i].th, NULL);
        session_release(&s[i]);
    }
    free(s);
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
    printf("  GPU[%d] %s (%s): copy bandwidth %s GB/s", d->index, d->name, rt_api_name(d->api), fmt_dec(g->copy_gbs, b1, sizeof b1));
    if (d->peak_gbs > 0) printf(" (theoretical %.0f GB/s: %.0f%%)", d->peak_gbs, 100.0 * g->copy_gbs / d->peak_gbs);
    printf("\n  GPU[%d] %s (%s): 32-bit integer instructions: mad.lo %s/s, mad.hi %s/s, 3-input add %s/s", d->index, d->name, rt_api_name(d->api),
           pz_fmt_si(g->lo_gops * 1e9, b1, sizeof b1), pz_fmt_si(g->hi_gops * 1e9, b2, sizeof b2), pz_fmt_si(g->add3_gops * 1e9, b3, sizeof b3));
    if (d->cu && d->mhz)
        printf("\n  GPU[%d]   = %s / %s / %s per %s per cycle at the rated clock (%u MHz; under load the real clock is usually lower)",
               d->index, fmt_dec(g->lo_gops * 1e3 / d->cu / d->mhz, b1, sizeof b1), fmt_dec(g->hi_gops * 1e3 / d->cu / d->mhz, b2, sizeof b2),
               fmt_dec(g->add3_gops * 1e3 / d->cu / d->mhz, b3, sizeof b3), d->api == RT_CUDA ? "SM" : "CU", d->mhz);
    printf("\n");
    if (d->l2_bytes) {
        char l2[32], sm[32], hb[32];
        printf("  GPU[%d]   L2 cache %s", d->index, pz_fmt_bytes((double)d->l2_bytes, l2, sizeof l2));
        if (d->smem_bytes) printf(", shared memory %s per %s", pz_fmt_bytes((double)d->smem_bytes, sm, sizeof sm), d->api == RT_CUDA ? "SM" : "CU");
        if (herd_bytes) printf("; the kangaroo herd (%s) is %.0f times the L2: every step goes through the card's memory", pz_fmt_bytes((double)herd_bytes, hb, sizeof hb),
                               (double)herd_bytes / (double)d->l2_bytes);
        printf("\n");
    }
}

void gpu_bench(void)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    int n = enumerate(devs, note, sizeof note);
    run_ctx *r = pz_xcalloc(1, sizeof *r);
    run_init(r);
    for (int i = 0; i < n; i++) {
        if (!selftest_allowed(&devs[i])) continue;
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        s->r = r;
        if (!session_open(s, &devs[i])) { session_release(s); free(s); continue; }
        linear_autotune(s, i);
        if (!linear_prepare(s)) { session_release(s); free(s); continue; }
        u8 h[20] = {0};
        lin_launch L;
        uint32_t ng = 2;
        u64 keys = 0;
        u256 start = u256_pow2(100);
        double t0 = pz_mono(), el;
        for (;;) {
            memset(&L, 0, sizeof L);
            L.key_start = start;
            L.ngroups = ng;
            L.nkeys = (u64)s->T_max * ng * (u64)(2 * s->half);
            double a = pz_mono();
            if (!linear_run(s, h, &L)) break;
            double dt = pz_mono() - a;
            keys += L.nkeys;
            u256_add_u64(&start, &start, L.nkeys);
            el = pz_mono() - t0;
            if (el > 4.0) break;
            if (dt < 0.3 && ng < 4096) ng *= 2;
        }
        char b[32];
        el = pz_mono() - t0;
        char nb[48];
        printf("  GPU[%d] %s (%s): %s keys/s (linear search, window %s)\n", devs[i].index, devs[i].name, rt_api_name(devs[i].api),
               pz_fmt_si((double)keys / el, b, sizeof b), variant_name(s->variant, nb, sizeof nb));
        fflush(stdout);
        linear_release_program(s);
        kang_autotune(s, i);
        double kr = kang_measure(s);
        if (kr > 0) {
            char mu[80];
            printf("  GPU[%d] %s (%s): %s steps/s (kangaroo, %s), memory used %s\n", devs[i].index, devs[i].name, rt_api_name(devs[i].api),
                   pz_fmt_si(kr, b, sizeof b), kvariant_name(kang_code(s), nb, sizeof nb), fmt_devused(&devs[i], s->mem_used, mu, sizeof mu));
        }
        gpu_diag dg;
        if (gpu_diag_run(s, &dg)) {
            gpu_diag_print(s, &dg, kr > 0 ? s->mem_used : 0);
            if (kr > 0 && devs[i].api == RT_CUDA) {   /* the static counts of the CUDA kernel (docs/en/ALGORITHMS.md 8.4) */
                char c1[32], c2[32], c3[32];
                printf("  GPU[%d]   kangaroo %s steps/s; model ceilings (~%d bytes and ~%d integer instructions per step): bandwidth %s, integer %s steps/s\n",
                       devs[i].index, pz_fmt_si(kr, c1, sizeof c1), KANG_MODEL_BYTES, KANG_MODEL_INSNS,
                       pz_fmt_si(dg.copy_gbs * 1e9 / KANG_MODEL_BYTES, c2, sizeof c2), pz_fmt_si(dg.lo_gops * 1e9 / KANG_MODEL_INSNS, c3, sizeof c3));
            }
        } else {
            printf("  GPU[%d] %s (%s): bandwidth and integer throughput probes cannot run\n", devs[i].index, devs[i].name, rt_api_name(devs[i].api));
        }
        fflush(stdout);
        session_release(s);
        free(s);
    }
    free(r);
}

/* ---------------- kangaroo ---------------- */

#define KSTRIDE 32
#define JSTRIDE 20
#define KANG_HIST_ROUNDS 64        /* depth of the jump history (rounds per launch) of a production herd */

static void pack_state(uint32_t *o, const kang_worker *w, int i)
{
    pack_fe(o, &w->x[i]);
    pack_fe(o + 8, &w->y[i]);
    for (int j = 0; j < 3; j++) { o[16 + 2 * j] = (uint32_t)w->d[i].w[j]; o[17 + 2 * j] = (uint32_t)(w->d[i].w[j] >> 32); }
    o[22] = w->kind[i] != 0;           /* the kernel only tells tame from wild (the species of schema 2 stay on the host: kang_dp_recode) */
    o[23] = w->sigma[i] < 0 ? 1u : 0u;
    o[24] = w->prev[i];
    o[25] = w->prev2[i];
    o[26] = w->ck[i];
    o[27] = w->mn[i];
    o[28] = w->esc[i];
    o[29] = (uint32_t)w->lastdp[i];
    o[30] = (uint32_t)(w->lastdp[i] >> 32);
    o[31] = 0;
}

static void kang_release(gpu_session *s)
{
    rt_buf_free(s->b_state); rt_buf_free(s->b_jump); rt_buf_free(s->b_dps); rt_buf_free(s->b_flags); rt_buf_free(s->b_rc); rt_buf_free(s->b_rl);
    rt_buf_free(s->b_hist);
    s->b_state = s->b_jump = s->b_dps = s->b_flags = s->b_rc = s->b_rl = s->b_hist = NULL;
    rt_kern_free(s->kstep);
    rt_mod_free(s->kprog);
    s->kstep = NULL;
    s->kprog = NULL;
}

/* Builds the kernel for s->NK kangaroos per work-item and the buffers for T work-items; a launch runs at most
 * `hist_rounds` rounds (the depth of the jump history, 16 bits per walker and round). */
static bool kang_prepare(gpu_session *s, const kang_params_t *P, int T, uint32_t hist_rounds)
{
    s->kprog = rt_build(s->c, RT_PROG_KANG, s->kvar);
    if (!s->kprog) return false;
    s->kstep = rt_kernel(s->kprog, "pz_kang_step", 17);
    if (!s->kstep) return false;
    s->T = T;
    s->hist_rounds = hist_rounds;
    s->gstep = 0;
    size_t K = (size_t)T * s->NK;
    const unsigned nj = P->R + KANG_NESC;
    uint32_t *jt = pz_xcalloc((size_t)nj * JSTRIDE, 4);
    for (unsigned j = 0; j < nj; j++) {
        pack_fe(jt + j * JSTRIDE, &P->jx[j]);
        pack_fe(jt + j * JSTRIDE + 8, &P->jy[j]);
        for (int w = 0; w < 4; w++) jt[j * JSTRIDE + 16 + w] = (uint32_t)(P->jv[j] >> (32 * w));
    }
    s->b_jump = rt_buf_new(s->c, (size_t)nj * JSTRIDE * 4, jt);
    free(jt);
    s->dps_cap = 1u << 20;
    s->rl_cap = 1u << 14;
    s->b_state = rt_buf_new(s->c, K * KSTRIDE * 4, NULL);
    s->b_dps = rt_buf_new(s->c, (size_t)s->dps_cap * 40, NULL);
    s->b_flags = rt_buf_new(s->c, 4, NULL);
    s->b_rc = rt_buf_new(s->c, 4, NULL);
    s->b_rl = rt_buf_new(s->c, (size_t)s->rl_cap * 4, NULL);
    s->b_hist = rt_buf_new(s->c, K * hist_rounds * 2, NULL);
    s->mem_used = (size_t)nj * JSTRIDE * 4 + K * KSTRIDE * 4 + (size_t)s->dps_cap * 40 + 8 + (size_t)s->rl_cap * 4 + K * hist_rounds * 2;
    return s->b_jump && s->b_state && s->b_dps && s->b_flags && s->b_rc && s->b_rl && s->b_hist;
}

/* One kernel launch of `rounds` rounds; returns the DP and restart counts. */
static bool kang_run(gpu_session *s, const kang_params_t *P, uint32_t rounds, uint32_t *ndp, uint32_t *nrl)
{
    if (rounds > s->hist_rounds) rounds = s->hist_rounds;
    uint32_t zero = 0;
    if (!rt_write(s->c, s->b_flags, 0, &zero, 4) || !rt_write(s->c, s->b_rc, 0, &zero, 4)) return false;
    rt_kern *k = s->kstep;
    rt_arg_buf(k, 0, s->b_state);
    rt_arg_buf(k, 1, s->b_jump);
    rt_arg_buf(k, 2, s->b_dps);
    rt_arg_buf(k, 3, s->b_flags);
    rt_arg_buf(k, 4, s->b_rl);
    rt_arg_buf(k, 5, s->b_rc);
    rt_arg_buf(k, 6, s->b_hist);
    rt_arg_u32(k, 7, rounds);
    rt_arg_u32(k, 8, (uint32_t)P->dp_mask);
    rt_arg_u32(k, 9, (uint32_t)(P->dp_mask >> 32));
    rt_arg_u32(k, 10, (uint32_t)P->cap_since);
    rt_arg_u32(k, 11, (uint32_t)(P->cap_since >> 32));
    rt_arg_u32(k, 12, (uint32_t)s->dps_cap);
    rt_arg_u32(k, 13, (uint32_t)s->rl_cap);
    rt_arg_u32(k, 14, P->R);
    rt_arg_u32(k, 15, (uint32_t)s->gstep);
    rt_arg_u32(k, 16, (uint32_t)(s->gstep >> 32));
    if (!rt_run(s->c, k, (size_t)s->T)) return false;
    s->gstep += rounds;
    return rt_read(s->c, s->b_flags, 0, ndp, 4) && rt_read(s->c, s->b_rc, 0, nrl, 4);
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
    bool ok = kang_prepare(s, &ks->P, T, (uint32_t)ROUNDS);
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
    if (ok) ok = kang_run(s, &ks->P, ROUNDS, &ndp, &nrl);
    if (ok) ok = rt_read(s->c, s->b_state, 0, back, (size_t)K * KSTRIDE * 4);
    if (ok && (ndp > s->dps_cap || nrl > s->rl_cap)) ok = false;
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
            }
        }
        if (ok && (int)ndp != w2->ndp) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: %u distinguished points against %d on the CPU", ndp, w2->ndp); ok = false; }
        if (ok && (int)nrl != w2->nrestart) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: %u restarts against %d on the CPU", nrl, w2->nrestart); ok = false; }
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
            if (memcmp(dpd, cpu, (size_t)ndp * 40) != 0) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: distinguished points differ from the CPU"); ok = false; }
            free(cpu);
            uint32_t *rc = pz_xcalloc((size_t)w2->nrestart + 1, 4);
            for (int i = 0; i < w2->nrestart; i++) rc[i] = (uint32_t)w2->restart[i];
            qsort(rc, (size_t)w2->nrestart, 4, cmp_u32);
            qsort(rld, nrl, 4, cmp_u32);
            if (nrl && memcmp(rc, rld, nrl * 4) != 0) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: restarts differ from the CPU"); ok = false; }
            free(rc);
        }
        if (ok && R == KANG_R && (w2->ndp == 0 || w2->nrestart == 0)) { pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: the test did not produce both distinguished points and restarts"); ok = false; }
    }
    free(st); free(back); free(dpd); free(rld);
    kang_worker_free(w1);
    kang_worker_free(w2);
    kang_free_test_state(ks);
    kang_release(s);
    return ok;
}

static bool test_kang(gpu_session *s)
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
    if (esc == 0) pz_log(PZ_LOG_ERR, "GPU kangaroo self-test: the test produced no cycle escapes");
    return esc > 0;
}

/* Work-items of the kangaroo kernel: CUDA keeps 512 threads per SM resident (126 registers: four blocks of 128),
 * OpenCL the historical 64 per compute unit; bounded by the state buffer in an eighth of the device memory. */
static size_t kang_threads(const gpu_session *s)
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
    const int T = (int)kang_threads(s);
    double rate = 0;
    if (kang_prepare(s, &ks->P, T, KANG_HIST_ROUNDS)) {
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
            if (!kang_run(s, &ks->P, rounds, &ndp, &nrl)) { rate = 0; break; }
            double dt = pz_mono() - a;
            if (dt < 0.25 && rounds < s->hist_rounds) { rounds *= 2; continue; }
            double r = (double)K * rounds / dt;
            if (r > rate) rate = r;
            if (pz_mono() - t_begin > 1.5) break;
        }
    }
    kang_release(s);
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
static void kang_autotune(gpu_session *s, int idx)
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
    const int nv = kvariants_of(d, vs);
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
    size_t o = (size_t)snprintf(line, sizeof line, "GPU[%d]: kangaroo tuning:", idx);
    for (int i = 0; i < nv && o < sizeof line; i++) {
        char r[32];
        o += (size_t)snprintf(line + o, sizeof line - o, "%s %s -> %s", i ? "," : "", kvariant_name(vs[i], nb, sizeof nb), rate[i] > 0 ? pz_fmt_si(rate[i], r, sizeof r) : "cannot run");
    }
    if (rate[best] <= 0) {
        kang_set_variant(s, default_kvariant(d));
        pz_log(PZ_LOG_WARN, "%s: using the default %s", line, kvariant_name(kang_code(s), nb, sizeof nb));
        return;
    }
    pz_log(PZ_LOG_INFO, "%s steps/s: picking %s", line, kvariant_name(vs[best], nb, sizeof nb));
    kang_set_variant(s, vs[best]);
    /* remembered only once the choice has passed the bit-exact test against the CPU */
    if (!test_kang(s)) {
        pz_log(PZ_LOG_WARN, "GPU[%d]: variant %s failed the correctness check: using the default", idx, kvariant_name(vs[best], nb, sizeof nb));
        kang_set_variant(s, default_kvariant(d));
        return;
    }
    tune_cache_put(d, true, kang_code(s));
}

int gpu_kangaroo_start(run_ctx *r, gpu_session **out, int *k_total)
{
    *k_total = 0;
    u256 rsz = cfg_range_size(&r->cfg);
    const long double sqrt_w = sqrtl(u256_to_ld(&rsz));
    gpu_session *arr = NULL;
    int n = open_sessions(r, &arr, true);
    *out = arr;
    for (int i = 0; i < n; i++) {
        gpu_session *s = &arr[i];
        size_t t = kang_threads(s);
        /* small ranges: no more GPU kangaroos than about sqrt(W)/256, so that each still walks a meaningful distance */
        if ((long double)t * s->NK > sqrt_w / 256.0L) {
            long double cap = sqrt_w / 256.0L / s->NK;
            t = cap < 64 ? 64 : (size_t)cap;
        }
        t = run_limit_items(t, r->limit_pct, 128);       /* --limit: fewer work-items in flight, the same kernel, flat out */
        s->T = (int)t;
        *k_total += (int)t * s->NK;
    }
    return n;
}

/* Buffers of the kangaroo kernel and a fresh herd on the device (host mirror in *mirror, replaced). `stream` selects the
 * starting points: a herd rebuilt after an error must not repeat the trajectories already walked. */
static bool kang_gpu_boot(gpu_session *s, kang_state *ks, kang_worker **mirror, u64 stream)
{
    const int K = s->T * s->NK;
    if (!kang_prepare(s, &ks->P, s->T, KANG_HIST_ROUNDS)) return false;
    if (*mirror) kang_worker_free(*mirror);
    *mirror = kang_worker_new(ks, K, stream);
    kang_init_herd_parallel(ks, *mirror, s->r->sys.ncpu, stream);
    uint32_t *st = pz_xcalloc((size_t)K * KSTRIDE, 4);
    for (int i = 0; i < K; i++) pack_state(st + (size_t)i * KSTRIDE, *mirror, i);
    const bool ok = rt_write(s->c, s->b_state, 0, st, (size_t)K * KSTRIDE * 4);
    free(st);
    return ok;
}

static void *gpu_kang_thread(void *p)
{
    gpu_session *s = p;
    kang_state *ks = s->ks;
    run_ctx *r = s->r;
    int T = s->T;
    const int K = T * s->NK;
    kang_worker *mirror = NULL;
    int consecutive = 0, total_fails = 0;                  /* failed launches in a row / in all */
    if (!kang_gpu_boot(s, ks, &mirror, 1000 + (u64)s->info.index)) { pz_log(PZ_LOG_ERR, "GPU %s: kangaroo setup failed", s->info.name); goto done; }
    {
        char mu[80];
        pz_log(PZ_LOG_INFO, "GPU[%d]: %d kangaroos (%d work-items x %d), memory used %s", s->info.index, K, T, s->NK, fmt_devused(&s->info, s->mem_used, mu, sizeof mu));
    }
    uint32_t *dpraw = pz_xcalloc((size_t)s->dps_cap, 40), *rlraw = pz_xcalloc((size_t)s->rl_cap, 4);
    dp_rec *recs = pz_xcalloc((size_t)s->dps_cap, sizeof(dp_rec));
    bool *dups = pz_xcalloc((size_t)s->dps_cap, sizeof(bool));
    /* keep the expected number of DPs per launch well below the device buffer */
    double edp = (double)K / ldexp(1.0, (int)ks->P.dp_bits);
    double rm = (double)(s->dps_cap / 2) / (edp > 1e-9 ? edp : 1e-9);
    const uint32_t rmax = rm < 1 ? 1 : (rm > s->hist_rounds ? s->hist_rounds : (uint32_t)rm);
    uint32_t rounds = rmax < 4 ? rmax : 4;
    int boosted = 0;
    while (!atomic_load(&r->stop)) {
        boost_sync(&boosted);
        if (atomic_load(&s->paused)) { pz_sleep(0.01); continue; }
        uint32_t ndp = 0, nrl = 0;
        double t0 = pz_mono();
        if (!kang_run(s, &ks->P, rounds, &ndp, &nrl)) goto device_error;
        consecutive = 0;
        double dt = pz_mono() - t0;
        mirror->round = s->gstep;               /* restarts below start counting from here */
        uint32_t nd = ndp > s->dps_cap ? (uint32_t)s->dps_cap : ndp;
        if (ndp > s->dps_cap) atomic_fetch_add(&ks->dropped, ndp - s->dps_cap);
        uint32_t nr = nrl > s->rl_cap ? (uint32_t)s->rl_cap : nrl;
        if (nd && !rt_read(s->c, s->b_dps, 0, dpraw, (size_t)nd * 40)) goto device_error;
        if (nr && !rt_read(s->c, s->b_rl, 0, rlraw, (size_t)nr * 4)) goto device_error;
        for (uint32_t i = 0; i < nd; i++) {
            const uint32_t *o = dpraw + (size_t)i * 10;
            recs[i].tagk = (u64)o[0] | (u64)o[1] << 32;
            for (int j = 0; j < 3; j++) recs[i].d.w[j] = (u64)o[2 + 2 * j] | (u64)o[3 + 2 * j] << 32;
            if (o[8] < (uint32_t)K) kang_dp_recode(ks, mirror, &recs[i], (int)o[8]);
        }
        atomic_fetch_add_explicit(&r->ws[s->widx].units, (u64)K * rounds, memory_order_relaxed);
        bool solved = nd && kang_insert_dps(ks, recs, (int)nd, dups);
        if (solved) break;
        uint32_t tmp[KSTRIDE];
        for (uint32_t i = 0; i < nd; i++) {
            if (!dups[i]) continue;
            uint32_t idx = dpraw[(size_t)i * 10 + 8];
            if (idx >= (uint32_t)K) continue;
            kang_restart_one(ks, mirror, (int)idx);
            pack_state(tmp, mirror, (int)idx);
            rt_write(s->c, s->b_state, (size_t)idx * KSTRIDE * 4, tmp, KSTRIDE * 4);
            atomic_fetch_add(&ks->dups, 1);
        }
        for (uint32_t i = 0; i < nr; i++) {
            uint32_t idx = rlraw[i];
            if (idx >= (uint32_t)K) continue;
            kang_restart_one(ks, mirror, (int)idx);
            pack_state(tmp, mirror, (int)idx);
            rt_write(s->c, s->b_state, (size_t)idx * KSTRIDE * 4, tmp, KSTRIDE * 4);
            atomic_fetch_add(&ks->restarts, 1);
        }
        if (dt > 0.002) {
            double f = 0.35 / dt;
            double nrnds = (double)rounds * (f < 0.5 ? 0.5 : (f > 2 ? 2 : f));
            rounds = (uint32_t)(nrnds < 1 ? 1 : (nrnds > rmax ? rmax : nrnds));
        }
        continue;
    device_error:
        /* typically the driver reset the device while the machine slept: reopen it and start a fresh herd (the walkers' progress
         * since their last distinguished point is lost; the points already found are on the host) */
        if (atomic_load(&r->stop)) break;
        pz_log(PZ_LOG_WARN, "GPU[%d] %s: execution error (it happens after the system sleeps): reopening the device", s->info.index, s->info.name);
        if (++consecutive > 3 || ++total_fails > 40 || !gpu_recover(s, true) || !kang_gpu_boot(s, ks, &mirror, 1000 + (u64)s->info.index + 7919ULL * (u64)total_fails)) {
            pz_log(PZ_LOG_ERR, "GPU %s: the device does not respond, excluding it", s->info.name);
            break;
        }
        pz_log(PZ_LOG_INFO, "GPU[%d]: device resumed with a new herd", s->info.index);
    }
    free(dpraw); free(rlraw); free(recs); free(dups);
done:
    if (mirror) kang_worker_free(mirror);
    if (atomic_fetch_sub(s->active, 1) == 1) atomic_store(&r->stop, 1);
    return NULL;
}

void gpu_kangaroo_launch(run_ctx *r, void *ksv, gpu_session *s, int n, _Atomic int *active)
{
    for (int i = 0; i < n; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "GPU%d", s[i].info.index);
        s[i].widx = run_alloc_worker(r, nm, 'G');
        s[i].active = active;
        s[i].ks = ksv;
        if (pthread_create(&s[i].th, NULL, gpu_kang_thread, &s[i]) != 0) pz_die("pthread_create failed");
        s[i].started = true;
    }
}

void gpu_kangaroo_join(gpu_session *s, int n) { gpu_linear_join(s, n); }

void gpu_pause(gpu_session *s, bool paused) { atomic_store(&s->paused, paused ? 1 : 0); }
void gpu_set_boost(bool on) { atomic_store_explicit(&g_boost, on ? 1 : 0, memory_order_relaxed); }
bool gpu_boost_get(void) { return atomic_load_explicit(&g_boost, memory_order_relaxed) != 0; }
int gpu_session_index(const gpu_session *s) { return s->info.index; }
gpu_session *gpu_session_at(gpu_session *s, int i) { return &s[i]; }

/* ---------------- 'doctor' ---------------- */

void gpu_doctor(bool deep)
{
    rt_dev devs[RT_MAX_DEVS];
    char note[200];
    const int n = enumerate(devs, note, sizeof note);
    if (!n) {
        doctor_line('i', "no GPU device (%s): campaigns use the CPU only", note[0] ? note : "no CUDA/OpenCL runtime");
        return;
    }
    if (note[0]) doctor_line('i', "%s", note);
    int used = 0;
    for (int i = 0; i < n; i++) {
        const rt_dev *d = &devs[i];
        char mem[160], l2[32] = "n/a";
        fmt_devmem(d, mem, sizeof mem);
        if (d->l2_bytes) pz_fmt_bytes((double)d->l2_bytes, l2, sizeof l2);
        if (!default_choice(d)) {
            doctor_line('i', "[%d] %s %s: not used by default (%s)", i, rt_api_name(d->api), d->name, d->dup_of >= 0 ? "same card as one already used via CUDA" : d->is_gpu ? "select with --gpu" : "not a GPU");
            continue;
        }
        used++;
        doctor_line('i', "[%d] %s %s: %u %s @ %u MHz, memory %s, L2 cache %s, %s", i, rt_api_name(d->api), d->name, d->cu, d->api == RT_CUDA ? "SM" : "CU", d->mhz, mem, l2, d->plat_name);
        if (d->unified)
            doctor_line('w', "[%d] integrated GPU: it shares the CPU's RAM and power (in a laptop they share the same cooling): 'bench --together' tells whether it is worth keeping", i);
        if (d->api == RT_OPENCL && strstr(d->vendor, "NVIDIA") && d->dup_of < 0)
            doctor_line('w', "[%d] NVIDIA card seen only through OpenCL: CUDA (faster) is not available, update the NVIDIA driver", i);
        gpu_session *s = pz_xcalloc(1, sizeof *s);
        if (session_open(s, d)) {
            const bool ok = test_fe(s);
            session_release(s);
            doctor_line(ok ? 'o' : 'x', "[%d] field arithmetic, hash and curve checked bit by bit against the CPU: %s", i, ok ? "passed" : "ERROR (the GPU would be excluded)");
        } else {
            doctor_line('x', "[%d] cannot open the device (damaged driver or runtime?)", i);
        }
        free(s);
    }
    if (!used) doctor_line('i', "no GPU usable by default");
    if (deep && used) {
        const int fails = gpu_selftest();
        doctor_line(fails ? 'x' : 'o', "full GPU self-test: %s", fails ? "ERRORS" : "passed");
    }
}

void gpu_doctor_cache(void)
{
    char path[512];
    if (!pz_cache_file("gpu_tune.txt", path, sizeof path)) return;
    char *t = pz_read_file(path, NULL);
    int cnt = 0;
    const size_t vl = strlen(PZ_VERSION);
    for (char *line = t; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strrchr(line, '=');
        if (eq && (size_t)(eq - line) > vl && !strncmp(eq - vl - 1, "|" PZ_VERSION, vl + 1)) { doctor_line('i', "saved GPU tuning: %.150s", line); cnt++; }
        line = nl ? nl + 1 : NULL;
    }
    free(t);
    if (!cnt) doctor_line('i', "no GPU kernel tuning saved for version %s: it is done the first time each GPU is used (20-60 s, one time only)", PZ_VERSION);
}
