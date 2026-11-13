/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.c - the node (see dht.h, and dht_priv.h for its files): its life (create, start, stop), its threads (accept, handlers, jobs,
 * maintenance), the answers to requests, and the limits by address block. */
#include "dbg.h"
#include "dht_priv.h"
#include "portmap.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define ANS_MAX MBOX_LEN           /* the longest answer */
#define JOBS_MAX 4096
#define ALERT_JOBS_MAX 256         /* of them, the alerts a custodian sends */
#define IN_MS 2000                 /* the handshake and the request of a connection that comes in: they come at once */
#define CONN_PER_BLOCK 4           /* connections of one address block queued or being answered at a time */
#define LIM_SLOTS 4096             /* address blocks whose limits are remembered */
#define OWNER_SHARE 16             /* an owner holds at most 1/16 of a custodian's records */
#define MBOX_CAP 4096              /* alert pairs a mailbox keeps, */
#define MBOX_PER_OWNER DHT_MBOX_MAX /* ... and of one owner */
#define BOOT_RETRY_MS 30000
#define SAVE_MS 300000
#define HANDLER_STACK (1 << 20)

/* the limits by address block (docs/en/NETWORK.md 7.4): a bucket of tokens of each kind, refilled at a rate up to a burst */
static const double LIM_RATE[LIM_KINDS] = {20, 50, 2, 1.0 / 60};      /* per second: connections, records, alerts in, coincidences */
static const double LIM_BURST[LIM_KINDS] = {100, 100000, 64, 8};       /* a republish sends all of an owner's records at once */

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

