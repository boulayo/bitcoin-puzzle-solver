/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* run_common.c - shared runtime: found-key handling, signals, session
 * persistence, live terminal reporting and report files. */
#include "run.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include "ui.h"
#include "platform.h"
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

static run_ctx *g_run;
static _Atomic int g_sig_count;
static _Atomic long long g_first_sig_ms;

static long long mono_ms(void) { return (long long)(pz_mono() * 1000.0); }   /* clock_gettime / QPC: async-signal-safe */

/* First signal: stop gracefully (workers finish, state is saved). A second signal more than half a second later
 * (an impatient human) leaves immediately; duplicates delivered together by tools such as `timeout`, which signal both
 * the process and its group, are ignored. */
static void on_signal(void)
{
    long long now = mono_ms();
    if (atomic_load(&g_sig_count) == 0) {
        atomic_store(&g_first_sig_ms, now);
        atomic_store(&g_sig_count, 1);
    } else if (now - atomic_load(&g_first_sig_ms) > 500) {
        _exit(130);
    }
    if (g_run) atomic_store(&g_run->stop, 1);
}

void run_install_signals(run_ctx *r)
{
    g_run = r;
    pz_on_interrupt(on_signal);
}

void run_init(run_ctx *r)
{
    memset(r, 0, sizeof *r);
    pthread_mutex_init(&r->found_mu, NULL);
    sysinfo_detect(&r->sys);
    atomic_store(&r->cpu_cap, INT_MAX);
    r->use_cpu = true;
    r->use_gpu = true;
    r->report_interval = 1;
    r->checkpoint_interval = 60;
}

int run_limit_threads(int n, int pct)
{
    if (n < 1) return n;
    if (pct < 1 || pct > 99) return n;
    int t = (int)(((long)n * pct + 50) / 100);       /* nearest whole thread */
    return t < 1 ? 1 : t > n ? n : t;
}

size_t run_limit_items(size_t n, int pct, size_t quantum)
{
    if (pct < 1 || pct > 99 || quantum == 0 || n <= quantum) return n;
    size_t t = (size_t)((unsigned long long)n * (unsigned)pct / 100);
    t = (t + quantum / 2) / quantum * quantum;
    return t < quantum ? quantum : t > n ? n : t;
}

void run_limit_setup(run_ctx *r)
{
    if (r->limit_pct < 1 || r->limit_pct > 99) { r->limit_pct = 0; return; }
    if (r->use_cpu) {
        r->nthreads = run_limit_threads(r->nthreads, r->limit_pct);
        sysinfo_spread_order(&r->sys);   /* thread 0, 1, ... go to different physical cores before any core gets a second thread */
    }
}

int run_alloc_worker(run_ctx *r, const char *name, char kind)
{
    if (r->nws >= MAX_WORKERS) pz_die("too many workers");
    int i = r->nws++;
    pz_strlcpy(r->ws[i].name, name, sizeof r->ws[i].name);
    r->ws[i].kind = kind;
    return i;
}

void run_paths(run_ctx *r)
{
    const char *d = r->cfg.dir;
    snprintf(r->path_state, sizeof r->path_state, "%s/state.dat", d);
    snprintf(r->path_status, sizeof r->path_status, "%s/status.json", d);
    snprintf(r->path_report, sizeof r->path_report, "%s/report.txt", d);
    snprintf(r->path_stats, sizeof r->path_stats, "%s/stats.csv", d);
    snprintf(r->path_log, sizeof r->path_log, "%s/log.txt", d);
    snprintf(r->path_found, sizeof r->path_found, "%s/FOUND.txt", d);
    snprintf(r->path_session, sizeof r->path_session, "%s/session.txt", d);
}

