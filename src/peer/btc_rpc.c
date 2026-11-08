/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* btc_rpc.c - the JSON-RPC client of bitcoind (btc_rpc.h). */
#include "btc_rpc.h"
#include <pzcore/seal.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL   /* macOS: SO_NOSIGPIPE on the socket instead */
#define MSG_NOSIGNAL 0
#endif

#define HEAD_MAX  (64u << 10)                     /* the status line and the headers of a reply */
#define REPLY_MAX (JSON_MAX_SIZE + (1u << 20))    /* a whole reply, with the framing of a chunked body */

static bool fail(btc_rpc *c, btc_rpc_kind kind, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    c->err.kind = kind;
    c->err.code = code;
    vsnprintf(c->err.msg, sizeof c->err.msg, fmt, ap);
    va_end(ap);
    /* The text may come from the server: nothing that would act on a terminal. Printable ASCII only, as bitcoind writes its messages:
     * a UTF-8 control of the C1 range (U+009B is CSI, the start of an escape sequence for xterm) is two bytes above 0x7f. */
    for (char *p = c->err.msg; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p >= 0x7f) *p = '?';
    return false;
}

/* ---- setup ---- */

static void base64(const uint8_t *p, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = T[v >> 18];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static bool set_auth(btc_rpc *c, const char *userpass, size_t n)
{
    if (!n || n > BTC_RPC_AUTH_MAX || !memchr(userpass, ':', n)) return false;
    base64((const uint8_t *)userpass, n, c->auth);
    return true;
}

/* The host goes into the Host header as it is: only what a name or an address has (an IPv6 address and its zone, without brackets),
 * so that no CR or LF can end the line. */
static bool init_common(btc_rpc *c, const char *host, unsigned port)
{
    memset(c, 0, sizeof *c);
    if (!host || !host[0] || strlen(host) >= sizeof c->host || !port || port > 65535) return false;
    for (const char *p = host; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr(".-_:%", *p)) return false;
    strcpy(c->host, host);
    snprintf(c->port, sizeof c->port, "%u", port);
    c->timeout_ms = 30000;
    c->scan_timeout_ms = 30 * 60000;
    return true;
}

bool btc_rpc_init(btc_rpc *c, const char *host, unsigned port, const char *user, const char *pass)
{
    if (!init_common(c, host, port) || !user || !pass || strchr(user, ':')) return false;   /* basic auth splits at the first ':' */
    char up[BTC_RPC_AUTH_MAX + 1];
    const int n = snprintf(up, sizeof up, "%s:%s", user, pass);
    const bool ok = n > 0 && (size_t)n < sizeof up && set_auth(c, up, (size_t)n);
    pz_secure_zero(up, sizeof up);
    if (!ok) btc_rpc_done(c);
    return ok;
}

/* the credentials of c->cookie: one line, user:password, as the node writes it without a newline (a hand-made one may have one) */
static bool read_cookie(btc_rpc *c)
{
    FILE *f = fopen(c->cookie, "rb");
    if (!f) return false;
    char buf[BTC_RPC_AUTH_MAX + 4];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    const bool ok = n <= BTC_RPC_AUTH_MAX && !memchr(buf, '\n', n) && set_auth(c, buf, n);
    pz_secure_zero(buf, sizeof buf);
    return ok;
}

bool btc_rpc_init_cookie(btc_rpc *c, const char *host, unsigned port, const char *cookie_path)
{
    if (init_common(c, host, port) && cookie_path && strlen(cookie_path) < sizeof c->cookie) {
        strcpy(c->cookie, cookie_path);
        if (read_cookie(c)) return true;
    }
    btc_rpc_done(c);
    return false;
}

bool btc_rpc_init_arg(btc_rpc *c, const char *arg, const char *cookie_path)
{
    char host[256];
    const char *h = arg, *he, *p;
    if (!arg) goto bad;
    if (*arg == '[') {                                          /* [IPv6]:PORT */
        h = arg + 1;
        if (!(he = strchr(h, ']'))) goto bad;
        p = he + 1;
    } else p = he = arg + strcspn(arg, ":");
    if (he == h || (size_t)(he - h) >= sizeof host || *p != ':' || !isdigit((unsigned char)p[1])) goto bad;
    memcpy(host, h, (size_t)(he - h));
    host[he - h] = 0;
    unsigned long port = 0;
    for (p++; isdigit((unsigned char)*p) && port <= 65535; p++) port = port * 10 + (unsigned long)(*p - '0');
    if (port > 65535) goto bad;
    if (!*p) {                                                  /* HOST:PORT: the credentials are in the cookie file */
        if (!cookie_path) goto bad;
        if (btc_rpc_init_cookie(c, host, (unsigned)port, cookie_path)) return true;
        return fail(c, BTC_RPC_ENET, 0, "cannot take the credentials of the cookie file %.200s (is the node running?)", cookie_path);
    }
    /* :USER:PASSWORD is the user:password of basic auth as it is: the user ends at the first ':' */
    if (*p != ':' || cookie_path || !init_common(c, host, (unsigned)port) || !set_auth(c, p + 1, strlen(p + 1))) goto bad;
    return true;
bad:
    btc_rpc_done(c);
    return fail(c, BTC_RPC_EPROTO, 0, "expected HOST:PORT:USER:PASSWORD, or HOST:PORT with the cookie file");
}

bool btc_rpc_set_wallet(btc_rpc *c, const char *name)
{
    if (!name || !name[0]) {
        c->wallet[0] = 0;
        return true;
    }
    const size_t n = strlen(name);
    if (n > sizeof c->wallet - sizeof "/wallet/") return false;
    for (size_t i = 0; i < n; i++) {
        const unsigned char ch = (unsigned char)name[i];
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '-' && ch != '_' && ch != '.')
            return false;
    }
    snprintf(c->wallet, sizeof c->wallet, "/wallet/%s", name);
    return true;
}

