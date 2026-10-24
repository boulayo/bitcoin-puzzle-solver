/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_ui.c - the dashboard / summary renderers: no overflow, no control characters when colours are off, every line fits the
 * terminal width, sane output for degenerate numbers (zero rate, infinite ETA, empty progress, huge counts). */
#include "../src/ui.h"
#include <math.h>
#include <ctype.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void check_text(const char *txt, int lines, int cols, bool color)
{
    int nl = 1, cur = 0;
    for (const char *p = txt; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') { nl++; CHECK(cur <= cols - 1 || cols < 22); cur = 0; continue; }
        if (ch == 0x1b) {                       /* skip an SGR sequence */
            CHECK(color);
            while (*p && *p != 'm') p++;
            if (!*p) break;
            continue;
        }
        CHECK(ch >= 32 && ch < 127);            /* printable ASCII only: widths are byte counts */
        cur++;
    }
    CHECK(cur <= cols - 1 || cols < 22);
    CHECK(nl == lines);
}


/* pz_fputs_wrap: no line longer than the width, the words kept in order, continuation lines indented, long words left whole */
static char *wrap_to_string(const char *text, int width, int hang, char *out, size_t n)
{
    FILE *f = tmpfile();
    if (!f) return NULL;
    pz_fputs_wrap(f, text, width, hang);
    rewind(f);
    size_t got = fread(out, 1, n - 1, f);
    out[got] = 0;
    fclose(f);
    return out;
}

static void squeeze(const char *in, char *out)      /* the text without any white space */
{
    for (; *in; in++) if (*in != ' ' && *in != '\n') *out++ = *in;
    *out = 0;
}

static void test_wrap(void)
{
    static const struct { const char *text; int hang; } cases[] = {
        {"[12:00:00] INFO   a message that is far too long for the line because it keeps going and going and going on", 18},
        {"  --opt X             description of the option that needs several lines, and a second sentence with more words", -1},
        {"  --opt X             first line of the description\n                      continued here, joined and wrapped", -1},
        {"short", 4},
    };
    char out[2048], a[2048], b[2048];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK(wrap_to_string(cases[i].text, 60, cases[i].hang, out, sizeof out));
        int cur = 0;
        for (const char *q = out;; q++) {
            if (*q == '\n' || !*q) { CHECK(cur <= 60); cur = 0; if (!*q) break; } else cur++;
        }
        squeeze(cases[i].text, a);
        squeeze(out, b);
        CHECK(!strcmp(a, b));
    }
    /* hanging indent: the continuation lines start under the description (column 22) */
    wrap_to_string(cases[1].text, 60, -1, out, sizeof out);
    const char *nl = strchr(out, '\n');
    CHECK(nl && nl[22] == ' ' && nl[23] != ' ');
    /* a word longer than the line is not cut */
    wrap_to_string("x /very/long/path/that/cannot/be/broken/anywhere/at/all/because/it/has/no/spaces/inside/it/ok y", 40, 2, out, sizeof out);
    CHECK(strstr(out, "/very/long/path/that/cannot/be/broken/anywhere/at/all/because/it/has/no/spaces/inside/it/ok"));
}

