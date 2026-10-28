/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* util.c - logging, formatting, file helpers. */
#include "common.h"
#include "platform.h"
#include "envopts.h"
#include <pzcore/u256.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

/* g_log_mu guards the log file, the terminal output of the messages and the dashboard block. pz_die can be called from any thread, also
 * from code that holds the lock, so the thread that holds it is recorded: pz_die must not wait for a lock that it holds itself. */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool t_log_held;
static FILE *g_log_fp;
static int g_verbose, g_quiet;
static int g_block_lines;   /* lines of the in-place dashboard currently on screen (0 = none), guarded by g_log_mu */

static void log_lock(void)
{
    pthread_mutex_lock(&g_log_mu);
    t_log_held = true;
}

static void log_unlock(void)
{
    t_log_held = false;
    pthread_mutex_unlock(&g_log_mu);
}

static void log_stamp(char *ts, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;
    pz_localtime(t, &tmv);
    strftime(ts, n, "%Y-%m-%d %H:%M:%S", &tmv);
}

void pz_die(const char *fmt, ...)
{
    char msg[1024], ts[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_stamp(ts, sizeof ts);
    const bool own = t_log_held;   /* called from inside the logging code: the lock is already ours */
    if (!own) log_lock();
    if (g_block_lines) { fputc('\n', stderr); g_block_lines = 0; }
    char ln[1100];
    snprintf(ln, sizeof ln, "ERROR: %s", msg);
    pz_fputs_wrap(stderr, ln, PZ_LINE_MAX, 7);
    fputc('\n', stderr);
    if (g_log_fp) {   /* the campaign log says why the run stopped */
        fprintf(g_log_fp, "%s [ERROR] %s\n", ts, msg);
        fflush(g_log_fp);
    }
    if (!own) log_unlock();
    exit(PZ_EXIT_ERROR);
}

void pz_log_set_file(const char *path)
{
    log_lock();
    if (g_log_fp) fclose(g_log_fp);
    g_log_fp = path ? fopen(path, "ab") : NULL;   /* binary: on Windows the text mode would write CRLF */
    if (g_log_fp) setvbuf(g_log_fp, NULL, _IOLBF, 0);
    log_unlock();
}
void pz_log_set_verbose(int v) { g_verbose = v; }
void pz_log_set_quiet(int q) { g_quiet = q; }
int pz_log_quiet(void) { return g_quiet; }

/* Erases the dashboard block: the cursor sits at the end of its last line. */
static void block_erase_locked(void)
{
    if (g_block_lines > 0) {
        if (g_block_lines > 1) fprintf(stderr, "\x1b[%dA", g_block_lines - 1);
        fputs("\r\x1b[J", stderr);
        g_block_lines = 0;
    }
}

void pz_ui_block(const char *text, int nlines)
{
    log_lock();
    block_erase_locked();
    fputs(text, stderr);
    fflush(stderr);
    g_block_lines = nlines;
    log_unlock();
}

void pz_log_terminal_line_break(void)
{
    log_lock();
    if (g_block_lines) {
        fputc('\n', stderr);
        g_block_lines = 0;
    }
    log_unlock();
}

static int text_cols(const char *s, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;   /* UTF-8 continuation bytes take no column */
    return c;
}

/* A token that is a unit or a suffix of the number before it ("34.78 M", "12 MiB", "2 minutes"): never separated from that number. */
static bool unit_token(const char *t, const char *end)
{
    static const char *const units[] = {"M", "G", "k", "K", "T", "MiB", "GiB", "KiB", "MB", "GB", "kB", "MHz", "GHz", "kHz", "s", "ms", "us", "ns",
                                        "%", "x", "B", "steps/s", "keys/s", "M/s", "G/s", "MB/s", "GB/s", "years", "days", "hours", "minutes",
                                        "seconds", "bytes", "steps", "keys", "threads", "thread", NULL};
    size_t n = 0;
    while (t + n < end && t[n] != ' ' && t[n] != ',' && t[n] != ';' && t[n] != ')' && t[n] != ':') n++;
    for (int i = 0; units[i]; i++)
        if (strlen(units[i]) == n && !strncmp(units[i], t, n)) return true;
    return false;
}

/* How good a place the space at q is to end a line: -1 forbidden, otherwise higher is better (the end of a sentence, a semicolon or a
 * colon, a comma, the start of a parenthesis). */
static int break_score(const char *s, const char *q, const char *end)
{
    if (q <= s || q + 1 >= end || q[1] == ' ') return -1;
    const char prev = q[-1];
    const char *tok = q;
    while (tok > s && tok[-1] != ' ') tok--;
    if ((size_t)(q - tok) == 2 && !strncmp(tok, "->", 2)) return -1;                          /* an arrow stays with its value */
    if (q[1] == '-' && q + 3 < end && q[2] == '>' && q[3] == ' ') return -1;
    if (prev >= '0' && prev <= '9' && unit_token(q + 1, end)) return -1;                       /* "27.61 M": number and unit stay together */
    if (prev == '.' && q + 1 < end && ((q[1] >= 'A' && q[1] <= 'Z') || q[1] == '(')) return 5; /* a new sentence */
    if (prev == ';' || prev == '|') return 4;
    if (prev == ':' || prev == ',') return 3;
    if (q[1] == '(') return 2;
    if (prev == ')') return 1;
    return 0;
}

/* Where to break the text s..end so that the first line fits `width` columns (it starts at column col0): at the best place in the
 * second half of the line, never leaving a last line of a few characters; NULL when there is no space to break at. */
static const char *pick_break(const char *s, const char *end, int col0, int width)
{
    const int low = col0 + (width - col0) * 45 / 100;
    const char *best = NULL, *fit = NULL, *first_after = NULL;
    int best_score = -1;
    int col = col0;
    for (const char *q = s; q < end; q++) {
        if (*q == ' ') {
            const int sc = break_score(s, q, end);
            if (sc >= 0) {
                if (col <= width) {
                    fit = q;
                    const int rest = text_cols(q + 1, (size_t)(end - q - 1));
                    const int value = sc * 10 + 40 * col / width;   /* a natural place counts for more, a fuller line for a little */
                    if (col >= low && (rest >= 10 || !best) && value >= best_score) { best = q; best_score = value; }
                } else if (!first_after) first_after = q;
            }
        }
        if (((unsigned char)*q & 0xC0) != 0x80) col++;
    }
    if (best) return best;
    if (fit) return fit;
    return first_after;                       /* a word longer than the line: break after it */
}

/* Column where the description of an option or command starts in this line, or -1 when the line has none. */
static int desc_col(const char *p, size_t len, int width)
{
    size_t lead = 0;
    while (lead < len && p[lead] == ' ') lead++;
    for (size_t i = lead; i + 1 < len; i++)
        if (p[i] == ' ' && p[i + 1] == ' ') {
            size_t j = i;
            while (j < len && p[j] == ' ') j++;
            if (j < len && text_cols(p, j) <= width / 2) return text_cols(p, j);
            break;
        }
    return -1;
}

void pz_fputs_wrap(FILE *fp, const char *text, int width, int hang)
{
    bool first = true;
    char *line = NULL;
    size_t cap = 0;
    for (const char *p = text; *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *next = nl ? nl + 1 : p + len;
        size_t lead = 0;
        while (lead < len && p[lead] == ' ') lead++;
        int h = (int)lead + 2;
        if (hang >= 0 && first) h = hang;
        else if (hang < 0) {
            int dc = desc_col(p, len, width);
            if (dc >= 0) {
                h = dc;
                /* the following lines indented at least as far continue this description: join them and wrap again */
                size_t need = len + 1;
                const char *q = next;
                size_t total = len;
                while (*q) {
                    const char *qn = strchr(q, '\n');
                    size_t ql = qn ? (size_t)(qn - q) : strlen(q);
                    size_t qlead = 0;
                    while (qlead < ql && q[qlead] == ' ') qlead++;
                    if (qlead < (size_t)dc || qlead == ql) break;
                    total += 1 + (ql - qlead);
                    q = qn ? qn + 1 : q + ql;
                }
                need = total + 1;
                if (q != next) {
                    if (need > cap) { cap = need; line = realloc(line, cap); if (!line) return; }
                    memcpy(line, p, len);
                    size_t o = len;
                    for (const char *r = next; r < q;) {
                        const char *rn = strchr(r, '\n');
                        size_t rl = rn ? (size_t)(rn - r) : strlen(r);
                        size_t rlead = 0;
                        while (rlead < rl && r[rlead] == ' ') rlead++;
                        line[o++] = ' ';
                        memcpy(line + o, r + rlead, rl - rlead);
                        o += rl - rlead;
                        r = rn ? rn + 1 : r + rl;
                    }
                    line[o] = 0;
                    p = line;
                    len = o;
                    next = q;
                }
            }
        }
        const char *s = p, *end = p + len;
        int col0 = 0;
        while (text_cols(s, (size_t)(end - s)) + col0 > width) {
            const char *brk = pick_break(s, end, col0, width);
            if (!brk) break;
            const char *e = brk;
            while (e > s && e[-1] == ' ') e--;
            fwrite(s, 1, (size_t)(e - s), fp);
            fputc('\n', fp);
            for (int i = 0; i < h; i++) fputc(' ', fp);
            col0 = h;
            s = brk;
            while (s < end && *s == ' ') s++;
        }
        fwrite(s, 1, (size_t)(end - s), fp);
        if (next > text && next[-1] == '\n') fputc('\n', fp);
        p = next;
        first = false;
    }
    free(line);
}

void pz_log(pz_loglevel lvl, const char *fmt, ...)
{
    static const char *tag[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    char ts[32];
    log_stamp(ts, sizeof ts);

    log_lock();
    if (g_log_fp) fprintf(g_log_fp, "%s [%s] %s\n", ts, tag[lvl], msg);
    if (lvl == PZ_LOG_DEBUG && !g_verbose) { log_unlock(); return; }
    if (g_quiet < 2 && (!g_quiet || lvl == PZ_LOG_ERR || lvl == PZ_LOG_WARN)) {
        block_erase_locked();   /* the dashboard is redrawn below the message at its next update */
        char ln[1100 + 32];
        snprintf(ln, sizeof ln, "[%s] %-6s %s", ts + 11, tag[lvl], msg);
        pz_fputs_wrap(stderr, ln, PZ_LINE_MAX, 18);
        fputc('\n', stderr);
    }
    log_unlock();
}

void pz_strlcpy(char *dst, const char *src, size_t n)
{
    if (!n) return;
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

int pz_atoi(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; } else if (*s == '+') s++;
    long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 2147483647LL) { v = 2147483647LL; break; }
        s++;
    }
    return (int)(neg ? -v : v);
}

