/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* gpu_tune_selftest.c - the self-test of the choice of a variant: the tuner run on invented rates and the helpers it uses (no device needed). */
#include "gpu_tune_priv.h"
#include "platform.h"
#include "envopts.h"
#include <math.h>

/* ---------------- self-test of the choice of a variant ---------------- */

/* gpu_tune_pick with invented rates: when the default stays, when another wins, what is saved and what is logged. No device is needed. */
static struct {
    double rate[3]; int measured; bool check_ok; int set_to; const double (*seq)[3]; int nth[3];
    double len_rate[4]; uint32_t len_size[4]; int len_calls;      /* the rates at the four lengths of a launch and the controller's sizes for them */
    int stop_after;                                               /* the measurement that stops the campaign of the session (0: none), as a Ctrl-C would */
} g_fake;
static const int FAKE_V[3] = {128, 256, 512};

static double fake_measure(gpu_session *s, int v)
{
    g_fake.measured++;
    if (g_fake.stop_after && g_fake.measured == g_fake.stop_after && s->r) atomic_store(&s->r->stop, 1);
    gpu_tune_linear_set_variant(s, v);
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
    tune_ops op = {.kang = false, .nv = 3, .def = 256, .measure = fake_measure, .set = gpu_tune_linear_set_variant, .check = fake_check, .code = gpu_tune_linear_code,
                   .name = gpu_variant_name, .code_text = gpu_variant_code, .size_unit = "groups", .cached_fmt = "GPU[%d]: cached %s", .intro_fmt = "GPU[%d]: tuning %d variants", .head = "table", .unit = "u/s", .entry = "window ",
                   .bad_check_fmt = "GPU[%d]: variant %s failed"};
    memcpy(op.vs, FAKE_V, sizeof FAKE_V);
    return op;
}

#define TCHK(cond) do { if (!(cond)) { fails++; printf("  GPU tuning: check failed at line %d: %s\n", __LINE__, #cond); } } while (0)

/* The choice for each of several sets of invented rates: the variant picked, the lines logged and the saved choice that the next device of the
 * model takes (and the measurement again when nothing was saved) */
static int tune_check_choices(void)
{
    int fails = 0;
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
        gpu_tune_pick(s, 3, &op);
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
        gpu_tune_pick(s, 3, &op);
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
    return fails;
}

/* a choice that fails the bit-exact test is not kept and not saved: the default, a warning, and the next device measures again */
static int tune_check_bad_choice(void)
{
    int fails = 0;
    gpu_session *s = fake_session("Test GPU bad");
    g_fake.rate[0] = 100e6; g_fake.rate[1] = 100e6; g_fake.rate[2] = 120e6;
    g_fake.check_ok = false;
    g_fake.measured = 0;
    tune_ops op = fake_ops();
    fake_log lg;
    memset(&lg, 0, sizeof lg);
    rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
    rt_logseq_enter(q, 0);
    gpu_tune_pick(s, 0, &op);
    TCHK(s->variant == 256 && lg.n == 4 && !strcmp(lg.line[3], "WARN GPU[0]: variant 512 failed"));
    g_fake.measured = 0;
    gpu_tune_pick(s, 0, &op);
    TCHK(g_fake.measured >= 3);
    rt_logseq_done(q, 0);
    rt_logseq_free(q);
    free(s);
    return fails;
}

/* a stop of the campaign (Ctrl-C) in the middle of the measurements ends them at once: the default, no table, nothing saved (the next start
 * measures again) */
static int tune_check_stopped(void)
{
    int fails = 0;
    static run_ctx run;
    gpu_session *s = fake_session("Test GPU stopped");
    s->r = &run;
    atomic_store(&run.stop, 0);
    g_fake.rate[0] = 100e6; g_fake.rate[1] = 100e6; g_fake.rate[2] = 120e6;
    g_fake.check_ok = true;
    g_fake.measured = 0;
    g_fake.stop_after = 2;
    tune_ops op = fake_ops();
    fake_log lg;
    memset(&lg, 0, sizeof lg);
    rt_logseq *q = rt_logseq_new(1, fake_sink, &lg);
    rt_logseq_enter(q, 0);
    gpu_tune_pick(s, 5, &op);
    TCHK(g_fake.measured == 2 && s->variant == 256 && lg.n == 1);
    g_fake.stop_after = 0;
    atomic_store(&run.stop, 0);
    g_fake.measured = 0;
    gpu_tune_pick(s, 5, &op);
    TCHK(g_fake.measured >= 3 && s->variant == 512);
    rt_logseq_done(q, 0);
    rt_logseq_free(q);
    free(s);
    return fails;
}