int main(void)
{
    static run_ctx r;
    memset(&r, 0, sizeof r);
    r.cfg.puzzle = 71;
    snprintf(r.cfg.name, sizeof r.cfg.name, "p71");
    snprintf(r.cfg.address, sizeof r.cfg.address, "1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU");
    r.cfg.range_start = u256_pow2(70);
    r.cfg.range_end = u256_pow2(71);
    u256_sub(&r.cfg.range_end, &r.cfg.range_end, &(u256){{1, 0, 0, 0}});
    r.sessions = 3;
    r.t_start = pz_mono() - 100;
    r.elapsed_prev = 1e6;
    r.peak_rate = 5e8;
    pz_strlcpy(r.ws[0].name, "CPU0", sizeof r.ws[0].name); r.ws[0].kind = 'C'; r.ws[0].rate = 2e7; r.nws = 1;
    pz_strlcpy(r.ws[1].name, "GPU0 with a decidedly very very long name that must be truncated completely", sizeof r.ws[1].name); r.ws[1].kind = 'G'; r.ws[1].rate = 3e8; r.nws = 2;
    const search_mode modes[3] = {MODE_LINEAR, MODE_KANGAROO, MODE_BSGS};
    for (int mi = 0; mi < 3; mi++) {
        r.cfg.mode = modes[mi];
        for (int variant = 0; variant < 9; variant++) {
            progress_info pi;
            memset(&pi, 0, sizeof pi);
            pi.unit = "keys";
            pi.rate = pi.rate_eta = variant == 0 ? 0 : 3.2e8;
            pi.total = variant == 3 ? 1e30L : 1.18e21L;
            pi.done = variant == 1 ? 0 : (variant == 4 ? pi.total : 7.2e19L);
            pi.fraction = (double)(pi.done / pi.total);
            if (variant == 5) { pi.fraction = 1e-300; pi.rate = pi.rate_eta = 1e-9; }
            if (variant == 6) { pi.rate = pi.rate_eta = 1e18; pi.done = pi.total / 2; pi.fraction = 0.5; }
            snprintf(pi.detail, sizeof pi.detail, "first line of the description with many details\nsecond line with more details and numbers 123456");
            if (variant == 7 || variant == 8) {   /* BSGS table phase: no search estimate yet */
                pi.unit = "baby step";
                pi.total = 4e9L; pi.done = 1e9L; pi.fraction = 0.25;
                pi.rate = pi.rate_eta = variant == 7 ? 1.5e7 : 0;
            }
            pi.eta = variant >= 7 ? (eta_in){ETA_NONE, pi.done, pi.total - pi.done, 0, false, 0}
                   : mi == 1 ? (eta_in){ETA_RAYLEIGH, pi.done, 0, 1.43e10L, false, 0} : (eta_in){ETA_UNIFORM, pi.done, pi.total - pi.done, 0, false, 0};
            eta_summary es;
            eta_summarize(&pi.eta, pi.rate_eta, &es);
            char buf[4096];
            for (int cols = 10; cols <= 200; cols += 7) {
                for (int color = 0; color < 2; color++) {
                    for (int fin = 0; fin < 2; fin++) {
                        int lines = ui_render_full(&r, &pi, &es, fin, cols, color, buf, sizeof buf);
                        CHECK(lines >= 5 && lines <= 12);
                        check_text(buf, lines, cols, color);
                        char one[600];
                        ui_render_line(&r, &pi, &es, cols, one, sizeof one);
                        CHECK(strlen(one) < 600 && (cols < 22 || (int)strlen(one) <= cols - 1));
                        CHECK(!strstr(one, "nan") && !strstr(one, "inf"));
                        CHECK(!strstr(buf, "nan") && !strstr(buf, "inf"));
                        /* fields are dropped whole: a probability is never cut after the '=' */
                        const char *pp = strstr(one, "P(24h)=");
                        if (pp) CHECK(one[strlen(one) - 1] == '%' || one[strlen(one) - 1] == 'a');
                        if (variant == 7 && cols >= 100) CHECK(strstr(buf, "current phase") && !strstr(buf, "50%:"));
                    }
                }
            }
            /* tiny output buffers must not overflow (lines are dropped instead) */
            char tiny[64];
            int l2 = ui_render_full(&r, &pi, &es, false, 80, false, tiny, sizeof tiny);
            CHECK(l2 >= 0 && strlen(tiny) < sizeof tiny);
        }
    }
    /* --limit: the panel says so on the line of the workers, at every width, and not without it */
    {
        r.cfg.mode = MODE_LINEAR;
        progress_info pi;
        memset(&pi, 0, sizeof pi);
        pi.unit = "keys";
        pi.rate = pi.rate_eta = 3.2e8;
        pi.total = 1.18e21L;
        pi.done = 7.2e19L;
        pi.fraction = (double)(pi.done / pi.total);
        pi.eta = (eta_in){ETA_UNIFORM, pi.done, pi.total - pi.done, 0, false, 0};
        eta_summary es;
        eta_summarize(&pi.eta, pi.rate_eta, &es);
        char buf[4096];
        ui_render_full(&r, &pi, &es, false, 100, false, buf, sizeof buf);
        CHECK(!strstr(buf, "limit"));
        r.limit_pct = 37;
        for (int cols = 40; cols <= 130; cols += 9) {
            int lines = ui_render_full(&r, &pi, &es, false, cols, false, buf, sizeof buf);
            check_text(buf, lines, cols, false);
        }
        ui_render_full(&r, &pi, &es, false, 100, false, buf, sizeof buf);
        CHECK(strstr(buf, "limit 37%"));
        r.limit_pct = 0;
    }
    CHECK(!strcmp(ui_fmt_prob(0, (char[16]){0}, 16), "0%"));
    CHECK(!strcmp(ui_fmt_prob(NAN, (char[16]){0}, 16), "n/a"));
    char b[32];
    CHECK(!strcmp(ui_fmt_prob(1.0, b, sizeof b), "100%"));
    CHECK(!strcmp(ui_fmt_eta(INFINITY, b, sizeof b), "n/a") && !strcmp(ui_fmt_eta(-1, b, sizeof b), "n/a"));
    test_wrap();
    puts(fails ? "t_ui: FAILED" : "t_ui: ok");
    return fails != 0;
}