int run_session_load(run_ctx *r)
{
    char *t = pz_read_file(r->path_session, NULL);
    if (!t) return 1;
    double el = 0;
    unsigned long long un = 0;
    int ses = 0;
    char *p;
    if ((p = strstr(t, "elapsed="))) el = atof(p + 8);
    if ((p = strstr(t, "units="))) un = pz_strtou64(p + 6);
    if ((p = strstr(t, "sessions="))) ses = pz_atoi(p + 9);
    free(t);
    r->elapsed_prev = el;
    r->prev_units_total = un;
    r->sessions = ses;
    return 0;
}

int run_session_save(run_ctx *r, double elapsed_total, u64 units_total)
{
    char b[256];
    int n = snprintf(b, sizeof b, "elapsed=%.1f\nunits=%llu\nsessions=%d\nupdated=%.0f\n", elapsed_total,
                     (unsigned long long)units_total, r->sessions, pz_now());
    return pz_write_file_atomic(r->path_session, b, (size_t)n, 0644);
}

sched_t *run_load_sched(run_ctx *r, const campaign_cfg *scfg, bool *resumed)
{
    char err[256], bak[700];
    snprintf(bak, sizeof bak, "%s.bak", r->path_state);
    sched_t *s = sched_new(scfg);
    *resumed = false;
    int lr = sched_load(s, r->path_state, err, sizeof err);
    if (lr == 0) { *resumed = true; return s; }
    if (lr == -2) pz_die("%s\n  (the state %s is intact but belongs to another campaign or to another part of the range: it was not touched)", err, r->path_state);
    /* corrupt or missing (a crash between the rename and the write leaves only the backup) */
    if (lr < 0) pz_log(PZ_LOG_WARN, "main checkpoint unusable (%s): trying the backup", err);
    sched_free(s);
    s = sched_new(scfg);
    int lb = sched_load(s, bak, err, sizeof err);
    if (lb < 0) pz_die("no usable checkpoint: %s\n  (remove %s and %s to start from scratch, losing the progress)", err, r->path_state, bak);
    if (lb == 0) { *resumed = true; return s; }
    if (lr < 0) pz_die("corrupt checkpoint and no backup available\n  (remove %s to start from scratch, losing the progress)", r->path_state);
    sched_free(s);
    return sched_new(scfg);   /* nothing saved yet: fresh start */
}

/* ---- found key ---- */

static bool key_matches(run_ctx *r, const pz_u256 *key)
{
    if (pz_u256_is_zero(key) || pz_u256_cmp(key, &pz_secp256k1_n) >= 0) return false;
    pz_ge p;
    if (!pz_ec_mul_g(&p, key)) return false;
    u8 pub[33], h[20];
    pz_ge_to_pub33(pub, &p);
    pz_hash160(pub, 33, h);
    if (memcmp(h, r->cfg.target_h160, 20) == 0) return true;
    return r->cfg.has_pubkey && pz_ge_equal(&p, &r->cfg.pubkey);
}

bool run_submit_key(run_ctx *r, const pz_u256 *key, const char *source)
{
    bool ok = key_matches(r, key);
    if (!ok) {
        atomic_fetch_add(&r->hits_bad, 1);
        pz_log(PZ_LOG_WARN, "candidate from %s discarded: the independent CPU check failed", source);
        return false;
    }
    pthread_mutex_lock(&r->found_mu);
    if (!r->found_valid) {
        r->found_key = *key;
        r->found_valid = true;
        r->found_time = pz_now();
        atomic_store(&r->found, 1);
        atomic_store(&r->stop, 1);
        run_write_found(r);
        pz_log(PZ_LOG_INFO, "KEY FOUND by %s: see %s (the key is not written to the log)", source, r->path_found);
    }
    pthread_mutex_unlock(&r->found_mu);
    return true;
}