/* the turns and the median: window 512 is lucky in the first turn only and loses, window 128 is far behind and is measured once,
 * the default and window 512 three times each */
static int tune_check_turns(void)
{
    int fails = 0;
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
    gpu_tune_pick(s, 2, &op);
    TCHK(s->variant == 256 && g_fake.measured == 7 && g_fake.nth[0] == 1 && g_fake.nth[1] == 3 && g_fake.nth[2] == 3);
    TCHK(lg.n == 3 && !strcmp(lg.line[1], "GPU[2]: table (u/s): window 128 80.00 M; window 256 100.00 M; window 512 100.00 M; picking window 256 (default; none >1% faster)"));
    g_fake.seq = NULL;
    rt_logseq_done(q, 0);
    rt_logseq_free(q);
    free(s);
    return fails;
}

/* with the warning the kangaroo's table has for a variant that cannot run, and for none that could */
static int tune_check_kang_no_data(void)
{
    int fails = 0;
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
    gpu_tune_pick(s, 1, &op);
    TCHK(s->variant == 256 && lg.n == 2 && !strcmp(lg.line[1], "WARN GPU[1]: table (u/s): 128 cannot run; 256 cannot run; 512 cannot run: using the default 256"));
    rt_logseq_done(q, 0);
    rt_logseq_free(q);
    free(s);
    return fails;
}

/* the wide kangaroo herds: offered where they add work-items, with at most 128 per compute unit on an Intel GPU and at most as many
 * walkers as the default CUDA herd, and only as the variants the tuner measures */
static int tune_check_wide_herds(void)
{
    int fails = 0;
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
        if (wc[c].probe) TCHK(gpu_tune_kang_threads_for(&d, KV_NK(wc[c].probe), false, KV_WIDE(wc[c].probe), false) == wc[c].threads);
        if (nwide != wc[c].nwide) printf("  GPU tuning: case %zu offers %d wide herds\n", c, nwide);
    }
    return fails;
}

/* The names and the codes of the variants as the tables print them and PZ_GPU_NK takes them */
static int tune_check_variant_names(void)
{
    int fails = 0;
    char nb[64];
    TCHK(!strcmp(gpu_kvariant_name(64 | 1 << 16 | KV_WIDEFLAG(KV_WIDE_X4), nb, sizeof nb), "NK 64 pairs, 4x herd"));
    TCHK(!strcmp(gpu_kvariant_name(64 | KV_WIDEFLAG(KV_WIDE_FULL), nb, sizeof nb), "NK 64, full herd"));
    TCHK(!strcmp(gpu_kvariant_name(4 | KV_BLOCK << 16, nb, sizeof nb), "NK 4 block herd"));
    TCHK(!strcmp(gpu_kvariant_name(128 | 2 << 16 | KV_METAFLAG, nb, sizeof nb), "NK 128 quads, tags once"));
    TCHK(!strcmp(gpu_kvariant_name(64 | KV_METAFLAG | KV_L2FLAG, nb, sizeof nb), "NK 64, tags once, L2 herd"));
    TCHK(!strcmp(gpu_kvariant_code(128 | 2 << 16 | KV_METAFLAG, nb, sizeof nb), "128cm") && !strcmp(gpu_kvariant_code(64 | 1 << 16 | KV_L2FLAG, nb, sizeof nb), "64bl"));
    TCHK(!strcmp(gpu_kvariant_code(64 | KV_METAFLAG | KV_L2FLAG, nb, sizeof nb), "64ml") && !strcmp(gpu_kvariant_code(64 | KV_WIDEFLAG(KV_WIDE_FULL), nb, sizeof nb), "-"));
    return fails;
}

