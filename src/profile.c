/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* profile.c - the best CPU/GPU combination of this machine, measured once and remembered (see profile.h). */
#include "profile.h"
#include "bench_tg.h"
#include "kangaroo.h"
#include "doctor.h"
#include "platform.h"
#include "envopts.h"
#include <time.h>
#include <unistd.h>

static bool profile_path(char *out, size_t n) { return pz_cache_file("machine_profile.txt", out, n); }

/* Key: mode, program version, CPU, topology, the step/engine in use and the GPUs. '=' and newlines cannot appear in it. */
/* The widest linear engine this CPU supports (the measured choice may differ from one run to the next when two are about as fast). */
static const char *engine_isa(void)
{
    int n;
    const cpu_engine *const *all = cpu_engine_all(&n);
    const char *name = "?";
    for (int i = 0; i < n; i++)
        if (all[i]->supported()) name = all[i]->name;
    return name;
}

static void profile_key(const run_ctx *r, char *out, size_t n)
{
    char gsig[400] = "nogpu";
    if (r->use_gpu) gpu_signature(gsig, sizeof gsig);       /* --no-gpu: no need to wake the drivers up for a key */
    const bool kang = r->cfg.mode == MODE_KANGAROO;
    snprintf(out, n, "%s|%s|%s|%d/%d|%s|%s%s", kang ? "kang" : "lin", PZ_VERSION, r->sys.model, r->sys.ncpu, r->sys.ncore,
             kang ? kang_round_isa() : engine_isa(), gsig,           /* not the measured choices: they may change between runs */
             r->prio_gpu == PRIO_YES ? "|prio-yes" : r->prio_gpu == PRIO_NO ? "|prio-no" : "");     /* a measurement under a fixed priority is its own entry */
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '=') *c = '_';
}

/* The whole part of a non-negative number: the rates are saved as integers ("%.0f": "1030000000"), so no strtod is needed; a fraction or an
 * exponent ends the number. */
static double parse_rate(const char *p)
{
    double v = 0;
    int digits = 0;
    for (; *p >= '0' && *p <= '9' && digits < 18; p++, digits++) v = v * 10 + (*p - '0');
    return v;
}

/* The saved value of `key` ("default" or "t=N g=LIST at=EPOCH ..."), false if there is none. */
static bool profile_load(const char *key, char *val, size_t vn)
{
    char path[512];
    if (!profile_path(path, sizeof path)) return false;
    char *t = pz_read_file(path, NULL);
    if (!t) return false;
    const char *v = pz_kv_find(t, key, '=', PZ_KV_LAST);      /* the key has no '=' (profile_key replaces them); the value has some */
    if (v) {
        size_t len = strcspn(v, "\n");
        if (len >= vn) len = vn - 1;
        memcpy(val, v, len);
        val[len] = 0;
    }
    free(t);
    return v != NULL;
}

static void profile_save(const char *key, const char *val)
{
    char path[512];
    if (!profile_path(path, sizeof path)) return;
    pz_keyed_file_put(path, key, val);        /* merged with what other programs saved meanwhile, under a lock */
}

/* Whether a profile can be kept at all: a cache folder exists and a file can be created in it. Without one (a service or a container user
 * with no HOME) the measurement of about two minutes would be repeated at every start and never remembered. */
static bool profile_storable(void)
{
    char path[512], probe[560];
    if (!profile_path(path, sizeof path)) return false;
    snprintf(probe, sizeof probe, "%s.probeXXXXXX", path);
    const int fd = pz_create_temp(probe);
    if (fd < 0) return false;
    close(fd);
    unlink(probe);
    return true;
}

