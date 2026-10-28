/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_env.c - the reading of the environment into g_env (src/envopts.c): the two-state rule of the flags, the numbers, the texts, and the
 * values that cannot be used (they leave the default and are reported once, as a warning in the log). */
#include "../src/platform.h"
#include "../src/envopts.h"
#include <stddef.h>
#include <stdlib.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Every variable the program reads. A variable that is set to the empty string counts as not set, which is how the test clears one (putenv
 * with "NAME=" works the same way on Windows, where it removes the variable). */
static const char *const ALL[] = {
    "PZ_GPU_API", "PZ_NO_CUDA", "PZ_NO_OPENCL", "PZ_OPENCL_ALL", "PZ_GPU_HALF", "PZ_GPU_NK", "PZ_GPU_TUNE", "PZ_NO_PROFILE", "PZ_NO_LARGE_PAGES",
    "PZ_BENCH_SCALE", "XDG_CACHE_HOME", "HOME", "LOCALAPPDATA", "TMPDIR", "TERM", "NO_COLOR", "PZ_KANG_VERBOSE", "PZ_KANG_MIX", "PZ_KANG_ROUND",
    "PZ_BSGS_SCALAR", "PZ_ASSUME_IFMA", "PZ_EXPECT_SHANI", "PZ_TEST_KTHREAD", "PZ_TEST_JUMP", "PZ_TEST_DP_CAP"};

static void setv(const char *name, const char *value)
{
    static struct { const char *name; char text[1300]; } slot[40];       /* putenv keeps the pointer: one buffer per variable, rewritten in place */
    int i = 0;
    while (i < 40 && slot[i].name && strcmp(slot[i].name, name) != 0) i++;
    if (i == 40) { puts("t_env: out of buffers"); exit(2); }
    slot[i].name = name;
    snprintf(slot[i].text, sizeof slot[i].text, "%s=%s", name, value);
    putenv(slot[i].text);
}

static void clear_all(void)
{
    for (size_t i = 0; i < sizeof ALL / sizeof *ALL; i++) setv(ALL[i], "");
}

static char g_logpath[700];

/* The text the log got since the last call (pz_env_init reports through pz_log). */
static char *logged(void)
{
    pz_log_set_file(NULL);
    char *t = pz_read_file(g_logpath, NULL);
    remove(g_logpath);
    pz_log_set_file(g_logpath);
    return t ? t : pz_xstrdup("");
}

/* Sets one variable to `value` on an otherwise clean environment and reads it; returns the number of warnings and keeps the text of the
 * last one in `last` (a "NAME=VALUE ignored" line when the value is refused). */
static int try_value(const char *name, const char *value, char *last, size_t n)
{
    clear_all();
    setv(name, value);
    free(logged());
    pz_env_init();
    char *t = logged();
    int warns = 0;
    last[0] = 0;
    for (char *p = t; (p = strstr(p, "[WARN] ")) != NULL; p += 7) {
        warns++;
        const char *e = strchr(p + 7, '\n');
        snprintf(last, n, "%.*s", (int)(e ? (size_t)(e - (p + 7)) : strlen(p + 7)), p + 7);
    }
    free(t);
    return warns;
}

static void t_defaults(void)
{
    clear_all();
    free(logged());
    pz_env_init();
    char *t = logged();
    CHECK(!*t);                                              /* nothing set: nothing to report */
    free(t);
    CHECK(g_env.gpu_api == PZ_ENV_API_AUTO && g_env.gpu_tune == PZ_ENV_TUNE_DEFAULT);
    CHECK(!g_env.no_cuda && !g_env.no_opencl && !g_env.opencl_all && !g_env.no_profile && !g_env.no_large_pages);
    CHECK(g_env.gpu_half == 0 && g_env.gpu_nk == 0 && g_env.gpu_nk_cfg == 0 && !g_env.gpu_nk_l2 && !g_env.gpu_nk_text[0]);
    CHECK(g_env.bench_scale == 1.0);
    CHECK(!g_env.xdg_cache_home[0] && !g_env.home[0] && !g_env.local_app_data[0] && !g_env.tmpdir[0] && !g_env.term_dumb && !g_env.no_color);
    CHECK(!g_env.kang_verbose && !g_env.kang_mix && !g_env.kang_round[0] && !g_env.bsgs_scalar && !g_env.assume_ifma && !g_env.expect_shani);
    CHECK(g_env.test_kthread == 0 && g_env.test_jump == 0 && g_env.test_dp_cap == -1);
}

