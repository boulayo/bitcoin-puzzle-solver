/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ui.c - text rendering of the campaign status. Pure formatting: no terminal I/O happens here. */
#include "ui.h"
#include "platform.h"
#include <pzcore/hash.h>
#include <math.h>
#include <string.h>

double ui_rates(const run_ctx *r, double *cpu, double *gpu, int *ncpu)
{
    double t = 0, c = 0, g = 0;
    int nc = 0;
    for (int i = 0; i < r->nws; i++) {
        t += r->ws[i].rate;
        if (r->ws[i].kind == 'C') { c += r->ws[i].rate; nc++; }
        else g += r->ws[i].rate;
    }
    if (cpu) *cpu = c;
    if (gpu) *gpu = g;
    if (ncpu) *ncpu = nc;
    return t;
}

const char *ui_state_name(const run_ctx *r, bool final)
{
    if (atomic_load(&r->found)) return "FOUND";
    if (r->sched && sched_finished(r->sched)) return "COMPLETED";
    return final ? "STOPPED" : "RUNNING";
}

char *ui_target_tag(const run_ctx *r, char *buf, size_t n)
{
    if (r->cfg.puzzle) snprintf(buf, n, "#%d", r->cfg.puzzle);
    else snprintf(buf, n, "%s", r->cfg.name);
    return buf;
}

char *ui_fmt_count(long double v, char *buf, size_t n)
{
    if (v < 1e15L) return pz_fmt_si((double)v, buf, n);
    snprintf(buf, n, "%.3Le", v);
    return buf;
}

char *ui_fmt_eta(double secs, char *buf, size_t n)
{
    if (!(secs >= 0) || isinf(secs)) { snprintf(buf, n, "n/a"); return buf; }
    return pz_fmt_duration(secs, buf, n);
}

char *ui_fmt_prob(double p, char *buf, size_t n)
{
    double pct = p * 100.0;
    if (isnan(p)) snprintf(buf, n, "n/a");
    else if (!(pct > 0)) snprintf(buf, n, "0%%");
    else if (pct >= 99.995) snprintf(buf, n, "100%%");
    else if (pct >= 0.01) snprintf(buf, n, "%.2f%%", pct);
    else snprintf(buf, n, "%.2g%%", pct);
    return buf;
}

/* "3h 12m (at 17:33)" / "2d 4h 1m (on 2026-10-23 14:05)"; the clock part only for horizons under a year. */
static char *fmt_eta_at(double secs, bool with_clock, char *buf, size_t n)
{
    char d[48];
    ui_fmt_eta(secs, d, sizeof d);
    if (!with_clock || !(secs >= 0) || secs > 365.0 * 86400) { snprintf(buf, n, "%s", d); return buf; }
    time_t t = (time_t)(pz_now() + secs);
    struct tm tmv;
    pz_localtime(t, &tmv);
    char c[40];
    if (secs < 86400) strftime(c, sizeof c, "at %H:%M", &tmv);
    else strftime(c, sizeof c, "on %Y-%m-%d %H:%M", &tmv);
    snprintf(buf, n, "%s (%s)", d, c);
    return buf;
}

static void bar(char *b, int width, double frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int filled = (int)(frac * width + 0.5);
    for (int i = 0; i < width; i++) b[i] = i < filled ? '#' : '.';
    b[width] = 0;
}

static void percent(double frac, char *buf, size_t n)
{
    double pct = frac * 100.0;
    if (pct >= 0.001 || pct == 0) snprintf(buf, n, "%.4f%%", pct);
    else snprintf(buf, n, "%.3e%%", pct);
}

/* Shortens a line of the panel to at most maxw columns without cutting a word: first a remark in brackets at its end ("(peak ...)",
 * "[starting: ...]"), then its last fields ("a | b | c", "a  |  b"), and as a last resort its last words. */