void btc_rpc_done(btc_rpc *c) { pz_secure_zero(c, sizeof *c); }

/* ---- transport ---- */

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* 1 when fd is ready for ev, 0 at the deadline, -1 on an error of poll */
static int wait_fd(int fd, short ev, int64_t deadline)
{
    for (;;) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = {.fd = fd, .events = ev};
        const int r = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (r > 0) return 1;   /* an error or a hang-up shows in the read or write that follows */
        if (r == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

static int tcp_connect(btc_rpc *c, int64_t deadline)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
    const int rc = getaddrinfo(c->host, c->port, &hints, &res);
    if (rc) {
        fail(c, BTC_RPC_ENET, 0, "%s: %s", c->host, gai_strerror(rc));
        return -1;
    }
    int fd = -1, why = 0;
    for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            why = errno;
            continue;
        }
        /* not inherited by pzfirma; non-blocking, so that every wait has the deadline */
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
        const int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        why = errno;
        if (why == EINPROGRESS) {
            const int w = wait_fd(fd, POLLOUT, deadline);
            socklen_t l = sizeof why;
            if (w > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &why, &l) == 0 && why == 0) break;
            if (w == 0) why = ETIMEDOUT;
            else if (w < 0) why = errno;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fail(c, BTC_RPC_ENET, 0, "cannot connect to %s port %s: %s", c->host, c->port, strerror(why));
    return fd;
}

static bool send_all(btc_rpc *c, int fd, const char *p, size_t n, int64_t deadline)
{
    while (n) {
        const ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int w = wait_fd(fd, POLLOUT, deadline);
            if (w > 0) continue;
            return fail(c, BTC_RPC_ENET, 0, w ? "sending the request: %s" : "timeout sending the request", strerror(errno));
        }
        return fail(c, BTC_RPC_ENET, 0, "sending the request: %s", strerror(errno));
    }
    return true;
}