/* The two-state variables: on with any value except "" and "0"; each one alone, and nothing else changes. */
static const struct { const char *name; size_t off; } FLAGS[] = {
    {"PZ_NO_CUDA", offsetof(struct pz_env, no_cuda)}, {"PZ_NO_OPENCL", offsetof(struct pz_env, no_opencl)},
    {"PZ_OPENCL_ALL", offsetof(struct pz_env, opencl_all)}, {"PZ_NO_PROFILE", offsetof(struct pz_env, no_profile)},
    {"PZ_NO_LARGE_PAGES", offsetof(struct pz_env, no_large_pages)}, {"PZ_KANG_VERBOSE", offsetof(struct pz_env, kang_verbose)},
    {"PZ_KANG_MIX", offsetof(struct pz_env, kang_mix)}, {"PZ_BSGS_SCALAR", offsetof(struct pz_env, bsgs_scalar)},
    {"PZ_ASSUME_IFMA", offsetof(struct pz_env, assume_ifma)}, {"PZ_EXPECT_SHANI", offsetof(struct pz_env, expect_shani)}};
#define NFLAGS ((int)(sizeof FLAGS / sizeof *FLAGS))

static bool flag_at(int i) { return *(const bool *)((const char *)&g_env + FLAGS[i].off); }

static void t_flags(void)
{
    static const char *const ON[] = {"1", "yes", "2", "00", "false", "0 ", " 0", "on"};
    static const char *const OFF[] = {"", "0"};
    char last[200];
    for (int i = 0; i < NFLAGS; i++) {
        for (size_t k = 0; k < sizeof ON / sizeof *ON; k++) {
            CHECK(try_value(FLAGS[i].name, ON[k], last, sizeof last) == 0);
            for (int j = 0; j < NFLAGS; j++) CHECK(flag_at(j) == (j == i));
        }
        for (size_t k = 0; k < sizeof OFF / sizeof *OFF; k++) {
            CHECK(try_value(FLAGS[i].name, OFF[k], last, sizeof last) == 0);
            for (int j = 0; j < NFLAGS; j++) CHECK(!flag_at(j));
        }
    }
}

static void t_gpu_api(void)
{
    char last[200];
    CHECK(try_value("PZ_GPU_API", "cuda", last, sizeof last) == 0 && g_env.gpu_api == PZ_ENV_API_CUDA);
    CHECK(try_value("PZ_GPU_API", "opencl", last, sizeof last) == 0 && g_env.gpu_api == PZ_ENV_API_OPENCL);
    CHECK(try_value("PZ_GPU_API", "auto", last, sizeof last) == 0 && g_env.gpu_api == PZ_ENV_API_AUTO);
    CHECK(try_value("PZ_GPU_API", "", last, sizeof last) == 0 && g_env.gpu_api == PZ_ENV_API_AUTO);
    CHECK(try_value("PZ_GPU_API", "vulkan", last, sizeof last) == 1 && g_env.gpu_api == PZ_ENV_API_AUTO);
    CHECK(strstr(last, "PZ_GPU_API=vulkan ignored (expected cuda or opencl)") != NULL);
    CHECK(try_value("PZ_GPU_API", "CUDA", last, sizeof last) == 1 && g_env.gpu_api == PZ_ENV_API_AUTO);      /* the names are lower case */
}