static void fit_line(char *t, int maxw)
{
    while ((int)strlen(t) > maxw) {
        size_t l = strlen(t);
        size_t cut = 0;
        if (t[l - 1] == ')' || t[l - 1] == ']') {
            const char close = t[l - 1], open = close == ')' ? '(' : '[';
            int depth = 0;
            for (size_t i = l; i-- > 0;) {
                if (t[i] == close) depth++;
                else if (t[i] == open && --depth == 0) { cut = i; break; }
            }
        }
        if (!cut) {
            const char *bar = NULL;
            for (const char *q = strstr(t, " |"); q; q = strstr(q + 1, " |")) bar = q;
            if (bar && bar - t > 14) cut = (size_t)(bar - t);
        }
        if (!cut) {
            for (size_t i = (size_t)maxw; i > 0; i--)
                if (t[i] == ' ') { cut = i; break; }
        }
        if (!cut) { t[maxw] = 0; return; }       /* a single word longer than the line */
        while (cut > 0 && t[cut - 1] == ' ') cut--;
        t[cut] = 0;
    }
}

/* Appends a line to buf (shortened to `cols - 1` columns by fit_line), separated by '\n'. Returns the new length. */
static size_t add_line(char *buf, size_t n, size_t len, int *lines, int cols, const char *prefix, const char *suffix, const char *text)
{
    char t[640];
    snprintf(t, sizeof t, "%s", text);
    int maxw = cols - 1;
    if (maxw < 20) maxw = 20;
    fit_line(t, maxw);
    int w = snprintf(buf + len, n - len, "%s%s%s%s", *lines ? "\n" : "", prefix, t, suffix);
    if (w < 0 || (size_t)w >= n - len) return len;   /* does not fit: drop the line */
    (*lines)++;
    return len + (size_t)w;
}

