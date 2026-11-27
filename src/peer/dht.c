/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.c - the node (see dht.h, and dht_priv.h for its files): its life (create, start, stop), its threads (accept, handlers, jobs,
 * maintenance, and the one that meets the others in the table of BitTorrent and keeps the port open on the router), the answers to
 * requests, and the limits by address block. */
#include "dbg.h"
#include "dht_priv.h"
#include "portmap.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ANS_MAX MBOX_LEN           /* the longest answer */
#define JOBS_MAX 4096
#define ALERT_JOBS_BLOCK 8         /* alerts from one address block */
#define IN_MS 2000                 /* the handshake and the request of a connection that comes in, together: they come at once */
#define CONN_PER_BLOCK 4           /* connections of one address block queued or being answered at a time */
#define LIM_SLOTS 4096             /* address blocks whose limits are remembered */
#define MBOX_CAP 4096              /* alert pairs a mailbox keeps, */
#define MBOX_PER_OWNER DHT_MBOX_MAX /* ... and of one owner */
#define BOOT_RETRY_MS 30000
#define SAVE_MS 300000
#define REPUB_FIRST_MS 120000      /* the first republish after a start: what went to nobody before the stop, what changed meanwhile */
#define REPUBLISH_MS (24 * 3600 * 1000LL)   /* then every day */
#define MEET_MS 900000             /* between two rounds in the table of BitTorrent, which forgets in half an hour; 2 minutes while alone */
#define HANDLER_STACK (1 << 20)

/* the limits by address block (docs/en/NETWORK.md 7.4): a bucket of tokens of each kind, refilled at a rate up to a burst */
static const double LIM_RATE[LIM_KINDS] = {20, 50};            /* per second: connections, records */
static const double LIM_BURST[LIM_KINDS] = {100, 100000};      /* a home that was alone sends all its records at once */

struct lim_entry {
    uint8_t block[16];
    bool used;
    int conns;                     /* connections queued or being answered */
    double tok[LIM_KINDS];
    int64_t at;                    /* the last refill */
};

void dlog(const dht *d, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg("node %s", line);
    if (d->cfg.verbose) fprintf(stderr, "pzpeer %02x%02x%02x%02x: %s\n", d->me.id[2], d->me.id[3], d->me.id[4], d->me.id[5], line);
}

bool spawn(pthread_t *th, void *(*fn)(void *), void *arg, size_t stack, bool detached)
{
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return false;
    (void)pthread_attr_setstacksize(&at, stack);
    if (detached) (void)pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const bool ok = pthread_create(th, &at, fn, arg) == 0;
    pthread_attr_destroy(&at);
    return ok;
}


/* ---- addresses and the limits by block ---- */

/* the entry of a block (l_mu held), its tokens refilled: found, or a free one, or the one used longest ago with no connection; NULL when
 * all are busy */
static lim_entry *lim_get(dht *d, const net_addr *a)
{
    uint8_t blk[16], m[48], hh[32];
    net_addr_block(a, blk);
    memcpy(m, d->lim_key, 32), memcpy(m + 32, blk, 16);       /* keyed: blocks chosen to meet in one place and push each other out */
    pz_sha256(m, sizeof m, hh);                                /* (back to a full share of tokens) would have to know the key */
    uint64_t h;
    memcpy(&h, hh, 8);
    lim_entry *best = NULL;                                    /* a free entry, else the idle one used longest ago */
    const int64_t now = sys_now_ms();
    for (int p = 0; p < 8; p++) {
        lim_entry *e = &d->lim[(h + (uint64_t)p) & (LIM_SLOTS - 1)];
        if (e->used && !memcmp(e->block, blk, 16)) {
            const double dt = (double)(now - e->at) / 1000;
            for (int k = 0; k < LIM_KINDS; k++) e->tok[k] = e->tok[k] + dt * LIM_RATE[k] < LIM_BURST[k] ? e->tok[k] + dt * LIM_RATE[k] : LIM_BURST[k];
            e->at = now;
            return e;
        }
        if (!e->used) {
            if (!best || best->used) best = e;
        } else if (!e->conns && (!best || (best->used && e->at < best->at))) {
            best = e;
        }
    }
    if (!best) return NULL;
    memset(best, 0, sizeof *best);
    memcpy(best->block, blk, 16);
    best->used = true;
    for (int k = 0; k < LIM_KINDS; k++) best->tok[k] = LIM_BURST[k];
    best->at = now;
    return best;
}

