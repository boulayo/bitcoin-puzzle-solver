/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* gpu_bench.c - the GPU benchmark ('bench') and the table of every variant of the kernels ('bench --variants'). */
#include "gpu_tune_priv.h"
#include "platform.h"
#include "outfmt.h"
#include <math.h>
#include <stdarg.h>

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
            if (!gpu_tune_linear_probe(s, &start, ng, &L, &dt)) break;
            keys += L.nkeys;
            el = pz_mono() - t0;
            if (el > 4.0) break;
            if (dt < 0.3 && ng < 4096) ng *= 2;
        }
        char b[32];
        el = pz_mono() - t0;
        char nb[48];
        pz_prop_line("linear search", "%s keys/s, window %s, launches of %.2f s", pz_fmt_si((double)keys / el, b, sizeof b), gpu_variant_name(s->variant, nb, sizeof nb), s->launch_s);
        fflush(stdout);
        gpu_linear_release_program(s);
        gpu_kang_autotune(s, i);
        double kr = gpu_tune_kang_measure(s);
        if (kr > 0) {
            char mu[80];
            pz_prop_line("kangaroo", "%s steps/s, %s, launches of %.2f s; memory used %s", pz_fmt_si(kr, b, sizeof b), gpu_kvariant_name(gpu_tune_kang_code(s), nb, sizeof nb),
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
        rate[i] = gpu_tune_median(smp[i], nsmp[i]);
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
    const int pick = gpu_tune_launch_choose(rate, n);
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
    gpu_tune_turns(s, &op, turns, false, di, smp, nsmp);
    double saved_s = GPU_LAUNCH_S;
    const int saved = gpu_tune_cache_get(&s->info, op.kang, &saved_s);
    bench_variants_table(&op, forced, turns, smp, nsmp, saved);
    /* the rule of the tuner on these medians: only the variants it measures (not those only forced) */
    double rate[32] = {0};
    for (int i = 0; i < op.nv; i++) rate[i] = forced[i] ? 0 : gpu_tune_median(smp[i], nsmp[i]);
    const int pick = gpu_tune_choose(&op, rate, op.nv, di);
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
        gpu_tune_cache_put(&s->info, op.kang, op.code(s), len);
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
        gpu_tune_linear_ops(&devs[i], &op);
        bench_variants_kernel(s, i, &op, turns, gpu_retune_requested(), 0, NULL);
        gpu_linear_release_program(s);
        int fv[8];
        const int nf = kvariants_forced_only(&devs[i], fv);
        gpu_tune_kang_set_variant(s, gpu_tune_default_kvariant(&devs[i]));
        gpu_tune_kang_ops(&devs[i], &op);
        bench_variants_kernel(s, i, &op, turns, gpu_retune_requested(), nf, fv);
        gpu_kang_release(s);
        gpu_session_release(s);
        free(s);
    }
    if (!titled) printf("no GPU to measure%s%s\n", note[0] ? ": " : "", note);
    free(r);
}
