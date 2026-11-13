/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* net.c - TCP and the encrypted channel (see net.h). The sockets are non-blocking and every read, write and connect waits until a
 * deadline (sys.h), so no peer can hold a thread for longer than the timeout of one step. */
#include "dbg.h"
#include "net.h"
#include "sys.h"
#include <pzcore/seal.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t V4MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

typedef union {
    struct sockaddr sa;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
    struct sockaddr_storage ss;
} sockaddr_any;

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

static bool v4_public(const uint8_t *b)
{
    return !(b[0] == 0 || b[0] == 10 || b[0] == 127 || b[0] >= 224 ||   /* this network, private, loopback, multicast and reserved */
             (b[0] == 100 && (b[1] & 0xc0) == 64) ||                  /* shared, behind a carrier's NAT (RFC 6598) */
             (b[0] == 169 && b[1] == 254) || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168) ||
             (b[0] == 192 && b[1] == 0 && (b[2] == 0 || b[2] == 2)) ||  /* protocol assignments, documentation */
             (b[0] == 198 && (b[1] & 0xfe) == 18) ||                 /* benchmarking */
             (b[0] == 198 && b[1] == 51 && b[2] == 100) || (b[0] == 203 && b[1] == 0 && b[2] == 113));
}

bool net_addr_public(const net_addr *a)
{
    const uint8_t *b = a->ip;
    static const uint8_t ZERO[12] = {0};
    if (!memcmp(b, V4MAPPED, 12)) return v4_public(b + 12);
    return !(!memcmp(b, ZERO, 12) ||                                  /* unspecified, loopback, IPv4-compatible */
             (b[0] & 0xfe) == 0xfc || b[0] == 0xff ||                  /* unique local, multicast */
             (b[0] == 0xfe && (b[1] & 0x80) == 0x80) ||                /* link-local and the old site-local */
             (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) ||   /* documentation */
             (b[0] == 0x01 && !memcmp(b + 1, ZERO, 7)));              /* discard-only (RFC 6666) */
}

void net_addr_block(const net_addr *a, uint8_t block[16])
{
    memcpy(block, a->ip, 16);
    if (!net_addr_public(a)) return;
    if (!memcmp(a->ip, V4MAPPED, 12)) block[15] = 0;
    else memset(block + 4, 0, 12);
}

/* ---- sockets ---- */

void net_close_fd(int fd) { sys_close(fd); }

int net_listen(const char *host, uint16_t port, uint16_t *bound)
{
    sockaddr_any s;
    socklen_t len;
    memset(&s, 0, sizeof s);
    int fd = -1;
    if (!host || !*host) {                                     /* every address: IPv6 with IPv4 mapped, or IPv4 alone */
        fd = sys_socket(AF_INET6, SOCK_STREAM);
        if (fd >= 0) {
            sys_setopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0);
            s.v6.sin6_family = AF_INET6;
            s.v6.sin6_addr = in6addr_any;
            s.v6.sin6_port = htons(port);
            len = sizeof s.v6;
        } else {
            fd = sys_socket(AF_INET, SOCK_STREAM);
            s.v4.sin_family = AF_INET;
            s.v4.sin_addr.s_addr = htonl(INADDR_ANY);
            s.v4.sin_port = htons(port);
            len = sizeof s.v4;
        }
    } else if (inet_pton(AF_INET, host, &s.v4.sin_addr) == 1) {
        fd = sys_socket(AF_INET, SOCK_STREAM);
        s.v4.sin_family = AF_INET;
        s.v4.sin_port = htons(port);
        len = sizeof s.v4;
    } else if (inet_pton(AF_INET6, host, &s.v6.sin6_addr) == 1) {
        fd = sys_socket(AF_INET6, SOCK_STREAM);
        s.v6.sin6_family = AF_INET6;
        s.v6.sin6_port = htons(port);
        len = sizeof s.v6;
    } else {
        return -1;
    }
    if (fd < 0) return -1;
    /* SO_REUSEADDR: a node that restarts gets its port back while the connections of its last run are in TIME_WAIT (on Windows the
     * option means something else: two programs on one port) */