/* n tokens of a kind from the block of a (n < 0 gives them back), and conns more of its connections at a time: 1 for one that comes (at
 * most CONN_PER_BLOCK: a peer that opens connections and says nothing holds at most that many handlers), -1 for one that ends. False
 * when the block has used its share. */
static bool lim(dht *d, const net_addr *a, int kind, double n, int conns)
{
    if (!limited(d, a)) return true;
    pthread_mutex_lock(&d->l_mu);
    lim_entry *e = lim_get(d, a);
    const bool ok = e && e->tok[kind] >= n && e->conns + conns >= 0 && e->conns + conns <= CONN_PER_BLOCK;
    if (ok) e->tok[kind] -= n, e->conns += conns;
    pthread_mutex_unlock(&d->l_mu);
    return ok;
}

bool lim_take(dht *d, const net_addr *a, int kind, double n) { return lim(d, a, kind, n, 0); }

/* ---- answers ---- */

/* the node at the other end of a connection, at the address it came from and the port it told */
static dht_contact conn_contact(const net_conn *c)
{
    dht_contact ct;
    memcpy(ct.id, c->remote_id, 32);
    memcpy(ct.xonly, c->remote, 32);
    ct.addr = c->addr;
    return ct;
}

/* MBOX: the pairs, each with the flags of its proofs and the proofs (zeros for one that did not come); its length */
static size_t mbox_encode(const mbox_pair *p, int n, uint8_t *ans)
{
    ans[0] = DHT_MBOX, ans[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *q = ans + 2 + (size_t)i * DHT_MBOX_ENTRY;
        record_encode(&p[i].r[0], q), record_encode(&p[i].r[1], q + RECORD_LEN);
        q[PAIR_LEN] = (uint8_t)(p[i].has[0] | p[i].has[1] << 1);
        for (int k = 0; k < 2; k++) {
            if (p[i].has[k]) memcpy(q + PAIR_LEN + 1 + 64 * k, p[i].proof[k], 64);
            else memset(q + PAIR_LEN + 1 + 64 * k, 0, 64);
        }
    }
    return 2 + (size_t)n * DHT_MBOX_ENTRY;
}