/* the entry of a block (l_mu held): found, or a free one, or the one used longest ago with no connection; NULL when all are busy */
static lim_entry *lim_get(dht *d, const net_addr *a)
{
    uint8_t blk[16];
    uint64_t h = 1469598103934665603ULL;                       /* FNV-1a: the blocks are not secret, the probes are few */
    net_addr_block(a, blk);
    for (int i = 0; i < 16; i++) h = (h ^ blk[i]) * 1099511628211ULL;
    lim_entry *best = NULL;                                    /* a free entry, else the idle one used longest ago */
    for (int p = 0; p < 8; p++) {
        lim_entry *e = &d->lim[(h + (uint64_t)p) & (LIM_SLOTS - 1)];
        if (e->used && !memcmp(e->block, blk, 16)) return e;
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
    best->at = sys_now_ms();
    return best;
}

static void lim_refill(lim_entry *e)
{
    const int64_t now = sys_now_ms();
    const double dt = (double)(now - e->at) / 1000;
    for (int k = 0; k < LIM_KINDS; k++) e->tok[k] = e->tok[k] + dt * LIM_RATE[k] < LIM_BURST[k] ? e->tok[k] + dt * LIM_RATE[k] : LIM_BURST[k];
    e->at = now;
}

/* n tokens of a kind from the block of a: false when it has used its share */
bool lim_take(dht *d, const net_addr *a, int kind, double n)
{
    if (!limited(d, a)) return true;
    pthread_mutex_lock(&d->l_mu);
    lim_entry *e = lim_get(d, a);
    bool ok = e != NULL;
    if (ok) lim_refill(e);
    if (ok && (ok = e->tok[kind] >= n)) e->tok[kind] -= n;
    pthread_mutex_unlock(&d->l_mu);
    return ok;
}

/* an accepted connection: within the rate of its block and CONN_PER_BLOCK at a time (a peer that opens connections and says nothing
 * holds at most that many handlers) */
static bool lim_conn_open(dht *d, const net_addr *a)
{
    if (!limited(d, a)) return true;
    pthread_mutex_lock(&d->l_mu);
    lim_entry *e = lim_get(d, a);
    bool ok = e != NULL;
    if (ok) lim_refill(e);
    if (ok && (ok = e->conns < CONN_PER_BLOCK && e->tok[LIM_CONN] >= 1)) e->tok[LIM_CONN] -= 1, e->conns++;
    pthread_mutex_unlock(&d->l_mu);
    return ok;
}

static void lim_conn_close(dht *d, const net_addr *a)
{
    if (!limited(d, a)) return;
    pthread_mutex_lock(&d->l_mu);
    lim_entry *e = lim_get(d, a);
    if (e && e->conns > 0) e->conns--;
    pthread_mutex_unlock(&d->l_mu);
}

/* ---- answers ---- */

/* The answer to a request into ans (ANS_MAX bytes): its length, 0 for a malformed request (the connection is closed without one). */
static size_t answer(dht *d, const net_conn *c, const uint8_t *m, size_t len, uint8_t *ans)
{
    record r, r2;
    switch (m[0]) {
    case DHT_PING:
        if (len != 1) return 0;
        ans[0] = DHT_PONG;
        return 1;
    case DHT_FIND_NODE: {
        /* no address twice, or the asker refuses the whole answer: a stale contact and the node now at its address are both in the
         * table until the stale one has failed FAILS_MAX times */
        dht_contact cs[DHT_K];
        int n = 0;
        if (len != 33) return 0;
        const int all = rt_closest(d, m + 1, cs, DHT_K);
        for (int i = 0; i < all; i++) {
            int j = 0;
            while (j < i && (memcmp(cs[j].addr.ip, cs[i].addr.ip, 16) || cs[j].addr.port != cs[i].addr.port)) j++;
            if (j == i) encode_contact(&cs[i], ans + 2 + (size_t)n++ * NODE_LEN);
        }
        ans[0] = DHT_NODES, ans[1] = (uint8_t)n;
        return 2 + (size_t)n * NODE_LEN;
    }
    case DHT_STORE:
        /* an owner stores its own records: another's would bypass the limits of its address, and custodians do not copy */
        if (len < 2 || m[1] == 0 || len != 2 + (size_t)m[1] * RECORD_LEN) return 0;
        ans[0] = DHT_STORED, ans[1] = m[1];
        for (int i = 0; i < m[1]; i++)
            ans[2 + i] = (uint8_t)(record_decode(m + 2 + (size_t)i * RECORD_LEN, &r) && !memcmp(r.owner, c->remote, 32) ? custody_put(d, &r, &c->addr)
                                                                                                                       : DHT_ST_INVALID);
        return 2 + (size_t)m[1];
    case DHT_ALERT: {
        /* the pair, and maybe the proof of the owner of the first record for the owner of the second over the pair (7.3); the rate
         * of the block first, before the checks that cost */
        const bool with = len == 1 + PAIR_LEN + 64;
        uint8_t h[32];
        if ((len != 1 + PAIR_LEN && !with) || !lim_take(d, &c->addr, LIM_ALERT, 1) || !record_decode(m + 1, &r) ||
            !record_decode(m + 1 + RECORD_LEN, &r2) || !pair_ok(d, &r, &r2))
            return 0;
        pair_hash(&r, &r2, h);
        if (with && !record_proof_ok(d->cfg.ctx, d->cfg.net, &r, r2.owner, h, m + 1 + PAIR_LEN)) return 0;
        (void)alert_in(d, &r, &r2, with ? m + 1 + PAIR_LEN : NULL);
        ans[0] = DHT_OK;
        return 1;
    }
    case DHT_MBOX_GET: {
        mbox_pair pairs[DHT_MBOX_MAX];
        if (len != 33) return 0;
        const int n = mbox_get(d->mb, m + 1, pairs, DHT_MBOX_MAX);
        ans[0] = DHT_MBOX, ans[1] = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            uint8_t *q = ans + 2 + (size_t)i * DHT_MBOX_ENTRY;
            record_encode(&pairs[i].r[0], q);
            record_encode(&pairs[i].r[1], q + RECORD_LEN);
            q[PAIR_LEN] = (uint8_t)(pairs[i].has[0] | pairs[i].has[1] << 1);
            for (int k = 0; k < 2; k++) {
                if (pairs[i].has[k]) memcpy(q + PAIR_LEN + 1 + 64 * k, pairs[i].proof[k], 64);
                else memset(q + PAIR_LEN + 1 + 64 * k, 0, 64);
            }
        }
        return 2 + (size_t)n * DHT_MBOX_ENTRY;
    }
    case DHT_POSSESS: {
        uint8_t dd[32];
        /* only for a record of this node, as it signed it: the proof binds the identity of the asker (Noise told it) and its nonce */
        if (len != 1 + RECORD_LEN + 32 || !record_decode(m + 1, &r) || !mine(d, &r) || !record_check(d->cfg.ctx, d->cfg.net, &r)) return 0;
        const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, r.x, r.t, dd) &&
                        record_prove(d->cfg.ctx, d->cfg.net, &r, dd, c->remote, m + 1 + RECORD_LEN, ans + 1);
        pz_secure_zero(dd, sizeof dd);
        if (!ok) return 0;
        ans[0] = DHT_PROOF;
        return 1 + 64;
    }
    default:
        return 0;
    }
}

