/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* doctor.c - 'pzsolve doctor' (see doctor.h). Everything it prints is meant to be pasted into a bug report as it is. */
#include "doctor.h"
#include "run.h"
#include "gpu.h"
#include <pzcore/hash.h>
#include "kangaroo_priv.h"
#include "campaign.h"
#include "campaigns.h"
#include "cpu_engine.h"
#include "selftest.h"
#include "profile.h"
#include "platform.h"
#include "envopts.h"
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>

static int g_warn, g_err;

void doctor_line(char level, const char *fmt, ...)
{
    static const char *tag[] = {"[..]", "[ok]", "[!!]", "[XX]"};
    const int k = level == 'o' ? 1 : level == 'w' ? 2 : level == 'x' ? 3 : 0;
    if (k == 2) g_warn++;
    if (k == 3) g_err++;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char ln[1100];
    snprintf(ln, sizeof ln, "  %s %s", tag[k], buf);
    pz_fputs_wrap(stdout, ln, PZ_LINE_MAX, 7);
    putchar('\n');
}

static void section(const char *t) { printf("\n== %s\n", t); }

static void check_program(void)
{
    section("Program");
    doctor_line('i', "pzsolve %s (%s)", PZ_VERSION, PZ_BUILD_INFO);
}

static void check_system(const sysinfo_t *s)
{
    section("System");
    char os[200], host[128], m1[32], m2[32];
    pz_os_description(os, sizeof os);
    pz_hostname(host, sizeof host);
    doctor_line('i', "%s, computer \"%s\"", os, host);
    doctor_line('i', "CPU: %s", s->model);
    doctor_line('i', "%d usable logical threads, %d physical cores, L2 cache %d KiB, L3 cache %d KiB", s->ncpu, s->ncore, s->l2_kib, s->l3_kib);
    if (s->ncpu > s->ncore && s->ncore > 0)
        doctor_line('i', "hyper-threading/SMT is on: kangaroo runs faster with all threads; for the linear search see 'bench' (one thread per core is sometimes better)");
    pz_fmt_bytes((double)s->mem_total, m1, sizeof m1);
    pz_fmt_bytes((double)s->mem_avail, m2, sizeof m2);
    if (s->mem_avail < ((u64)2 << 30)) doctor_line('w', "available RAM %s of %s: low (kangaroo and BSGS keep their tables in RAM: close other programs or use --mem)", m2, m1);
    else doctor_line('o', "RAM: %s total, %s available", m1, m2);
    char ps[128];
    pz_power_scheme(ps, sizeof ps);
    if (ps[0]) doctor_line('i', "active power plan: %s (a power-saving plan slows the CPU and GPU down; use 'High performance' for long campaigns)", ps);
    if (pz_boost_possible()) doctor_line('i', "high priority for the GPU threads: possible (the first-run tuning tries it by itself; --prio-gpu yes|no forces it)");
    else doctor_line('i', "high priority for the GPU threads: not possible for this user (on Linux it needs root or CAP_SYS_NICE): stays normal");
}

static void check_cpu_engines(void)
{
    section("CPU engines");
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    cpu_tables_init();
    int bad = 0, ok = 0;
    for (int i = 0; i < n; i++) {
        if (!all[i]->supported()) continue;
        if (cpu_engine_quickcheck(all[i])) ok++;
        else { bad++; doctor_line('x', "engine %s gave a wrong result in the quick check", all[i]->name); }
    }
    doctor_line(bad ? 'w' : 'o', "%d of %d engines available, quick check with a known key: %s", ok + bad, n, bad ? "ERRORS" : "passed");
    const cpu_engine *best = cpu_engine_autotune(0.12, false);
    if (best) doctor_line('i', "chosen by --isa auto for the linear search: %s (%d lanes)", best->name, best->lanes);
    kang_pick_round();
    doctor_line('i', "kangaroo step chosen: %s", kang_round_name());
}