/* The block herd: never tuned; two blocks of 128 work-items per SM with CUDA */
static int tune_check_block_herd(void)
{
    int fails = 0;
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
        TCHK(gpu_tune_kang_threads_for(&d, 4, false, 0, true) == (api ? 20u * KANG_GROUP : 20u * KANG_BLOCK_ITEMS_CU));
    }
    return fails;
}

/* reading the saved choices: the last line of the key counts, other keys and an empty value do not disturb it */
static int tune_check_saved_file(void)
{
    int fails = 0;
    gpu_session *s = fake_session("Test GPU file");
    char path[512], key[400], text[1700];
    TCHK(gpu_tune_cache_path(path, sizeof path));
    gpu_tune_key(&s->info, false, key, sizeof key);
    snprintf(text, sizeof text, "%sx=128\n%s=128\nother=64\n%s=512\n", key, key, key);
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, NULL) == 512);
    snprintf(text, sizeof text, "%s=512\n%s=\n128\n", key, key);           /* the last line has no value: the digits below it are another line */
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, NULL) == 0);
    snprintf(text, sizeof text, "%s=300\n", key);                            /* a window this device does not offer */
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, NULL) == 0);
    snprintf(text, sizeof text, "%s=256", key);                               /* no newline at the end */
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, NULL) == 256);
    /* the length of a launch after the variant; a line without it (the files of 1.19.x), or with one out of range, means 0.35 s */
    double ls = 0;
    snprintf(text, sizeof text, "%s=512 0.70\n", key);
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - 0.7) < 1e-9);
    snprintf(text, sizeof text, "%s=512\n", key);
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
    snprintf(text, sizeof text, "%s=512 7\n%s=512 0.0\n", key, key);
    TCHK(pz_write_file_atomic(path, text, strlen(text), 0644) == 0 && gpu_tune_cache_get(&s->info, false, &ls) == 512 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
    TCHK(gpu_tune_value("", &ls) == 0 && gpu_tune_value("\n512 0.7", &ls) == 0 && fabs(ls - GPU_LAUNCH_S) < 1e-9);
    free(s);
    return fails;
}

/* the length of a launch: the shortest within 1% of the best; a length the device cannot reach (the same size) is not tried; saved
 * with the variant and read back by the next device of the model */
static int tune_check_launch_length(void)
{
    int fails = 0;
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
        gpu_tune_pick(s, 4, &op);
        TCHK(s->variant == 512 && fabs(s->launch_s - lc[c].picked) < 1e-9 && g_fake.len_calls == lc[c].calls);
        if (fabs(s->launch_s - lc[c].picked) >= 1e-9) printf("  GPU tuning: length case %zu picked %.2f s\n", c, s->launch_s);
        char want[80];
        snprintf(want, sizeof want, "GPU[4]: launches of %.2f s (", lc[c].picked);
        TCHK(lc[c].rate[0] <= 0 || (lg.n == 5 && !strncmp(lg.line[4], want, strlen(want))));
        if (lc[c].rate[0] <= 0) TCHK(lg.n == 4 && !strncmp(lg.line[3], "WARN GPU[4]: launch length (u/s; the size in groups): 0.15 s cannot run;", 72));
        s->launch_s = 0;
        s->variant = 0;
        g_fake.measured = 0;
        gpu_tune_pick(s, 4, &op);                                     /* the next device of the model: the saved variant and length */
        TCHK(g_fake.measured == 0 && s->variant == 512 && fabs(s->launch_s - lc[c].picked) < 1e-9);
        rt_logseq_done(q, 0);
        rt_logseq_free(q);
        free(s);
    }
    const double a[4] = {100, 100.5, 101, 101.5}, b[4] = {-1, 0, 100, 99.5}, z[4] = {0, 0, -1, 0};
    TCHK(gpu_tune_launch_choose(a, 4) == 1 && gpu_tune_launch_choose(b, 4) == 2 && gpu_tune_launch_choose(z, 4) == -1);   /* 100.5 is within 1% of 101.5, 100 is not */
    return fails;
}

/* The codes built from their fields are the numbers that PZ_GPU_HALF, PZ_GPU_NK and gpu_tune.txt have always had, and the text of PZ_GPU_NK reads
 * back into them (and nothing else does). */
