/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* envopts.c - the one place that reads the environment (see envopts.h). */
#include "envopts.h"

#ifdef _WIN32
#define ENVIRON _environ
extern char **_environ;
#else
extern char **environ;
#define ENVIRON environ
#endif

#define ENV_INIT {.bench_scale = 1.0, .kang_mix = -1, .test_dp_cap = -1}

struct pz_env g_env = ENV_INIT;

/* The value of a variable; NULL when it is not set or empty (the program makes no difference between the two). */
static const char *env_get(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] ? v : NULL;
}

static void env_ignored(const char *name, const char *v, const char *why)
{
    pz_log(PZ_LOG_WARN, "%s=%s ignored (%s)", name, v, why);
}

/* The two-state rule: on with any value except "" and "0" (PZ_NO_CUDA=0 does not switch CUDA off). */
static bool env_flag(const char *name)
{
    const char *v = env_get(name);
    return v && strcmp(v, "0") != 0;
}

/* A decimal integer between lo and hi; false (and nothing changed) when the variable is not set or its value is not that. */
static bool env_int(const char *name, long long lo, long long hi, long long *out)
{
    const char *v = env_get(name);
    if (!v) return false;
    i64 n;
    if (!pz_parse_i64(v, &n) || n < lo || n > hi) {
        char why[96];
        snprintf(why, sizeof why, "expected an integer between %lld and %lld", lo, hi);
        env_ignored(name, v, why);
        return false;
    }
    *out = n;
    return true;
}

/* A text of at most cap - 1 bytes; a longer one is not kept. */
static void env_text(const char *name, char *out, size_t cap)
{
    const char *v = env_get(name);
    if (!v) return;
    if (strlen(v) >= cap) {
        pz_log(PZ_LOG_WARN, "%s ignored (longer than %zu bytes)", name, cap - 1);
        return;
    }
    memcpy(out, v, strlen(v) + 1);
}

bool pz_env_parse_nk(const char *t, int *nk, int *cfg, bool *l2)
{
    long n = 0;
    const char *e = t;
    while (*e >= '0' && *e <= '9' && n <= 0xFFFF) n = n * 10 + (*e++ - '0');   /* no strtol: glibc >= 2.38 would be needed */
    if (n <= 0 || n > 0xFFFF) return false;
    int c = 0;
    if (*e == 'b') { c = 1; e++; }
    else if (*e == 'c') { c = 2; e++; }                         /* products every fourth walker */
    bool l = false;
    if (*e == 'l') { l = true; e++; }                           /* herd sized to the L2 cache */
    if (*e) return false;
    *nk = (int)n;
    *cfg = c;
    *l2 = l;
    return true;
}

static void env_gpu_api(void)
{
    const char *v = env_get("PZ_GPU_API");
    if (!v) return;
    if (!strcmp(v, "cuda")) g_env.gpu_api = PZ_ENV_API_CUDA;
    else if (!strcmp(v, "opencl")) g_env.gpu_api = PZ_ENV_API_OPENCL;
    else if (strcmp(v, "auto") != 0) env_ignored("PZ_GPU_API", v, "expected cuda or opencl");
}

static void env_gpu_tune(void)
{
    const char *v = env_get("PZ_GPU_TUNE");
    if (!v) return;
    if (!strcmp(v, "0")) g_env.gpu_tune = PZ_ENV_TUNE_OFF;
    else if (!strcmp(v, "1")) g_env.gpu_tune = PZ_ENV_TUNE_ALL;
    else env_ignored("PZ_GPU_TUNE", v, "expected 0 or 1");
}

static void env_gpu_nk(void)
{
    const char *v = env_get("PZ_GPU_NK");
    if (!v) return;
    if (strlen(v) >= sizeof g_env.gpu_nk_text || !pz_env_parse_nk(v, &g_env.gpu_nk, &g_env.gpu_nk_cfg, &g_env.gpu_nk_l2)) {
        env_ignored("PZ_GPU_NK", v, "expected a number of walkers, with the suffix b or c and then l if wanted, as in 128b");
        return;
    }
    memcpy(g_env.gpu_nk_text, v, strlen(v) + 1);
}

static void env_bench_scale(void)
{
    const char *v = env_get("PZ_BENCH_SCALE");
    if (!v) return;
    bool plain = (v[0] >= '0' && v[0] <= '9') || v[0] == '.';             /* strtod alone would also take blanks, a sign, "inf", "nan", hexadecimal */
    for (const char *q = v; *q && plain; q++) plain = (*q >= '0' && *q <= '9') || strchr(".eE+-", *q) != NULL;
    char *end = NULL;
    const double x = plain ? strtod(v, &end) : 0;
    if (plain && !*end && x >= 0.01 && x <= 100) g_env.bench_scale = x;
    else env_ignored("PZ_BENCH_SCALE", v, "expected a number between 0.01 and 100");
}

int pz_env_each_pz(void (*fn)(const char *entry, void *ctx), void *ctx)
{
    int n = 0;
    for (char **e = ENVIRON; e && *e; e++)
        if (!strncmp(*e, "PZ_", 3)) { fn(*e, ctx); n++; }
    return n;
}

void pz_env_init(void)
{
    g_env = (struct pz_env)ENV_INIT;
    long long n;
    env_gpu_api();
    g_env.no_cuda = env_flag("PZ_NO_CUDA");
    g_env.no_opencl = env_flag("PZ_NO_OPENCL");
    g_env.opencl_all = env_flag("PZ_OPENCL_ALL");
    if (env_int("PZ_GPU_HALF", 1, 1 << 20, &n)) g_env.gpu_half = (int)n;
    env_gpu_nk();
    env_gpu_tune();
    g_env.no_profile = env_flag("PZ_NO_PROFILE");
    g_env.no_large_pages = env_flag("PZ_NO_LARGE_PAGES");
    env_bench_scale();

    env_text("XDG_CACHE_HOME", g_env.xdg_cache_home, sizeof g_env.xdg_cache_home);
    env_text("HOME", g_env.home, sizeof g_env.home);
    env_text("LOCALAPPDATA", g_env.local_app_data, sizeof g_env.local_app_data);
    env_text("TMPDIR", g_env.tmpdir, sizeof g_env.tmpdir);
    const char *term = env_get("TERM");
    g_env.term_dumb = term && !strcmp(term, "dumb");
    g_env.no_color = env_get("NO_COLOR") != NULL;

    g_env.kang_verbose = env_flag("PZ_KANG_VERBOSE");
    if (env_get("PZ_KANG_MIX")) g_env.kang_mix = env_flag("PZ_KANG_MIX");     /* unset: measured */
    env_text("PZ_KANG_ROUND", g_env.kang_round, sizeof g_env.kang_round);
    g_env.bsgs_scalar = env_flag("PZ_BSGS_SCALAR");
    g_env.assume_ifma = env_flag("PZ_ASSUME_IFMA");
    g_env.expect_shani = env_flag("PZ_EXPECT_SHANI");
    if (env_int("PZ_TEST_KTHREAD", 16, 1 << 20, &n)) g_env.test_kthread = (int)n;
    if (env_int("PZ_TEST_JUMP", 1, 2147483647, &n)) g_env.test_jump = (int)n;
    if (env_int("PZ_TEST_DP_CAP", 0, (long long)1 << 62, &n)) g_env.test_dp_cap = n;
}
