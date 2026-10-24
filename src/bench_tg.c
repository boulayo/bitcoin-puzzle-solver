/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bench_tg.c - the scan shared by 'bench --together' and the first-run calibration (see bench_tg.h). */
#include "bench_tg.h"
#include "platform.h"
#include <math.h>

typedef struct { int cap; unsigned off; double cpu, gpu[16], total; } tg_res;

int tg_silent;

double tg_scale(void)
{
    const char *e = getenv("PZ_BENCH_SCALE");
    if (e && e[0]) {
        char *end;
        double v = strtod(e, &end);
        if (end != e && !*end && v >= 0.01 && v <= 100) return v;
    }
    return 1.0;
}

/* Rates (units/s) with `cap` CPU workers running and the GPUs whose bit is set in `off` idle. After `settle` seconds,
 * over `window` seconds: the GPU counts only change at the end of a launch (about every 0.35 s), so a long window keeps
 * that granularity near 2%. */
static tg_res tg_run(const tg_scan_cfg *c, int cap, unsigned off, double settle, double window)
{
    run_ctx *r = c->r;
    c->set_cpu(c->ctx, cap);
    for (int j = 0; j < c->ngpu; j++) gpu_pause(gpu_session_at(c->g, j), (off >> j) & 1);
    pz_sleep(settle);
    u64 a[MAX_WORKERS];
    const int nws = r->nws;
    for (int i = 0; i < nws; i++) a[i] = atomic_load(&r->ws[i].units);
    const double t0 = pz_mono();
    pz_sleep(window);
    const double dt = pz_mono() - t0;
    tg_res o = {cap, off, 0, {0}, 0};
    for (int i = 0; i < nws; i++)
        if (r->ws[i].kind == 'C') o.cpu += (double)(atomic_load(&r->ws[i].units) - a[i]) / dt;
    o.total = o.cpu;
    for (int j = 0; j < c->ngpu; j++) {
        o.gpu[j] = (double)(atomic_load(&r->ws[c->gw[j]].units) - a[c->gw[j]]) / dt;
        o.total += o.gpu[j];
    }
    return o;
}

static void tg_print_tag(const tg_scan_cfg *c, const tg_res *o, const char *tag)
{
    char b[32], cb[32], line[400];
    size_t n = (size_t)snprintf(line, sizeof line, "  CPU %2d thread%s", o->cap, o->cap == 1 ? "" : "s");
    for (int j = 0; j < c->ngpu; j++)
        if (!((o->off >> j) & 1)) n += (size_t)snprintf(line + n, sizeof line - n, " + GPU%d", c->gidx[j]);
    tg_printf("%-34s %10s %s  (CPU %s", line, pz_fmt_si(o->total, b, sizeof b), c->unit, pz_fmt_si(o->cpu, cb, sizeof cb));
    for (int j = 0; j < c->ngpu; j++)
        if (!((o->off >> j) & 1)) tg_printf(" | GPU%d %s", c->gidx[j], pz_fmt_si(o->gpu[j], b, sizeof b));
    tg_printf(")%s%s\n", tag ? "  " : "", tag ? tag : "");
}

static void tg_print(const tg_scan_cfg *c, const tg_res *o) { tg_print_tag(c, o, NULL); }

bool tg_prio_verdict(double off1, double on1, double on2, double off2, double min_pct, double *gain)
{
    const double a = off1 + off2, b = on1 + on2;
    const double g = a > 0 ? 100.0 * (b / a - 1.0) : 0.0;
    if (gain) *gain = g;
    if (!(off1 > 0 && off2 > 0 && on1 > 0 && on2 > 0)) return false;
    return g >= min_pct && on1 > off1 && on2 > off2;       /* better overall and in each of the two pairs: a drift cannot fake it */
}

/* The threads that serve the GPUs launch the kernels and collect the results; with every core busy computing they can wait for
 * their turn while the GPU idles. Measured at the default configuration (all CPU threads), where that is likeliest: normal then high
 * priority; only a first pair that favours the high one by 1% is confirmed by the opposite order (high, normal), which cancels a drift. */