int ui_render_full(const run_ctx *r, const progress_info *pi, const eta_summary *es, bool final, int cols, bool color, char *buf, size_t n)
{
    if (cols > 110) cols = 110;
    char idb[17], ln[640], a[96], b[96], c[96];
    seed_to_id(r->cfg.seed, idb);
    int lines = 0;
    size_t len = 0;
    buf[0] = 0;
    const char *B0 = color ? "\x1b[1m" : "", *B1 = color ? "\x1b[0m" : "";
    const char *D0 = color ? "\x1b[2m" : "", *D1 = color ? "\x1b[0m" : "";
    const bool kang = r->cfg.mode == MODE_KANGAROO;

    char pz[32] = "";
    if (r->cfg.puzzle) snprintf(pz, sizeof pz, " | puzzle #%d", r->cfg.puzzle);   /* a custom target: the campaign name says it all */
    snprintf(ln, sizeof ln, "pzsolve %s | campaign %s%s | %s | seed %.8s | %s", PZ_VERSION, r->cfg.name, pz,
             mode_name(r->cfg.mode), idb, ui_state_name(r, final));
    if ((int)strlen(ln) > cols - 1)                    /* a narrow terminal: the state matters more than the version */
        snprintf(ln, sizeof ln, "campaign %s%s | %s | seed %.8s | %s", r->cfg.name, pz, mode_name(r->cfg.mode), idb, ui_state_name(r, final));
    len = add_line(buf, n, len, &lines, cols, B0, B1, ln);

    pz_u256 rsz = cfg_range_size(&r->cfg);
    unsigned rbits = pz_u256_bits(&rsz) ? pz_u256_bits(&rsz) - 1 : 0;
    snprintf(ln, sizeof ln, "Target        %s   range of 2^%u keys", r->cfg.address, rbits);
    len = add_line(buf, n, len, &lines, cols, "", "", ln);

    char bb[48], pc[32];
    bar(bb, 30, pi->fraction);
    percent(pi->fraction, pc, sizeof pc);
    if (kang) {
        ui_fmt_count(pi->done, a, sizeof a);
        ui_fmt_count(pi->total, b, sizeof b);
        snprintf(ln, sizeof ln, "Success prob. [%s] %s   (%s %s done, ~%s expected on average)", bb, pc, a, pi->unit, b);
    } else {
        ui_fmt_count(pi->done, a, sizeof a);
        ui_fmt_count(pi->total, b, sizeof b);
        snprintf(ln, sizeof ln, "Progress      [%s] %s   (%s / %s %s)", bb, pc, a, b, pi->unit);
    }
    len = add_line(buf, n, len, &lines, cols, "", "", ln);

    double cpur, gpur;
    int ncpu;
    ui_rates(r, &cpur, &gpur, &ncpu);
    double el_session = run_session_elapsed(r);
    u64 session_units = 0;
    for (int i = 0; i < r->nws; i++) session_units += atomic_load(&r->ws[i].units);
    /* the session average of the current unit: BSGS counts its giant steps from the end of the table phase */
    const double el_unit = el_session - r->t_base;
    const u64 base = r->t_base > 0 ? atomic_load(&r->units_base) : 0;
    double avg = el_unit > 1 && session_units > base ? (double)(session_units - base) / el_unit : 0;
    pz_fmt_si(pi->rate, a, sizeof a);
    pz_fmt_si(r->peak_rate, b, sizeof b);
    pz_fmt_si(avg, c, sizeof c);
    if (pi->rate > 0 && !r->all_started)                      /* the first seconds: a GPU that has not finished a launch is not counted yet */
        snprintf(ln, sizeof ln, "Speed         %s %s/s   [starting: not all engines are counted yet]", a, pi->unit);
    else if (pi->rate > 0 && r->peak_rate > 0) snprintf(ln, sizeof ln, "Speed         %s %s/s   (peak %s, session average %s)", a, pi->unit, b, c);
    else if (pi->rate > 0) snprintf(ln, sizeof ln, "Speed         %s %s/s", a, pi->unit);
    else snprintf(ln, sizeof ln, "Speed         measuring...");
    len = add_line(buf, n, len, &lines, cols, "", "", ln);

    if (r->nws > 0) {
        size_t o = 0;
        ln[0] = 0;
        o += (size_t)snprintf(ln + o, sizeof ln - o, "              ");
        if (r->limit_pct) o += (size_t)snprintf(ln + o, sizeof ln - o, "limit %d%%   ", r->limit_pct);
        if (ncpu > 0) {
            pz_fmt_si(cpur, a, sizeof a);
            o += (size_t)snprintf(ln + o, sizeof ln - o, "CPU x%d: %s", ncpu, a);
        }
        int shown = 0;
        for (int i = 0; i < r->nws && o < 400; i++) {
            if (r->ws[i].kind != 'G') continue;
            pz_fmt_si(r->ws[i].rate, a, sizeof a);
            o += (size_t)snprintf(ln + o, sizeof ln - o, "%s%.40s: %s", (ncpu > 0 || shown) ? "  |  " : "", r->ws[i].name, a);
            shown++;
        }
        len = add_line(buf, n, len, &lines, cols, D0, D1, ln);
    }

    double el_total = r->elapsed_prev + el_session;
    pz_fmt_duration(el_session, a, sizeof a);
    pz_fmt_duration(el_total, b, sizeof b);
    snprintf(ln, sizeof ln, "Time          this session %s | total %s | session no. %d", a, b, r->sessions);
    len = add_line(buf, n, len, &lines, cols, "", "", ln);

    if (final && atomic_load(&r->found)) {
        len = add_line(buf, n, len, &lines, cols, B0, B1, "KEY FOUND");   /* the box below gives the file */
    } else if (pi->beyond) {
        len = add_line(buf, n, len, &lines, cols, B0, B1, "Time left     unknown (beyond the model)");
    } else if (pi->eta.model == ETA_NONE) {
        /* the search itself has not started (BSGS table phase): the only meaningful time is the end of this phase */
        if (pi->rate > 0 && pi->total > pi->done) {
            fmt_eta_at((double)((pi->total - pi->done) / pi->rate), 1, a, sizeof a);
            snprintf(ln, sizeof ln, "Time left     current phase: %s | the search estimate appears when it starts", a);
        } else snprintf(ln, sizeof ln, "Time left     calculating (the search estimate appears when it starts)");
        len = add_line(buf, n, len, &lines, cols, B0, B1, ln);
    } else if (pi->rate > 0 && pi->est_state != RATE_MEASURING) {
        const double tlast = kang ? es->t99 : es->t_full;
        const char *lastlab = kang ? "99%" : "100%";
        /* richest layout that fits the terminal: with clock times, without them, then dropping the last fields */
        int variant = 0;
        for (; variant < 4; variant++) {
            const int clock = variant == 0;
            fmt_eta_at(es->t50, clock, a, sizeof a);
            fmt_eta_at(es->t90, clock, b, sizeof b);
            fmt_eta_at(tlast, clock, c, sizeof c);
            if (variant < 2) snprintf(ln, sizeof ln, "Time left     50%%: %s | 90%%: %s | %s: %s", a, b, lastlab, c);
            else if (variant == 2) snprintf(ln, sizeof ln, "Time left     50%%: %s | 90%%: %s", a, b);
            else snprintf(ln, sizeof ln, "Time left     50%%: %s", a);
            if ((int)strlen(ln) < cols - 1) break;
        }
        len = add_line(buf, n, len, &lines, cols, B0, B1, ln);
        ln[0] = 0;
        size_t o = (size_t)snprintf(ln, sizeof ln, "Chance to find it from now:");
        for (int i = 0; i < ETA_NWIN; i++) {
            char one[48];
            ui_fmt_prob(es->p_win[i], a, sizeof a);
            int w = snprintf(one, sizeof one, "%s %s %s", i ? " |" : "", ETA_WINDOW_NAMES[i], a);
            if (i > 0 && (int)(o + (size_t)w) >= cols - 1) break;   /* drop whole windows rather than cutting one in half */
            if (o + (size_t)w >= sizeof ln) break;
            memcpy(ln + o, one, (size_t)w + 1);
            o += (size_t)w;
        }
        len = add_line(buf, n, len, &lines, cols, "", "", ln);
        if (pi->est_state == RATE_PROVISIONAL) {
            const char *from = r->rate_prior_kind == 2 ? "from the remembered speed" : r->rate_prior_kind == 1 ? "from the tuning run" : "";
            const double left = r->settle_at > 0 ? r->settle_at - pz_mono() : -1;   /* never a time that has already passed */
            char when[48] = "";
            if (left >= 60) snprintf(when, sizeof when, "settles in ~%d min", (int)ceil(left / 60));
            else if (left > 0) snprintf(when, sizeof when, "settles in ~%d s", (int)ceil(left));
            if (from[0] && when[0]) snprintf(ln, sizeof ln, "              provisional (%s, %s)", from, when);
            else if (from[0] || when[0]) snprintf(ln, sizeof ln, "              provisional (%s%s)", from, when);
            else snprintf(ln, sizeof ln, "              provisional");
            len = add_line(buf, n, len, &lines, cols, D0, D1, ln);
        }
    } else if (pi->rate > 0) {
        len = add_line(buf, n, len, &lines, cols, "", "", "Time left     measuring...");
    } else {
        len = add_line(buf, n, len, &lines, cols, "", "", "Time left     calculating...");
    }

    if (pi->detail[0]) {
        char det[320];
        pz_strlcpy(det, pi->detail, sizeof det);
        for (char *p = det; p && *p;) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            snprintf(ln, sizeof ln, "              %s", p);
            len = add_line(buf, n, len, &lines, cols, D0, D1, ln);
            p = nl ? nl + 1 : NULL;
        }
    }
    if (!final) len = add_line(buf, n, len, &lines, cols, D0, D1, "Ctrl-C: stop and save progress; run the same command again to resume.");
    (void)len;
    return lines;
}