#ifndef _WIN32
    sys_setopt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    sockaddr_any got;
    socklen_t gl = sizeof got;
    net_addr a;
    if (bind(fd, &s.sa, len) != 0 || listen(fd, 128) != 0 || getsockname(fd, &got.sa, &gl) != 0 || !from_sa(&got, &a)) {
        sys_close(fd);
        return -1;
    }
    if (bound) *bound = a.port;
    return fd;
}

int net_accept(int lfd, int timeout_ms, net_addr *from)
{
    if (sys_wait(lfd, false, sys_now_ms() + timeout_ms) <= 0) return -1;
    sockaddr_any s;
    socklen_t len = sizeof s;
    const int fd = sys_accept(lfd, &s.sa, &len);
    if (fd < 0) return fd == SYS_AGAIN ? -1 : -2;
    if (!from_sa(&s, from)) {
        sys_close(fd);
        return -1;
    }
    return fd;
}

static int dial_fd(const net_addr *to, int timeout_ms)
{
    sockaddr_any s;
    const socklen_t len = to_sa(to, &s);
    return sys_connect(&s.sa, len, sys_now_ms() + timeout_ms);
}

/* ---- the handshake ---- */

static net_conn *conn_new(int fd, const net_cfg *cfg)
{
    net_conn *c = malloc(sizeof *c);
    if (!c) return NULL;
    memset(c, 0, offsetof(net_conn, sbuf));
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
        if (tries == 4 || !sys_random(e, sizeof e)) goto out;
        if (noise_hs_init(&hs, cfg->ctx, initiator, cfg->prologue, sizeof cfg->prologue, cfg->self->sec, e)) break;
    }
    const int64_t until = sys_now_ms() + c->timeout_ms;            /* the whole handshake: a peer that drips its bytes gains nothing */
    if (initiator) {
        const uint8_t port[2] = {(uint8_t)(cfg->port >> 8), (uint8_t)cfg->port};
        if ((n = noise_hs_write(&hs, NULL, 0, m)) != NET_HS1 || !sys_send_all(c->fd, m, n, until)) goto out;
        if (!sys_recv_all(c->fd, m, NET_HS2, until) || !noise_hs_read(&hs, m, NET_HS2, pl, &plen) || plen != 0) goto out;
        if (!peer_ok(cfg, hs.rs, expect)) goto out;            /* before this side shows its own key */
        if ((n = noise_hs_write(&hs, port, 2, m)) != NET_HS3 || !sys_send_all(c->fd, m, n, until)) goto out;
    } else {
        if (!sys_recv_all(c->fd, m, NET_HS1, until) || !noise_hs_read(&hs, m, NET_HS1, pl, &plen) || plen != 0) goto out;
        if ((n = noise_hs_write(&hs, NULL, 0, m)) != NET_HS2 || !sys_send_all(c->fd, m, n, until)) goto out;
        if (!sys_recv_all(c->fd, m, NET_HS3, until) || !noise_hs_read(&hs, m, NET_HS3, pl, &plen) || plen != 2) goto out;
        if (!peer_ok(cfg, hs.rs, expect)) goto out;
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
        sys_close(fd);
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
        sys_close(fd);
        return NULL;
    }
    c->addr = *from;
    if (!handshake(cfg, c, false, NULL)) {
        net_close(c);
        return NULL;
    }
    return c;
}

bool net_rehandshake(const net_cfg *cfg, net_conn *c, bool initiator, const uint8_t expect[32])
{
    const int fd = c->fd, t = c->timeout_ms;
    const net_addr a = c->addr;
    pz_secure_zero(c, offsetof(net_conn, sbuf));
    c->fd = fd, c->timeout_ms = t, c->addr = a;
    return handshake(cfg, c, initiator, expect);
}

/* ---- frames ---- */

/* a frame in the debug log: the other side, the direction, the type (the first byte) and the length; never the content */
static void trace(const net_conn *c, char dir, const uint8_t *body, size_t len, bool ok)
{
    char a[64];
    net_addr_str(&c->addr, a, sizeof a);
    if (ok) dbg("net %c %s type %u len %zu", dir, a, len ? body[0] : 0u, len);
    else dbg("net %c %s failed", dir, a);
}

bool net_send(net_conn *c, const uint8_t *body, size_t len)
{
    if (len > NOISE_FRAME_MAX) return false;
    const size_t n = noise_frame_seal(&c->tx, body, len, c->sbuf);
    const bool ok = sys_send_all(c->fd, c->sbuf, n, sys_now_ms() + c->timeout_ms);
    trace(c, '>', body, len, ok);
    return ok;
}