/* ---- HTTP ---- */

static int hex_digit(unsigned c)
{
    if (c - '0' < 10) return (int)(c - '0');
    c |= 0x20;
    if (c - 'a' < 6) return (int)(c - 'a' + 10);
    return -1;
}

/* A reply as it comes. It is read as it arrives, each byte once: the search for the end of the head goes on where it stopped, and a
 * chunked body is decoded in place, chunk by chunk, as each one is whole (the data moves back over the framing before it). */
typedef struct {
    char *p;
    size_t len, cap;
    size_t head;      /* the length of the status line and the headers with the empty line, once they are all in; 0 before */
    size_t at;        /* where the reading goes on: in the head, then at the next chunk */
    size_t body;      /* the length of the body (decoded) once it is complete; it starts at p + head */
    int status;
    bool chunked;
    bool trailer;     /* the last chunk has come: the trailer lines follow, up to an empty one */
    long long clen;   /* Content-Length, or -1 */
} reply;

/* the value of header name in the headers h[0..n), trimmed, or NULL; *vl is its length */
static const char *header(const char *h, size_t n, const char *name, size_t *vl)
{
    const size_t nl = strlen(name);
    for (const char *p = h, *end = h + n; p < end;) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) break;
        if ((size_t)(eol - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char *v = p + nl + 1, *e = eol;
            while (v < e && (*v == ' ' || *v == '\t')) v++;
            while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            *vl = (size_t)(e - v);
            return v;
        }
        p = eol + 1;
    }
    return NULL;
}

/* The status line and the headers, once the empty line that ends them has come: 1 when parsed, 0 when more is to come, -1 when
 * malformed. */
static int parse_head(reply *r)
{
    if (r->head) return 1;
    for (; r->at + 4 <= r->len && r->at < HEAD_MAX; r->at++)
        if (r->p[r->at] == '\r' && !memcmp(r->p + r->at, "\r\n\r\n", 4)) break;
    if (r->at + 4 > r->len) return 0;
    if (r->at >= HEAD_MAX) return -1;
    r->head = r->at += 4;
    const char *p = r->p;
    if (r->head < 14 || (memcmp(p, "HTTP/1.1 ", 9) && memcmp(p, "HTTP/1.0 ", 9))) return -1;
    r->status = 0;
    for (int k = 9; k < 12; k++) {
        if (p[k] < '0' || p[k] > '9') return -1;
        r->status = r->status * 10 + (p[k] - '0');
    }
    if (p[12] != ' ' && p[12] != '\r') return -1;
    size_t vl;
    const char *v = header(p, r->head, "Transfer-Encoding", &vl);
    r->chunked = v && vl >= 7 && !strncasecmp(v + vl - 7, "chunked", 7);   /* the last coding is the one that frames the body */
    r->clen = -1;
    v = header(p, r->head, "Content-Length", &vl);
    if (v && !r->chunked) {   /* with both, the chunked coding frames the body (RFC 9112 6.3) */
        long long n = 0;
        if (!vl || vl > 10) return -1;
        for (size_t k = 0; k < vl; k++) {
            if (v[k] < '0' || v[k] > '9') return -1;
            n = n * 10 + (v[k] - '0');
        }
        if (n > JSON_MAX_SIZE) return -1;
        r->clen = n;
    }
    return 1;
}

/* The chunks that are whole (RFC 9112 7.1), from r->at: 1 when the last one and the trailer have come, 0 when more is to come, -1
 * when malformed. A line (the size of a chunk with its extension, which is ignored, or a trailer) ends with CRLF and is at most 4 kB. */