bool dht_serve(dht *d, net_conn *c, const uint8_t *msg, size_t len)
{
    uint8_t *ans = malloc(ANS_MAX);
    const size_t n = ans && len && msg[0] != DHT_RELAY ? answer(d, c, msg, len, ans) : 0;
    const bool ok = n && net_send(c, ans, n);
    free(ans);
    return ok;
}

static void serve(dht *d, int fd, const net_addr *from, uint8_t *req, uint8_t *ans)
{
    net_cfg in = d->ncfg;                                      /* a peer that opens and says nothing holds a handler for IN_MS */
    if (in.timeout_ms > IN_MS) in.timeout_ms = IN_MS;
    net_conn *c = net_handshake(&in, fd, from);
    size_t len;
    if (!c) return;
    if (!is_me(d, c->remote_id) && addr_ok(d, from)) atomic_store(&d->in_ms, sys_now_ms());   /* the port of this node is open (reached) */
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
        dht_contact ct;                                        /* a node that asks is alive: a contact, once it answers at its port */
        memcpy(ct.id, c->remote_id, 32);
        memcpy(ct.xonly, c->remote, 32);
        ct.addr = c->addr;
        rt_asked(d, &ct);
    }
    net_close(c);
}

void path_of(const dht *d, const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", d->dir, name); }

/* ---- threads ---- */

bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c)
{
    job *j = calloc(1, sizeof *j);
    if (!j) return false;
    j->kind = kind;
    if (a) j->a = *a;
    if (b) j->b = *b;
    if (c) j->c = *c;
    pthread_mutex_lock(&d->j_mu);
    const bool ok = d->jn < JOBS_MAX && (kind != JOB_ALERT_OUT || d->jn_alert < ALERT_JOBS_MAX) && !stopping(d);
    if (ok) {
        job **at = &d->jhead;                                  /* an alert goes before the pings that wait */
        while (*at && (kind == JOB_PING || (*at)->kind != JOB_PING)) at = &(*at)->next;
        j->next = *at;
        *at = j;
        d->jn++;
        d->jn_alert += kind == JOB_ALERT_OUT;
        pthread_cond_signal(&d->j_cv);
    }
    pthread_mutex_unlock(&d->j_mu);
    if (!ok) free(j);
    return ok;
}