/* The answer to a request into ans (ANS_MAX bytes): its length, 0 for a malformed request (the connection is closed without one). */
static size_t answer(dht *d, const net_conn *c, const uint8_t *m, size_t len, uint8_t *ans)
{
    record r, r2;
    /* A linear search has no records: any x on the curve with the zero bits passes the checks of one, with no walk (at 64 zero bits too),
     * so a node of such a network takes none, and no alert or proof of one */
    if (d->cfg.blocks && (m[0] == DHT_STORE || m[0] == DHT_ALERT || m[0] == DHT_PROVE || m[0] == DHT_POSSESS)) return 0;
    switch (m[0]) {
    case DHT_PING:
        if (len != 1) return 0;
        ans[0] = DHT_PONG;
        return 1;
    case DHT_FIND_NODE: {
        dht_contact cs[DHT_K];
        if (len != 33) return 0;
        const int n = rt_closest(d, m + 1, cs, DHT_K);
        for (int i = 0; i < n; i++) encode_contact(&cs[i], ans + 2 + (size_t)i * NODE_LEN);
        ans[0] = DHT_NODES, ans[1] = (uint8_t)n;
        return 2 + (size_t)n * NODE_LEN;
    }
    case DHT_STORE:
        /* an owner stores its own records, without its identity, which is the sender's: another's would bypass the limits of its address,
         * and custodians do not copy */
        if (len < 2 || m[1] == 0 || len != 2 + (size_t)m[1] * RECORD_WIRE) return 0;
        ans[0] = DHT_STORED, ans[1] = m[1];
        for (int i = 0; i < m[1]; i++) {
            const bool ok = record_wire_decode(m + 2 + (size_t)i * RECORD_WIRE, c->remote, &r);
            ans[2 + i] = (uint8_t)(ok ? custody_put(d, &r, &c->addr) : DHT_ST_INVALID);
        }
        return 2 + (size_t)m[1];
    case DHT_ALERT: {
        /* the pair, and the proof of the owner of the first record for the owner of the second over the pair (7.3): a pair that no
         * owner proves is nobody's business, so a stranger cannot make an alert of a point it does not hold */
        uint8_t h[32];
        if (len != 1 + PAIR_LEN + 64 || !record_decode(m + 1, &r) || !record_decode(m + 1 + RECORD_LEN, &r2) || !pair_ok(d, &r, &r2)) return 0;
        pair_hash(&r, &r2, h);
        if (!record_proof_ok(d->cfg.ctx, d->cfg.net, &r, r2.owner, h, m + 1 + PAIR_LEN)) return 0;
        (void)alert_in(d, &r, &r2, m + 1 + PAIR_LEN, &c->addr);
        ans[0] = DHT_OK;
        return 1;
    }
    case DHT_MBOX_GET: {
        /* the owner's own pairs, to the owner only: they show the points of its coincidences */
        mbox_pair pairs[DHT_MBOX_MAX];
        if (len != 33 || memcmp(m + 1, c->remote, 32)) return 0;
        return mbox_encode(pairs, mbox_get(d->mb, m + 1, pairs, DHT_MBOX_MAX), ans);
    }
    case DHT_PROVE: {
        /* the owner's proofs of records of its own, and the answer: the other owners of their x (custody_proved) */
        mbox_pair pairs[DHT_PROVE_MAX * STORE_X_OWNERS];
        const int n = len >= 2 ? m[1] : 0;
        int np = 0;
        if (n < 1 || n > DHT_PROVE_MAX || len != 2 + (size_t)n * PROVE_ENTRY) return 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *q = m + 2 + (size_t)i * PROVE_ENTRY;
            const int k = record_wire_decode(q, c->remote, &r) ? custody_proved(d, &r, q + RECORD_WIRE, &c->addr, pairs + np) : -1;
            if (k < 0) return 0;
            np += k;
        }
        return mbox_encode(pairs, np, ans);
    }
    case DHT_POSSESS: {
        uint8_t dd[32];
        /* only for a record of this node whose distance it holds (any other gets no proof: own_distance, record_prove): the proof binds
         * the identity of the asker (Noise told it) and its nonce */
        if (len != 1 + RECORD_LEN + 32 || !record_decode(m + 1, &r) || !mine(d, &r)) return 0;
        const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, r.x, r.t, dd) &&
                        record_prove(d->cfg.ctx, d->cfg.net, &r, dd, c->remote, m + 1 + RECORD_LEN, ans + 1);
        pz_secure_zero(dd, sizeof dd);
        if (!ok) return 0;
        ans[0] = DHT_PROOF;
        return 1 + 64;
    }
    case DHT_CLAIM:
    case DHT_CLAIM_GET:
        return claim_answer(d, c, m, len, ans);
    default:
        return 0;
    }
}

static void serve(dht *d, int fd, const net_addr *from, uint8_t *req, uint8_t *ans)
{
    net_cfg in = d->ncfg;                                      /* a peer that opens and says nothing holds a handler for IN_MS */
    if (in.timeout_ms > IN_MS) in.timeout_ms = IN_MS;
    const int64_t until = sys_now_ms() + in.timeout_ms;        /* of the request too: a handshake does not earn it a time of its own */
    net_conn *c = net_handshake(&in, fd, from);
    size_t len;
    if (!c) return;
    if (!is_me(d, c->remote_id) && addr_ok(d, from)) atomic_store(&d->in_ms, sys_now_ms());   /* the port of this node is open (reached) */
    const int64_t left = until - sys_now_ms();
    net_set_timeout(c, left > 0 ? (int)left : 1);
    if (!net_recv(c, req, NOISE_FRAME_MAX, &len) || len == 0) {
        net_close(c);
        return;
    }
    net_set_timeout(c, d->ncfg.timeout_ms);
    if (req[0] >= DHT_SESSION_FIRST && req[0] <= DHT_SESSION_LAST) {
        if (d->cfg.on_session) d->cfg.on_session(d->cfg.user, c, req, len);   /* the daemon owns the connection now */
        else net_close(c);
        return;
    }
    if (req[0] == DHT_RELAY) {                                 /* the relay keeps it, waiting or relayed */
        if (!relay_in(d, c, req, len)) net_close(c);
        return;
    }
    const size_t n = answer(d, c, req, len, ans);
    if (n && net_send(c, ans, n)) {
        const dht_contact ct = conn_contact(c);                /* a node that asks is alive: a contact, once it answers at its port */
        rt_asked(d, &ct);
    }
    net_close(c);
}