static void check_primitives(void)
{
    section("Primitives");
    pz_u256 k = pz_u256_from_u64(1);
    pz_ge p;
    u8 pub[33], h[20];
    char hx[48];
    pz_ec_init();
    if (!pz_ec_mul_g(&p, &k)) { doctor_line('x', "curve multiplication failed"); return; }
    pz_ge_to_pub33(pub, &p);
    pz_hash160(pub, 33, h);
    pz_hex_encode(h, 20, hx);
    if (!strcmp(hx, "751e76e8199196d454941c45d1b3a323f1433bd6")) doctor_line('o', "key 1 gives the correct hash160 (curve, SHA-256, RIPEMD-160)");
    else doctor_line('x', "key 1 gives the WRONG hash160 (%s)", hx);
}

static void check_cache(void)
{
    section("Tuning and machine profile (cache)");
    char path[512];
    if (!pz_cache_file("gpu_tune.txt", path, sizeof path)) {
        doctor_line('w', "no cache folder available: the kernel tuning and the machine profile are neither remembered nor reused");
        return;
    }
    char dir[512];
    pz_strlcpy(dir, path, sizeof dir);
    char *sl = strrchr(dir, '/');
    if (!sl) sl = strrchr(dir, '\\');
    if (sl) *sl = 0;
    doctor_line('i', "folder: %s", dir);
    gpu_doctor_cache();
    profile_doctor();
}

static void check_gpus(void)
{
    section("GPU");
    gpu_doctor(false);   /* the kernel tests of --full run once, in the full self-test at the end: it covers the GPUs too */
}

static long long mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_mtime : 0;
}

static void check_campaigns(const char *base)
{
    section("Campaigns");
    doctor_line('i', "folder: %s", base);
    DIR *d = opendir(base);
    if (!d) { doctor_line('i', "does not exist yet ('hunt' creates it): no campaigns"); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char conf[1200], dir[1100];
        snprintf(dir, sizeof dir, "%s/%s", base, e->d_name);
        snprintf(conf, sizeof conf, "%s/campaign.conf", dir);
        if (mtime_of(conf) == 0) continue;
        campaign_cfg c;
        char err[256];
        if (cfg_load(&c, conf, err, sizeof err) != 0) { doctor_line('x', "%s: campaign.conf unreadable (%s)", e->d_name, err); continue; }
        n++;
        char file[1300], age[56] = "never";
        long long mt = 0;
        const char *names[] = {"state.dat", "dp.bin", "session.txt"};
        for (size_t i = 0; i < 3; i++) {
            snprintf(file, sizeof file, "%s/%s", dir, names[i]);
            long long t = mtime_of(file);
            if (t > mt) mt = t;
        }
        if (mt) {
            char ago[48];
            pz_fmt_duration((double)(time(NULL) - mt), ago, sizeof ago);
            snprintf(age, sizeof age, "%s ago", ago);
        }
        const bool running = campaign_is_running(dir);
        snprintf(file, sizeof file, "%s/FOUND.txt", dir);
        const bool found = mtime_of(file) != 0;
        /* suspensions the campaign has gone through (logged by the reporter) */
        snprintf(file, sizeof file, "%s/log.txt", dir);
        const long long sleeps = pz_file_count(file, "system suspension detected");   /* streamed: after months the log is tens of MB */
        doctor_line(found ? 'o' : 'i', "%-10s %-9s puzzle %d: %s%s, last save %s", e->d_name, mode_name(c.mode), c.puzzle,
                    running ? "RUNNING" : "stopped", found ? ", KEY FOUND" : "", age);
        if (sleeps > 0) doctor_line('i', "%-10s system suspensions detected during the campaign: %lld (handled: suspended time is not counted and the state is saved)", e->d_name, sleeps);
    }
    closedir(d);
    if (!n) doctor_line('i', "no campaigns");
}

