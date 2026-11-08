/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* net.c - TCP and the encrypted channel (see net.h). The sockets are non-blocking and every read, write and connect waits in poll() until
 * a deadline, so no peer can hold a thread for longer than the timeout of one step. */
#include "net.h"
#include <pzcore/seal.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0             /* macOS: SO_NOSIGPIPE on the socket instead */
#endif

static const uint8_t V4MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

typedef union {
    struct sockaddr sa;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
    struct sockaddr_storage ss;
} sockaddr_any;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void net_cfg_init(net_cfg *cfg, const secp256k1_context *ctx, const home_identity *self, const uint8_t fp[32], unsigned id_bits,
                  uint16_t port, int timeout_ms)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->ctx = ctx;
    cfg->self = self;
    memcpy(cfg->prologue, "pznet1", 6);
    memcpy(cfg->prologue + 6, fp, 32);
    cfg->id_bits = id_bits;
    cfg->port = port;
    cfg->timeout_ms = timeout_ms > 0 ? timeout_ms : NET_TIMEOUT_MS;
}

bool net_random(uint8_t *out, size_t n)
{
    size_t got = 0;
#if defined(__linux__)
    while (got < n) {
        const ssize_t r = getrandom(out + got, n - got, 0);
        if (r > 0) got += (size_t)r;
        else if (r < 0 && errno != EINTR) break;               /* ENOSYS on an old kernel: the device below */
    }
    if (got == n) return true;
#endif
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
}

/* ---- addresses ---- */

static socklen_t to_sa(const net_addr *a, sockaddr_any *s)
{
    memset(s, 0, sizeof *s);
    if (!memcmp(a->ip, V4MAPPED, 12)) {                        /* an IPv4 socket: it works where the system has no IPv6 */
        s->v4.sin_family = AF_INET;
        s->v4.sin_port = htons(a->port);
        memcpy(&s->v4.sin_addr, a->ip + 12, 4);
        return sizeof s->v4;
    }
    s->v6.sin6_family = AF_INET6;
    s->v6.sin6_port = htons(a->port);
    memcpy(&s->v6.sin6_addr, a->ip, 16);
    return sizeof s->v6;
}

static bool from_sa(const sockaddr_any *s, net_addr *a)
{
    memset(a, 0, sizeof *a);
    if (s->sa.sa_family == AF_INET) {
        memcpy(a->ip, V4MAPPED, 12);
        memcpy(a->ip + 12, &s->v4.sin_addr, 4);
        a->port = ntohs(s->v4.sin_port);
        return true;
    }
    if (s->sa.sa_family == AF_INET6) {
        memcpy(a->ip, &s->v6.sin6_addr, 16);
        a->port = ntohs(s->v6.sin6_port);
        return true;
    }
    return false;
}

bool net_parse_addr(const char *hostport, net_addr *out)
{
    char host[256];
    const char *colon;
    size_t hl;
    if (hostport[0] == '[') {
        const char *close = strchr(hostport, ']');
        if (!close || close[1] != ':') return false;
        hl = (size_t)(close - hostport - 1);
        hostport++;
        colon = close + 1;
    } else {
        colon = strrchr(hostport, ':');
        if (!colon || memchr(hostport, ':', (size_t)(colon - hostport))) return false;   /* an IPv6 address needs its brackets */
        hl = (size_t)(colon - hostport);
    }
    if (hl == 0 || hl >= sizeof host) return false;
    memcpy(host, hostport, hl);
    host[hl] = 0;
    unsigned long port = 0;
    const char *p = colon + 1;
    if (!*p) return false;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || (port = port * 10 + (unsigned long)(*p - '0')) > 65535) return false;
    }
    if (port == 0) return false;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    sockaddr_any s;
    memset(&s, 0, sizeof s);
    memcpy(&s, res->ai_addr, res->ai_addrlen < sizeof s ? res->ai_addrlen : sizeof s);
    freeaddrinfo(res);
    if (!from_sa(&s, out)) return false;
    out->port = (uint16_t)port;
    return true;
}

void net_addr_str(const net_addr *a, char *out, size_t cap)
{
    char ip[INET6_ADDRSTRLEN];
    if (!memcmp(a->ip, V4MAPPED, 12)) {
        inet_ntop(AF_INET, a->ip + 12, ip, sizeof ip);
        snprintf(out, cap, "%s:%u", ip, a->port);
    } else {
        inet_ntop(AF_INET6, a->ip, ip, sizeof ip);
        snprintf(out, cap, "[%s]:%u", ip, a->port);
    }
}

/* ---- sockets ---- */

/* non-blocking, closed on exec, no Nagle (a request is one small write and its answer one small read), no SIGPIPE */
static bool prepare(int fd)
{
    const int one = 1;
    const int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return true;
}