void path_of(const dht *d, const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", d->dir, name); }

/* ---- threads ---- */

bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c, const uint8_t *block)
{
    job *j = calloc(1, sizeof *j);
    if (!j) return false;
    j->kind = kind;
    if (a) j->a = *a;
    if (b) j->b = *b;
    if (c) j->c = *c;
    if ((j->blocked = block != NULL)) memcpy(j->block, block, 16);
    pthread_mutex_lock(&d->j_mu);
    int same = 0;                                              /* slow alerts of a few blocks cannot hold the whole queue */
    job **at = &d->jhead;
    for (; *at; at = &(*at)->next) same += j->blocked && (*at)->blocked && !memcmp((*at)->block, j->block, 16);
    const bool ok = d->jn < JOBS_MAX && same < ALERT_JOBS_BLOCK && !stopping(d);
    if (ok) {
        *at = j;
        d->jn++;
        pthread_cond_signal(&d->j_cv);
    }
    pthread_mutex_unlock(&d->j_mu);
    if (!ok) free(j);
    return ok;
}

/* The next job (j_mu held): the oldest of the kind whose turn it is, else of the other kind. The kinds take turns, so the pings do not
 * wait behind a flood of alerts for this node, nor these behind them. */
static job *job_next(dht *d)
{
    for (int k = 0; k < JOB_KINDS; k++) {
        const int kind = (d->jturn + k) % JOB_KINDS;
        for (job **at = &d->jhead; *at; at = &(*at)->next)
            if ((*at)->kind == kind) {
                job *j = *at;
                *at = j->next;
                d->jturn = kind + 1;
                d->jn--;
                return j;
            }
    }
    return NULL;
}

static void *jobs_main(void *arg)
{
    dht *d = arg;
    for (;;) {
        pthread_mutex_lock(&d->j_mu);
        while (!d->jhead && !stopping(d)) pthread_cond_wait(&d->j_cv, &d->j_mu);
        job *j = stopping(d) ? NULL : job_next(d);
        pthread_mutex_unlock(&d->j_mu);
        if (!j) return NULL;
        if (j->kind == JOB_ALERT) job_alert(d, &j->a, &j->b);
        else job_ping(d, &j->c);
        free(j);
    }
}

static void *accept_main(void *arg)
{
    dht *d = arg;
    while (!stopping(d)) {
        net_addr from;
        const int fd = net_accept(d->lfd, 100, &from);
        if (fd == -2) sys_sleep_ms(50);                                      /* out of descriptors: let the handlers close some */
        if (fd < 0) continue;
        if (!lim(d, &from, LIM_CONN, 1, 1)) {                  /* its block has its share of the handlers, or of the rate */
            sys_close(fd);
            continue;
        }
        pthread_mutex_lock(&d->q_mu);
        const bool room = d->qn < QUEUE_MAX && !stopping(d);
        if (room) {
            d->q[(d->qhead + d->qn) % QUEUE_MAX] = (pending){fd, from};
            d->qn++;
            pthread_cond_signal(&d->q_cv);
        }
        pthread_mutex_unlock(&d->q_mu);
        if (!room) {                                           /* busy: the dialler tries another node */
            sys_close(fd);
            (void)lim(d, &from, LIM_CONN, 0, -1);
        }
    }
    return NULL;
}

static void *handler_main(void *arg)
{
    dht *d = arg;
    uint8_t *req = malloc(NOISE_FRAME_MAX), *ans = malloc(ANS_MAX);
    for (;;) {
        pthread_mutex_lock(&d->q_mu);
        while (!d->qn && !stopping(d)) pthread_cond_wait(&d->q_cv, &d->q_mu);
        const bool go = !stopping(d);
        pending p = d->q[d->qhead];
        if (go) d->qhead = (d->qhead + 1) % QUEUE_MAX, d->qn--;
        pthread_mutex_unlock(&d->q_mu);
        if (!go) break;
        if (req && ans) serve(d, p.fd, &p.from, req, ans);
        else sys_close(p.fd);
        (void)lim(d, &p.from, LIM_CONN, 0, -1);
    }
    free(req);
    free(ans);
    return NULL;
}

/* The way in without a server of this network: every so often the node announces its port in the table of BitTorrent under the key of
 * the network and takes the addresses announced there (bt.h); they are candidates, dialled and checked by the next bootstrap as any
 * other. The same thread keeps the port open on the router of the LAN, when asked (portmap.h). */