static int dechunk(reply *r)
{
    for (;;) {
        const char *p = r->p + r->at, *lf = memchr(p, '\n', r->len - r->at);
        if (!lf) return r->len - r->at > 4096 ? -1 : 0;
        const size_t ll = (size_t)(lf - p);   /* the line without its LF */
        if (!ll || p[ll - 1] != '\r') return -1;
        if (r->trailer) {
            r->at += ll + 1;
            if (ll == 1) return 1;
            continue;
        }
        size_t size = 0, k = 0;
        for (int d; k < ll - 1 && (d = hex_digit((unsigned char)p[k])) >= 0; k++) {
            if (size > JSON_MAX_SIZE) return -1;
            size = size << 4 | (size_t)d;
        }
        if (!k || (k < ll - 1 && p[k] != ';')) return -1;
        const size_t data = r->at + ll + 1;
        if (!size) {
            r->trailer = true;
            r->at = data;
            continue;
        }
        if (r->len - data < size + 2) return 0;
        if (r->p[data + size] != '\r' || r->p[data + size + 1] != '\n' || r->body + size > JSON_MAX_SIZE) return -1;
        memmove(r->p + r->head + r->body, r->p + data, size);
        r->body += size;
        r->at = data + size + 2;
    }
}

/* whether the body is all in: 1 yes, 0 not yet, -1 malformed; at eof, 0 means it was cut short */
static int body_done(reply *r, bool eof)
{
    const int h = parse_head(r);
    if (h <= 0) return h;
    if (r->chunked) return dechunk(r);
    const size_t have = r->len - r->head;
    if (r->clen >= 0 ? have < (size_t)r->clen : !eof) return 0;
    r->body = r->clen >= 0 ? (size_t)r->clen : have;   /* without a length, the body ends where the server closes */
    return 1;
}

static bool recv_reply(btc_rpc *c, int fd, int64_t deadline, reply *r)
{
    for (;;) {
        if (r->len == r->cap) {
            if (r->cap >= REPLY_MAX) return fail(c, BTC_RPC_EPROTO, 0, "the reply is too large");
            size_t cap = r->cap ? r->cap * 2 : 65536;
            if (cap > REPLY_MAX) cap = REPLY_MAX;
            char *p = realloc(r->p, cap);
            if (!p) return fail(c, BTC_RPC_EPROTO, 0, "out of memory");
            r->p = p;
            r->cap = cap;
        }
        const ssize_t k = recv(fd, r->p + r->len, r->cap - r->len, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            const int w = wait_fd(fd, POLLIN, deadline);
            if (w > 0) continue;
            return fail(c, BTC_RPC_ENET, 0, w ? "reading the reply: %s" : "timeout waiting for the reply", strerror(errno));
        }
        if (k < 0) return fail(c, BTC_RPC_ENET, 0, "reading the reply: %s", strerror(errno));
        r->len += (size_t)k;
        const int d = body_done(r, k == 0);
        if (d < 0) return fail(c, BTC_RPC_EPROTO, 0, "the reply is not valid HTTP");
        if (d > 0) return true;
        if (k == 0) return fail(c, BTC_RPC_ENET, 0, r->head ? "the reply was cut short" : "the connection closed without a reply");
    }
}

/* One request and its reply, on a connection of its own. */
static bool exchange(btc_rpc *c, bool wallet, const char *body, size_t bl, int timeout_ms, reply *r)
{
    /* the head carries the credentials: wiped once sent */
    char head[sizeof c->auth + sizeof c->host + sizeof c->wallet + 256];
    const bool v6 = strchr(c->host, ':') != NULL;
    const int hl = snprintf(head, sizeof head,
                            "POST %s HTTP/1.1\r\nHost: %s%s%s:%s\r\nAuthorization: Basic %s\r\nContent-Type: application/json\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                            wallet && c->wallet[0] ? c->wallet : "/", v6 ? "[" : "", c->host, v6 ? "]" : "", c->port, c->auth, bl);
    *r = (reply){0};
    bool ok = hl > 0 && (size_t)hl < sizeof head;
    if (!ok) fail(c, BTC_RPC_EPROTO, 0, "cannot build the request");
    const int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 1);
    const int fd = ok ? tcp_connect(c, deadline) : -1;
    ok = fd >= 0 && send_all(c, fd, head, (size_t)hl, deadline) && send_all(c, fd, body, bl, deadline) && recv_reply(c, fd, deadline, r);
    if (fd >= 0) close(fd);
    pz_secure_zero(head, sizeof head);
    if (!ok) {
        free(r->p);
        *r = (reply){0};
    }
    return ok;
}