void ui_render_line(const run_ctx *r, const progress_info *pi, const eta_summary *es, int cols, char *buf, size_t n)
{
    char rate[32], bb[24], pc[32], t1[48], t2[48], t3[48], pr[32], tag[96];
    ui_target_tag(r, tag, sizeof tag);
    pz_fmt_si(pi->rate, rate, sizeof rate);
    bar(bb, 20, pi->fraction);
    percent(pi->fraction, pc, sizeof pc);
    ui_fmt_eta(es->t50, t1, sizeof t1);
    ui_fmt_eta(es->t90, t2, sizeof t2);
    const bool kang = r->cfg.mode == MODE_KANGAROO;
    ui_fmt_eta(kang ? es->t99 : es->t_full, t3, sizeof t3);
    ui_fmt_prob(es->p_win[1], pr, sizeof pr);
    const int limit = cols > 20 ? cols - 1 : 0;   /* cols == 0: no terminal (log file), never shorten */
    if (pi->eta.model == ETA_NONE) {
        char tt[48] = "";
        if (pi->rate > 0 && pi->total > pi->done) ui_fmt_eta((double)((pi->total - pi->done) / pi->rate), tt, sizeof tt);
        snprintf(buf, n, "%s [%s] %s %s %s/s | table phase%s%s", tag, bb, pc, rate, pi->unit, tt[0] ? ", done in " : "", tt);
        if (limit && (int)strlen(buf) > limit) snprintf(buf, n, "%s %s %s %s/s", tag, pc, rate, pi->unit);
    } else if (pi->beyond) {
        snprintf(buf, n, "%s [%s] %s %s %s/s | time left unknown (beyond the model)", tag, bb, pc, rate, pi->unit);
        if (limit && (int)strlen(buf) > limit) snprintf(buf, n, "%s %s %s %s/s | time left unknown", tag, pc, rate, pi->unit);
    } else if (pi->rate > 0 && pi->est_state != RATE_MEASURING) {
        /* richest layout that fits, then drop fields from the least important: bar, P(24h), the last quantile */
        for (int v = 0; v < 5; v++) {
            if (v == 0) snprintf(buf, n, "%s [%s] %s %s %s/s | 50%%:%s 90%%:%s %s:%s | P(24h)=%s", tag, bb, pc, rate, pi->unit, t1, t2, kang ? "99%" : "100%", t3, pr);
            else if (v == 1) snprintf(buf, n, "%s %s %s %s/s | 50%%:%s 90%%:%s %s:%s | P(24h)=%s", tag, pc, rate, pi->unit, t1, t2, kang ? "99%" : "100%", t3, pr);
            else if (v == 2) snprintf(buf, n, "%s %s %s %s/s | 50%%:%s 90%%:%s %s:%s", tag, pc, rate, pi->unit, t1, t2, kang ? "99%" : "100%", t3);
            else if (v == 3) snprintf(buf, n, "%s %s %s %s/s | 50%%:%s 90%%:%s", tag, pc, rate, pi->unit, t1, t2);
            else snprintf(buf, n, "%s %s %s/s | 50%%:%s", tag, pc, rate, t1);
            if (!limit || (int)strlen(buf) <= limit) break;
        }
        if (pi->est_state == RATE_PROVISIONAL) {                       /* the estimate is still settling: say so if it fits */
            const size_t l = strlen(buf);
            if (l + 15 < n && (!limit || (int)l + 14 <= limit)) memcpy(buf + l, " (provisional)", 15);
        }
    } else
        snprintf(buf, n, "%s [%s] %s | measuring speed...", tag, bb, pc);
    if (limit && (int)strlen(buf) > limit) buf[limit] = 0;
}

void ui_render_final_line(const run_ctx *r, const progress_info *pi, const eta_summary *es, char *buf, size_t n)
{
    char pc[32], tag[96], cnt[48], a[128];
    ui_target_tag(r, tag, sizeof tag);
    percent(pi->fraction, pc, sizeof pc);
    const bool kang = r->cfg.mode == MODE_KANGAROO;
    const char *state = ui_state_name(r, true);
    if (kang) snprintf(a, sizeof a, "%s steps, success probability %s", ui_fmt_count(pi->done, cnt, sizeof cnt), pc);
    else snprintf(a, sizeof a, "%s covered", pc);
    if (strcmp(state, "STOPPED") != 0) snprintf(buf, n, "%s %s, %s", tag, state, a);   /* FOUND, COMPLETED: nothing left to estimate */
    else if (pi->rate > 0 && pi->est_state != RATE_MEASURING && !pi->beyond) ui_render_line(r, pi, es, 0, buf, n);
    else snprintf(buf, n, "%s %s, %s", tag, state, a);
}
