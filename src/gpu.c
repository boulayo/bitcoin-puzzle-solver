/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu.c - GPU support: device selection, sessions and the worker threads for linear search and the kangaroo. Written against the
 * runtime interface of gpu_rt.h, so the same code drives CUDA and OpenCL devices. The kernel variants, their autotuning and the
 * benchmark are in gpu_tune.c, the self-test in gpu_selftest.c. */
#include "gpu_priv.h"
#include "doctor.h"
#include "platform.h"
#include "cpu_engine.h"
#include <pzcore/hash.h>
#include "outfmt.h"
#include <unistd.h>
#include <time.h>
#include <math.h>

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
const char *gpu_fmt_devused(const rt_dev *d, size_t used, char *out, size_t n)
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

bool gpu_selftest_allowed(const rt_dev *d) { return default_choice(d) || (!d->is_gpu && pz_env_on("PZ_OPENCL_ALL")) || (d->dup_of >= 0 && pz_env_on("PZ_OPENCL_ALL")); }

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
    if (!n) { printf("  none (%s): the CPU only\n", note[0] ? note : "no GPU runtime"); return 0; }
    for (int i = 0; i < n; i++) {
        const rt_dev *d = &devs[i];
        char mem[160], extra[96] = "";
        if (d->api == RT_CUDA) snprintf(extra, sizeof extra, ", compute %d.%d", d->cc / 10, d->cc % 10);
        const char *use = default_choice(d) ? "used by default"
                        : d->dup_of >= 0 ? "the same GPU as [%d] via OpenCL: a fallback if CUDA fails, or choose it with --gpu %d"
                                         : "not used by default: choose it with --gpu %d";
        char usebuf[160];
        snprintf(usebuf, sizeof usebuf, use, d->dup_of >= 0 && !default_choice(d) ? d->dup_of : i, i);
        char ln[900];
        printf("  [%d] %s via %s\n", i, d->name, rt_api_name(d->api));
        snprintf(ln, sizeof ln, "      %s, %u %s at %u MHz%s, %s; %s", d->is_gpu ? "GPU" : "CPU device", d->cu, d->api == RT_CUDA ? "SM" : "CU", d->mhz, extra,
                 fmt_devmem(d, mem, sizeof mem), usebuf);
        pz_fputs_wrap(stdout, ln, PZ_LINE_MAX, 8);
        putchar('\n');
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

void gpu_session_release(gpu_session *s)
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

bool gpu_session_open(gpu_session *s, const rt_dev *d)
{
    s->info = *d;
    s->c = rt_open(d);
    return s->c != NULL;
}

static void make_gtab(uint32_t *tab, int half)
{
    pz_ge *g = pz_xmalloc(sizeof(pz_ge) * (size_t)half);
    cpu_gmultiples(g, half);
    for (int i = 0; i < half; i++) pack_ge(tab + i * 16, &g[i]);
    free(g);
}

static _Atomic int g_boost;   /* service threads above the CPU workers' priority (gpu_set_boost) */
/* Called by a service thread before every launch: applies the requested priority when it changed. A refusal is not retried. */
static void boost_sync(int *cur)
{
    const int want = atomic_load_explicit(&g_boost, memory_order_relaxed);
    if (want == *cur) return;
    *cur = want;
    (void)pz_thread_boost(want != 0);
}

/* ---------------- linear program ---------------- */

void gpu_linear_release_program(gpu_session *s)
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

bool gpu_linear_prepare_variant(gpu_session *s, int variant)
{
    gpu_linear_release_program(s);
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

bool gpu_linear_prepare(gpu_session *s) { return gpu_linear_prepare_variant(s, s->variant ? s->variant : gpu_default_variant(&s->info)); }

/* Runs one launch covering at least nkeys keys; fills hits. Returns false on API errors. */
bool gpu_linear_run(gpu_session *s, const u8 target[20], lin_launch *L)
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
    pz_u256 center = L->key_start;
    pz_u256_add_u64(&center, &center, HALFK);
    pz_ge P0, D, GS;
    if (!pz_ec_mul_g(&P0, &center)) return false;
    pz_u256 step = pz_u256_from_u64((u64)L->ngroups * GRPK);
    if (!pz_ec_mul_g(&D, &step)) return false;
    pz_u256 gs = pz_u256_from_u64(GRPK);
    if (!pz_ec_mul_g(&GS, &gs)) return false;
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

/* ---------------- sessions ---------------- */

/* Opens and self-tests one device; false (logged, session released) when it cannot be used. */
static bool try_device(run_ctx *r, gpu_session *s, const rt_dev *d, bool for_kangaroo)
{
    memset(s, 0, sizeof *s);
    s->r = r;
    char mem[160];
    pz_log(PZ_LOG_INFO, "GPU[%d]: %s via %s, %u %s, %s - checking correctness...", d->index, d->name, rt_api_name(d->api), d->cu,
           d->api == RT_CUDA ? "SM" : "CU", fmt_devmem(d, mem, sizeof mem));
    bool ok = gpu_session_open(s, d) && gpu_test_fe(s);
    if (ok && !for_kangaroo) gpu_linear_autotune(s, d->index);
    if (ok && for_kangaroo) gpu_kang_autotune(s, d->index);
    ok = ok && (for_kangaroo ? gpu_test_kang(s) : gpu_test_linear(s));
    if (!ok) {
        pz_log(PZ_LOG_ERR, "GPU[%d] %s (%s): self-test failed, device excluded", d->index, d->name, rt_api_name(d->api));
        gpu_session_release(s);
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

static void cpu_support_hit(void *user, const pz_u256 *key) { run_submit_key((run_ctx *)user, key, "CPU (GPU support)"); }

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

/* The size (groups per work-item of the linear kernel, rounds of the kangaroo) of the launches that follow a failed one: a quarter, at
 * least 1. A GPU shared with a game or a desktop can slow down so much that a launch tuned to 0.35 s runs past the 2 s of the watchdog
 * of the system; the same size after the recovery would fail again, four times, and the GPU would be given up. The size grows back
 * after the successful launches by the timing rule of the loops. */
uint32_t gpu_launch_after_failure(uint32_t n) { return n >= 4 ? n / 4 : 1; }

bool gpu_health_failed(gpu_health *h)
{
    h->good = false;
    h->streak++;
    h->total++;
    return h->streak > GPU_FAIL_STREAK || h->total > GPU_FAIL_TOTAL;
}

void gpu_health_ok(gpu_health *h, double now)
{
    h->streak = 0;
    if (!h->good) { h->good = true; h->since = now; }
    else if (h->total && now - h->since >= GPU_FAIL_FORGET) h->total = 0;      /* long enough without a failure: the earlier ones are forgotten */
}

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
        if (kangaroo) gpu_kang_release(s); else gpu_linear_release_program(s);
        gpu_session_release(s);                           /* clears everything after the context */
        s->T = keep.T; s->NK = keep.NK; s->kvar = keep.kvar; s->variant = keep.variant; s->l2fit = keep.l2fit;
        atomic_store(&s->paused, paused);
        pz_sleep(wait);
        wait = wait < 30 ? wait * 2 : 30;
        if (!gpu_session_open(s, &info)) continue;
        bool ok = gpu_test_fe(s) && (kangaroo ? gpu_test_kang(s) : gpu_test_linear(s));
        if (ok && !kangaroo) { gpu_linear_release_program(s); ok = gpu_linear_prepare_variant(s, keep.variant); }
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
    gpu_health health = {0};
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
        if (!gpu_linear_run(s, r->cfg.target_h160, &L)) {
            sched_complete(r->sched, &c, 0);                  /* the block goes back to the pool */
            if (atomic_load(&r->stop)) break;
            pz_log(PZ_LOG_WARN, "GPU[%d] %s: execution error (it happens after the system sleeps): reopening the device", s->info.index, s->info.name);
            ngroups = gpu_launch_after_failure(ngroups);
            if (gpu_health_failed(&health) || !gpu_recover(s, false)) {
                pz_log(PZ_LOG_ERR, "GPU %s: the device does not respond, excluding it", s->info.name);
                break;
            }
            pz_log(PZ_LOG_INFO, "GPU[%d]: device resumed (launches reduced to %u groups per work-item, growing back while they succeed)", s->info.index, ngroups);
            continue;
        }
        const double t1 = pz_mono();
        gpu_health_ok(&health, t1);
        double dt = t1 - t0;
        u64 covered = c.nkeys;
        if (L.err) {
            pz_log(PZ_LOG_WARN, "GPU %s: numeric anomaly in the block, rescanning on the CPU", s->info.name);
            cpu_scan_claim(r, &c, &covered);
        }
        for (uint32_t i = 0; i < L.nhits; i++) {
            pz_u256 k = c.key_start;
            pz_u256_add_u64(&k, &k, L.hits[i]);
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
        gpu_session_release(&s[i]);
    }
    free(s);
}

/* ---------------- kangaroo ---------------- */

#define JSTRIDE 20

void gpu_kang_release(gpu_session *s)
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
bool gpu_kang_prepare(gpu_session *s, const kang_params_t *P, int T, uint32_t hist_rounds)
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
bool gpu_kang_run(gpu_session *s, const kang_params_t *P, uint32_t rounds, uint32_t *ndp, uint32_t *nrl)
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

int gpu_kangaroo_start(run_ctx *r, gpu_session **out, int *k_total)
{
    *k_total = 0;
    pz_u256 rsz = cfg_range_size(&r->cfg);
    const long double sqrt_w = sqrtl(pz_u256_to_ld(&rsz));
    gpu_session *arr = NULL;
    int n = open_sessions(r, &arr, true);
    *out = arr;
    for (int i = 0; i < n; i++) {
        gpu_session *s = &arr[i];
        size_t t = gpu_kang_threads(s);
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
    if (!gpu_kang_prepare(s, &ks->P, s->T, KANG_HIST_ROUNDS)) return false;
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
    gpu_health health = {0};
    int recoveries = 0;                                    /* the herds rebuilt so far: each one starts from other points */
    if (!kang_gpu_boot(s, ks, &mirror, 1000 + (u64)s->info.index)) { pz_log(PZ_LOG_ERR, "GPU %s: kangaroo setup failed", s->info.name); goto done; }
    {
        char mu[80];
        pz_log(PZ_LOG_INFO, "GPU[%d]: %d kangaroos (%d work-items x %d), memory used %s", s->info.index, K, T, s->NK, gpu_fmt_devused(&s->info, s->mem_used, mu, sizeof mu));
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
        if (!gpu_kang_run(s, &ks->P, rounds, &ndp, &nrl)) goto device_error;
        const double t1 = pz_mono();
        gpu_health_ok(&health, t1);
        double dt = t1 - t0;
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
            if (o[8] < (uint32_t)K) kang_dp_recode(mirror, &recs[i], (int)o[8]);
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
        rounds = gpu_launch_after_failure(rounds);
        if (gpu_health_failed(&health) || !gpu_recover(s, true) || !kang_gpu_boot(s, ks, &mirror, 1000 + (u64)s->info.index + 7919ULL * (u64)++recoveries)) {
            pz_log(PZ_LOG_ERR, "GPU %s: the device does not respond, excluding it", s->info.name);
            break;
        }
        pz_log(PZ_LOG_INFO, "GPU[%d]: device resumed with a new herd (launches reduced to %u rounds, growing back while they succeed)", s->info.index, rounds);
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
        if (gpu_session_open(s, d)) {
            const bool ok = gpu_test_fe(s);
            gpu_session_release(s);
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