static void *meet_main(void *arg)
{
    dht *d = arg;
    static const char TAG[] = "pznet1/bt";
    uint8_t m[sizeof TAG - 1 + 32], h[32];
    bt_config bc = {.port = d->port, .routers = d->cfg.bt_routers, .nrouters = d->cfg.nbt_routers, .stop = &d->stop};
    memcpy(m, TAG, sizeof TAG - 1), memcpy(m + sizeof TAG - 1, d->cfg.net->fp, 32);
    pz_sha256(m, sizeof m, h);
    memcpy(bc.infohash, h, 20);
    int64_t next_meet = 0, next_map = 0;
    bool mapped = false;
    char why[400];
    while (!stopping(d)) {
        const int64_t now = sys_now_ms();
        if (d->cfg.portmap && d->port && now >= next_map) {    /* a lease of two hours, asked again before half of it passes */
            const unsigned s = portmap(d->port, 7200, d->me.xonly, NULL, why, sizeof why);
            dlog(d, "router: %s", why);
            mapped |= s != 0;
            next_map = now + (s ? 500LL * (s < 120 ? 120 : s) : 600000);
        }
        if (d->cfg.nbt_routers && now >= next_meet) {
            net_addr peers[BT_PEERS_MAX];
            int took = 0, fresh = 0;
            const int n = bt_rendezvous(&bc, peers, BT_PEERS_MAX, &took);
            pthread_mutex_lock(&d->f_mu);
            for (int i = 0; i < n; i++)
                if (addr_ok(d, &peers[i]) && fresh < BT_PEERS_MAX) d->found[fresh++] = peers[i];
            if (n >= 0) d->nfound = fresh;
            pthread_mutex_unlock(&d->f_mu);
            dlog(d, "rendezvous: %s, %d addresses, announced to %d nodes", n < 0 ? "no node of the table of BitTorrent answered" : "done",
                 fresh, took);
            if (fresh && dht_count(d) < DHT_K) (void)bootstrap(d);
            next_meet = sys_now_ms() + (dht_count(d) ? MEET_MS : 120000);
        }
        sys_sleep_ms(200);
    }
    if (mapped) (void)portmap(d->port, 0, d->me.xonly, NULL, why, sizeof why), dlog(d, "router: %s", why);   /* none left behind */
    return NULL;
}

/* between two polls of the mailboxes: a node the others reach gets its alerts by itself, and its mailboxes hold the few that missed it */
static int64_t poll_ms(const dht *d) { return 1000LL * (d->cfg.mbox_poll_s ? d->cfg.mbox_poll_s : reached(d) ? 3600 : 60); }

static void *maint_main(void *arg)
{
    dht *d = arg;
    const int64_t t0 = sys_now_ms();
    int64_t polled = t0, next_refresh = t0 + 60000, next_save = t0 + SAVE_MS, next_repub = t0 + REPUB_FIRST_MS,
            next_boot = t0 + BOOT_RETRY_MS, next_retry = t0 + 1000;
    bool alone = !dht_count(d);
    /* in slices of 200 ms, on the clock that only goes forward: before, a wait until a time of the wall clock lasted as long again as
     * the clock was set back meanwhile */
    for (sys_sleep_ms(200); !stopping(d); sys_sleep_ms(200)) {
        const int64_t now = sys_now_ms();
        if (now >= next_boot) {
            if (!dht_count(d)) (void)bootstrap(d);               /* alone: the bootstrap list and peers.txt again */
            next_boot = sys_now_ms() + BOOT_RETRY_MS;
        }
        if (now >= next_retry) {
            retry_alerts(d);
            pthread_mutex_lock(&d->r_mu);
            relay_sweep(d);                                     /* the homes that waited at this relay in vain */
            pthread_mutex_unlock(&d->r_mu);
            next_retry = now + 1000;
        }
        if (now >= polled + poll_ms(d)) {
            (void)dht_mbox_poll(d);
            polled = sys_now_ms();
        }
        if (now >= next_refresh) {
            refresh(d, false);
            next_refresh = sys_now_ms() + 60000;
        }
        if (now >= next_save) {
            save_peers(d);
            next_save = now + SAVE_MS;
        }
        const int nc = dht_count(d);
        const bool met = alone && nc && atomic_exchange(&d->unplaced, false);   /* the first contacts after none: what went to nobody */
        alone = !nc;
        if (now >= next_repub || met) {
            (void)dht_republish(d, met);
            next_repub = sys_now_ms() + REPUBLISH_MS;
        }
    }
    return NULL;
}

