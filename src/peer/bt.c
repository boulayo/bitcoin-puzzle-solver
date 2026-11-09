/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* bt.c - a client of the Mainline DHT (see bt.h): get_peers towards the key, wave after wave, then announce_peer to the closest nodes that
 * answered. It asks and never answers: it is a guest of that table, for a few packets every half hour. Every packet that comes is from
 * a stranger: it counts only when it comes from the address asked, with the transaction number given, and is read within its bounds. */
#include "bt.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NODES_MAX 256              /* nodes of that table kept in a round */
#define WAVE 8                     /* questions at a time, and the nodes announced to */
#define ASK_MAX 96                 /* questions of a round */
#define TOKEN_MAX 64

const char *const *bt_routers(bool main_chain, bool off, const char *const *given, int ngiven, int *n)
{
    static const char *const KNOWN[] = {"router.bittorrent.com:6881", "dht.transmissionbt.com:6881", "router.utorrent.com:6881",
                                        "dht.libtorrent.org:25401"};
    *n = off ? 0 : ngiven ? ngiven : main_chain ? (int)(sizeof KNOWN / sizeof *KNOWN) : 0;
    return ngiven ? given : KNOWN;
}

/* ---- bencoding ---- */

/* the length of the item at p (0: not a valid item within n bytes) */
static size_t item_len(const uint8_t *p, size_t n, int depth)
{
    if (!n || depth > 8) return 0;
    if (p[0] == 'i') {
        const uint8_t *e = memchr(p, 'e', n);
        return e ? (size_t)(e - p) + 1 : 0;
    }
    if (p[0] == 'l' || p[0] == 'd') {
        size_t at = 1;
        while (at < n && p[at] != 'e') {
            const size_t l = item_len(p + at, n - at, depth + 1);
            if (!l) return 0;
            at += l;
        }
        return at < n ? at + 1 : 0;
    }
    size_t len = 0, at = 0;
    while (at < n && p[at] >= '0' && p[at] <= '9' && at < 9) len = len * 10 + (size_t)(p[at++] - '0');
    if (!at || at >= n || p[at] != ':' || len > n - at - 1) return 0;
    return at + 1 + len;
}

bool bt_string(const uint8_t *v, size_t n, const uint8_t **s, size_t *sn)
{
    const size_t l = item_len(v, n, 0);
    if (!l || v[0] < '0' || v[0] > '9') return false;
    const uint8_t *colon = memchr(v, ':', l);
    *s = colon + 1, *sn = l - (size_t)(colon + 1 - v);
    return true;
}

bool bt_dict_get(const uint8_t *d, size_t n, const char *key, const uint8_t **v, size_t *vn)
{
    const size_t kl = strlen(key), total = item_len(d, n, 0);
    if (!total || d[0] != 'd') return false;
    for (size_t at = 1; at < total - 1;) {
        const uint8_t *k;
        size_t kn;
        const size_t l = item_len(d + at, total - 1 - at, 1);
        if (!l || !bt_string(d + at, l, &k, &kn)) return false;
        at += l;
        const size_t vl = item_len(d + at, total - 1 - at, 1);
        if (!vl) return false;
        if (kn == kl && !memcmp(k, key, kl)) {
            *v = d + at, *vn = vl;
            return true;
        }
        at += vl;
    }
    return false;
}

/* ---- the round ---- */

typedef struct {
    struct sockaddr_in a;
    uint8_t id[20];                /* a router: unknown, all ones (the farthest) */
    uint8_t token[TOKEN_MAX];
    size_t tlen;
    bool asked, answered;
} node;

typedef struct {
    const bt_config *cfg;
    int fd;
    uint8_t me[20];
    node n[NODES_MAX];
    int nn, asked;
    net_addr *out;
    int nout, max;
} round_t;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* whether a is closer to the key than b */
static bool closer(const round_t *r, const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 20; i++) {
        const uint8_t x = a[i] ^ r->cfg->infohash[i], y = b[i] ^ r->cfg->infohash[i];
        if (x != y) return x < y;
    }
    return false;
}