void run_write_found(run_ctx *r)
{
    char hx[80], wif[80], addr[64], buf[2048], seed[65], pk[80];
    u8 kb[32], h[20], pub[33];
    pz_ge p;
    pz_u256_to_hex(&r->found_key, hx);
    pz_u256_to_be(&r->found_key, kb);
    pz_privkey_to_wif(kb, true, wif, sizeof wif);
    pz_ec_mul_g(&p, &r->found_key);
    pz_ge_to_pub33(pub, &p);
    pz_hex_encode(pub, 33, pk);
    pz_hash160(pub, 33, h);
    pz_hash160_to_address(h, addr, sizeof addr);
    pz_hex_encode(r->cfg.seed, 32, seed);
    time_t t = (time_t)r->found_time;
    char ts[40];
    struct tm tmv;
    pz_localtime(t, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    int n = snprintf(buf, sizeof buf,
        "*** PRIVATE KEY FOUND ***\n\n"
        "date:            %s\n"
        "campaign:        %s\n"
        "puzzle:          %d\n"
        "address:         %s\n"
        "private key hex: %s\n"
        "WIF compressed:  %s\n"
        "public key:      %s\n"
        "campaign seed:   %s\n"
        "version:         %s\n\n"
        "KEEP THIS FILE IN A SAFE PLACE and move the funds as soon as possible.\n",
        ts, r->cfg.name, r->cfg.puzzle, addr, hx, wif, pk, seed, PZ_VERSION);
    if (pz_write_file_atomic(r->path_found, buf, (size_t)n, 0600) != 0)
        pz_log(PZ_LOG_ERR, "cannot write %s: %s", r->path_found, strerror(errno));
}

/* ---- reporting ---- */

static void json_num(char *b, size_t n, double v)
{
    if (isfinite(v)) snprintf(b, n, "%.6g", v);
    else snprintf(b, n, "null");
}

static void make_summary(const progress_info *pi, eta_summary *es)
{
    eta_summarize(&pi->eta, pi->rate_eta > 0 ? pi->rate_eta : (pi->est_state == RATE_MEASURING ? 0 : pi->rate), es);
}

void run_final_progress(run_ctx *r, progress_info *pi)
{
    memset(pi, 0, sizeof *pi);
    pi->unit = "keys";
    pi->rate = ui_rates(r, NULL, NULL, NULL);
    pi->est_state = r->est_state;
    pi->rate_eta = r->rate_eta > 0 ? r->rate_eta : (r->est_state == RATE_MEASURING ? 0 : pi->rate);
    if (r->progress) r->progress(r, pi);
}

/* The fields of a detail line ("a | b | c") on lines of at most `width` columns, broken between fields; the continuation lines are
 * indented by `hang` columns. */
static void wrap_fields(const char *in, int width, int hang, char *out, size_t n)
{
    size_t o = 0;
    int col = 0;
    for (const char *p = in; *p && o + 2 < n;) {
        const char *sep = strstr(p, " | ");
        const size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (col > 0 && col + 3 + (int)len > width) {
            o += (size_t)snprintf(out + o, n - o, " |\n%*s", hang, "");
            col = 0;
        } else if (col > 0) {
            o += (size_t)snprintf(out + o, n - o, " | ");
            col += 3;
        }
        o += (size_t)snprintf(out + o, n - o, "%.*s", (int)len, p);
        col += (int)len;
        p = sep ? sep + 3 : p + len;
    }
    out[o < n ? o : n - 1] = 0;
}

void run_write_report_files(run_ctx *r, const progress_info *pi, bool final)
{
    char buf[8192], tb[40], seedhex[65], idb[17], rs[80], re[80], addr[80], hx[48], elapsed[48], state[24];
    double el = r->elapsed_prev + run_session_elapsed(r);
    time_t now = time(NULL);
    struct tm tmv;
    pz_localtime(now, &tmv);
    strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", &tmv);
    pz_hex_encode(r->cfg.seed, 32, seedhex);
    seed_to_id(r->cfg.seed, idb);
    pz_u256_to_hex(&r->cfg.range_start, rs);
    pz_u256_to_hex(&r->cfg.range_end, re);
    pz_hex_encode(r->cfg.target_h160, 20, hx);
    if (r->cfg.address[0]) pz_strlcpy(addr, r->cfg.address, sizeof addr);
    else pz_hash160_to_address(r->cfg.target_h160, addr, sizeof addr);
    pz_fmt_duration(el, elapsed, sizeof elapsed);
    pz_strlcpy(state, ui_state_name(r, final), sizeof state);
    eta_summary es;
    make_summary(pi, &es);
    double cpur, gpur;
    int ncpu;
    ui_rates(r, &cpur, &gpur, &ncpu);
    const bool kang = r->cfg.mode == MODE_KANGAROO;
    char rate[32], done[48], total[48], cr[32], gr[32], t50[48], t90[48], tfull[48], pw[ETA_NWIN][24];
    pz_fmt_si(pi->rate, rate, sizeof rate);
    pz_fmt_si(cpur, cr, sizeof cr);
    pz_fmt_si(gpur, gr, sizeof gr);
    ui_fmt_count(pi->done, done, sizeof done);
    ui_fmt_count(pi->total, total, sizeof total);
    ui_fmt_eta(es.t50, t50, sizeof t50);
    ui_fmt_eta(es.t90, t90, sizeof t90);
    ui_fmt_eta(kang ? es.t99 : es.t_full, tfull, sizeof tfull);
    for (int i = 0; i < ETA_NWIN; i++) ui_fmt_prob(es.p_win[i], pw[i], sizeof pw[i]);
    char partline[PART_TEXT + 200] = "";
    if (r->cfg.part[0]) snprintf(partline, sizeof partline, "Part:           %s  (this computer scans only its shares of the blocks)\n", r->cfg.part);
    if (r->limit_pct) {
        const size_t pl = strlen(partline);
        snprintf(partline + pl, sizeof partline - pl, "Limit:          %d%% of the machine (fewer CPU threads and GPU work-items, always at full speed)\n", r->limit_pct);
    }
    pz_u256 rsz = cfg_range_size(&r->cfg);
    unsigned rbits = pz_u256_bits(&rsz) ? pz_u256_bits(&rsz) - 1 : 0;

    char detail[900];
    wrap_fields(pi->detail, 100, 16, detail, sizeof detail);
    int n = snprintf(buf, sizeof buf,
        "=== pzsolve %s - campaign report ===\n"
        "Updated:        %s\n"
        "State:          %s\n"
        "Campaign:       %s   (seed id %s)\n"
        "Seed:           %s\n"
        "Target:         puzzle #%d  address %s\n"
        "                hash160 %s%s\n"
        "Mode:           %s\n"
        "Range:          0x%s - 0x%s  (~2^%u keys)\n"
        "%s"
        "Sessions:       %d, total compute time %s\n"
        "\n"
        "%s%.6f %%   (%s of %s %s)\n"
        "Speed:          %s %s/s   (CPU %s, GPU %s)\n"
        "Time left:      50%%: %s   90%%: %s   %s: %s   (at the recent average speed)\n"
        "Chance within:  %s: %s   %s: %s   %s: %s   %s: %s   %s: %s\n"
        "Detail:         %s\n"
        "Rejected:       %llu candidate%s that failed the verification\n"
        "\nWorkers:\n",
        PZ_VERSION, tb, state, r->cfg.name, idb, seedhex, r->cfg.puzzle, addr, hx,
        r->cfg.has_pubkey ? "  (public key known)" : "", mode_name(r->cfg.mode), rs, re, rbits, partline,
        r->sessions, elapsed, kang ? "Success prob.:  " : "Progress:       ",
        pi->fraction * 100.0, done, total, pi->unit, rate, pi->unit, cr, gr, t50, t90, kang ? "99%" : "100%", tfull,
        ETA_WINDOW_NAMES[0], pw[0], ETA_WINDOW_NAMES[1], pw[1], ETA_WINDOW_NAMES[2], pw[2], ETA_WINDOW_NAMES[3], pw[3], ETA_WINDOW_NAMES[4], pw[4],
        detail, (unsigned long long)atomic_load(&r->hits_bad), atomic_load(&r->hits_bad) == 1 ? "" : "s");
    for (int i = 0; i < r->nws && n < (int)sizeof buf - 200; i++) {
        pz_fmt_si(r->ws[i].rate, rs, sizeof rs);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "  %-10s %12s %s/s\n", r->ws[i].name, rs, pi->unit);
    }
    if (atomic_load(&r->found)) n += snprintf(buf + n, sizeof buf - (size_t)n, "\nKEY FOUND: see %s\n", r->path_found);
    pz_write_file_atomic(r->path_report, buf, (size_t)n, 0644);

    /* status.json (machine readable; used by the 'list' command) */
    char j[8192], e50[32], e90[32], efull[32], pj[ETA_NWIN][32];
    json_num(e50, sizeof e50, es.t50);
    json_num(e90, sizeof e90, es.t90);
    json_num(efull, sizeof efull, kang ? es.t99 : es.t_full);
    for (int i = 0; i < ETA_NWIN; i++) json_num(pj[i], sizeof pj[i], es.p_win[i]);
    int m = snprintf(j, sizeof j,
        "{\n  \"program\": \"pzsolve\",\n  \"version\": \"%s\",\n  \"campaign\": \"%s\",\n  \"seed_id\": \"%s\",\n  \"part\": \"%s\",\n  \"limit_percent\": %d,\n"
        "  \"puzzle\": %d,\n  \"mode\": \"%s\",\n  \"state\": \"%s\",\n  \"updated_unix\": %lld,\n"
        "  \"elapsed_seconds\": %.1f,\n  \"sessions\": %d,\n  \"unit\": \"%s\",\n  \"done\": \"%.0Lf\",\n  \"total\": \"%.0Lf\",\n"
        "  \"fraction\": %.12g,\n  \"rate_per_second\": %.1f,\n  \"rate_cpu\": %.1f,\n  \"rate_gpu\": %.1f,\n"
        "  \"eta_seconds_50\": %s,\n  \"eta_seconds_90\": %s,\n  \"eta_seconds_%s\": %s,\n"
        "  \"prob_1h\": %s,\n  \"prob_24h\": %s,\n  \"prob_7d\": %s,\n  \"prob_30d\": %s,\n  \"prob_1y\": %s,\n"
        "  \"found\": %s,\n  \"rejected_candidates\": %llu,\n  \"workers\": [",
        PZ_VERSION, r->cfg.name, idb, r->cfg.part, r->limit_pct, r->cfg.puzzle, mode_name(r->cfg.mode), state, (long long)now, el,
        r->sessions, pi->unit, pi->done, pi->total, pi->fraction, pi->rate, cpur, gpur, e50, e90, kang ? "99" : "100", efull,
        pj[0], pj[1], pj[2], pj[3], pj[4],
        atomic_load(&r->found) ? "true" : "false", (unsigned long long)atomic_load(&r->hits_bad));
    for (int i = 0; i < r->nws && m < (int)sizeof j - 200; i++)
        m += snprintf(j + m, sizeof j - (size_t)m, "%s\n    {\"name\": \"%s\", \"kind\": \"%c\", \"rate\": %.1f, \"units\": %llu}", i ? "," : "",
                      r->ws[i].name, r->ws[i].kind, r->ws[i].rate, (unsigned long long)atomic_load(&r->ws[i].units));
    m += snprintf(j + m, sizeof j - (size_t)m, "\n  ]\n}\n");
    pz_write_file_atomic(r->path_status, j, (size_t)m, 0644);
}