/* ---- the node ---- */

static char *dup_str(const char *s)
{
    const size_t n = strlen(s) + 1;
    char *c = malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

dht *dht_create(const dht_config *cfg, char *err, size_t errlen)
{
    dht *d = calloc(1, sizeof *d);
    char path[4096];
    if (!d || !cfg->dir || !(d->dir = dup_str(cfg->dir))) {          /* a copy: the caller's may be on its stack (the tests) */
        snprintf(err, errlen, "out of memory");
        free(d);
        return NULL;
    }
    d->cfg = *cfg;
    d->cfg.dir = d->dir;
    d->lfd = d->ownfd = -1;
    d->retry_ms = 1000LL * (cfg->retry_s ? cfg->retry_s : 60);
    d->alert_ms = 1000LL * (cfg->alert_s ? cfg->alert_s : 86400);
    d->slot_ms = 1000LL * (cfg->relay_slot_s ? cfg->relay_slot_s : 60);
    if (d->cfg.handlers <= 0) d->cfg.handlers = 8;
    if (d->cfg.relay_max <= 0) d->cfg.relay_max = 32;
    if (!d->cfg.relay_bytes) d->cfg.relay_bytes = 32ull << 20;
    if (!d->cfg.store_max) d->cfg.store_max = DHT_STORE_DEFAULT;
    memcpy(d->me.id, cfg->self->id, 32);
    memcpy(d->me.xonly, cfg->self->xonly, 32);
    pthread_mutex_init(&d->rt_mu, NULL);
    pthread_mutex_init(&d->l_mu, NULL);
    pthread_mutex_init(&d->q_mu, NULL);
    pthread_cond_init(&d->q_cv, NULL);
    pthread_mutex_init(&d->j_mu, NULL);
    pthread_cond_init(&d->j_cv, NULL);
    pthread_mutex_init(&d->a_mu, NULL);
    pthread_mutex_init(&d->r_mu, NULL);
    pthread_cond_init(&d->r_cv, NULL);
    pthread_mutex_init(&d->p_mu, NULL);
    pthread_mutex_init(&d->f_mu, NULL);
    const int64_t now = sys_now_ms();
    for (int i = 0; i < DHT_BUCKETS; i++) d->b[i].looked = now;
    net_cfg_init(&d->ncfg, cfg->ctx, cfg->self, cfg->net->fp, cfg->id_bits, 0, cfg->timeout_ms);
    d->ncfg0 = d->ncfg;
    d->t0 = now;
    d->probe_ms = 1000LL * (cfg->probe_s ? cfg->probe_s : 120);
    atomic_init(&d->in_ms, now - REACH_MS + d->probe_ms);
    if (!(d->lim = calloc(LIM_SLOTS, sizeof *d->lim)) || !(d->active = calloc((size_t)d->cfg.relay_max, sizeof *d->active)) ||
        !sys_random(d->lim_key, sizeof d->lim_key)) {
        snprintf(err, errlen, "out of memory, or no randomness from the system");
        dht_stop(d);
        return NULL;
    }
    if (!sys_mkdir(d->dir)) {
        snprintf(err, errlen, "%s: cannot make the data folder", d->dir);
        dht_stop(d);
        return NULL;
    }
    path_of(d, cfg->blocks ? "claims.bin" : "store.bin", path, sizeof path);   /* a linear search keeps claims, and no records (answer) */
    if (cfg->blocks ? !(d->claims = claims_open(path, cfg->net->fp, err, errlen))
                    : !(d->custody = store_open(path, cfg->ctx, cfg->net, err, errlen))) {
        dht_stop(d);
        return NULL;
    }
    if (d->custody) store_limit(d->custody, d->cfg.store_max);
    const bool own = own_open(d, err, errlen);
    if (!own || !(d->mb = mbox_new(MBOX_CAP, MBOX_PER_OWNER))) {
        if (own) snprintf(err, errlen, "out of memory");
        dht_stop(d);
        return NULL;
    }
    return d;
}

/* the end of a start that failed, and why, on stderr */
static int start_failed(const char *why) { return fprintf(stderr, "pzpeer: %s\n", why), -1; }

int dht_start(dht *d)
{
    if (d->started) return -1;
    if (d->cfg.listen_port != 0) {
        const uint16_t want = d->cfg.listen_port == DHT_ANY_PORT ? 0 : (uint16_t)d->cfg.listen_port;
        if ((d->lfd = net_listen(d->cfg.listen_host, want, &d->port)) < 0) {
            char why[300];
            snprintf(why, sizeof why, "cannot listen on port %u: %s (another program may have it: --listen PORT takes another, --listen 0 "
                     "does not listen)", (unsigned)want, sys_net_why());
            return start_failed(why);
        }
        d->ncfg.port = d->me.addr.port = d->port;
    }
    d->started = true;
    if (!(d->th_handlers = calloc((size_t)d->cfg.handlers, sizeof *d->th_handlers))) return start_failed("out of memory");
    if (d->lfd >= 0) {
        while (d->nhandlers < d->cfg.handlers && spawn(&d->th_handlers[d->nhandlers], handler_main, d, HANDLER_STACK, false)) d->nhandlers++;
        d->has_accept = d->nhandlers > 0 && spawn(&d->th_accept, accept_main, d, HANDLER_STACK, false);
    }
    d->has_jobs = spawn(&d->th_jobs, jobs_main, d, HANDLER_STACK, false);
    if ((d->lfd >= 0 && !d->has_accept) || !d->has_jobs) return start_failed("cannot start the threads of the node");
    const int n = bootstrap(d);
    if (n) (void)dht_mbox_poll(d);                             /* the alerts that came while this node was away */
    d->has_maint = spawn(&d->th_maint, maint_main, d, HANDLER_STACK, false);
    if (d->cfg.nbt_routers || (d->cfg.portmap && d->port)) d->has_meet = spawn(&d->th_meet, meet_main, d, HANDLER_STACK, false);
    if (!d->has_maint) return start_failed("cannot start the threads of the node");
    dlog(d, "started, %s, %d contacts", d->port ? "listening" : "not listening", n);
    return n;
}

void dht_stop(dht *d)
{
    if (!d) return;
    atomic_store(&d->stop, true);
    pthread_mutex_lock(&d->q_mu);
    pthread_cond_broadcast(&d->q_cv);
    pthread_mutex_unlock(&d->q_mu);
    pthread_mutex_lock(&d->j_mu);
    pthread_cond_broadcast(&d->j_cv);
    pthread_mutex_unlock(&d->j_mu);
    if (d->has_accept) pthread_join(d->th_accept, NULL);
    sys_close(d->lfd);                                         /* new connections are refused from here on */
    for (int i = 0; i < d->nhandlers; i++) pthread_join(d->th_handlers[i], NULL);
    for (; d->qn; d->qn--, d->qhead = (d->qhead + 1) % QUEUE_MAX) sys_close(d->q[d->qhead].fd);
    if (d->has_jobs) pthread_join(d->th_jobs, NULL);
    if (d->has_maint) pthread_join(d->th_maint, NULL);
    if (d->has_meet) pthread_join(d->th_meet, NULL);
    pthread_mutex_lock(&d->r_mu);                              /* the relays see the stop within a slice of their wait */
    while (d->nactive > 0) pthread_cond_wait(&d->r_cv, &d->r_mu);
    for (int i = 0; i < RELAY_WAITING; i++)
        if (d->waiting[i].used) net_close(d->waiting[i].c), d->waiting[i].used = false;
    pthread_mutex_unlock(&d->r_mu);
    if (d->started) save_peers(d);
    for (job *j = d->jhead, *nx; j; j = nx) nx = j->next, free(j);
    store_close(d->custody);
    claims_close(d->claims);
    if (d->ownfd >= 0) (void)sys_sync(d->ownfd), close(d->ownfd);
    free(d->cc.e);
    mbox_free(d->mb);
    free(d->th_handlers);
    free(d->alerts);
    free(d->lim);
    free(d->active);
    free(d->dir);
    pthread_mutex_destroy(&d->rt_mu);
    pthread_mutex_destroy(&d->l_mu);
    pthread_mutex_destroy(&d->q_mu);
    pthread_cond_destroy(&d->q_cv);
    pthread_mutex_destroy(&d->j_mu);
    pthread_cond_destroy(&d->j_cv);
    pthread_mutex_destroy(&d->a_mu);
    pthread_mutex_destroy(&d->r_mu);
    pthread_cond_destroy(&d->r_cv);
    pthread_mutex_destroy(&d->p_mu);
    pthread_mutex_destroy(&d->f_mu);
    free(d);
}


uint16_t dht_port(const dht *d) { return d->port; }