static int tune_check_variant_encoding(void)
{
    int fails = 0;
    TCHK(VAR_MAKE(256, 4) == 262400 && VAR_MAKE(128, 0) == 128 && VAR_HALF(262400) == 256 && VAR_MINB(262400) == 4);
    TCHK(KV_MAKE(128, 2, 1, 0, 0) == 1179776 && KV_MAKE(64, 1, 1, 1, KV_WIDE_FULL) == 118554688 && KV_MAKE(4, KV_BLOCK, 0, 0, 0) == 196612);
    TCHK(KV_MAKE(64, 0, 1, 1, 0) == 17825856 && KV_MAKE(64, 1, 0, 1, KV_WIDE_FULL) == 117506112);
    const int v = 118554688;
    TCHK(KV_NK(v) == 64 && KV_CFG(v) == 1 && KV_META(v) == 1 && KV_L2(v) == 1 && KV_WIDE(v) == KV_WIDE_FULL && KV_KERNEL(v) == (64 | 1 << 16 | 1 << 20));
    static const struct { const char *text; int code; } good[] = {
        {"128", 128}, {"64b", 64 | 1 << 16}, {"256c", 256 | 2 << 16}, {"4g", 4 | 3 << 16}, {"128cm", 1179776}, {"64l", 64 | 1 << 24}, {"64ml", 17825856},
        {"32bml", 32 | 1 << 16 | 1 << 20 | 1 << 24}, {"65535", 65535},
    };
    int code = -1;
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) TCHK(gpu_kvariant_parse(good[i].text, &code) && code == good[i].code);
    static const char *const bad[] = {"", "0", "b", "12k", "64lm", "64bb", "64bc", "64x", "65536", "99999999999", "-4", " 64", "64 ", "64b ", "m"};
    code = -1;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) TCHK(!gpu_kvariant_parse(bad[i], &code) && code == -1);
    char text[24];
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        gpu_kvariant_code(good[i].code, text, sizeof text);
        TCHK(!strcmp(text, good[i].text) || !strcmp(good[i].text, "65535"));
        TCHK(gpu_kvariant_parse(text, &code) && code == good[i].code);
    }
    TCHK(!strcmp(gpu_variant_code(262400, text, sizeof text), "262400") && !strcmp(gpu_variant_name(262400, text, sizeof text), "256, register cap"));
    TCHK(!strcmp(gpu_kvariant_cl_config(0), "") && !strcmp(gpu_kvariant_cl_config(1), " -DMC=2") && !strcmp(gpu_kvariant_cl_config(2), " -DMC=4"));
    TCHK(!strcmp(gpu_kvariant_cl_config(3), " -DKANG_BLOCK=1") && !strcmp(gpu_kvariant_cl_config(4), "") && !strcmp(gpu_kvariant_cl_config(15), ""));
    TCHK(!strcmp(gpu_kvariant_cl_meta(0), "") && !strcmp(gpu_kvariant_cl_meta(1), " -DKANG_META=1"));
    return fails;
}

/* The tuner on invented rates, the saved choices, the herds and the names of the variants; the saved choices of the test go to a folder of its own. */
int gpu_tune_selftest(void)
{
    int fails = 0;
    char tmp[512];
    if (!pz_make_temp_dir(tmp, sizeof tmp, "pztune")) return 0;       /* nothing to test with: the other checks of the self-test say more */
    char old[PZ_ENV_PATH];
    pz_strlcpy(old, g_env.xdg_cache_home, sizeof old);
    pz_strlcpy(g_env.xdg_cache_home, tmp, sizeof g_env.xdg_cache_home);       /* the saved choices of the test go to a folder of its own */
    fails += tune_check_choices();
    fails += tune_check_bad_choice();
    fails += tune_check_stopped();
    fails += tune_check_turns();
    fails += tune_check_kang_no_data();
    fails += tune_check_wide_herds();
    fails += tune_check_variant_names();
    fails += tune_check_variant_encoding();
    fails += tune_check_block_herd();
    fails += tune_check_saved_file();
    fails += tune_check_launch_length();
    pz_strlcpy(g_env.xdg_cache_home, old, sizeof g_env.xdg_cache_home);
    pz_remove_tree(tmp);
    return fails;
}