static void append_stats_csv(run_ctx *r, const progress_info *pi)
{
    bool fresh = access(r->path_stats, F_OK) != 0;
    FILE *f = fopen(r->path_stats, "a");
    if (!f) return;
    if (fresh) fprintf(f, "unix_time,elapsed_s,fraction,done,rate_total,rate_cpu,rate_gpu\n");
    double cpur, gpur;
    ui_rates(r, &cpur, &gpur, NULL);
    fprintf(f, "%lld,%.1f,%.12g,%.0Lf,%.1f,%.1f,%.1f\n", (long long)time(NULL), r->elapsed_prev + run_session_elapsed(r), pi->fraction,
            pi->done, pi->rate, cpur, gpur);
    fclose(f);
}

static int term_cols(void)
{
    int c = pz_terminal_cols();
    return c > 20 ? c : 100;
}

/* The effective display mode: escape sequences are only ever written to a terminal. */
static ui_mode effective_ui(const run_ctx *r)
{
    bool tty = pz_stderr_is_terminal();
    ui_mode m = r->ui;
    if (m == UI_AUTO) m = tty ? UI_FULL : UI_LOG;
    if (!tty && (m == UI_FULL || m == UI_LINE)) m = UI_LOG;
    if (m == UI_FULL && term_cols() < 60) m = UI_LINE;
    return m;
}