static bool tg_prio_test(const tg_scan_cfg *c, double settle, double window, double min_pct, double *gain)
{
    tg_printf("  priority of the GPU threads, normal and high in alternating pairs (all CPU threads):\n");
    gpu_set_boost(false);
    tg_res off1 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &off1, "[normal priority]");
    if (atomic_load(&c->r->stop)) return false;
    gpu_set_boost(true);
    tg_res on1 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &on1, "[high priority]");
    *gain = off1.total > 0 ? 100.0 * (on1.total / off1.total - 1.0) : 0.0;
    if (atomic_load(&c->r->stop) || !(on1.total > off1.total * 1.01)) return false;
    tg_res on2 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &on2, "[high priority]");
    gpu_set_boost(false);
    tg_res off2 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &off2, "[normal priority]");
    if (atomic_load(&c->r->stop)) return false;
    return tg_prio_verdict(off1.total, on1.total, on2.total, off2.total, min_pct, gain);
}

static tg_choice tg_scan_inner(const tg_scan_cfg *c)
{
    run_ctx *r = c->r;
    tg_choice out = {0};
    out.ngpu = c->ngpu;
    for (int j = 0; j < c->ngpu; j++) out.gidx[j] = c->gidx[j];
    out.cap = c->full;
    out.prio = c->prio;
    out.boost = c->prio == PRIO_YES;
    gpu_set_boost(out.boost);                 /* the warm-up and (unless measured below) the whole scan run with this */

    /* CPU thread counts: all, three quarters, one per physical core, half of those, none */
    int cand[8], nc = 0;
    if (c->fixed) cand[nc++] = c->full;
    else {
        const int P = r->sys.ncore > 0 && r->sys.ncore <= c->full ? r->sys.ncore : c->full;
        const int v[5] = {c->full, (c->full * 3 + 2) / 4, P, P / 2, 0};
        for (int i = 0; i < 5; i++) {
            bool dup = false;
            for (int j = 0; j < nc; j++) dup = dup || cand[j] == v[i];
            if (!dup && v[i] <= c->full) cand[nc++] = v[i];
        }
    }
    const double sc = tg_scale(), settle = 2.0 * sc, window = 8.0 * sc;
    tg_printf("  %.0f s settling + %.0f s measuring for each configuration (typical noise: 2-3%%)\n", settle, window);
    /* The first configuration is the default one: measured right after the herds are built, with the GPU clocks still rising, it
     * would look worse than it is. A warm-up at the default settings comes first and is thrown away. */
    if (!atomic_load(&r->stop)) { tg_printf("  warm-up (%.0f s at the default configuration, not counted)\n", window); (void)tg_run(c, c->full, 0, 0, window); }
    if (c->prio == PRIO_AUTO && c->ngpu > 0 && pz_boost_possible() && !atomic_load(&r->stop)) {
        double g = 0;
        out.boost_tested = true;
        out.boost = tg_prio_test(c, settle, window, 2.0, &g);
        out.boost_gain_pct = g;
        gpu_set_boost(out.boost);
        if (!atomic_load(&r->stop)) tg_printf("  -> high priority for the GPU threads: %+.1f%% (%s)\n", g, out.boost ? "worth it: used for the measurements that follow" : "not worth it: stays normal");
    }
    tg_res res[32];
    int nres = 0, best = 0;
    for (int i = 0; i < nc && !atomic_load(&r->stop); i++) {
        res[nres] = tg_run(c, cand[i], 0, settle, window);
        tg_print(c, &res[nres]);
        if (res[nres].total > res[best].total) best = nres;
        nres++;
    }
    if (!nres) return out;
    int fastest = 0;
    for (int j = 1; j < c->ngpu; j++)
        if (res[best].gpu[j] > res[best].gpu[fastest]) fastest = j;
    for (int j = 0; j < c->ngpu && c->ngpu > 1 && !atomic_load(&r->stop); j++) {
        if (j == fastest) continue;
        int cap = res[best].cap + gpu_cores_needed(gpu_session_at(c->g, j), 1);      /* the core set aside for this GPU is free again */
        if (cap > c->total) cap = c->total;
        res[nres] = tg_run(c, cap, 1u << j, settle, window);
        tg_print(c, &res[nres]);
        if (res[nres].total > res[best].total) best = nres;
        nres++;
    }
    /* The scan runs while the machine warms up, which favours the first configurations: a candidate that beats the default is
     * confirmed by alternating the two (default, candidate, default, candidate), which cancels a slow drift. */
    double conf_a = 0, conf_b = 0, noise = 2.0;
    if (best != 0 && !atomic_load(&r->stop)) {
        tg_printf("  confirmation in alternating pairs, best against default:\n");
        double a_first = 0;
        for (int pass = 0; pass < 2 && !atomic_load(&r->stop); pass++) {
            tg_res x = tg_run(c, res[0].cap, res[0].off, settle, window);
            tg_res y = tg_run(c, res[best].cap, res[best].off, settle, window);
            tg_print(c, &x);
            tg_print(c, &y);
            conf_a += x.total / 2;
            conf_b += y.total / 2;
            if (pass == 0) a_first = x.total;
            else noise = fmax(noise, 100.0 * fabs(x.total / a_first - 1.0));
        }
    } else if (!atomic_load(&r->stop)) {
        tg_res again = tg_run(c, res[0].cap, 0, settle, window);
        noise = fmax(noise, fabs(100.0 * (again.total / res[0].total - 1.0)));
        tg_printf("  repeat of the first configuration: %+.1f%% (differences of this size above are noise or thermal drift)\n",
               100.0 * (again.total / res[0].total - 1.0));
    }
    out.def_rate = conf_a > 0 ? conf_a : res[0].total;
    out.best_rate = conf_b > 0 ? conf_b : res[0].total;
    out.gain_pct = conf_a > 0 ? 100.0 * (conf_b / conf_a - 1.0) : 0;
    if (best != 0 && conf_a > 0 && out.gain_pct >= noise + 1.0 && !atomic_load(&r->stop)) {
        out.better = true;
        out.cap = res[best].cap;
        out.off = res[best].off;
    }
    return out;
}