static void add_node(round_t *r, const struct sockaddr_in *a, const uint8_t *id)
{
    uint8_t nid[20];
    if (!a->sin_port || !a->sin_addr.s_addr) return;
    if (id) memcpy(nid, id, 20);
    else for (int i = 0; i < 20; i++) nid[i] = (uint8_t)~r->cfg->infohash[i];
    for (int i = 0; i < r->nn; i++)
        if (r->n[i].a.sin_addr.s_addr == a->sin_addr.s_addr && r->n[i].a.sin_port == a->sin_port) return;
    int at = r->nn;
    if (at == NODES_MAX) {                                     /* full: in place of the farthest one not asked yet, when this is closer */
        at = -1;
        for (int i = 0; i < r->nn; i++)
            if (!r->n[i].asked && (at < 0 || closer(r, r->n[at].id, r->n[i].id))) at = i;
        if (at < 0 || !closer(r, nid, r->n[at].id)) return;
    } else r->nn++;
    r->n[at] = (node){.a = *a};
    memcpy(r->n[at].id, nid, 20);
}

static void add_peer(round_t *r, const uint8_t *p)
{
    net_addr a = {.ip = {[10] = 0xff, [11] = 0xff, [12] = p[0], [13] = p[1], [14] = p[2], [15] = p[3]}, .port = (uint16_t)(p[4] << 8 | p[5])};
    if (!a.port || r->nout == r->max) return;
    for (int i = 0; i < r->nout; i++)
        if (!memcmp(&r->out[i], &a, sizeof a)) return;
    r->out[r->nout++] = a;
}

/* a query to node i: its number is the transaction ID */
static void send_query(round_t *r, int i, bool announce)
{
    uint8_t m[256 + TOKEN_MAX];
    size_t l = 0;
    const node *n = &r->n[i];
    l += (size_t)snprintf((char *)m + l, sizeof m - l, "d1:ad2:id20:");
    memcpy(m + l, r->me, 20), l += 20;
    if (announce) l += (size_t)snprintf((char *)m + l, sizeof m - l, "12:implied_porti0e");
    l += (size_t)snprintf((char *)m + l, sizeof m - l, "9:info_hash20:");
    memcpy(m + l, r->cfg->infohash, 20), l += 20;
    if (announce) {
        l += (size_t)snprintf((char *)m + l, sizeof m - l, "4:porti%ue5:token%zu:", (unsigned)r->cfg->port, n->tlen);
        memcpy(m + l, n->token, n->tlen), l += n->tlen;
    }
    l += (size_t)snprintf((char *)m + l, sizeof m - l, "e1:q%s1:t2:", announce ? "13:announce_peer" : "9:get_peers");
    m[l++] = (uint8_t)(i >> 8), m[l++] = (uint8_t)i;
    l += (size_t)snprintf((char *)m + l, sizeof m - l, "1:y1:qe");
    (void)sendto(r->fd, m, l, 0, (const struct sockaddr *)&n->a, sizeof n->a);
}

/* One packet. An answer counts when its transaction ID is the number of a node asked and it comes from that node's address. Returns the
 * node, or -1. */
static int take(round_t *r, const uint8_t *m, size_t len, const struct sockaddr_in *from)
{
    const uint8_t *v, *s, *res;
    size_t vn, sn, rn;
    if (!bt_dict_get(m, len, "t", &v, &vn) || !bt_string(v, vn, &s, &sn) || sn != 2) return -1;
    const int i = s[0] << 8 | s[1];
    if (i >= r->nn || !r->n[i].asked || r->n[i].a.sin_addr.s_addr != from->sin_addr.s_addr || r->n[i].a.sin_port != from->sin_port) return -1;
    if (!bt_dict_get(m, len, "r", &res, &rn)) return -1;       /* an error, or a query of theirs */
    node *n = &r->n[i];
    if (n->answered) return i;                                 /* the answer to the announce */
    n->answered = true;
    if (bt_dict_get(res, rn, "id", &v, &vn) && bt_string(v, vn, &s, &sn) && sn == 20) memcpy(n->id, s, 20);
    if (bt_dict_get(res, rn, "token", &v, &vn) && bt_string(v, vn, &s, &sn) && sn && sn <= TOKEN_MAX) memcpy(n->token, s, sn), n->tlen = sn;
    if (bt_dict_get(res, rn, "values", &v, &vn) && vn >= 2 && v[0] == 'l')
        for (size_t at = 1; at < vn - 1;) {
            if (!bt_string(v + at, vn - 1 - at, &s, &sn)) break;
            if (sn == 6) add_peer(r, s);
            at += (size_t)(s + sn - (v + at));
        }
    if (bt_dict_get(res, rn, "nodes", &v, &vn) && bt_string(v, vn, &s, &sn))
        for (size_t at = 0; at + 26 <= sn; at += 26) {
            struct sockaddr_in a = {.sin_family = AF_INET};
            memcpy(&a.sin_addr, s + at + 20, 4), memcpy(&a.sin_port, s + at + 24, 2);
            add_node(r, &a, s + at);
        }
    return i;
}

