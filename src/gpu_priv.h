/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_priv.h - internals of the GPU code shared by gpu.c (devices, sessions, worker threads), gpu_tune.c (kernel variants,
 * autotuning, benchmark) and gpu_selftest.c (the kernels against the CPU). */
#ifndef PZ_GPU_PRIV_H
#define PZ_GPU_PRIV_H

#include "gpu.h"
#include "gpu_rt.h"
#include "kangaroo_priv.h"

/* Compile-time variants of the linear kernel: half-window (group = 2*half keys) | minimum resident blocks per SM << 16
 * (CUDA only: 4 caps the kernel at 128 registers, trading a few spills for more resident warps). The autotuner measures
 * them on the device and keeps the fastest. */
#define VAR_HALF(v) ((v) & 0xFFFF)
#define VAR_MINB(v) ((v) >> 16)

/* Kangaroo kernel variants: kangaroos per work-item (NK: one field inversion is shared by NK steps, so larger is cheaper
 * in arithmetic but needs more private memory) | configuration << 16: 0 keeps the running product of the batch inversion
 * for every walker, 1 only for every other one (half the traffic for those products, half a field multiplication more per
 * step). Measured on the device like the linear variants. */
#define KV_NK(v) ((v) & 0xFFFF)
#define KV_CFG(v) (((v) >> 16) & 0xF)
#define KV_L2(v) (((v) >> 24) & 1)            /* the herd is sized to fit the L2 cache (fewer work-items), see kang_l2_threads */
#define KV_KERNEL(v) ((v) & 0xFFFFF)          /* NK | cfg << 16: what the kernel build takes */
#define KV_L2FLAG (1 << 24)

/* GPUs with a large last-level cache (RTX 30/40 series: 6-72 MiB; some laptop GPUs have 2) can keep a herd of some
 * hundred thousand walkers (about 130 bytes each in the work-items' private arrays) on the chip, where the traffic that
 * limits the kernel stays in the cache instead of going to the video memory. */
#define KANG_L2_MIN (8u << 20)
#define KANG_WALKER_BYTES 128
static const int KV_L2_NK[] = {16, 32, 64};
/* kernels built with products every fourth walker that the self-test checks on every device (the OpenCL tuner does not offer them) */
static const int KV_MC4_NK[] = {64, 128};

struct gpu_session {
    run_ctx *r;
    rt_dev info;
    /* owned by the launcher and the thread, before `c`: gpu_session_release() clears everything after it and a recovery keeps these */
    bool mismatch;               /* a self-test found a result that differs from the CPU (as opposed to an error of the API): the device is not retried */
    u64 rs;                      /* state of the generator of the self-tests (gpu_test_rand): one per session, the GPUs recover together after a sleep */
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
    int kprog_var;               /* the kernel variant (kvar) that kprog was built for */
    rt_buf *b_state, *b_jump, *b_dps, *b_flags, *b_rc, *b_rl, *b_hist;
    size_t rc_off;               /* the restart count is b_rc, a part of b_flags at this offset (one transfer reads both counts); 0: a buffer of its own */
    int T, NK, kvar;             /* kangaroo kernel: walkers per work-item, kernel variant (NK | cfg << 16) */
    bool l2fit;                  /* the herd is sized to the L2 cache (see kang_l2_threads_for) */
    u64 dps_cap, rl_cap;
    uint32_t hist_rounds;        /* depth of the jump history: the most rounds a launch can run */
    u64 gstep;                   /* steps taken by every walker so far (the step number of the next round) */
    size_t mem_used;             /* bytes of device buffers of the last kangaroo preparation */
    _Atomic int paused;          /* the kangaroo thread idles ('bench --together' switches GPUs off to measure them) */
};

/* Packing of field elements, points and kangaroo states into the 32-bit words the kernels use. */
static inline void pack_fe(uint32_t out[8], const pz_fe *f)
{
    pz_fe t = *f;
    pz_fe_normalize(&t);
    for (int i = 0; i < 4; i++) { out[2 * i] = (uint32_t)t.n[i]; out[2 * i + 1] = (uint32_t)(t.n[i] >> 32); }
}
static inline void unpack_fe(pz_fe *f, const uint32_t in[8])
{
    for (int i = 0; i < 4; i++) f->n[i] = (u64)in[2 * i] | (u64)in[2 * i + 1] << 32;
}
static inline void pack_ge(uint32_t out[16], const pz_ge *p) { pack_fe(out, &p->x); pack_fe(out + 8, &p->y); }