static void *jobs_main(void *arg)
{
    dht *d = arg;
    for (;;) {
        pthread_mutex_lock(&d->j_mu);
        while (!d->jhead && !stopping(d)) pthread_cond_wait(&d->j_cv, &d->j_mu);
        job *j = stopping(d) ? NULL : d->jhead;
        if (j) {
            d->jhead = j->next;
            d->jn--;
            d->jn_alert -= j->kind == JOB_ALERT_OUT;
        }
        pthread_mutex_unlock(&d->j_mu);
        if (!j) return NULL;
        if (j->kind == JOB_ALERT_OUT) job_alert_out(d, &j->a, &j->b);
        else if (j->kind == JOB_ALERT_OWN) job_alert_own(d, &j->a, &j->b);
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
        if (!lim_conn_open(d, &from)) {                        /* its block has its share of the handlers, or of the rate */
            net_close_fd(fd);
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
            net_close_fd(fd);
            lim_conn_close(d, &from);
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
        else net_close_fd(p.fd);
        lim_conn_close(d, &p.from);
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
    const int64_t period = (int64_t)(d->cfg.rendezvous_s ? d->cfg.rendezvous_s : 900) * 1000;   /* that table forgets in half an hour */
    int64_t next_meet = 0, next_map = 0;
    while (!stopping(d)) {
        const int64_t now = sys_now_ms();
        if (d->cfg.portmap && d->port && now >= next_map) {    /* a lease of two hours, asked again before half of it passes */
            char why[400];
            const unsigned s = portmap(d->port, 7200, d->me.xonly, d->cfg.router, why, sizeof why);
            dlog(d, "router: %s", why);
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
            if (fresh && rt_count(d) < DHT_K) (void)bootstrap(d);
            next_meet = sys_now_ms() + (rt_count(d) ? period : period < 120000 ? period : 120000);
        }
        sys_sleep_ms(200);
    }
    return NULL;
}

/* between two polls of the mailboxes: a node the others reach gets its alerts by itself, and its mailboxes hold the few that missed it */
static int64_t poll_ms(const dht *d) { return 1000LL * (d->cfg.mbox_poll_s ? d->cfg.mbox_poll_s : reached(d) ? 3600 : 60); }

static void *maint_main(void *arg)
{
    dht *d = arg;
    const int64_t t0 = sys_now_ms();
    int64_t polled = t0, next_refresh = t0 + 60000, next_repub = t0 + d->republish_ms, next_save = t0 + SAVE_MS,
            next_boot = t0 + BOOT_RETRY_MS, next_retry = t0 + 1000;
    bool alone = !rt_count(d);
    if (next_refresh > t0 + d->refresh_ms) next_refresh = t0 + d->refresh_ms;
    pthread_mutex_lock(&d->m_mu);
    while (!stopping(d)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000000;
        if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
        (void)pthread_cond_timedwait(&d->m_cv, &d->m_mu, &ts);
        if (stopping(d)) break;
        pthread_mutex_unlock(&d->m_mu);
        const int64_t now = sys_now_ms();
        if (now >= next_boot) {
            if (!rt_count(d)) (void)bootstrap(d);                /* alone: the bootstrap list and peers.txt again */
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
            next_refresh = sys_now_ms() + (d->refresh_ms < 60000 ? d->refresh_ms : 60000);
        }
        if (now >= next_save) {
            save_peers(d);
            next_save = now + SAVE_MS;
        }
        const bool met = alone && rt_count(d);                 /* the first contacts after none: what went to nobody goes now */
        alone = !rt_count(d);
        if (now >= next_repub || met) {
            republish(d);
            next_repub = sys_now_ms() + d->republish_ms;
        }
        pthread_mutex_lock(&d->m_mu);
    }
    pthread_mutex_unlock(&d->m_mu);
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
    if (!d || !cfg->dir || !(d->dir = dup_str(cfg->dir)) || !(d->boot = calloc((size_t)cfg->nbootstrap + 1, sizeof *d->boot))) {
        snprintf(err, errlen, "out of memory");
        if (d) free(d->dir), free(d);
        return NULL;
    }
    d->cfg = *cfg;
    d->cfg.dir = d->dir;
    for (int i = 0; i < cfg->nbootstrap; i++)
        if (!(d->boot[i] = dup_str(cfg->bootstrap[i]))) {
            d->cfg.nbootstrap = i;
            break;
        }
    d->cfg.bootstrap = (const char *const *)d->boot;
    d->lfd = -1;
    d->refresh_ms = 1000LL * (cfg->refresh_s ? cfg->refresh_s : 3600);
    d->republish_ms = 1000LL * (cfg->republish_s ? cfg->republish_s : 86400);
    d->retry_ms = 1000LL * (cfg->retry_s ? cfg->retry_s : 60);
    d->relay_ms = 1000LL * (cfg->relay_s ? cfg->relay_s : 4 * 3600);
    d->slot_ms = 1000LL * (cfg->relay_slot_s ? cfg->relay_slot_s : 60);
    if (d->cfg.handlers <= 0) d->cfg.handlers = 8;
    if (d->cfg.relay_max <= 0) d->cfg.relay_max = 8;
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
    pthread_mutex_init(&d->m_mu, NULL);
    pthread_mutex_init(&d->f_mu, NULL);
    pthread_cond_init(&d->m_cv, NULL);
    const int64_t now = sys_now_ms();
    for (int i = 0; i < DHT_BUCKETS; i++) d->b[i].looked = now;
    net_cfg_init(&d->ncfg, cfg->ctx, cfg->self, cfg->net->fp, cfg->id_bits, 0, cfg->timeout_ms);
    d->ncfg0 = d->ncfg;
    d->t0 = now;
    d->probe_ms = 1000LL * (cfg->probe_s ? cfg->probe_s : 120);
    atomic_init(&d->in_ms, now - REACH_MS + d->probe_ms);
    if (!(d->lim = calloc(LIM_SLOTS, sizeof *d->lim)) || !(d->active = calloc((size_t)d->cfg.relay_max, sizeof *d->active))) {
        snprintf(err, errlen, "out of memory");
        dht_stop(d);
        return NULL;
    }
    if (!sys_mkdir(d->dir)) {
        snprintf(err, errlen, "%s: cannot make the data folder", d->dir);
        dht_stop(d);
        return NULL;
    }
    path_of(d, "store.bin", path, sizeof path);
    if (!(d->custody = store_open(path, cfg->ctx, cfg->net, err, errlen))) {
        dht_stop(d);
        return NULL;
    }
    store_limits(d->custody, d->cfg.store_max, d->cfg.store_max / OWNER_SHARE);
    path_of(d, "own.bin", path, sizeof path);
    if (!(d->own = store_open(path, cfg->ctx, cfg->net, err, errlen)) || !(d->mb = mbox_new(MBOX_CAP, MBOX_PER_OWNER))) {
        if (d->own) snprintf(err, errlen, "out of memory");
        dht_stop(d);
        return NULL;
    }
    (void)store_last_seq(d->own, d->me.xonly, &d->seq);
    const uint64_t saved = load_seq(d);
    if (saved > d->seq) d->seq = saved;
    return d;
}

int dht_start(dht *d)
{
    if (d->started) return -1;
    if (d->cfg.listen_port != 0) {
        const uint16_t want = d->cfg.listen_port == DHT_ANY_PORT ? 0 : (uint16_t)d->cfg.listen_port;
        if ((d->lfd = net_listen(d->cfg.listen_host, want, &d->port)) < 0) return -1;
        d->ncfg.port = d->me.addr.port = d->port;
    }
    d->started = true;
    if (!(d->th_handlers = calloc((size_t)d->cfg.handlers, sizeof *d->th_handlers))) return -1;
    if (d->lfd >= 0) {
        while (d->nhandlers < d->cfg.handlers && spawn(&d->th_handlers[d->nhandlers], handler_main, d, HANDLER_STACK, false)) d->nhandlers++;
        d->has_accept = d->nhandlers > 0 && spawn(&d->th_accept, accept_main, d, HANDLER_STACK, false);
    }
    d->has_jobs = spawn(&d->th_jobs, jobs_main, d, HANDLER_STACK, false);
    if ((d->lfd >= 0 && !d->has_accept) || !d->has_jobs) return -1;
    const int n = bootstrap(d);
    if (n) (void)dht_mbox_poll(d);                             /* the alerts that came while this node was away */
    d->has_maint = spawn(&d->th_maint, maint_main, d, HANDLER_STACK, false);
    if (d->cfg.nbt_routers || (d->cfg.portmap && d->port)) d->has_meet = spawn(&d->th_meet, meet_main, d, HANDLER_STACK, false);
    dlog(d, "started, %s, %d contacts", d->port ? "listening" : "not listening", n);
    return d->has_maint ? n : -1;
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
    pthread_mutex_lock(&d->m_mu);
    pthread_cond_broadcast(&d->m_cv);
    pthread_mutex_unlock(&d->m_mu);
    if (d->has_accept) pthread_join(d->th_accept, NULL);
    net_close_fd(d->lfd);                                      /* new connections are refused from here on */
    for (int i = 0; i < d->nhandlers; i++) pthread_join(d->th_handlers[i], NULL);
    for (; d->qn; d->qn--, d->qhead = (d->qhead + 1) % QUEUE_MAX) net_close_fd(d->q[d->qhead].fd);
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
    store_close(d->own);
    mbox_free(d->mb);
    for (int i = 0; d->boot && d->boot[i]; i++) free(d->boot[i]);
    free(d->boot);
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
    pthread_mutex_destroy(&d->m_mu);
    pthread_mutex_destroy(&d->f_mu);
    pthread_cond_destroy(&d->m_cv);
    free(d);
}


uint16_t dht_port(const dht *d) { return d->port; }