/* ---- JSON-RPC ---- */

static bool rpc(btc_rpc *c, bool wallet, const char *method, json_w *params, int timeout_ms, json_doc *doc, const json_val **result)
{
    *doc = (json_doc){0};
    *result = NULL;
    c->err = (btc_rpc_err){0};
    const char *pt = "[]";
    size_t pl = 2;
    if (params && !(pt = jw_done(params, &pl))) return fail(c, BTC_RPC_EPROTO, 0, "%s: bad parameters", method);
    const uint64_t id = ++c->id;
    json_w w;
    jw_init(&w);
    jw_obj(&w);
    jw_key(&w, "jsonrpc");
    jw_str(&w, "1.0");   /* answered by every version, with the error in the body (and an HTTP status that says it) */
    jw_key(&w, "id");
    jw_u64(&w, id);
    jw_key(&w, "method");
    jw_str(&w, method);
    jw_key(&w, "params");
    jw_raw(&w, pt, pl);
    jw_end(&w);
    size_t bl;
    const char *body = jw_done(&w, &bl);
    reply r;
    bool ok = body && exchange(c, wallet, body, bl, timeout_ms, &r);
    if (!body) fail(c, BTC_RPC_EPROTO, 0, "%s: cannot build the request", method);
    /* A node that started again since the cookie was read has a new one: read again, the call is made once more. */
    if (ok && r.status == 401 && c->cookie[0]) {
        char old[sizeof c->auth];
        memcpy(old, c->auth, sizeof old);
        if (read_cookie(c) && strcmp(old, c->auth)) {
            free(r.p);
            ok = exchange(c, wallet, body, bl, timeout_ms, &r);
        }
        pz_secure_zero(old, sizeof old);
    }
    jw_free(&w);
    if (!ok) return false;
    const char *b = r.p + r.head;
    const size_t n = r.body;
    const int status = r.status;
    if (!n || !json_parse(doc, b, n)) {
        free(r.p);
        if (status != 200) return fail(c, BTC_RPC_EHTTP, status, "%s: HTTP status %d%s", method, status,
                                       status == 401 ? " (wrong RPC user or password)" : "");
        return fail(c, BTC_RPC_EPROTO, 0, "%s: the reply is not JSON", method);
    }
    free(r.p);
    const json_val *root = doc->v, *err = json_get(root, "error"), *res = json_get(root, "result");
    uint64_t rid;
    if (root->type != JSON_OBJECT) {
        json_free(doc);
        return fail(c, BTC_RPC_EPROTO, 0, "%s: the reply is not a JSON-RPC object", method);
    }
    if (err && !json_is_null(err)) {
        int64_t code;
        const char *msg = json_str(json_get(err, "message"));
        const bool coded = json_i64(json_get(err, "code"), &code) && code >= INT_MIN && code <= INT_MAX;
        fail(c, coded ? BTC_RPC_ERPC : BTC_RPC_EPROTO, coded ? (int)code : 0, "%s: %s", method, msg ? msg : "error without a message");
        json_free(doc);
        return false;
    }
    if (status != 200 || !res || !json_u64(json_get(root, "id"), &rid) || rid != id) {
        json_free(doc);
        if (status != 200) return fail(c, BTC_RPC_EHTTP, status, "%s: HTTP status %d", method, status);
        return fail(c, BTC_RPC_EPROTO, 0, "%s: %s", method, res ? "the reply is for another request" : "the reply has no result");
    }
    *result = res;
    return true;
}