/* Applies a saved or fresh verdict to the campaign. */
static void apply_value(run_ctx *r, const char *val, bool fresh)
{
    char when[32] = "";
    const char *at = strstr(val, "at=");
    if (at) {
        long long secs = 0;                      /* no strtoll: on glibc >= 2.38 it would pull in __isoc23_strtoll */
        for (const char *c = at + 3; *c >= '0' && *c <= '9' && secs < 100000000000LL; c++) secs = secs * 10 + (*c - '0');
        time_t t = (time_t)secs;
        struct tm tmv;
        if (t > 0) { pz_localtime(t, &tmv); strftime(when, sizeof when, "%Y-%m-%d", &tmv); }
    }
    const char *rp = strstr(val, "rate=");
    r->rate_cold = rp ? parse_rate(rp + 5) : 0;           /* what the calibration measured for this entry: the first guess of the completion times */
    /* "prio=1": the threads serving the GPUs were faster at a higher priority (unless --prio-gpu decided) */
    const bool prio = strstr(val, "prio=1") != NULL && r->prio_gpu == PRIO_AUTO;
    if (prio) r->gpu_boost = true;
    const char *pn = prio ? ", high priority for the GPU threads" : "";
    if (!strncmp(val, "default", 7)) {
        /* a saved verdict that changes nothing is not worth a line on the terminal (the measurement that has just ended is) */
        (fresh || prio ? pz_log : pz_log_file)(PZ_LOG_INFO, "profile of this machine%s%s%s: the default values (all threads, all GPUs) are already the best%s",
                                               fresh ? " (just measured)" : "", when[0] ? " from " : "", when, pn);
        return;
    }
    int t = -1;
    char g[64] = "";
    const char *tp = strstr(val, "t=");
    if (tp && tp[2] >= '0' && tp[2] <= '9') t = pz_atoi(tp + 2);
    const char *gp = strstr(val, "g=");
    if (gp) {
        size_t i = 0;
        for (gp += 2; *gp && *gp != ' ' && i + 1 < sizeof g; gp++) g[i++] = *gp;
        g[i] = 0;
    }
    if (t < 0 || t > MAX_WORKERS - 16) return;          /* not a count a measurement writes (an edited file): the entry is ignored */
    r->nthreads = t;
    r->threads_explicit = true;
    if (t == 0) r->use_cpu = false;
    if (g[0]) pz_strlcpy(r->gpu_filter, g, sizeof r->gpu_filter);
    pz_log(PZ_LOG_INFO, "profile of this machine%s%s%s: %d CPU thread%s%s%s%s (-t and --gpu override them; --retune measures again)",
           fresh ? " (just measured)" : "", when[0] ? " from " : "", when, t, t == 1 ? "" : "s", g[0] ? ", GPU " : ", all GPUs", g, pn);
}

void profile_apply(run_ctx *r, bool force, bool disable)
{
    if (disable || g_env.no_profile) return;
    if (r->cfg.mode == MODE_BSGS || r->limit_pct) return;       /* --limit: no measurement at full load, no remembered choice */
    if (r->threads_explicit || r->gpu_filter[0] || !r->use_cpu || !r->use_gpu) return;   /* the user chose: nothing to decide */
    if (gpu_count_usable() <= 0) return;
    char key[900], val[200];
    profile_key(r, key, sizeof key);
    if (!force && profile_load(key, val, sizeof val)) { apply_value(r, val, false); return; }
    if (!profile_storable()) {
        pz_log(PZ_LOG_INFO, "no cache folder that can be written to (HOME or XDG_CACHE_HOME): the measurement of the best CPU thread count and GPUs "
               "is skipped, as its result could not be remembered; using all threads and all GPUs");
        return;
    }
    const bool kang = r->cfg.mode == MODE_KANGAROO;
    pz_log(PZ_LOG_INFO, "%s: measuring which CPU thread count and which GPUs work best (about 2 minutes, once; --no-tune skips the measurement, --retune repeats it)",
           force ? "tuning requested" : "first run of this version on this machine");
    tg_choice ch;
    memset(&ch, 0, sizeof ch);
    ch.prio = r->prio_gpu;                      /* PRIO_AUTO unless --prio-gpu fixed it: then there is nothing to measure */
    tg_silent = pz_log_quiet();                 /* a quiet hunt gets no tables */
    int rc = kang ? kangaroo_bench_together(0, NULL, &ch) : linear_bench_together(r->engine, 0, NULL, &ch);
    if (rc) { tg_silent = 0; pz_log(PZ_LOG_WARN, "CPU+GPU tuning failed: using the default values (will try again on the next run)"); return; }
    if (run_interrupted()) { tg_silent = 0; return; }       /* Ctrl-C: what was measured so far is not the verdict, and the caller stops */
    if (!(ch.def_rate > 0)) {                   /* stopped before anything was measured (every GPU worker ended, say): nothing to remember */
        tg_silent = 0;
        pz_log(PZ_LOG_WARN, "CPU+GPU tuning measured nothing: using the default values (will try again on the next run)");
        return;
    }
    tg_print_advice(&ch, kang ? "steps/s" : "keys/s");
    tg_silent = 0;
    const char *pt = ch.boost && ch.boost_tested ? " prio=1" : "";      /* only a measured result is remembered */
    const double cold = ch.better ? ch.best_rate : ch.def_rate;        /* units/s of the configuration that is applied, measured cold */
    char rt[40] = "";
    if (cold > 0 && cold < 1e17) snprintf(rt, sizeof rt, " rate=%.0f", cold);
    if (ch.better) {
        char gl[64];
        tg_gpu_list(&ch, gl, sizeof gl);
        snprintf(val, sizeof val, "t=%d%s%s%s at=%lld gain=%.1f%s", ch.cap, gl[0] ? " g=" : "", gl, pt, (long long)time(NULL), ch.gain_pct, rt);
    } else {
        snprintf(val, sizeof val, "default%s at=%lld%s", pt, (long long)time(NULL), rt);
    }
    profile_save(key, val);
    apply_value(r, val, true);
}