static bool wait_fd(int fd, short ev, int64_t deadline)
{
    for (;;) {
        const int64_t left = deadline - now_ms();
        if (left <= 0) return false;
        struct pollfd p = {.fd = fd, .events = ev};
        const int r = poll(&p, 1, (int)left);
        if (r > 0) return true;                                /* readable, writable or an error: the next call tells */
        if (r == 0 || errno != EINTR) return false;
    }
}

static bool read_full(int fd, uint8_t *p, size_t n, int64_t deadline)
{
    while (n) {
        const ssize_t r = recv(fd, p, n, 0);
        if (r > 0) {
            p += r, n -= (size_t)r;
        } else if (r == 0) {
            return false;                                      /* closed before the end */
        } else if (errno != EINTR && ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(fd, POLLIN, deadline))) {
            return false;
        }
    }
    return true;
}

static bool write_full(int fd, const uint8_t *p, size_t n, int64_t deadline)
{
    while (n) {
        const ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r > 0) {
            p += r, n -= (size_t)r;
        } else if (r < 0 && errno != EINTR && ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(fd, POLLOUT, deadline))) {
            return false;
        }
    }
    return true;
}

void net_close_fd(int fd)
{
    if (fd >= 0) close(fd);
}

int net_listen(const char *host, uint16_t port, uint16_t *bound)
{
    sockaddr_any s;
    socklen_t len;
    memset(&s, 0, sizeof s);
    int fd = -1;
    const int one = 1, zero = 0;
    if (!host || !*host) {                                     /* every address: IPv6 with IPv4 mapped, or IPv4 alone */
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd >= 0) {
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
            s.v6.sin6_family = AF_INET6;
            s.v6.sin6_addr = in6addr_any;
            s.v6.sin6_port = htons(port);
            len = sizeof s.v6;
        } else {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            s.v4.sin_family = AF_INET;
            s.v4.sin_addr.s_addr = htonl(INADDR_ANY);
            s.v4.sin_port = htons(port);
            len = sizeof s.v4;
        }
    } else if (inet_pton(AF_INET, host, &s.v4.sin_addr) == 1) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        s.v4.sin_family = AF_INET;
        s.v4.sin_port = htons(port);
        len = sizeof s.v4;
    } else if (inet_pton(AF_INET6, host, &s.v6.sin6_addr) == 1) {
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        s.v6.sin6_family = AF_INET6;
        s.v6.sin6_port = htons(port);
        len = sizeof s.v6;
    } else {
        return -1;
    }
    if (fd < 0) return -1;
    /* SO_REUSEADDR: a node that restarts gets its port back while the connections of its last run are in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 || bind(fd, &s.sa, len) < 0 || listen(fd, 128) < 0 || !prepare(fd)) {
        close(fd);
        return -1;
    }
    sockaddr_any got;
    socklen_t gl = sizeof got;
    net_addr a;
    if (getsockname(fd, &got.sa, &gl) < 0 || !from_sa(&got, &a)) {
        close(fd);
        return -1;
    }
    if (bound) *bound = a.port;
    return fd;
}

int net_accept(int lfd, int timeout_ms, net_addr *from)
{
    if (!wait_fd(lfd, POLLIN, now_ms() + timeout_ms)) return -1;
    sockaddr_any s;
    socklen_t len = sizeof s;
    const int fd = accept(lfd, &s.sa, &len);
    if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED ? -1 : -2;
    if (!prepare(fd) || !from_sa(&s, from)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int dial_fd(const net_addr *to, int timeout_ms)
{
    sockaddr_any s;
    const socklen_t len = to_sa(to, &s);
    const int fd = socket(s.sa.sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!prepare(fd)) {
        close(fd);
        return -1;
    }
    if (connect(fd, &s.sa, len) < 0) {
        int err = 0;
        socklen_t el = sizeof err;
        if ((errno != EINPROGRESS && errno != EINTR) || !wait_fd(fd, POLLOUT, now_ms() + timeout_ms) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* ---- the handshake ---- */

static net_conn *conn_new(int fd, const net_cfg *cfg)
{
    net_conn *c = malloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, offsetof(net_conn, buf));
    c->fd = fd;
    c->timeout_ms = cfg->timeout_ms;
    return c;
}

/* the static key of the other side: its node ID has the zero bits, and it is the expected identity when there is one */
static bool peer_ok(const net_cfg *cfg, const uint8_t rs[33], const uint8_t expect[32])
{
    uint8_t id[32];
    keys_node_id(rs + 1, id);
    return keys_id_ok(id, cfg->id_bits) && (!expect || !memcmp(rs + 1, expect, 32));
}