bool btc_rpc_call(btc_rpc *c, bool wallet, const char *method, json_w *params, json_doc *doc, const json_val **result)
{
    return rpc(c, wallet, method, params, c->timeout_ms, doc, result);
}

/* ---- the calls ---- */

/* The params of most calls are a few values: written into w, which the call frees. */
static bool call(btc_rpc *c, bool wallet, const char *method, json_w *w, json_doc *doc, const json_val **res)
{
    const bool ok = rpc(c, wallet, method, w, c->timeout_ms, doc, res);
    if (w) jw_free(w);
    return ok;
}

/* the end of a call that read what it needed from the result: false, with the message, when it was not what was expected */
static bool done(btc_rpc *c, json_doc *doc, bool ok, const char *method)
{
    json_free(doc);
    return ok || fail(c, BTC_RPC_EPROTO, 0, "%s: unexpected result", method);
}

static bool get_hash(const json_val *v, uint8_t h[32]) { return btc_hash_from_hex(json_str(v), h); }

bool btc_rpc_getblockcount(btc_rpc *c, uint64_t *height)
{
    json_doc d;
    const json_val *res;
    if (!call(c, false, "getblockcount", NULL, &d, &res)) return false;
    return done(c, &d, json_u64(res, height), "getblockcount");
}

bool btc_rpc_getblockhash(btc_rpc *c, uint64_t height, uint8_t hash[32])
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_u64(&w, height);
    jw_end(&w);
    if (!call(c, false, "getblockhash", &w, &d, &res)) return false;
    return done(c, &d, get_hash(res, hash), "getblockhash");
}

bool btc_rpc_getblockchaininfo(btc_rpc *c, btc_chaininfo *info)
{
    json_doc d;
    const json_val *res;
    if (!call(c, false, "getblockchaininfo", NULL, &d, &res)) return false;
    btc_chaininfo x = {0};
    const char *chain = json_str(json_get(res, "chain"));
    const bool ok = chain && strlen(chain) < sizeof x.chain && json_u64(json_get(res, "blocks"), &x.blocks) &&
                    json_u64(json_get(res, "headers"), &x.headers) && json_bool(json_get(res, "initialblockdownload"), &x.ibd) &&
                    json_bool(json_get(res, "pruned"), &x.pruned);
    if (ok) {
        strcpy(x.chain, chain);
        *info = x;
    }
    return done(c, &d, ok, "getblockchaininfo");
}

static void put_hash(json_w *w, const uint8_t h[32])
{
    char hex[65];
    btc_hash_to_hex(h, hex);
    jw_str(w, hex);
}

bool btc_rpc_getrawtransaction(btc_rpc *c, const uint8_t txid[32], const uint8_t *blockhash, uint8_t **raw, size_t *len)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    put_hash(&w, txid);
    jw_u64(&w, 0);   /* verbosity 0: the hex; every version takes the number */
    if (blockhash) put_hash(&w, blockhash);
    jw_end(&w);
    if (!call(c, false, "getrawtransaction", &w, &d, &res)) return false;
    const char *hex = json_str(res);
    const size_t n = hex ? res->n / 2 : 0;
    uint8_t *b = n ? malloc(n) : NULL;
    const bool ok = b && btc_unhex(hex, res->n, b);
    if (!ok) free(b);
    else {
        *raw = b;
        *len = n;
    }
    return done(c, &d, ok, "getrawtransaction");
}

/* the fields an unspent output has in both scantxoutset and gettxout: the script in hex and the coinbase flag */
static bool get_spk(const json_val *v, btc_utxo *u)
{
    const char *hex = json_str(v);
    if (!hex || v->n > 2 * BTC_SPK_MAX || !btc_unhex(hex, v->n, u->spk)) return false;
    u->spk_len = (uint8_t)(v->n / 2);
    return true;
}