static void ui_draw(run_ctx *r, const progress_info *pi, bool final)
{
    eta_summary es;
    make_summary(pi, &es);
    ui_mode m = effective_ui(r);
    if (m == UI_FULL) {
        char buf[4096];
        const char *nc = getenv("NO_COLOR");
        int lines = ui_render_full(r, pi, &es, final, term_cols(), !(nc && nc[0]), buf, sizeof buf);
        pz_ui_block(buf, lines);
    } else if (m == UI_LINE) {
        char buf[512];
        ui_render_line(r, pi, &es, term_cols(), buf, sizeof buf);
        pz_ui_block(buf, 1);
    } else if (!final) {
        char buf[512];
        ui_render_line(r, pi, &es, 0, buf, sizeof buf);
        pz_log(PZ_LOG_INFO, "%s", buf);
    }
}

void run_ui_final(run_ctx *r, const progress_info *pi)
{
    ui_mode m = effective_ui(r);
    if (m == UI_FULL || m == UI_LINE) {
        ui_draw(r, pi, true);
        pz_log_terminal_line_break();
    } else {
        char buf[512];
        eta_summary es;
        make_summary(pi, &es);
        ui_render_line(r, pi, &es, 0, buf, sizeof buf);
        pz_log(PZ_LOG_INFO, "final state: %s", buf);
    }
}