static void t_numbers(void)
{
    char last[200];
    /* PZ_GPU_HALF: any positive integer (whether a device takes it is checked per device) */
    CHECK(try_value("PZ_GPU_HALF", "128", last, sizeof last) == 0 && g_env.gpu_half == 128);
    CHECK(try_value("PZ_GPU_HALF", "+64", last, sizeof last) == 0 && g_env.gpu_half == 64);
    CHECK(try_value("PZ_GPU_HALF", "", last, sizeof last) == 0 && g_env.gpu_half == 0);
    static const char *const bad_half[] = {"0", "-5", "12x", "x", " 128", "128 ", "1.5", "99999999999", "0x80", "1048577"};
    for (size_t k = 0; k < sizeof bad_half / sizeof *bad_half; k++) {
        CHECK(try_value("PZ_GPU_HALF", bad_half[k], last, sizeof last) == 1 && g_env.gpu_half == 0);
        CHECK(strstr(last, "PZ_GPU_HALF=") && strstr(last, " ignored (expected an integer between 1 and 1048576)"));
    }
    /* PZ_TEST_KTHREAD: 16 or more */
    CHECK(try_value("PZ_TEST_KTHREAD", "16", last, sizeof last) == 0 && g_env.test_kthread == 16);
    CHECK(try_value("PZ_TEST_KTHREAD", "1048576", last, sizeof last) == 0 && g_env.test_kthread == 1048576);
    static const char *const bad_kt[] = {"15", "8", "0", "-16", "abc", "16x", "1048577", "2147483648", "9999999999999999999999"};
    for (size_t k = 0; k < sizeof bad_kt / sizeof *bad_kt; k++)
        CHECK(try_value("PZ_TEST_KTHREAD", bad_kt[k], last, sizeof last) == 1 && g_env.test_kthread == 0);
    /* PZ_TEST_JUMP: positive, up to what an int holds */
    CHECK(try_value("PZ_TEST_JUMP", "1", last, sizeof last) == 0 && g_env.test_jump == 1);
    CHECK(try_value("PZ_TEST_JUMP", "2147483647", last, sizeof last) == 0 && g_env.test_jump == 2147483647);
    static const char *const bad_tj[] = {"0", "-3", "2147483648", "1.5", "x", "3 "};
    for (size_t k = 0; k < sizeof bad_tj / sizeof *bad_tj; k++)
        CHECK(try_value("PZ_TEST_JUMP", bad_tj[k], last, sizeof last) == 1 && g_env.test_jump == 0);
    /* PZ_TEST_DP_CAP: zero is a value (the table then has the minimum size), unset is -1 */
    CHECK(try_value("PZ_TEST_DP_CAP", "0", last, sizeof last) == 0 && g_env.test_dp_cap == 0);
    CHECK(try_value("PZ_TEST_DP_CAP", "64", last, sizeof last) == 0 && g_env.test_dp_cap == 64);
    CHECK(try_value("PZ_TEST_DP_CAP", "123456789012", last, sizeof last) == 0 && g_env.test_dp_cap == 123456789012LL);
    CHECK(try_value("PZ_TEST_DP_CAP", "", last, sizeof last) == 0 && g_env.test_dp_cap == -1);
    static const char *const bad_cap[] = {"-1", "x", "64k", "4611686018427387905", "18446744073709551616"};
    for (size_t k = 0; k < sizeof bad_cap / sizeof *bad_cap; k++)
        CHECK(try_value("PZ_TEST_DP_CAP", bad_cap[k], last, sizeof last) == 1 && g_env.test_dp_cap == -1);
    /* PZ_BENCH_SCALE: a number from 0.01 to 100 */
    CHECK(try_value("PZ_BENCH_SCALE", "0.2", last, sizeof last) == 0 && g_env.bench_scale == 0.2);
    CHECK(try_value("PZ_BENCH_SCALE", "100", last, sizeof last) == 0 && g_env.bench_scale == 100);
    CHECK(try_value("PZ_BENCH_SCALE", "0.01", last, sizeof last) == 0 && g_env.bench_scale == 0.01);
    CHECK(try_value("PZ_BENCH_SCALE", "1e1", last, sizeof last) == 0 && g_env.bench_scale == 10);
    CHECK(try_value("PZ_BENCH_SCALE", "", last, sizeof last) == 0 && g_env.bench_scale == 1.0);
    static const char *const bad_sc[] = {"0", "0.001", "101", "abc", "1x", "nan", "inf", "-1", "+1", " 2", "0x10", "1e", "1e400", ".", "1.5.2"};
    for (size_t k = 0; k < sizeof bad_sc / sizeof *bad_sc; k++) {
        CHECK(try_value("PZ_BENCH_SCALE", bad_sc[k], last, sizeof last) == 1 && g_env.bench_scale == 1.0);
        CHECK(strstr(last, "PZ_BENCH_SCALE=") && strstr(last, " ignored (expected a number between 0.01 and 100)"));
    }
}