/* The key of the steady rate of this configuration: the machine key plus what changes the speed (threads, GPUs, priority). */
static void rate_key(const run_ctx *r, char *out, size_t n)
{
    char base[900];
    profile_key(r, base, sizeof base);
    /* "x": the thread count was chosen (-t or the profile), "a": the default, from which a core is set aside for every OpenCL GPU later on */
    char lim[16] = "";
    if (r->limit_pct) snprintf(lim, sizeof lim, "|lim=%d", r->limit_pct);            /* a limited run has its own steady rate */
    snprintf(out, n, "sus|%s|t=%d%s|g=%s%s%s", base, r->use_cpu ? r->nthreads : 0, r->threads_explicit ? "x" : "a",
             r->use_gpu ? (r->gpu_filter[0] ? r->gpu_filter : "all") : "none", r->gpu_boost ? "|hi" : "", lim);
    for (char *c = out; *c; c++) if (*c == '\n' || *c == '\r' || *c == '=') *c = '_';
}

static void remember_rate(run_ctx *r, double rate)
{
    char val[64];
    snprintf(val, sizeof val, "%.0f at=%lld", rate, (long long)time(NULL));
    profile_save(r->rate_key, val);                  /* the key made at the start: r->nthreads may have changed since (cores set aside for OpenCL GPUs) */
}

void profile_rate_setup(run_ctx *r)
{
    if (r->cfg.mode == MODE_BSGS) return;
    char val[96];
    rate_key(r, r->rate_key, sizeof r->rate_key);
    r->remember_rate = remember_rate;
    r->rate_prior = 0;
    r->rate_prior_kind = 0;
    if (profile_load(r->rate_key, val, sizeof val) && parse_rate(val) > 0) {
        r->rate_prior = parse_rate(val);
        r->rate_prior_kind = 2;
    } else if (r->rate_cold > 0) {
        r->rate_prior = r->rate_cold;
        r->rate_prior_kind = 1;
    }
}

void profile_doctor(void)
{
    char path[512];
    if (!profile_path(path, sizeof path)) return;
    char *t = pz_read_file(path, NULL);
    int cnt = 0;
    char needle[64];
    snprintf(needle, sizeof needle, "|%s|", PZ_VERSION);
    char *eq;
    for (char *p = t, *line; (line = pz_keyed_line(&p, &eq));) {
        if (strstr(line, needle)) {
            const char *cfg = strstr(line, "|t_");                       /* "sus|...|t_16|g_all|hi=826000000 at=...": a steady rate */
            if (!strncmp(line, "sus|", 4) && eq && cfg && cfg < eq) {
                char b[32], c[96];
                size_t i = 0;
                for (const char *q = cfg + 3; q < eq && *q >= '0' && *q <= '9' && i + 1 < sizeof c; q++) c[i++] = *q;      /* thread count */
                c[i] = 0;
                const char *rest = cfg + 3 + i;                                                                        /* "a|g_all|hi" */
                char g[64] = "";
                const char *gp = strstr(rest, "|g_");
                if (gp) {
                    size_t k = 0;
                    for (gp += 3; gp < eq && *gp != '|' && k + 1 < sizeof g; gp++) g[k++] = *gp;
                    g[k] = 0;
                }
                const char *lp = strstr(rest, "|lim_");
                char lim[40] = "";
                if (lp && lp < eq) snprintf(lim, sizeof lim, ", limit %d%%", pz_atoi(lp + 5));
                doctor_line('i', "remembered steady speed (%s threads%s, GPU %s%s%s): %s/s", c, *rest == 'x' ? " chosen" : " by default", g,
                            strstr(rest, "|hi") && strstr(rest, "|hi") < eq ? ", high priority" : "", lim, pz_fmt_si(parse_rate(eq + 1), b, sizeof b));
            } else doctor_line('i', "saved profile: %.170s", line);
            cnt++;
        }
    }
    free(t);
    if (!cnt) doctor_line('i', "no CPU+GPU profile saved for version %s: the first 'hunt' on a machine with a GPU measures it by itself (about 2 minutes; --no-tune skips it)", PZ_VERSION);
}