tg_choice tg_scan(const tg_scan_cfg *c)
{
    const bool was = gpu_boost_get();
    tg_choice out = tg_scan_inner(c);
    gpu_set_boost(was);                       /* the caller decides what the campaign runs with */
    return out;
}

void tg_gpu_list(const tg_choice *ch, char *out, size_t n)
{
    out[0] = 0;
    if (!ch->off) return;
    size_t o = 0;
    for (int j = 0; j < ch->ngpu && o + 8 < n; j++)
        if (!((ch->off >> j) & 1)) o += (size_t)snprintf(out + o, n - o, "%s%d", o ? "," : "", ch->gidx[j]);
}

void tg_print_advice(const tg_choice *ch, const char *unit)
{
    char b1[32], b2[32], gl[64];
    const char *prio = ch->boost ? " --prio-gpu yes" : "";
    if (ch->boost_tested && ch->boost)
        tg_printf("  -> high priority for the GPU threads: %+.1f%% (confirmed in alternating pairs): add  --prio-gpu yes\n", ch->boost_gain_pct);
    if (!ch->better) {
        tg_printf("  -> no configuration is clearly better than the default (%s %s): leave the default values%s\n",
                  pz_fmt_si(ch->def_rate, b1, sizeof b1), unit, ch->boost ? " (with high priority for the GPU threads)" : "");
        return;
    }
    tg_gpu_list(ch, gl, sizeof gl);
    tg_printf("  -> best: %s %s (%+.1f%% over the default configuration, %s, in the pairwise confirmation): start with  -t %d%s%s%s\n",
           pz_fmt_si(ch->best_rate, b1, sizeof b1), unit, ch->gain_pct, pz_fmt_si(ch->def_rate, b2, sizeof b2), ch->cap, gl[0] ? " --gpu " : "", gl, prio);
}