/* the packets that come until the deadline, or until want answers came */
static int listen_until(round_t *r, int64_t deadline, int want)
{
    int got = 0;
    while (got < want) {
        const int64_t left = deadline - now_ms();
        struct pollfd p = {.fd = r->fd, .events = POLLIN};
        if (left <= 0 || (r->cfg->stop && atomic_load(r->cfg->stop))) break;
        if (poll(&p, 1, left < 200 ? (int)left : 200) <= 0) continue;
        uint8_t m[2048];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t l = recvfrom(r->fd, m, sizeof m, 0, (struct sockaddr *)&from, &fl);
        if (l > 0 && fl == sizeof from && take(r, m, (size_t)l, &from) >= 0) got++;
    }
    return got;
}

/* the closest nodes that fit, at most WAVE, into pick: not asked yet (ask), or answered with a token (!ask) */
static int closest(const round_t *r, bool ask, int pick[WAVE])
{
    int n = 0;
    for (int i = 0; i < r->nn; i++) {
        if (ask ? r->n[i].asked : !(r->n[i].answered && r->n[i].tlen)) continue;
        int at = WAVE - 1;
        if (n < WAVE) at = n++;
        else if (!closer(r, r->n[i].id, r->n[pick[at]].id)) continue;
        pick[at] = i;
        for (; at > 0 && closer(r, r->n[pick[at]].id, r->n[pick[at - 1]].id); at--) {
            const int t = pick[at];
            pick[at] = pick[at - 1], pick[at - 1] = t;
        }
    }
    return n;
}

int bt_rendezvous(const bt_config *cfg, net_addr *out, int max, int *announced)
{
    round_t r = {0};                                           /* about 30 KB */
    r.cfg = cfg, r.out = out, r.max = max < BT_PEERS_MAX ? max : BT_PEERS_MAX;
    if (announced) *announced = 0;
    if (!net_random(r.me, sizeof r.me) || (r.fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) return -1;
    for (int i = 0; i < cfg->nrouters; i++) {
        char host[256];
        const char *colon = strrchr(cfg->routers[i], ':');
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res;
        if (!colon || (size_t)(colon - cfg->routers[i]) >= sizeof host) continue;
        memcpy(host, cfg->routers[i], (size_t)(colon - cfg->routers[i])), host[colon - cfg->routers[i]] = 0;
        if (getaddrinfo(host, colon + 1, &hints, &res)) continue;
        add_node(&r, (const struct sockaddr_in *)res->ai_addr, NULL);
        freeaddrinfo(res);
    }
    const int64_t end = now_ms() + (cfg->budget_ms ? cfg->budget_ms : 20000);
    const int wave_ms = cfg->wave_ms ? cfg->wave_ms : 1500;
    int answers = 0, pick[WAVE];
    while (r.asked < ASK_MAX && now_ms() < end && !(cfg->stop && atomic_load(cfg->stop))) {              /* until the closest nodes known have all been asked */
        const int n = closest(&r, true, pick);
        int best[WAVE], sent = 0;
        const int nb = closest(&r, false, best);
        for (int i = 0; i < n; i++) {
            /* a node farther than the WAVE closest that answered cannot bring the lookup closer */
            if (nb == WAVE && !closer(&r, r.n[pick[i]].id, r.n[best[WAVE - 1]].id)) continue;
            r.n[pick[i]].asked = true, r.asked++, sent++;
            send_query(&r, pick[i], false);
        }
        if (!sent) break;
        const int64_t until = now_ms() + wave_ms;
        answers += listen_until(&r, until < end ? until : end, sent);
    }
    if (answers && cfg->port) {
        const int n = closest(&r, false, pick);
        for (int i = 0; i < n; i++) send_query(&r, pick[i], true);
        const int took = listen_until(&r, now_ms() + wave_ms, n);
        if (announced) *announced = took;
    }
    close(r.fd);
    return answers ? r.nout : -1;
}