bool net_recv(net_conn *c, uint8_t *body, size_t cap, size_t *len)
{
    const int64_t deadline = sys_now_ms() + c->timeout_ms;         /* the head and the body together */
    size_t l;
    if (!sys_recv_all(c->fd, c->rbuf, NOISE_HEAD, deadline) || !noise_frame_head(&c->rx, c->rbuf, &l) || l > cap ||
        !sys_recv_all(c->fd, c->rbuf, l + NOISE_TAG, deadline) || !noise_frame_body(&c->rx, c->rbuf, l, body))
        return trace(c, '<', NULL, 0, false), false;
    *len = l;
    trace(c, '<', body, l, true);
    return true;
}

void net_set_timeout(net_conn *c, int timeout_ms) { c->timeout_ms = timeout_ms > 0 ? timeout_ms : NET_TIMEOUT_MS; }

bool net_readable(net_conn *c, int ms) { return sys_wait(c->fd, false, sys_now_ms() + ms) > 0; }

/* Nothing to read is what an idle connection looks like: its end, an error and a byte it should not have sent all make it readable. */
bool net_alive(net_conn *c) { return sys_wait(c->fd, false, 0) == 0; }

void net_close(net_conn *c)
{
    if (!c) return;
    sys_close(c->fd);
    pz_secure_zero(c, offsetof(net_conn, sbuf));
    free(c);
}

uint64_t net_splice(net_conn *a, net_conn *b, uint64_t max_bytes, int64_t max_ms, int64_t idle_ms, const atomic_bool *stop)
{
    /* direction i copies fd[i] to fd[1 - i] through the receive buffer of its connection, which the relay no longer needs */
    const int fd[2] = {a->fd, b->fd};
    uint8_t *buf[2] = {a->rbuf, b->rbuf};
    size_t len[2] = {0, 0}, off[2] = {0, 0};
    bool eof[2] = {false, false};
    uint64_t moved = 0;
    const int64_t until = sys_now_ms() + max_ms;
    int64_t quiet = sys_now_ms() + idle_ms;
    pz_secure_zero(&a->tx, sizeof a->tx), pz_secure_zero(&a->rx, sizeof a->rx);
    pz_secure_zero(&b->tx, sizeof b->tx), pz_secure_zero(&b->rx, sizeof b->rx);
    while (!(stop && atomic_load(stop))) {
        const int64_t left = (until < quiet ? until : quiet) - sys_now_ms();
        if (left <= 0 || (eof[0] && off[0] == len[0]) || (eof[1] && off[1] == len[1])) break;   /* one side is done and delivered */
        struct pollfd p[2] = {{.fd = -1}, {.fd = -1}};       /* an end with nothing to wait for is left out */
        for (int i = 0; i < 2; i++) {
            const int at = off[i] < len[i] ? 1 - i : i;
            p[at].fd = fd[at], p[at].events |= off[i] < len[i] ? POLLOUT : POLLIN;
        }
        const int r = sys_poll(p, 2, left < 200 ? (int)left : 200);  /* in slices: the stop flag is looked at */
        if (r < 0) break;
        if (r == 0) continue;
        bool broken = false;
        for (int i = 0; i < 2 && !broken; i++) {
            const int o = 1 - i;
            if (off[i] == len[i] && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) {
                const long n = sys_recv_now(fd[i], buf[i], sizeof a->rbuf);
                if (n > 0 && moved + (uint64_t)n <= max_bytes) off[i] = 0, len[i] = (size_t)n, moved += (uint64_t)n, quiet = sys_now_ms() + idle_ms;
                else if (n == 0) eof[i] = true;
                else if (n != SYS_AGAIN) broken = true;          /* over the limit, or an error */
            }
            if (off[i] < len[i] && (p[o].revents & (POLLOUT | POLLHUP | POLLERR))) {
                const long n = sys_send_now(fd[o], buf[i] + off[i], len[i] - off[i]);
                if (n > 0) off[i] += (size_t)n;
                else if (n != SYS_AGAIN) broken = true;
                if (off[i] == len[i]) off[i] = len[i] = 0;
            }
        }
        if (broken) break;
    }
    net_close(a);
    net_close(b);
    return moved;
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