u64 pz_strtou64(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    u64 v = 0;
    while (*s >= '0' && *s <= '9') {
        u64 d = (u64)(*s - '0');
        if (v > (~(u64)0 - d) / 10) return ~(u64)0;
        v = v * 10 + d;
        s++;
    }
    return v;
}

bool pz_parse_u64(const char *s, u64 *out)
{
    if (!s || !*s) return false;
    u64 v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        u64 d = (u64)(*s - '0');
        if (v > (~(u64)0 - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

bool pz_parse_i64(const char *s, i64 *out)
{
    if (!s) return false;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    u64 v;
    if (!pz_parse_u64(s, &v)) return false;
    if (neg ? v > (u64)INT64_MAX + 1 : v > (u64)INT64_MAX) return false;
    *out = neg ? (i64)(0 - v) : (i64)v;
    return true;
}

static const char *kv_at(const char *p, const char *key, size_t kl, char sep, unsigned flags)
{
    if (strncmp(p, key, kl) != 0) return NULL;
    p += kl;
    if (flags & PZ_KV_PAD)
        while (*p == ' ' || *p == '\t') p++;
    return *p == sep ? p + 1 : NULL;
}

const char *pz_kv_find(const char *text, const char *key, char sep, unsigned flags)
{
    const size_t kl = strlen(key);
    const char *hit = NULL;
    for (const char *p = text; p && *p;) {
        const char *v = kv_at(p, key, kl, sep, flags);
        if (v) {
            if (!(flags & PZ_KV_LAST)) return v;
            hit = v;
        }
        if (flags & PZ_KV_ANYWHERE) p++;
        else if ((p = strchr(p, '\n')) != NULL) p++;
    }
    return hit;
}

bool pz_parse_rate(const char *s, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end != s && end[0] && !end[1]) {
        switch (end[0]) {
        case 'k': case 'K': v *= 1e3; end++; break;
        case 'm': case 'M': v *= 1e6; end++; break;
        case 'g': case 'G': v *= 1e9; end++; break;
        case 't': case 'T': v *= 1e12; end++; break;
        default: break;
        }
    }
    if (end == s || *end || errno == ERANGE || !(v > 0) || v > 1e15) return false;
    *out = v;
    return true;
}

long long pz_arg_int(const char *name, const char *s, long long lo, long long hi)
{
    i64 v;
    if (!pz_parse_i64(s, &v) || v < lo || v > hi) pz_die("%s: invalid value '%s' (expected an integer between %lld and %lld)", name, s, lo, hi);
    return v;
}

bool pz_index_list_valid(const char *s)
{
    if (!*s) return false;
    for (const char *q = s;; q++) {
        const bool digit = *q >= '0' && *q <= '9';
        const bool end = (*q == ',' || *q == 0) && q > s && q[-1] >= '0' && q[-1] <= '9';
        if (!digit && !end) return false;
        if (!*q) return true;
    }
}

void *pz_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) pz_die("out of memory (malloc %zu bytes)", n);
    return p;
}
char *pz_xstrdup(const char *s)
{
    size_t l = strlen(s) + 1;
    return memcpy(pz_xmalloc(l), s, l);
}

