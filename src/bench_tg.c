/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* bench_tg.c - the scan shared by 'bench --together' and the first-run calibration (see bench_tg.h). */
#include "bench_tg.h"
#include "platform.h"
#include "envopts.h"
#include "outfmt.h"
#include <math.h>
#include <stdarg.h>

typedef struct { int cap; unsigned off; double cpu, gpu[16], total; } tg_res;

int tg_silent;

void tg_section(const char *title)
{
    if (!tg_silent) pz_section(title);
}

void tg_say(const char *fmt, ...)
{
    if (tg_silent) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (char *q = buf; *q; q++) {                       /* no blanks at the end of a line */
        if (*q != ' ') continue;
        char *e = q;
        while (*e == ' ') e++;
        if (*e == '\n' || !*e) memmove(q, e, strlen(e) + 1);
        else q = e - 1;
    }
    pz_fputs_wrap(stdout, buf, PZ_LINE_MAX, 4);
    fflush(stdout);
}

/* Sleeps `secs` seconds, less when the run is stopped (r->stop) or the program is interrupted (Ctrl-C): then the stop flag of the run is set, so
 * that the scan ends at the next check instead of after the measurements that are left. */
static void tg_sleep(run_ctx *r, double secs)
{
    const double end = pz_mono() + secs;
    for (double now; !atomic_load(&r->stop) && (now = pz_mono()) < end;) {
        if (run_interrupted()) { atomic_store(&r->stop, 1); break; }
        pz_sleep(end - now < 0.05 ? end - now : 0.05);
    }
}

/* Rates (units/s) with `cap` CPU workers running and the GPUs whose bit is set in `off` idle. After `settle` seconds,
 * over `window` seconds: the GPU counts only change at the end of a launch (about every 0.35 s), so a long window keeps
 * that granularity near 2%. */
static tg_res tg_run(const tg_scan_cfg *c, int cap, unsigned off, double settle, double window)
{
    run_ctx *r = c->r;
    c->set_cpu(c->ctx, cap);
    for (int j = 0; j < c->ngpu; j++) gpu_pause(gpu_session_at(c->g, j), (off >> j) & 1);
    tg_sleep(r, settle);
    u64 a[MAX_WORKERS];
    const int nws = r->nws;
    for (int i = 0; i < nws; i++) a[i] = atomic_load(&r->ws[i].units);
    const double t0 = pz_mono();
    tg_sleep(r, window);
    const double dt = pz_mono() - t0;
    tg_res o = {cap, off, 0, {0}, 0};
    if (atomic_load(&r->stop)) return o;            /* abandoned: a window cut short measures nothing */
    for (int i = 0; i < nws; i++)
        if (r->ws[i].kind == 'C') o.cpu += (double)(atomic_load(&r->ws[i].units) - a[i]) / dt;
    o.total = o.cpu;
    for (int j = 0; j < c->ngpu; j++) {
        o.gpu[j] = (double)(atomic_load(&r->ws[c->gw[j]].units) - a[c->gw[j]]) / dt;
        o.total += o.gpu[j];
    }
    return o;
}

/* The columns of the table: threads, total, CPU, then one column per GPU (a switched-off GPU shows "off"); beyond 5 GPUs one column
 * with their sum. */
#define TG_MAXCOLS 5

static void tg_header(const tg_scan_cfg *c)
{
    char line[300];
    size_t n = (size_t)snprintf(line, sizeof line, "  %-12s%12s%12s", "threads", "total", "CPU");
    if (c->ngpu <= TG_MAXCOLS) {
        for (int j = 0; j < c->ngpu; j++) {
            char h[16];
            snprintf(h, sizeof h, "GPU[%d]", c->gidx[j]);
            n += (size_t)snprintf(line + n, sizeof line - n, "%12s", h);
        }
    } else n += (size_t)snprintf(line + n, sizeof line - n, "%12s", "GPUs");
    tg_say("%s    (%s)\n", line, c->unit);
}