static inline void pack_state(uint32_t *o, const kang_worker *w, int i)
{
    pack_fe(o, &w->x[i]);
    pack_fe(o + 8, &w->y[i]);
    for (int j = 0; j < 3; j++) { o[16 + 2 * j] = (uint32_t)w->d[i].w[j]; o[17 + 2 * j] = (uint32_t)(w->d[i].w[j] >> 32); }
    o[22] = w->kind[i] != 0;           /* the kernel only tells tame from wild (the species stay on the host: kang_dp_recode) */
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

#define KSTRIDE KANG_STATE_WORDS   /* words of the state of a kangaroo on the device (pz_kang.h) */
#define KANG_HIST_ROUNDS 64        /* the least depth of the jump history (rounds per launch) of a production herd: gpu_kang_threads sizes it for this */
#define KANG_HIST_MAX 256          /* the most (gpu_kang_depth) */

typedef struct {
    pz_u256 key_start;
    u64 nkeys;
    size_t T;
    uint32_t ngroups;
    uint32_t nhits, err;
    u64 hits[64];
} lin_launch;

static inline int enumerate(rt_dev *devs, char *note, size_t nl) { return rt_enumerate(devs, RT_MAX_DEVS, note, nl); }

/* gpu.c: devices, sessions, the programs and launches of the linear kernel and the buffers of the kangaroo kernel */
bool gpu_selftest_allowed(const rt_dev *d);
const char *gpu_fmt_devused(const rt_dev *d, size_t used, char *out, size_t n);
bool gpu_session_open(gpu_session *s, const rt_dev *d);
void gpu_session_release(gpu_session *s);
void gpu_linear_release_program(gpu_session *s);
bool gpu_linear_prepare_variant(gpu_session *s, int variant);
bool gpu_linear_prepare(gpu_session *s);
bool gpu_linear_run(gpu_session *s, const u8 target[20], lin_launch *L);
void gpu_kang_release(gpu_session *s);
void gpu_kang_release_buffers(gpu_session *s);
bool gpu_kang_prepare(gpu_session *s, const kang_params_t *P, int T, uint32_t hist_rounds);
bool gpu_kang_run(gpu_session *s, const kang_params_t *P, uint32_t rounds, uint32_t *ndp, uint32_t *nrl);
uint32_t gpu_launch_after_failure(uint32_t n);          /* the size of the launches after a failed one */
/* The size of the next launch (groups per work-item, rounds) from the duration `dt` of the last one: towards 0.35 s, at most halved or
 * doubled at a time, from 1 to `max`. A launch of `min_dt` seconds or less is too short to time and changes nothing. */
uint32_t gpu_launch_resize(uint32_t n, double dt, double min_dt, uint32_t max);
void gpu_sleep_unless_stopped(run_ctx *r, double secs); /* the wait of a recovery: returns at once when the campaign is stopped */

/* How many failed launches a device is allowed: more than GPU_FAIL_STREAK in a row, or more than GPU_FAIL_TOTAL without GPU_FAIL_FORGET
 * seconds of successful launches between them, and it is given up. A laptop that sleeps every night has one or two recoveries a day
 * and must not lose its GPU on the 41st. */
#define GPU_FAIL_STREAK 3
#define GPU_FAIL_TOTAL 40
#define GPU_FAIL_FORGET 600.0
typedef struct { int streak, total; bool good; double since; } gpu_health;   /* zero: no failure yet; `since`: the launches have been fine since then */
bool gpu_health_failed(gpu_health *h);                  /* a launch failed: true when the device is to be given up */
void gpu_health_ok(gpu_health *h, double now);          /* a launch succeeded (now: seconds on a monotonic clock) */

/* gpu_tune.c: the variants of the kernels, their choice for a device (remembered on disk) and the herd size */
int gpu_variants_of(const rt_dev *d, const int **v);
int gpu_default_variant(const rt_dev *d);
int gpu_kvariants_of(const rt_dev *d, int *out);
size_t gpu_kang_threads(const gpu_session *s);
uint32_t gpu_kang_depth(const rt_dev *d, size_t K);      /* the depth of the jump history of a herd of K walkers */
void gpu_linear_autotune(gpu_session *s, int idx);
void gpu_kang_autotune(gpu_session *s, int idx);
/* The kernel variant in use, as the start-up line of a device names it: "window 256", "kangaroo NK 128". */
char *gpu_variant_text(const gpu_session *s, bool kang, char *buf, size_t n);
int gpu_tune_selftest(void);                             /* the choice of a variant from invented rates (no device needed) */

/* gpu_selftest.c: the kernels against the CPU, and the rules of the recovery (no device needed) */
int gpu_policy_selftest(void);
u64 gpu_test_rand(gpu_session *s);                      /* the random numbers of the tests of one session (xorshift64, not shared with the other sessions) */
bool gpu_test_fe(gpu_session *s);
bool gpu_test_linear(gpu_session *s);
bool gpu_test_kang(gpu_session *s);

#endif