static void t_tune_and_nk(void)
{
    char last[200];
    CHECK(try_value("PZ_GPU_TUNE", "0", last, sizeof last) == 0 && g_env.gpu_tune == PZ_ENV_TUNE_OFF);
    CHECK(try_value("PZ_GPU_TUNE", "1", last, sizeof last) == 0 && g_env.gpu_tune == PZ_ENV_TUNE_ALL);
    CHECK(try_value("PZ_GPU_TUNE", "", last, sizeof last) == 0 && g_env.gpu_tune == PZ_ENV_TUNE_DEFAULT);
    static const char *const bad_tune[] = {"2", "yes", "01", "10", "0x", " 1"};
    for (size_t k = 0; k < sizeof bad_tune / sizeof *bad_tune; k++) {
        CHECK(try_value("PZ_GPU_TUNE", bad_tune[k], last, sizeof last) == 1 && g_env.gpu_tune == PZ_ENV_TUNE_DEFAULT);
        CHECK(strstr(last, "PZ_GPU_TUNE=") && strstr(last, " ignored (expected 0 or 1)"));
    }
    static const struct { const char *text; int nk, cfg; bool l2; } good[] = {
        {"128", 128, 0, false}, {"128b", 128, 1, false}, {"64c", 64, 2, false}, {"64l", 64, 0, true}, {"32bl", 32, 1, true}, {"16cl", 16, 2, true},
        {"65535", 65535, 0, false}};
    for (size_t k = 0; k < sizeof good / sizeof *good; k++) {
        CHECK(try_value("PZ_GPU_NK", good[k].text, last, sizeof last) == 0);
        CHECK(g_env.gpu_nk == good[k].nk && g_env.gpu_nk_cfg == good[k].cfg && g_env.gpu_nk_l2 == good[k].l2 && !strcmp(g_env.gpu_nk_text, good[k].text));
    }
    CHECK(try_value("PZ_GPU_NK", "", last, sizeof last) == 0 && g_env.gpu_nk == 0);
    static const char *const bad_nk[] = {"b", "l", "0", "128x", "128lb", "128bc", "128bb", "65536", "-1", "128 ", " 128", "+128", "1.5", "12345678901234567890"};
    for (size_t k = 0; k < sizeof bad_nk / sizeof *bad_nk; k++) {
        CHECK(try_value("PZ_GPU_NK", bad_nk[k], last, sizeof last) == 1);
        CHECK(g_env.gpu_nk == 0 && g_env.gpu_nk_cfg == 0 && !g_env.gpu_nk_l2 && !g_env.gpu_nk_text[0]);
        CHECK(strstr(last, "PZ_GPU_NK=") && strstr(last, " ignored (expected a number of walkers"));
    }
    int nk = 7, cfg = 7;                                       /* a refused text leaves the outputs alone */
    bool l2 = true;
    CHECK(!pz_env_parse_nk("", &nk, &cfg, &l2) && !pz_env_parse_nk("12k", &nk, &cfg, &l2) && nk == 7 && cfg == 7 && l2);
    CHECK(pz_env_parse_nk("256b", &nk, &cfg, &l2) && nk == 256 && cfg == 1 && !l2);
}

