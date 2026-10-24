/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* util.c - logging, formatting, file helpers. */
#include "common.h"
#include "platform.h"
#include "u256.h"
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp;
static int g_verbose, g_quiet;
static int g_block_lines;   /* lines of the in-place dashboard currently on screen (0 = none), guarded by g_log_mu */

void pz_die(const char *fmt, ...)
{
    va_list ap;
    if (g_block_lines) { fputc('\n', stderr); g_block_lines = 0; }
    fputs("ERROR: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void pz_log_set_file(const char *path)
{
    pthread_mutex_lock(&g_log_mu);
    if (g_log_fp) fclose(g_log_fp);
    g_log_fp = path ? fopen(path, "a") : NULL;
    if (g_log_fp) setvbuf(g_log_fp, NULL, _IOLBF, 0);
    pthread_mutex_unlock(&g_log_mu);
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
    pthread_mutex_lock(&g_log_mu);
    block_erase_locked();
    fputs(text, stderr);
    fflush(stderr);
    g_block_lines = nlines;
    pthread_mutex_unlock(&g_log_mu);
}

void pz_log_terminal_line_break(void)
{
    pthread_mutex_lock(&g_log_mu);
    if (g_block_lines) {
        fputc('\n', stderr);
        g_block_lines = 0;
    }
    pthread_mutex_unlock(&g_log_mu);
}

void pz_log(pz_loglevel lvl, const char *fmt, ...)
{
    static const char *tag[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    time_t t = time(NULL);
    struct tm tmv;
    pz_localtime(t, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_log_mu);
    if (g_log_fp) fprintf(g_log_fp, "%s [%s] %s\n", ts, tag[lvl], msg);
    if (lvl == PZ_LOG_DEBUG && !g_verbose) { pthread_mutex_unlock(&g_log_mu); return; }
    if (!g_quiet || lvl == PZ_LOG_ERR || lvl == PZ_LOG_WARN) {
        block_erase_locked();   /* the dashboard is redrawn below the message at its next update */
        fprintf(stderr, "[%s] %-6s %s\n", ts + 11, tag[lvl], msg);
    }
    pthread_mutex_unlock(&g_log_mu);
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

char *pz_fmt_duration(double s, char *buf, size_t n)
{
    if (!(s >= 0)) { snprintf(buf, n, "n/a"); return buf; }
    double y = s / 31557600.0;
    if (y >= 1e12) { snprintf(buf, n, "%.1e years", y); return buf; }
    if (y >= 1e9) { snprintf(buf, n, "%.1f billion years", y / 1e9); return buf; }
    if (y >= 1e6) { snprintf(buf, n, "%.1f million years", y / 1e6); return buf; }
    if (y >= 1000.0) { snprintf(buf, n, "%.0f years", y); return buf; }
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

int pz_write_file_atomic(const char *path, const void *data, size_t len, int mode)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
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
    if (pz_fd_sync(fd)) { close(fd); unlink(tmp); return -1; }
    if (close(fd)) { unlink(tmp); return -1; }
    if (pz_rename_replace(tmp, path)) { unlink(tmp); return -1; }
    return 0;
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

bool pz_env_on(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0;
}

bool pz_cache_file(const char *name, char *out, size_t n)
{
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME"), *la = getenv("LOCALAPPDATA");
    char dir[480];
    if (x && x[0]) snprintf(dir, sizeof dir, "%s/pzsolve", x);
#ifdef _WIN32
    else if (la && la[0]) snprintf(dir, sizeof dir, "%s/pzsolve", la);
#endif
    else if (h && h[0]) snprintf(dir, sizeof dir, "%s/.cache/pzsolve", h);
    else return false;
    (void)la;
    if (pz_mkdir_p(dir) != 0) return false;
    snprintf(out, n, "%s/%s", dir, name);
    return true;
}