static void tg_print_tag(const tg_scan_cfg *c, const tg_res *o, const char *tag)
{
    char b[32], line[300];
    char th[24];
    snprintf(th, sizeof th, "%d thread%s", o->cap, o->cap == 1 ? "" : "s");
    size_t n = (size_t)snprintf(line, sizeof line, "  %-12s", th);
    n += (size_t)snprintf(line + n, sizeof line - n, "%12s", pz_fmt_cell(o->total, b, sizeof b));
    n += (size_t)snprintf(line + n, sizeof line - n, "%12s", pz_fmt_cell(o->cpu, b, sizeof b));
    if (c->ngpu <= TG_MAXCOLS) {
        for (int j = 0; j < c->ngpu; j++)
            n += (size_t)snprintf(line + n, sizeof line - n, "%12s", (o->off >> j) & 1 ? "off" : pz_fmt_cell(o->gpu[j], b, sizeof b));
    } else {
        double sum = 0;
        for (int j = 0; j < c->ngpu; j++) sum += (o->off >> j) & 1 ? 0 : o->gpu[j];
        n += (size_t)snprintf(line + n, sizeof line - n, "%12s", pz_fmt_cell(sum, b, sizeof b));
    }
    tg_say("%s%s%s\n", line, tag ? "    " : "", tag ? tag : "");
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
    tg_printf("  priority of the GPU threads, normal and high, in alternating pairs (all CPU threads):\n");
    tg_header(c);
    gpu_set_boost(false);
    tg_res off1 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &off1, "normal priority");
    if (atomic_load(&c->r->stop)) return false;
    gpu_set_boost(true);
    tg_res on1 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &on1, "high priority");
    *gain = off1.total > 0 ? 100.0 * (on1.total / off1.total - 1.0) : 0.0;
    if (atomic_load(&c->r->stop) || !(on1.total > off1.total * 1.01)) return false;
    tg_res on2 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &on2, "high priority");
    gpu_set_boost(false);
    tg_res off2 = tg_run(c, c->full, 0, settle, window);
    tg_print_tag(c, &off2, "normal priority");
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
    const double sc = g_env.bench_scale, settle = 2.0 * sc, window = 8.0 * sc;
    tg_printf("  each configuration: %.0f s of settling, then %.0f s of measuring (the noise is typically 2-3%%)\n", settle, window);
    /* The first configuration is the default one: measured right after the herds are built, with the GPU clocks still rising, it
     * would look worse than it is. A warm-up at the default settings comes first and is thrown away. */
    if (!atomic_load(&r->stop)) { tg_printf("  warm-up: %.0f s at the default configuration (not counted)\n", window); (void)tg_run(c, c->full, 0, 0, window); }
    if (c->prio == PRIO_AUTO && c->ngpu > 0 && pz_boost_possible() && !atomic_load(&r->stop)) {
        double g = 0;
        out.boost_tested = true;
        out.boost = tg_prio_test(c, settle, window, 2.0, &g);
        out.boost_gain_pct = g;
        gpu_set_boost(out.boost);
        if (!atomic_load(&r->stop)) tg_printf("  -> high priority for the GPU threads: %+.1f%%, %s\n", g, out.boost ? "worth it: used for the measurements that follow" : "not worth it: stays normal");
    }
    tg_header(c);
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
        tg_printf("  confirming the best against the default, in alternating pairs:\n");
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
        tg_printf("  the first configuration measured again: %+.1f%%; differences of that size between configurations are noise or heat drift\n",
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
        tg_printf("  -> high priority for the GPU threads: %+.1f%%, confirmed in alternating pairs: add --prio-gpu yes\n", ch->boost_gain_pct);
    if (!ch->better) {
        tg_printf("  -> no configuration is clearly better than the default (%s %s): leave the default values%s\n",
                  pz_fmt_si(ch->def_rate, b1, sizeof b1), unit, ch->boost ? ", with high priority for the GPU threads" : "");
        return;
    }
    tg_gpu_list(ch, gl, sizeof gl);
    tg_printf("  -> best: %s %s, %+.1f%% over the default (%s) when confirmed in pairs: start with -t %d%s%s%s\n",
           pz_fmt_si(ch->best_rate, b1, sizeof b1), unit, ch->gain_pct, pz_fmt_si(ch->def_rate, b2, sizeof b2), ch->cap, gl[0] ? " --gpu " : "", gl, prio);
}