int run_clock_gap(double dm, double dw, double *gap)
{
    if (dm > 15.0) { *gap = dm - 0.1; return RUN_GAP_COUNTED; }
    if (dw - dm > 15.0) { *gap = dw - dm; return RUN_GAP_SKIPPED; }
    *gap = 0;
    return 0;
}

void run_reporter_loop(run_ctx *r)
{
    const ui_mode m0 = effective_ui(r);
    const double term_period = m0 == UI_LOG ? 60.0 : (double)r->report_interval;
    double t_last = pz_mono(), prev_mono = t_last, prev_wall = pz_now();
    double next_term = t_last + (m0 == UI_LOG ? 10.0 : 1.0);   /* the first frame appears after a second (a minute-long wait would look hung) */
    double next_files = t_last + 2, next_ckpt = t_last + r->checkpoint_interval, next_csv = t_last + 60;
    progress_info pi;
    const double alpha = 0.35;
    const double alpha_eta = 1.0 - exp(-1.0 / 30.0);   /* BSGS only: ~30 s time constant for the estimates */
    const bool use_est = r->cfg.mode != MODE_BSGS;     /* BSGS changes rate at the end of the table phase: the smoothed rate follows that better */
    rate_est est;
    rate_est_init(&est, use_est ? r->rate_prior : 0);
    r->est_state = use_est ? RATE_MEASURING : RATE_STEADY;
    r->all_started = !use_est;
    double t_any = -1, next_remember = t_last + 300;   /* first moment a worker produced; next time the steady rate is remembered */
    while (!atomic_load(&r->stop)) {
        pz_sleep(0.1);
        double now = pz_mono();
        {
            /* The loop wakes up every 0.1 s: a much longer gap means the machine slept or hibernated (the monotonic clock
             * may or may not run during sleep: compare it with the wall clock) or the process was frozen. */
            const double wall = pz_now(), dm = now - prev_mono, dw = wall - prev_wall;
            prev_mono = now;
            prev_wall = wall;
            double gap = 0;
            const int kind = run_clock_gap(dm, dw, &gap);
            if (kind) {
                char gb[48];
                if (kind == RUN_GAP_COUNTED) {                   /* the monotonic clock ran through it: it comes off the time of the session */
                    r->paused_total += gap;
                    rate_est_gap(&est, gap);
                }
                for (int i = 0; i < r->nws; i++) { r->ws[i].last_units = atomic_load(&r->ws[i].units); r->ws[i].rate = 0; }
                t_last = now;
                pz_log(PZ_LOG_WARN, "system suspension detected (about %s without computing): not counted in times and speeds; saving the state", pz_fmt_duration(gap, gb, sizeof gb));
                next_ckpt = now;                       /* save at once: a hibernation may end in a power cut */
            }
        }
        if (now - t_last >= 1.0) {
            double dt = now - t_last;
            double tot = 0;
            for (int i = 0; i < r->nws; i++) {
                u64 u = atomic_load(&r->ws[i].units);
                double inst = (double)(u - r->ws[i].last_units) / dt;
                r->ws[i].rate = r->ws[i].rate == 0 ? inst : alpha * inst + (1 - alpha) * r->ws[i].rate;
                r->ws[i].last_units = u;
                tot += r->ws[i].rate;
            }
            if (use_est) {
                /* "started": every worker has produced something (a GPU is counted only when its first launch ends); a worker that
                 * never does (a GPU that failed to start) is waited for 30 s after the first one produced */
                u64 sum = 0;
                bool all = r->nws > 0, any = false;
                for (int i = 0; i < r->nws; i++) {
                    const u64 u = atomic_load(&r->ws[i].units);
                    sum += u;
                    if (u) any = true; else all = false;
                }
                if (any && t_any < 0) t_any = now;
                const bool started = all || (any && now - t_any > 30.0);
                r->all_started = started;
                rate_est_add(&est, now, (double)sum, started);
                r->rate_eta = rate_est_value(&est, now, &r->est_state);
                if (r->remember_rate && now >= next_remember) {
                    const double s = rate_est_sustained(&est, now);
                    if (s > 0) r->remember_rate(r, s);
                    next_remember = now + 300;
                }
            } else {
                r->rate_eta = r->rate_eta == 0 ? tot : alpha_eta * tot + (1 - alpha_eta) * r->rate_eta;
            }
            if (now - r->t_start > 5.0 && tot > r->peak_rate) r->peak_rate = tot;
            t_last = now;
        }
        bool need_pi = now >= next_term || now >= next_files || now >= next_ckpt || now >= next_csv;
        if (!need_pi) continue;
        run_final_progress(r, &pi);
        if (now >= next_term) { ui_draw(r, &pi, false); next_term = now + term_period; }
        if (now >= next_files) { run_write_report_files(r, &pi, false); next_files = now + 5; }
        if (now >= next_csv) { append_stats_csv(r, &pi); next_csv = now + 60; }
        if (now >= next_ckpt) {
            if (r->checkpoint && r->checkpoint(r) != 0) pz_log(PZ_LOG_WARN, "checkpoint save failed");
            next_ckpt = now + r->checkpoint_interval;
        }
    }
    if (use_est && r->remember_rate) {
        const double sus = rate_est_sustained(&est, pz_mono());
        if (sus > 0) r->remember_rate(r, sus);
    }
}