static void check_disk(const char *base)
{
    section("Disk");
    const char *where = pz_is_dir(base) ? base : ".";
    u64 fr, tot;
    char b1[32], b2[32];
    if (pz_disk_space(where, &fr, &tot)) {
        pz_fmt_bytes((double)fr, b1, sizeof b1);
        pz_fmt_bytes((double)tot, b2, sizeof b2);
        if (fr < ((u64)200 << 20)) doctor_line('x', "free space %s of %s: too little (the kangaroo distinguished points keep growing for days)", b1, b2);
        else if (fr < ((u64)5 << 30)) doctor_line('w', "free space %s of %s: low for a long kangaroo campaign", b1, b2);
        else doctor_line('o', "free space %s of %s", b1, b2);
    } else doctor_line('w', "free space cannot be read for %s", where);
    char t[1100];
    snprintf(t, sizeof t, "%s/.doctor_test", where);
    const char payload[] = "pzsolve doctor";
    if (pz_write_file_atomic(t, payload, sizeof payload - 1, 0644) == 0) {
        char *back = pz_read_file(t, NULL);
        const bool same = back && !strcmp(back, payload);
        free(back);
        remove(t);
        doctor_line(same ? 'o' : 'x', "atomic write of a file in %s: %s", where, same ? "succeeded" : "the content read back differs");
    } else doctor_line('x', "cannot write to %s (permissions or read-only disk): campaigns cannot save their state", where);
}

static void check_time(void)
{
    section("Time and suspension");
    const double m0 = pz_mono(), w0 = pz_now();
    pz_sleep(0.3);
    const double dm = pz_mono() - m0, dw = pz_now() - w0;
    if (dm < 0.25 || dm > 0.6) doctor_line('w', "the 0.3 s timer took %.2f s: unusual clock or scheduler", dm);
    else if (dw - dm > 0.2 || dm - dw > 0.2) doctor_line('w', "monotonic clock (%.2f s) and system clock (%.2f s) disagree", dm, dw);
    else doctor_line('o', "monotonic clock and system clock agree");
    doctor_line('i', "if the computer sleeps or hibernates during a campaign: the program notices the time jump, does not count the suspended time, saves its state and, if the GPU stops responding after waking up, reopens it by itself");
#if defined(_WIN32) || defined(__APPLE__)
    doctor_line('i', "to keep the computer from sleeping while the campaign runs: hunt --keep-awake");
#else
    doctor_line('i', "to keep the computer from sleeping: systemd-inhibit pzsolve hunt ...");
#endif
}

static void env_entry_line(const char *entry, void *ctx)
{
    (void)ctx;
    doctor_line('i', "%s", entry);
}

static void check_env(void)
{
    section("Environment variables PZ_*");
    const int n = pz_env_each_pz(env_entry_line, NULL);
    if (!n) doctor_line('i', "none set");
    else doctor_line('w', "%d PZ_* variables set: they change the behavior (tuning, GPU, engines); if you did not set them yourself, remove them", n);
}

int doctor_run(int argc, char **argv)
{
    const char *base = "campaigns";
    bool deep = false;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dir")) && i + 1 < argc) {
            base = argv[++i];
            if (!base[0]) pz_die("--dir: invalid value '' (expected the name of a folder)");
        }
        else if (!strcmp(argv[i], "--full")) deep = true;
        else pz_die("unrecognized option '%s' (doctor accepts -d DIR and --full)", argv[i]);
    }
    sysinfo_t s;
    sysinfo_detect(&s);
    printf("pzsolve doctor - checks the program, the machine and the saved state (paste this page if you ask for help)\n");
    check_program();
    check_system(&s);
    check_primitives();
    check_cpu_engines();
    check_gpus();
    check_cache();
    check_campaigns(base);
    check_disk(base);
    check_time();
    check_env();
    if (deep) {
        section("Full selftest");
        int rc = selftest_run(0, NULL);
        doctor_line(rc ? 'x' : 'o', "full selftest: %s", rc ? "ERRORS" : "passed");
    }
    printf("\n");
    if (g_err) printf("RESULT: %d errors, %d warnings, see the [XX] and [!!] lines\n", g_err, g_warn);
    else if (g_warn) printf("RESULT: no errors, %d warnings, see the [!!] lines\n", g_warn);
    else printf("RESULT: all good\n");
    return g_err ? 1 : 0;
}