void *pz_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) pz_die("out of memory (calloc %zu x %zu bytes)", n, sz);
    return p;
}
void *pz_xaligned(size_t align, size_t n)
{
    void *p = NULL;
    size_t sz = (n + align - 1) / align * align;
    if (sz == 0) sz = align;
    if (!(p = pz_aligned_raw(align, sz))) pz_die("out of memory (aligned %zu bytes)", n);
    memset(p, 0, sz);
    return p;
}
void *pz_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) pz_die("out of memory (realloc %zu bytes)", n);
    return q;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int pz_hex_decode(const char *hex, u8 *out, size_t maxlen)
{
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > maxlen) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (u8)(a << 4 | b);
    }
    return (int)(n / 2);
}

void pz_hex_encode(const u8 *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

char *u128_to_dec(u128 v, char *buf)
{
    char tmp[48];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int j = 0;
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}

char *pz_fmt_si(double v, char *buf, size_t n)
{
    static const char *suf[] = {"", " k", " M", " G", " T", " P", " E"};
    int i = 0;
    while (v >= 1000.0 && i < 6) { v /= 1000.0; i++; }
    snprintf(buf, n, "%.2f%s", v, suf[i]);
    return buf;
}

char *pz_fmt_bytes(double v, char *buf, size_t n)
{
    static const char *suf[] = {" B", " KiB", " MiB", " GiB", " TiB", " PiB"};
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    snprintf(buf, n, "%.2f%s", v, suf[i]);
    return buf;
}

/* "1234567" -> "1,234,567" (digits only; any other text is copied unchanged). */
char *pz_group_digits(const char *in, char *out, size_t n)
{
    size_t len = strlen(in);
    bool digits = len > 0;
    for (size_t i = 0; i < len; i++) if (in[i] < '0' || in[i] > '9') digits = false;
    if (!digits || len + (len - 1) / 3 + 1 > n) { pz_strlcpy(out, in, n); return out; }
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = in[i];
    }
    out[o] = 0;
    return out;
}