static bool get_u32(const json_val *v, uint32_t *out)
{
    uint64_t x;
    if (!json_u64(v, &x) || x > UINT32_MAX) return false;
    *out = (uint32_t)x;
    return true;
}

/* whether a goes before b in the order of the coins kept by a scan: the larger value first, then the smaller outpoint */
static bool before(const btc_utxo *a, const btc_utxo *b)
{
    if (a->value != b->value) return a->value > b->value;
    const int c = memcmp(a->txid, b->txid, 32);
    return c ? c < 0 : a->vout < b->vout;
}

bool btc_rpc_scan(btc_rpc *c, const char *const *addrs, size_t n_addr, btc_utxo *out, size_t cap, size_t *n, uint64_t *height)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_str(&w, "start");
    jw_arr(&w);
    for (size_t k = 0; k < n_addr; k++) {
        char desc[128];
        /* an address is letters and digits: nothing else may change the descriptor around it */
        static const char ALNUM[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        if (!addrs[k] || !addrs[k][0] || addrs[k][strspn(addrs[k], ALNUM)] ||
            snprintf(desc, sizeof desc, "addr(%s)", addrs[k]) >= (int)sizeof desc) {
            jw_free(&w);
            return fail(c, BTC_RPC_EPROTO, 0, "scantxoutset: bad address");
        }
        jw_str(&w, desc);
    }
    jw_end(&w);
    jw_end(&w);
    const bool ok = rpc(c, false, "scantxoutset", &w, c->scan_timeout_ms, &d, &res);
    jw_free(&w);
    if (!ok) {
        if (c->err.kind == BTC_RPC_ENET) {   /* the node goes on with a scan whose client has gone, and refuses the next one meanwhile */
            const btc_rpc_err e = c->err;
            jw_init(&w);
            jw_arr(&w);
            jw_str(&w, "abort");
            jw_end(&w);
            if (call(c, false, "scantxoutset", &w, &d, &res)) json_free(&d);
            c->err = e;
        }
        return false;
    }
    bool success = false;
    const json_val *list = json_get(res, "unspents");
    if (!json_bool(json_get(res, "success"), &success) || !success || !json_u64(json_get(res, "height"), height) || !list ||
        list->type != JSON_ARRAY)
        return done(c, &d, false, "scantxoutset");
    const json_val *e = list + 1;
    for (uint32_t k = 0; k < list->n; k++, e = json_next(e)) {
        btc_utxo u = {0};
        if (!get_hash(json_get(e, "txid"), u.txid) || !get_u32(json_get(e, "vout"), &u.vout) ||
            !json_sats(json_get(e, "amount"), &u.value) || !get_u32(json_get(e, "height"), &u.height) ||
            !get_spk(json_get(e, "scriptPubKey"), &u))
            return done(c, &d, false, "scantxoutset");
        const json_val *cb = json_get(e, "coinbase");
        if (cb && !json_bool(cb, &u.coinbase)) return done(c, &d, false, "scantxoutset");
        if (k < cap) {
            out[k] = u;
            continue;
        }
        size_t last = 0;   /* full: u takes the place of the last of those kept, when it goes before it */
        for (size_t i = 1; i < cap; i++)
            if (before(&out[last], &out[i])) last = i;
        if (cap && before(&u, &out[last])) out[last] = u;
    }
    *n = list->n;
    return done(c, &d, true, "scantxoutset");
}

bool btc_rpc_gettxout(btc_rpc *c, const uint8_t txid[32], uint32_t vout, bool mempool, bool *found, btc_utxo *out)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    put_hash(&w, txid);
    jw_u64(&w, vout);
    jw_bool(&w, mempool);
    jw_end(&w);
    if (!call(c, false, "gettxout", &w, &d, &res)) return false;
    *found = !json_is_null(res);
    if (!*found) return done(c, &d, true, "gettxout");
    btc_utxo u = {.vout = vout};
    memcpy(u.txid, txid, 32);
    const json_val *cb = json_get(res, "coinbase");
    const bool ok = json_sats(json_get(res, "value"), &u.value) && get_u32(json_get(res, "confirmations"), &u.confirmations) &&
                    get_spk(json_get(json_get(res, "scriptPubKey"), "hex"), &u) && (!cb || json_bool(cb, &u.coinbase));
    if (ok) *out = u;
    return done(c, &d, ok, "gettxout");
}