static bool handshake(const net_cfg *cfg, net_conn *c, bool initiator, const uint8_t expect[32])
{
    noise_hs hs;
    uint8_t e[32], m[NOISE_HS_MAX], pl[NOISE_PAYLOAD_MAX], rs[33];
    size_t n, plen;
    bool ok = false;
    for (int tries = 0;; tries++) {                            /* a random value that is not a valid key: practically never */
        if (tries == 4 || !net_random(e, sizeof e)) goto out;
        if (noise_hs_init(&hs, cfg->ctx, initiator, cfg->prologue, sizeof cfg->prologue, cfg->self->sec, e)) break;
    }
    const int t = c->timeout_ms;
    if (initiator) {
        const uint8_t port[2] = {(uint8_t)(cfg->port >> 8), (uint8_t)cfg->port};
        if ((n = noise_hs_write(&hs, NULL, 0, m)) != NET_HS1 || !write_full(c->fd, m, n, now_ms() + t)) goto out;
        if (!read_full(c->fd, m, NET_HS2, now_ms() + t) || !noise_hs_read(&hs, m, NET_HS2, pl, &plen) || plen != 0) goto out;
        if (!peer_ok(cfg, hs.rs, expect)) goto out;            /* before this side shows its own key */
        if ((n = noise_hs_write(&hs, port, 2, m)) != NET_HS3 || !write_full(c->fd, m, n, now_ms() + t)) goto out;
    } else {
        if (!read_full(c->fd, m, NET_HS1, now_ms() + t) || !noise_hs_read(&hs, m, NET_HS1, pl, &plen) || plen != 0) goto out;
        if ((n = noise_hs_write(&hs, NULL, 0, m)) != NET_HS2 || !write_full(c->fd, m, n, now_ms() + t)) goto out;
        if (!read_full(c->fd, m, NET_HS3, now_ms() + t) || !noise_hs_read(&hs, m, NET_HS3, pl, &plen) || plen != 2) goto out;
        if (!peer_ok(cfg, hs.rs, NULL)) goto out;
        c->addr.port = (uint16_t)(pl[0] << 8 | pl[1]);
    }
    if (!noise_hs_split(&hs, &c->tx, &c->rx, rs, c->hash)) goto out;
    memcpy(c->remote, rs + 1, 32);
    keys_node_id(c->remote, c->remote_id);
    ok = true;
out:
    pz_secure_zero(&hs, sizeof hs);
    pz_secure_zero(e, sizeof e);
    return ok;
}

net_conn *net_dial(const net_cfg *cfg, const net_addr *to, const uint8_t expect[32])
{
    const int fd = dial_fd(to, cfg->timeout_ms);
    if (fd < 0) return NULL;
    net_conn *c = conn_new(fd, cfg);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->addr = *to;
    if (!handshake(cfg, c, true, expect)) {
        net_close(c);
        return NULL;
    }
    return c;
}

net_conn *net_handshake(const net_cfg *cfg, int fd, const net_addr *from)
{
    net_conn *c = conn_new(fd, cfg);
    if (!c) {
        close(fd);
        return NULL;
    }
    c->addr = *from;
    if (!handshake(cfg, c, false, NULL)) {
        net_close(c);
        return NULL;
    }
    return c;
}

/* ---- frames ---- */

bool net_send(net_conn *c, const uint8_t *body, size_t len)
{
    if (len > NOISE_FRAME_MAX) return false;
    const size_t n = noise_frame_seal(&c->tx, body, len, c->buf);
    return write_full(c->fd, c->buf, n, now_ms() + c->timeout_ms);
}

bool net_recv(net_conn *c, uint8_t *body, size_t cap, size_t *len)
{
    const int64_t deadline = now_ms() + c->timeout_ms;         /* the head and the body together */
    size_t l;
    if (!read_full(c->fd, c->buf, NOISE_HEAD, deadline) || !noise_frame_head(&c->rx, c->buf, &l) || l > cap ||
        !read_full(c->fd, c->buf, l + NOISE_TAG, deadline) || !noise_frame_body(&c->rx, c->buf, l, body))
        return false;
    *len = l;
    return true;
}

void net_set_timeout(net_conn *c, int timeout_ms) { c->timeout_ms = timeout_ms > 0 ? timeout_ms : NET_TIMEOUT_MS; }

void net_close(net_conn *c)
{
    if (!c) return;
    close(c->fd);
    pz_secure_zero(c, offsetof(net_conn, buf));
    free(c);
}

bool net_exchange(const net_cfg *cfg, const net_addr *to, const uint8_t expect[32], const uint8_t *req, size_t rlen, uint8_t *ans,
                  size_t cap, size_t *alen, uint8_t remote[32])
{
    net_conn *c = net_dial(cfg, to, expect);
    const bool ok = c && net_send(c, req, rlen) && net_recv(c, ans, cap, alen);
    if (ok && remote) memcpy(remote, c->remote, 32);
    net_close(c);
    return ok;
}