char *pz_fmt_duration(double s, char *buf, size_t n)
{
    if (!(s >= 0)) { snprintf(buf, n, "n/a"); return buf; }
    double y = s / 31557600.0;
    if (y >= 1e12) { snprintf(buf, n, "%.1e years", y); return buf; }
    if (y >= 1e9) { snprintf(buf, n, "%.1f billion years", y / 1e9); return buf; }
    if (y >= 1e6) { snprintf(buf, n, "%.1f million years", y / 1e6); return buf; }
    if (y >= 1000.0) {
        char d[32], g[48];
        snprintf(d, sizeof d, "%.0f", y);
        snprintf(buf, n, "%s years", pz_group_digits(d, g, sizeof g));
        return buf;
    }
    if (y >= 1.0) { snprintf(buf, n, "%.1f years", y); return buf; }
    unsigned long long t = (unsigned long long)s;
    unsigned long long d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60, sec = t % 60;
    if (d) snprintf(buf, n, "%llud %lluh %llum", d, h, m);
    else if (h) snprintf(buf, n, "%lluh %llum %llus", h, m, sec);
    else if (m) snprintf(buf, n, "%llum %llus", m, sec);
    else if (s < 0.05) snprintf(buf, n, "<0.1s");
    else snprintf(buf, n, "%.1fs", s);
    return buf;
}

int pz_mkdir_p(const char *path)
{
    char tmp[1024];
    if (strlen(path) >= sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            if (p[-1] == ':' || p[-1] == '/' || p[-1] == '\\') continue;   /* "C:\", "//server" */
            char c = *p;
            *p = 0;
            if (pz_mkdir1(tmp) != 0) return -1;
            *p = c;
        }
    }
    return pz_mkdir1(tmp);   /* an existing path must be a directory */
}