bool btc_rpc_estimatesmartfee(btc_rpc *c, unsigned target, uint64_t *sat_per_kvb)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_u64(&w, target);
    jw_end(&w);
    if (!call(c, false, "estimatesmartfee", &w, &d, &res)) return false;
    const json_val *rate = json_get(res, "feerate");
    bool ok = res->type == JSON_OBJECT;
    if (!rate) *sat_per_kvb = 0;   /* "errors" instead: no estimate */
    else ok = ok && json_sats(rate, sat_per_kvb);
    return done(c, &d, ok, "estimatesmartfee");
}

bool btc_rpc_testmempoolaccept(btc_rpc *c, const uint8_t *raw, size_t len, bool *ok, char *reason, size_t cap)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_arr(&w);
    jw_hex(&w, raw, len);
    jw_end(&w);
    jw_end(&w);
    if (!call(c, false, "testmempoolaccept", &w, &d, &res)) return false;
    const json_val *r0 = json_at(res, 0);
    const char *why = json_str(json_get(r0, "reject-reason"));
    const bool good = res->n == 1 && json_bool(json_get(r0, "allowed"), ok) && (*ok || why);
    if (good && cap) snprintf(reason, cap, "%s", *ok ? "" : why);
    return done(c, &d, good, "testmempoolaccept");
}

bool btc_rpc_sendrawtransaction(btc_rpc *c, const uint8_t *raw, size_t len, uint8_t txid[32])
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_hex(&w, raw, len);
    jw_end(&w);
    if (!call(c, false, "sendrawtransaction", &w, &d, &res)) return false;
    return done(c, &d, get_hash(res, txid), "sendrawtransaction");
}

bool btc_rpc_createwallet(btc_rpc *c, const char *name)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_str(&w, name);
    jw_end(&w);
    if (!call(c, false, "createwallet", &w, &d, &res)) return false;
    return done(c, &d, res->type == JSON_OBJECT, "createwallet");
}

bool btc_rpc_getnewaddress(btc_rpc *c, const char *type, char *addr, size_t cap)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    if (type) {
        jw_str(&w, "");   /* the label */
        jw_str(&w, type);
    }
    jw_end(&w);
    if (!call(c, true, "getnewaddress", &w, &d, &res)) return false;
    const char *a = json_str(res);
    const bool ok = a && res->n < cap;
    if (ok) memcpy(addr, a, res->n + 1);
    return done(c, &d, ok, "getnewaddress");
}

bool btc_rpc_sendtoaddress(btc_rpc *c, const char *addr, uint64_t sats, uint8_t txid[32])
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_str(&w, addr);
    jw_sats(&w, sats);
    jw_end(&w);
    if (!call(c, true, "sendtoaddress", &w, &d, &res)) return false;
    return done(c, &d, get_hash(res, txid), "sendtoaddress");
}

bool btc_rpc_generatetoaddress(btc_rpc *c, unsigned blocks, const char *addr, uint8_t last[32])
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w);
    jw_arr(&w);
    jw_u64(&w, blocks);
    jw_str(&w, addr);
    jw_end(&w);
    if (!call(c, false, "generatetoaddress", &w, &d, &res)) return false;
    bool ok = res->type == JSON_ARRAY && res->n == blocks;
    if (ok && last && blocks) ok = get_hash(json_at(res, blocks - 1), last);
    return done(c, &d, ok, "generatetoaddress");
}