static void t_places(void)
{
    char last[200];
    CHECK(try_value("XDG_CACHE_HOME", "/tmp/cache dir", last, sizeof last) == 0 && !strcmp(g_env.xdg_cache_home, "/tmp/cache dir"));
    CHECK(try_value("HOME", "/home/u", last, sizeof last) == 0 && !strcmp(g_env.home, "/home/u") && !g_env.xdg_cache_home[0]);
    CHECK(try_value("LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local", last, sizeof last) == 0 && !strcmp(g_env.local_app_data, "C:\\Users\\u\\AppData\\Local"));
    CHECK(try_value("TMPDIR", "/var/tmp", last, sizeof last) == 0 && !strcmp(g_env.tmpdir, "/var/tmp"));
    CHECK(try_value("TMPDIR", "", last, sizeof last) == 0 && !g_env.tmpdir[0]);
    char longp[PZ_ENV_PATH + 8];                               /* the longest folder name that is kept has PZ_ENV_PATH - 1 characters */
    memset(longp, 'a', sizeof longp);
    longp[PZ_ENV_PATH - 1] = 0;
    CHECK(try_value("HOME", longp, last, sizeof last) == 0 && strlen(g_env.home) == PZ_ENV_PATH - 1);
    longp[PZ_ENV_PATH - 1] = 'a';
    longp[PZ_ENV_PATH] = 0;
    CHECK(try_value("HOME", longp, last, sizeof last) == 1 && !g_env.home[0]);
    CHECK(!strcmp(last, "HOME ignored (longer than 1023 bytes)"));
    CHECK(try_value("TERM", "dumb", last, sizeof last) == 0 && g_env.term_dumb);
    CHECK(try_value("TERM", "xterm-256color", last, sizeof last) == 0 && !g_env.term_dumb);
    CHECK(try_value("TERM", "", last, sizeof last) == 0 && !g_env.term_dumb);
    CHECK(try_value("NO_COLOR", "1", last, sizeof last) == 0 && g_env.no_color);
    CHECK(try_value("NO_COLOR", "0", last, sizeof last) == 0 && g_env.no_color);           /* the convention: any text; only the empty one is off */
    CHECK(try_value("NO_COLOR", "", last, sizeof last) == 0 && !g_env.no_color);
    CHECK(try_value("PZ_KANG_ROUND", "avx512i2", last, sizeof last) == 0 && !strcmp(g_env.kang_round, "avx512i2"));
    CHECK(try_value("PZ_KANG_ROUND", "x86", last, sizeof last) == 0 && !strcmp(g_env.kang_round, "x86"));
    CHECK(try_value("PZ_KANG_ROUND", "no-such-step-at-all", last, sizeof last) == 1 && !g_env.kang_round[0]);
}

/* The reading starts from the defaults every time, and several bad values give one warning each. */
static void t_reinit(void)
{
    char last[200];
    clear_all();
    setv("PZ_NO_CUDA", "1");
    setv("PZ_GPU_HALF", "256");
    pz_env_init();
    CHECK(g_env.no_cuda && g_env.gpu_half == 256);
    setv("PZ_NO_CUDA", "");
    setv("PZ_GPU_HALF", "");
    pz_env_init();
    CHECK(!g_env.no_cuda && g_env.gpu_half == 0);
    clear_all();
    setv("PZ_GPU_HALF", "x");
    setv("PZ_TEST_JUMP", "-1");
    setv("PZ_BENCH_SCALE", "0");
    setv("PZ_GPU_TUNE", "9");
    setv("PZ_NO_PROFILE", "1");
    free(logged());
    pz_env_init();
    char *t = logged();
    int warns = 0;
    for (const char *p = t; (p = strstr(p, "[WARN] ")) != NULL; p += 7) warns++;
    CHECK(warns == 4 && g_env.no_profile);
    free(t);
    (void)last;
}

/* The list `doctor` prints: every entry whose name starts with PZ_, the unknown names too, and nothing else. */
static void count_entry(const char *entry, void *ctx)
{
    int *c = ctx;
    c[0] += !strcmp(entry, "PZ_T_ENV_UNKNOWN=abc");
    c[1] += !strcmp(entry, "PZ_NO_CUDA=1");
    c[2] += strncmp(entry, "PZ_", 3) != 0;
}

static void t_each(void)
{
    clear_all();
    setv("PZ_T_ENV_UNKNOWN", "abc");
    setv("PZ_NO_CUDA", "1");
    setv("T_ENV_NOT_PZ", "1");
    int c[3] = {0, 0, 0};
    const int n = pz_env_each_pz(count_entry, c);
    CHECK(c[0] == 1 && c[1] == 1 && c[2] == 0 && n >= 2);
    setv("T_ENV_NOT_PZ", "");
}

int main(void)
{
    pz_log_set_quiet(2);                                       /* the warnings are read from the log file */
    char dir[600];
    if (!pz_make_temp_dir(dir, sizeof dir, "pzenv")) { puts("t_env: no temporary folder"); return 2; }
    snprintf(g_logpath, sizeof g_logpath, "%s/log.txt", dir);
    pz_log_set_file(g_logpath);
    t_defaults();
    t_flags();
    t_gpu_api();
    t_numbers();
    t_tune_and_nk();
    t_places();
    t_reinit();
    t_each();
    pz_log_set_file(NULL);
    clear_all();
    pz_env_init();
    pz_remove_tree(dir);
    if (fails) { printf("t_env: %d checks failed\n", fails); return 1; }
    puts("t_env: OK");
    return 0;
}