int pz_write_file_atomic(const char *path, const void *data, size_t len, int mode) { return pz_write_file_atomic_ex(path, data, len, mode, true); }

int pz_write_file_atomic_ex(const char *path, const void *data, size_t len, int mode, bool durable)
{
    /* The temporary file has a name of its own (process and counter): programs and threads that write the same file at the same
     * moment, such as the caches shared by every pzsolve of the user, must not share it. */
    static _Atomic unsigned seq;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.%d.%u.tmp", path, pz_getpid(), atomic_fetch_add(&seq, 1));
    int fd = pz_open_trunc(tmp, mode);        /* FOUND.txt holds the private key: 0600 */
    if (fd < 0) return -1;
    const u8 *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left > 0x40000000 ? 0x40000000 : left);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    if (durable && pz_fd_sync(fd)) { close(fd); unlink(tmp); return -1; }
    if (close(fd)) { unlink(tmp); return -1; }
    if (pz_rename_replace_ex(tmp, path, durable)) { unlink(tmp); return -1; }
    return 0;
}

int pz_keyed_file_put(const char *path, const char *key, const char *val)
{
    /* The read-merge-write runs under an exclusive lock of path.lock, so that two programs that save at the same moment keep both
     * lines. The lock is a courtesy, not a requirement: where the system gives none (a file system without locks) or it is not
     * obtained within a second, the line is saved without it. */
    char lockp[1100];
    snprintf(lockp, sizeof lockp, "%s.lock", path);
    const int lfd = pz_open_rw(lockp, true);
    for (int i = 0; lfd >= 0 && i < 100 && pz_lock_try(lfd, true) == 1; i++) pz_sleep(0.01);
    char *old = pz_read_file(path, NULL);
    const size_t kl = strlen(key), cap = (old ? strlen(old) : 0) + kl + strlen(val) + 8;
    char *out = pz_xcalloc(cap, 1);
    size_t len = 0;
    for (char *line = old; line && *line;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        const char *eq = memchr(line, '=', ll);
        const bool same = eq && (size_t)(eq - line) == kl && !strncmp(line, key, kl);
        if (!same && ll > 0 && len + ll + 2 < cap) { memcpy(out + len, line, ll); len += ll; out[len++] = '\n'; }
        line = nl ? nl + 1 : NULL;
    }
    len += (size_t)snprintf(out + len, cap - len, "%s=%s\n", key, val);
    const int rc = pz_write_file_atomic(path, out, len, 0644);
    free(out);
    free(old);
    if (lfd >= 0) close(lfd);                 /* releases the lock */
    return rc;
}

char *pz_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = pz_xmalloc(cap);
    for (;;) {
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (n < cap - 1) break;
        cap *= 2;
        buf = pz_xrealloc(buf, cap);
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

long long pz_file_count(const char *path, const char *needle)
{
    const size_t nl = strlen(needle);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    enum { BLOCK = 65536 };
    char *buf = pz_xmalloc(BLOCK + nl);
    size_t keep = 0;                          /* the last nl - 1 bytes of the previous block: a match can begin in them */
    long long count = 0;
    for (size_t got; nl && (got = fread(buf + keep, 1, BLOCK, f)) > 0;) {
        const size_t len = keep + got;
        for (size_t i = 0; i + nl <= len; i++) {   /* every start position is looked at once: the ones that do not fit wait for the next block */
            const char *q = memchr(buf + i, needle[0], len - nl + 1 - i);
            if (!q) break;
            i = (size_t)(q - buf);
            if (!memcmp(q, needle, nl)) count++;
        }
        keep = nl - 1 < len ? nl - 1 : len;
        memmove(buf, buf + len - keep, keep);
    }
    free(buf);
    fclose(f);
    return count;
}

bool pz_cache_file(const char *name, char *out, size_t n)
{
    const char *x = g_env.xdg_cache_home, *h = g_env.home, *la = g_env.local_app_data;
    char dir[PZ_ENV_PATH + 32];                  /* room for any folder name kept in g_env and the part after it */
    if (x[0]) snprintf(dir, sizeof dir, "%s/pzsolve", x);
#ifdef _WIN32
    else if (la[0]) snprintf(dir, sizeof dir, "%s/pzsolve", la);
#endif
    else if (h[0]) snprintf(dir, sizeof dir, "%s/.cache/pzsolve", h);
    else return false;
    (void)la;
    if (pz_mkdir_p(dir) != 0) return false;
    snprintf(out, n, "%s/%s", dir, name);
    return true;
}
