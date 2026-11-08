/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.c - the node (see dht.h). Each shared structure has its own mutex, and no lock is held across a connection: the routing table, the
 * queue of accepted connections, the jobs, the alerts, the publishing. The stores and the mailbox lock themselves. */
#include "dht.h"
#include "store.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define NODE_LEN 82                /* a contact in NODES: node id, xonly, ip, port */
#define PAIR_LEN (2 * RECORD_LEN)
#define NODES_LEN (2 + DHT_K * NODE_LEN)
#define MBOX_LEN (2 + DHT_MBOX_MAX * PAIR_LEN)
#define ANS_MAX MBOX_LEN           /* the longest answer */
#define QUEUE_MAX 64               /* accepted connections waiting for a handler: more are closed at once */
#define JOBS_MAX 4096
#define FAILS_MAX 3                /* failed exchanges in a row that drop a contact */
#define SHORT_MAX (3 * DHT_K)      /* candidates of a lookup */
#define MBOX_CAP 4096              /* alert pairs a mailbox keeps */
#define SENT_MAX 256               /* pairs a custodian remembers having alerted */
#define ALERTS_MAX 1024            /* alerts of this node remembered */
#define RESEND_MS (3600 * 1000LL)  /* a custodian alerts the same pair at most once an hour (a republished record alerts again) */
#define ALERT_TTL_MS (24 * 3600 * 1000LL)
#define BOOT_TRY 64                /* addresses tried at a bootstrap, BOOT_BATCH at a time (so a list of dead addresses costs a few timeouts) */
#define BOOT_BATCH 16
#define BOOT_WANT 8                /* ... until this many answered */
#define BOOT_RETRY_MS 30000
#define SAVE_MS 300000
#define PEERS_MAX 1024
#define HANDLER_STACK (1 << 20)
#define QUERY_STACK (256 << 10)

typedef struct {
    dht_contact c;
    int fails;                     /* failed exchanges in a row */
    bool answered;                 /* it answered a request of this node (peers.txt keeps those) */
} rt_entry;

typedef struct {
    rt_entry e[DHT_K];             /* the least recently seen first */
    int n;
    bool pinging, has_cand, cand_answered;
    dht_contact cand;              /* a contact that waits for the ping of e[0] */
    int64_t looked;                /* the last lookup in the range of the bucket */
} bucket;

enum { JOB_ALERT_OUT, JOB_ALERT_OWN, JOB_PING };

typedef struct job {
    struct job *next;
    int kind;
    record a, b;
    dht_contact c;
} job;

enum { AL_WAIT, AL_DONE, AL_REFUSED };

typedef struct {
    uint8_t h[32];
    record a, b;
    int state;
    int64_t until, next;           /* a waiting alert is retried at next, until until */
} own_alert;

typedef struct {
    uint8_t h[32];
    int64_t at;
} sent_alert;

typedef struct {
    int fd;
    net_addr from;
} pending;

typedef struct {
    net_addr a;
    bool has_id;
    uint8_t xonly[32];
} boot_addr;

struct dht {
    dht_config cfg;
    net_cfg ncfg;
    dht_contact me;
    char *dir, **boot;
    store *custody, *own;
    mbox *mb;
    atomic_bool stop;
    int lfd;
    uint16_t port;
    int64_t refresh_ms, republish_ms, poll_ms, retry_ms;
    pthread_mutex_t rt_mu;         /* the routing table */
    bucket b[DHT_BUCKETS];
    pthread_mutex_t q_mu;          /* accepted connections */
    pthread_cond_t q_cv;
    pending q[QUEUE_MAX];
    int qhead, qn;
    pthread_mutex_t j_mu;          /* jobs */
    pthread_cond_t j_cv;
    job *jhead, *jtail;
    int jn;
    pthread_mutex_t a_mu;          /* alerts of this node, alerts sent as a custodian */
    own_alert *alerts;
    int nalerts, calerts;
    sent_alert sent[SENT_MAX];
    int nsent;
    pthread_mutex_t p_mu;          /* publishing: the sequence numbers and their order on the wire */
    uint64_t seq;
    pthread_mutex_t m_mu;          /* the sleep of the maintenance thread */
    pthread_cond_t m_cv;
    pthread_t th_accept, th_jobs, th_maint, *th_handlers;
    int nhandlers;
    bool has_accept, has_jobs, has_maint, started;
};

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static bool stopping(dht *d) { return atomic_load(&d->stop); }

static bool is_me(const dht *d, const uint8_t id[32]) { return !memcmp(id, d->me.id, 32); }

static bool mine(const dht *d, const record *r) { return !memcmp(r->owner, d->me.xonly, 32); }

static void dlog(const dht *d, const char *fmt, ...)
{
    if (!d->cfg.verbose) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "pzpeer %02x%02x%02x%02x: %s\n", d->me.id[2], d->me.id[3], d->me.id[4], d->me.id[5], line);
}

static bool spawn(pthread_t *th, void *(*fn)(void *), void *arg, size_t stack)
{
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return false;
    (void)pthread_attr_setstacksize(&at, stack);
    const bool ok = pthread_create(th, &at, fn, arg) == 0;
    pthread_attr_destroy(&at);
    return ok;
}

/* whether a is closer to t than b (XOR distance) */
static bool closer(const uint8_t t[32], const uint8_t a[32], const uint8_t b[32])
{
    for (int i = 0; i < 32; i++)
        if ((a[i] ^ t[i]) != (b[i] ^ t[i])) return (a[i] ^ t[i]) < (b[i] ^ t[i]);
    return false;
}

/* the bucket of an id: the number of leading bits it shares with this node's; DHT_BUCKETS for this node */
static int bucket_of(const uint8_t me[32], const uint8_t id[32])
{
    for (int i = 0; i < 32; i++) {
        uint8_t x = me[i] ^ id[i];
        if (!x) continue;
        int b = 0;
        for (; !(x & 0x80); b++) x = (uint8_t)(x << 1);
        return i * 8 + b;
    }
    return DHT_BUCKETS;
}

void dht_mbox_key(const uint8_t owner[32], uint8_t k[32])
{
    uint8_t m[11 + 32] = "pznet1/mbox";
    memcpy(m + 11, owner, 32);
    pz_sha256(m, sizeof m, k);
}

/* the owner of a record is a node: its node ID has the zero bits (one hash; the custodian alerts it by its ID) */
static bool owner_ok(const dht *d, const record *r)
{
    uint8_t id[32];
    keys_node_id(r->owner, id);
    return keys_id_ok(id, d->cfg.id_bits);
}

static bool pair_ok(const dht *d, const record *a, const record *b)
{
    return owner_ok(d, a) && owner_ok(d, b) && store_pair_ok(d->cfg.ctx, d->cfg.net, a, b);
}

/* SHA256("pznet1/pair" || the two records in the order of their owners): the name of a pair, whichever order it came in */
static void pair_hash(const record *a, const record *b, uint8_t h[32])
{
    uint8_t m[11 + PAIR_LEN] = "pznet1/pair";
    const bool swap = memcmp(a->owner, b->owner, 32) > 0;
    record_encode(swap ? b : a, m + 11);
    record_encode(swap ? a : b, m + 11 + RECORD_LEN);
    pz_sha256(m, sizeof m, h);
}

static void encode_contact(const dht_contact *c, uint8_t *p)
{
    memcpy(p, c->id, 32);
    memcpy(p + 32, c->xonly, 32);
    memcpy(p + 64, c->addr.ip, 16);
    p[80] = (uint8_t)(c->addr.port >> 8), p[81] = (uint8_t)c->addr.port;
}

/* NODES: every contact must be whole (its ID from its identity, with the zero bits, and a port), or the answer is refused */
static bool parse_nodes(const dht *d, const uint8_t *m, size_t len, dht_contact *out, int *n)
{
    if (len < 2 || m[0] != DHT_NODES || m[1] > DHT_K || len != 2 + (size_t)m[1] * NODE_LEN) return false;
    for (int i = 0; i < m[1]; i++) {
        const uint8_t *p = m + 2 + (size_t)i * NODE_LEN;
        dht_contact *c = &out[i];
        uint8_t id[32];
        memcpy(c->id, p, 32);
        memcpy(c->xonly, p + 32, 32);
        memcpy(c->addr.ip, p + 64, 16);
        c->addr.port = (uint16_t)(p[80] << 8 | p[81]);
        keys_node_id(c->xonly, id);
        if (memcmp(id, c->id, 32) || !keys_id_ok(id, d->cfg.id_bits) || c->addr.port == 0) return false;
    }
    *n = m[1];
    return true;
}

/* ---- the routing table ---- */

static bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c);

static int rt_index(const bucket *b, const uint8_t id[32])
{
    for (int i = 0; i < b->n; i++)
        if (!memcmp(b->e[i].c.id, id, 32)) return i;
    return -1;
}

static void rt_drop(bucket *b, int at)
{
    memmove(&b->e[at], &b->e[at + 1], (size_t)(b->n - at - 1) * sizeof *b->e);
    b->n--;
}

static void rt_push(bucket *b, const dht_contact *c, bool answered)
{
    rt_entry *e = &b->e[b->n++];
    e->c = *c;
    e->fails = 0;
    e->answered = answered;
}

/* A contact heard from goes to the tail of its bucket. A full bucket drops a contact that failed, if it has one; otherwise its oldest
 * contact is pinged (a job) and the new one waits: Kademlia keeps the contacts that have lived longest, which an attacker cannot
 * replace by flooding the node with new ones. */
static void rt_seen(dht *d, const dht_contact *c, bool answered)
{
    if (is_me(d, c->id) || c->addr.port == 0) return;
    dht_contact lrs;
    bool ping = false;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, c->id)];
    const int at = rt_index(b, c->id);
    if (at >= 0) {
        rt_entry e = b->e[at];
        rt_drop(b, at);
        e.c.addr = c->addr;
        e.fails = 0;
        e.answered = e.answered || answered;
        b->e[b->n++] = e;
    } else if (b->n < DHT_K) {
        rt_push(b, c, answered);
    } else {
        int worst = -1;
        for (int i = 0; i < b->n; i++)
            if (b->e[i].fails && (worst < 0 || b->e[i].fails > b->e[worst].fails)) worst = i;
        if (worst >= 0) {
            rt_drop(b, worst);
            rt_push(b, c, answered);
        } else {
            b->cand = *c;
            b->cand_answered = answered;
            b->has_cand = true;
            if (!b->pinging) {
                b->pinging = ping = true;
                lrs = b->e[0].c;
            }
        }
    }
    pthread_mutex_unlock(&d->rt_mu);
    if (ping && !job_push(d, JOB_PING, NULL, NULL, &lrs)) {
        pthread_mutex_lock(&d->rt_mu);
        b->pinging = false;
        pthread_mutex_unlock(&d->rt_mu);
    }
}

static void rt_failed(dht *d, const uint8_t id[32])
{
    if (is_me(d, id)) return;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, id)];
    const int at = rt_index(b, id);
    if (at >= 0 && ++b->e[at].fails >= FAILS_MAX) {
        rt_drop(b, at);
        if (b->has_cand && !b->pinging) {
            rt_push(b, &b->cand, b->cand_answered);
            b->has_cand = false;
        }
    }
    pthread_mutex_unlock(&d->rt_mu);
}

/* the max contacts of the table closest to t, nearest first */
static int rt_closest(dht *d, const uint8_t t[32], dht_contact *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n; j++) {
            const dht_contact *c = &d->b[i].e[j].c;
            int at = n;
            while (at > 0 && closer(t, c->id, out[at - 1].id)) at--;
            if (at >= max) continue;
            if (n == max) n--;                                 /* the farthest goes */
            memmove(&out[at + 1], &out[at], (size_t)(n - at) * sizeof *out);
            out[at] = *c;
            n++;
        }
    pthread_mutex_unlock(&d->rt_mu);
    return n;
}

static int rt_count(dht *d)
{
    int n = 0;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++) n += d->b[i].n;
    pthread_mutex_unlock(&d->rt_mu);
    return n;
}

static bool rt_get(dht *d, const uint8_t id[32], dht_contact *out)
{
    if (is_me(d, id)) return false;
    pthread_mutex_lock(&d->rt_mu);
    const bucket *b = &d->b[bucket_of(d->me.id, id)];
    const int at = rt_index(b, id);
    if (at >= 0) *out = b->e[at].c;
    pthread_mutex_unlock(&d->rt_mu);
    return at >= 0;
}

/* ---- exchanges ---- */

/* One request to a contact, dialled with its identity, and its answer, whose type must be the request's + 1. */
static bool exchange(dht *d, const dht_contact *c, const uint8_t *req, size_t rlen, uint8_t *ans, size_t cap, size_t *alen)
{
    net_conn *k = net_dial(&d->ncfg, &c->addr, c->xonly);
    const bool ok = k && net_send(k, req, rlen) && net_recv(k, ans, cap, alen) && *alen >= 1 && ans[0] == req[0] + 1;
    net_close(k);
    return ok;
}

/* what the routing table learns from an exchange whose answer was checked */
static void heard(dht *d, const dht_contact *c, bool ok)
{
    if (ok) rt_seen(d, c, true);
    else rt_failed(d, c->id);
}

static void job_ping(dht *d, const dht_contact *lrs)
{
    const uint8_t req = DHT_PING;
    uint8_t ans[1];
    size_t n;
    const bool alive = exchange(d, lrs, &req, 1, ans, sizeof ans, &n) && n == 1;
    heard(d, lrs, alive);                                      /* alive: to the tail of its bucket */
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, lrs->id)];
    b->pinging = false;
    if (!alive) {
        const int at = rt_index(b, lrs->id);
        if (at >= 0) rt_drop(b, at);
        if (b->has_cand && b->n < DHT_K && rt_index(b, b->cand.id) < 0) rt_push(b, &b->cand, b->cand_answered);
    }
    b->has_cand = false;
    pthread_mutex_unlock(&d->rt_mu);
    dlog(d, alive ? "the oldest contact of a full bucket answered: kept" : "the oldest contact of a full bucket did not answer: replaced");
}

/* ---- lookups ---- */

enum { C_NEW, C_OK, C_FAIL };

typedef struct {
    dht_contact c;
    int state;
} cand;

typedef struct {
    dht *d;
    dht_contact c;
    const uint8_t *target;
    dht_contact got[DHT_K];
    int ngot;
    bool ok;
} query;

/* into the candidates, sorted by distance to t, at most SHORT_MAX (the farthest goes) */
static void cand_add(cand *s, int *n, const uint8_t t[32], const dht_contact *c, int state)
{
    for (int i = 0; i < *n; i++)
        if (!memcmp(s[i].c.id, c->id, 32)) return;
    int at = *n;
    while (at > 0 && closer(t, c->id, s[at - 1].c.id)) at--;
    if (at >= SHORT_MAX) return;
    const int keep = *n < SHORT_MAX ? *n : SHORT_MAX - 1;
    memmove(&s[at + 1], &s[at], (size_t)(keep - at) * sizeof *s);
    s[at].c = *c;
    s[at].state = state;
    *n = keep + 1;
}

static void *query_main(void *arg)
{
    query *q = arg;
    uint8_t req[33], ans[NODES_LEN];
    size_t len;
    req[0] = DHT_FIND_NODE;
    memcpy(req + 1, q->target, 32);
    q->ok = exchange(q->d, &q->c, req, sizeof req, ans, sizeof ans, &len) && parse_nodes(q->d, ans, len, q->got, &q->ngot);
    heard(q->d, &q->c, q->ok);
    return NULL;
}

/* the queries of a round at the same time: the first on this thread, the others on threads of their own */
static void run_queries(query *q, int n)
{
    pthread_t th[DHT_ALPHA];
    bool started[DHT_ALPHA] = {false};
    for (int i = 1; i < n; i++) started[i] = spawn(&th[i], query_main, &q[i], QUERY_STACK);
    query_main(&q[0]);
    for (int i = 1; i < n; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else query_main(&q[i]);
    }
}

int dht_lookup(dht *d, const uint8_t target[32], dht_contact *out, int want)
{
    if (want > DHT_K) want = DHT_K;
    cand s[SHORT_MAX];
    dht_contact seed[DHT_K];
    int n = 0;
    const int ns = rt_closest(d, target, seed, DHT_K);
    for (int i = 0; i < ns; i++) cand_add(s, &n, target, &seed[i], C_NEW);
    if (d->port) cand_add(s, &n, target, &d->me, C_OK);        /* a node that listens is a node of the table like the others */
    const int bk = bucket_of(d->me.id, target);
    if (bk < DHT_BUCKETS) {
        pthread_mutex_lock(&d->rt_mu);
        d->b[bk].looked = now_ms();
        pthread_mutex_unlock(&d->rt_mu);
    }
    query q[DHT_ALPHA];
    /* Rounds of up to alpha queries to the closest candidates not asked yet, among the want closest that did not fail; the lookup ends
     * when those want have all answered. */
    while (!stopping(d)) {
        int nq = 0, live = 0;
        for (int i = 0; i < n && live < want && nq < DHT_ALPHA; i++) {
            if (s[i].state == C_FAIL) continue;
            live++;
            if (s[i].state != C_NEW) continue;
            q[nq].d = d, q[nq].c = s[i].c, q[nq].target = target, q[nq].ngot = 0, q[nq].ok = false;
            nq++;
        }
        if (!nq) break;
        run_queries(q, nq);
        for (int j = 0; j < nq; j++)
            for (int i = 0; i < n; i++)
                if (!memcmp(s[i].c.id, q[j].c.id, 32)) s[i].state = q[j].ok ? C_OK : C_FAIL;
        for (int j = 0; j < nq; j++)
            for (int g = 0; g < q[j].ngot; g++)
                if (!is_me(d, q[j].got[g].id)) cand_add(s, &n, target, &q[j].got[g], C_NEW);
    }
    int m = 0;
    for (int i = 0; i < n && m < want; i++)
        if (s[i].state == C_OK) out[m++] = s[i].c;
    return m;
}

bool dht_find(dht *d, const uint8_t xonly[32], dht_contact *out)
{
    uint8_t id[32];
    dht_contact cs[DHT_K];
    keys_node_id(xonly, id);
    if (rt_get(d, id, out)) return true;
    const int n = dht_lookup(d, id, cs, DHT_K);
    for (int i = 0; i < n; i++)
        if (!memcmp(cs[i].id, id, 32) && !is_me(d, id)) {
            *out = cs[i];
            return true;
        }
    return false;
}

net_conn *dht_dial(dht *d, const dht_contact *c) { return net_dial(&d->ncfg, &c->addr, c->xonly); }

/* ---- custody and alerts ---- */

/* whether a custodian may alert this pair now: not within RESEND_MS of the last time */
static bool sent_check(dht *d, const uint8_t h[32])
{
    const int64_t now = now_ms();
    bool ok = true;
    int slot = -1, oldest = 0;
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nsent && slot < 0; i++) {
        if (!memcmp(d->sent[i].h, h, 32)) slot = i, ok = now - d->sent[i].at >= RESEND_MS;
        else if (d->sent[i].at < d->sent[oldest].at) oldest = i;
    }
    if (slot < 0) slot = d->nsent < SENT_MAX ? d->nsent++ : oldest;
    if (ok) {
        memcpy(d->sent[slot].h, h, 32);
        d->sent[slot].at = now;
    }
    pthread_mutex_unlock(&d->a_mu);
    return ok;
}

/* A record for the custody of this node: its status for STORED. Each coincidence becomes a job that alerts the owners and their
 * mailboxes, also for a duplicate (a republished record alerts again, at most once an hour: an alert lost on the way comes back). */
static int custody_put(dht *d, const record *r)
{
    record held, others[8];
    int no;
    if (!owner_ok(d, r)) return DHT_ST_INVALID;
    const store_status s = store_put(d->custody, r, &held, others, 8, &no);
    for (int i = 0; i < no; i++) {
        uint8_t h[32];
        pair_hash(&held, &others[i], h);
        if (sent_check(d, h) && job_push(d, JOB_ALERT_OUT, &held, &others[i], NULL))
            dlog(d, "coincidence (%s): alerting the owners", held.t == others[i].t ? "merge" : "useful");
    }
    switch (s) {
    case STORE_OK: return no ? DHT_ST_COINCIDENCE : DHT_ST_STORED;
    case STORE_DUP: return DHT_ST_DUP;
    case STORE_INVALID: return DHT_ST_INVALID;
    case STORE_OLD: return DHT_ST_OLD;
    default: return DHT_ST_FAILED;
    }
}

static int alert_state(dht *d, const uint8_t h[32])
{
    int st = -1;
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts && st < 0; i++)
        if (!memcmp(d->alerts[i].h, h, 32)) st = d->alerts[i].state;
    pthread_mutex_unlock(&d->a_mu);
    return st;
}

static void alert_set(dht *d, const uint8_t h[32], int state, int64_t next)
{
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts; i++)
        if (!memcmp(d->alerts[i].h, h, 32)) d->alerts[i].state = state, d->alerts[i].next = next;
    pthread_mutex_unlock(&d->a_mu);
}

/* a new alert of this node; false when the pair is known (it came from another custodian, or from a mailbox) */
static bool own_alert_add(dht *d, const record *a, const record *b)
{
    uint8_t h[32];
    pair_hash(a, b, h);
    const int64_t now = now_ms();
    bool fresh = true;
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts && fresh; i++) fresh = memcmp(d->alerts[i].h, h, 32) != 0;
    if (fresh && d->nalerts == ALERTS_MAX) {                  /* full: the oldest that is not waiting goes */
        int at = 0;
        while (at < d->nalerts && d->alerts[at].state == AL_WAIT) at++;
        if (at < d->nalerts) memmove(&d->alerts[at], &d->alerts[at + 1], (size_t)(d->nalerts - at - 1) * sizeof *d->alerts), d->nalerts--;
        else fresh = false;
    }
    if (fresh && d->nalerts == d->calerts) {
        const int nc = d->calerts ? 2 * d->calerts : 16;
        own_alert *g = realloc(d->alerts, (size_t)nc * sizeof *g);
        if (g) d->alerts = g, d->calerts = nc;
        else fresh = false;
    }
    if (fresh) {
        own_alert *e = &d->alerts[d->nalerts++];
        memcpy(e->h, h, 32);
        e->a = *a, e->b = *b;
        e->state = AL_WAIT;
        e->until = now + ALERT_TTL_MS;
        e->next = now + d->retry_ms;
    }
    pthread_mutex_unlock(&d->a_mu);
    return fresh;
}

/* A checked pair that came to this node: kept as a mailbox, and an alert of this node when one record is its own. True for a new alert of
 * this node. */
static bool alert_in(dht *d, const record *a, const record *b)
{
    if (mbox_put(d->mb, a, b)) dlog(d, "an alert pair in the mailbox");
    if (!(mine(d, a) || mine(d, b)) || !own_alert_add(d, a, b)) return false;
    dlog(d, "an alert for this node: checking the proofs");
    (void)job_push(d, JOB_ALERT_OWN, a, b, NULL);             /* if the queue is full, the maintenance thread retries it */
    return true;
}

static void send_alert(dht *d, const dht_contact *c, const record *a, const record *b)
{
    if (is_me(d, c->id)) {
        (void)alert_in(d, a, b);
        return;
    }
    uint8_t req[1 + PAIR_LEN], ans[1];
    size_t n;
    req[0] = DHT_ALERT;
    record_encode(a, req + 1);
    record_encode(b, req + 1 + RECORD_LEN);
    heard(d, c, exchange(d, c, req, sizeof req, ans, sizeof ans, &n) && n == 1);
}

/* the job of a custodian: ALERT to both owners (by their node IDs) and to the mailboxes of each */
static void job_alert_out(dht *d, const record *a, const record *b)
{
    for (int w = 0; w < 2 && !stopping(d); w++) {
        const record *r = w ? b : a;
        dht_contact c, mb[DHT_KSTORE];
        uint8_t k[32];
        if (mine(d, r)) (void)alert_in(d, a, b);
        else if (dht_find(d, r->owner, &c)) send_alert(d, &c, a, b);
        dht_mbox_key(r->owner, k);
        const int m = dht_lookup(d, k, mb, DHT_KSTORE);
        for (int i = 0; i < m && !stopping(d); i++) send_alert(d, &mb[i], a, b);
    }
}

/* this node knows the distance of its own record: the proof it would give, made and checked */
static bool own_proof(dht *d, const record *r)
{
    uint8_t dd[32], nonce[32], sig[64];
    const bool ok = d->cfg.own_distance && net_random(nonce, sizeof nonce) && d->cfg.own_distance(d->cfg.user, r->x, r->t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, r, dd, d->me.xonly, nonce, sig) &&
                    record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, nonce, sig);
    pz_secure_zero(dd, sizeof dd);
    return ok;
}

int dht_proof_on(dht *d, net_conn *c, const record *r)
{
    if (memcmp(c->remote, r->owner, 32)) return 0;
    uint8_t req[1 + RECORD_LEN + 32], ans[1 + 64];
    size_t n;
    req[0] = DHT_POSSESS;
    record_encode(r, req + 1);
    if (!net_random(req + 1 + RECORD_LEN, 32) || !net_send(c, req, sizeof req) || !net_recv(c, ans, sizeof ans, &n)) return -1;
    return n == sizeof ans && ans[0] == DHT_PROOF && record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, req + 1 + RECORD_LEN, ans + 1);
}

int dht_ask_proof(dht *d, const record *r)
{
    dht_contact c;
    if (mine(d, r)) return own_proof(d, r) ? 1 : 0;
    if (!dht_find(d, r->owner, &c)) return -1;
    net_conn *k = dht_dial(d, &c);
    if (!k) {
        rt_failed(d, c.id);
        return -1;
    }
    const int st = dht_proof_on(d, k, r);
    net_close(k);
    if (st >= 0) rt_seen(d, &c, true);
    return st;
}

/* The job of an alert of this node: its own proof, then the other owner's; on_alert when both check out. The record of this node is
 * signed by it, so when its distance is missing the daemon has not read that point yet (dp.bin at a start): the alert waits, as it does
 * for an owner that does not answer. Only a wrong proof of the other owner refuses it. */
static void job_alert_own(dht *d, const record *a, const record *b)
{
    uint8_t h[32];
    pair_hash(a, b, h);
    if (alert_state(d, h) != AL_WAIT) return;                 /* done already (the job came twice) */
    const record *me_r = mine(d, a) ? a : b, *them = me_r == a ? b : a;
    const int p = own_proof(d, me_r) ? dht_ask_proof(d, them) : -2;
    const int st = p == 1 ? AL_DONE : p == 0 ? AL_REFUSED : AL_WAIT;
    alert_set(d, h, st, now_ms() + d->retry_ms);
    dlog(d, p == 1    ? "alert: both proofs check out"
            : p == 0  ? "alert refused: the other owner's proof does not check out"
            : p == -1 ? "alert: the other owner did not answer, retried later"
                      : "alert: this node does not know the distance of its point (yet), retried later");
    if (st == AL_DONE && d->cfg.on_alert) d->cfg.on_alert(d->cfg.user, me_r, them, me_r->t == them->t);
}

static void retry_alerts(dht *d)
{
    record pairs[16][2];
    int n = 0;
    const int64_t now = now_ms();
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts; i++) {
        own_alert *e = &d->alerts[i];
        if (e->state != AL_WAIT || now < e->next) continue;
        if (now >= e->until) e->state = AL_REFUSED;
        else if (n < 16) e->next = now + d->retry_ms, pairs[n][0] = e->a, pairs[n][1] = e->b, n++;
    }
    pthread_mutex_unlock(&d->a_mu);
    for (int i = 0; i < n; i++) (void)job_push(d, JOB_ALERT_OWN, &pairs[i][0], &pairs[i][1], NULL);
}

int dht_mbox_poll(dht *d)
{
    uint8_t k[32], req[33];
    dht_contact mb[DHT_KSTORE];
    dht_mbox_key(d->me.xonly, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    uint8_t *ans = malloc(MBOX_LEN);
    int fresh = 0;
    req[0] = DHT_MBOX_GET;
    memcpy(req + 1, d->me.xonly, 32);
    for (int i = 0; i < m && ans && !stopping(d); i++) {
        size_t len;
        if (is_me(d, mb[i].id)) continue;                      /* what came here was handled when it came */
        const bool ok = exchange(d, &mb[i], req, sizeof req, ans, MBOX_LEN, &len) && len >= 2 && ans[1] <= DHT_MBOX_MAX &&
                        len == 2 + (size_t)ans[1] * PAIR_LEN;
        heard(d, &mb[i], ok);
        for (int p = 0; ok && p < ans[1]; p++) {
            record a, b;
            uint8_t h[32];
            const uint8_t *q = ans + 2 + (size_t)p * PAIR_LEN;
            if (!record_decode(q, &a) || !record_decode(q + RECORD_LEN, &b) || !(mine(d, &a) || mine(d, &b))) continue;
            pair_hash(&a, &b, h);
            if (alert_state(d, h) < 0 && pair_ok(d, &a, &b)) fresh += alert_in(d, &a, &b);   /* the signatures only of a new pair */
        }
    }
    free(ans);
    if (fresh) dlog(d, "%d alerts from the mailboxes", fresh);
    return fresh;
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
        dht_contact cs[DHT_K];
        if (len != 33) return 0;
        const int n = rt_closest(d, m + 1, cs, DHT_K);
        ans[0] = DHT_NODES, ans[1] = (uint8_t)n;
        for (int i = 0; i < n; i++) encode_contact(&cs[i], ans + 2 + (size_t)i * NODE_LEN);
        return 2 + (size_t)n * NODE_LEN;
    }
    case DHT_STORE:
        if (len < 2 || m[1] == 0 || len != 2 + (size_t)m[1] * RECORD_LEN) return 0;
        ans[0] = DHT_STORED, ans[1] = m[1];
        for (int i = 0; i < m[1]; i++)
            ans[2 + i] = (uint8_t)(record_decode(m + 2 + (size_t)i * RECORD_LEN, &r) ? custody_put(d, &r) : DHT_ST_INVALID);
        return 2 + (size_t)m[1];
    case DHT_ALERT:
        if (len != 1 + PAIR_LEN || !record_decode(m + 1, &r) || !record_decode(m + 1 + RECORD_LEN, &r2) || !pair_ok(d, &r, &r2)) return 0;
        (void)alert_in(d, &r, &r2);
        ans[0] = DHT_OK;
        return 1;
    case DHT_MBOX_GET: {
        record pairs[DHT_MBOX_MAX][2];
        if (len != 33) return 0;
        const int n = mbox_get(d->mb, m + 1, pairs, DHT_MBOX_MAX);
        ans[0] = DHT_MBOX, ans[1] = (uint8_t)n;
        for (int i = 0; i < n; i++) {
            record_encode(&pairs[i][0], ans + 2 + (size_t)i * PAIR_LEN);
            record_encode(&pairs[i][1], ans + 2 + (size_t)i * PAIR_LEN + RECORD_LEN);
        }
        return 2 + (size_t)n * PAIR_LEN;
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
    const size_t n = ans && len ? answer(d, c, msg, len, ans) : 0;
    const bool ok = n && net_send(c, ans, n);
    free(ans);
    return ok;
}

static void serve(dht *d, int fd, const net_addr *from, uint8_t *req, uint8_t *ans)
{
    net_conn *c = net_handshake(&d->ncfg, fd, from);
    size_t len;
    if (!c) return;
    if (!net_recv(c, req, NOISE_FRAME_MAX, &len) || len == 0) {
        net_close(c);
        return;
    }
    if (req[0] >= DHT_SESSION_FIRST && req[0] <= DHT_SESSION_LAST) {
        if (d->cfg.on_session) d->cfg.on_session(d->cfg.user, c, req, len);   /* the daemon owns the connection now */
        else net_close(c);
        return;
    }
    const size_t n = answer(d, c, req, len, ans);
    if (n && net_send(c, ans, n)) {
        dht_contact ct;                                        /* a node that asks is alive (one that does not listen is not taken) */
        memcpy(ct.id, c->remote_id, 32);
        memcpy(ct.xonly, c->remote, 32);
        ct.addr = c->addr;
        rt_seen(d, &ct, false);
    }
    net_close(c);
}

/* ---- publishing ---- */

typedef struct {
    dht_contact c;
    int *idx, n, cap;
} dest;

/* each record to the DHT_KSTORE nodes closest to its K: one lookup per record, then one STORE per custodian with all its records, in the
 * order they came (the order of their sequence numbers); the records that at least one custodian holds */
static int send_records(dht *d, const record *recs, int n)
{
    dest *ds = NULL;
    int nd = 0, cd = 0, done = 0;
    int *held = calloc((size_t)(n ? n : 1), sizeof *held);
    uint8_t *req = malloc(2 + DHT_STORE_MAX * RECORD_LEN);
    bool ok = held && req;
    for (int i = 0; i < n && ok && !stopping(d); i++) {
        uint8_t k[32];
        dht_contact cs[DHT_KSTORE];
        store_key(recs[i].x, k);
        const int m = dht_lookup(d, k, cs, DHT_KSTORE);
        for (int j = 0; j < m && ok; j++) {
            int at = 0;
            while (at < nd && memcmp(ds[at].c.id, cs[j].id, 32)) at++;
            if (at == nd && nd == cd) {
                const int nc = cd ? 2 * cd : 16;
                dest *g = realloc(ds, (size_t)nc * sizeof *g);
                if (!(ok = g != NULL)) break;
                ds = g, cd = nc;
            }
            if (at == nd) ds[nd++] = (dest){cs[j], NULL, 0, 0};
            dest *e = &ds[at];
            if (e->n == e->cap) {
                const int nc = e->cap ? 2 * e->cap : 16;
                int *g = realloc(e->idx, (size_t)nc * sizeof *g);
                if (!(ok = g != NULL)) break;
                e->idx = g, e->cap = nc;
            }
            e->idx[e->n++] = i;
        }
    }
    for (int a = 0; a < nd && ok && !stopping(d); a++) {
        for (int o = 0; o < ds[a].n; o += DHT_STORE_MAX) {
            const int m = ds[a].n - o < DHT_STORE_MAX ? ds[a].n - o : DHT_STORE_MAX;
            uint8_t st[DHT_STORE_MAX], ans[2 + DHT_STORE_MAX];
            if (is_me(d, ds[a].c.id)) {
                for (int j = 0; j < m; j++) st[j] = (uint8_t)custody_put(d, &recs[ds[a].idx[o + j]]);
            } else {
                size_t len;
                req[0] = DHT_STORE, req[1] = (uint8_t)m;
                for (int j = 0; j < m; j++) record_encode(&recs[ds[a].idx[o + j]], req + 2 + (size_t)j * RECORD_LEN);
                const bool got = exchange(d, &ds[a].c, req, 2 + (size_t)m * RECORD_LEN, ans, sizeof ans, &len) && len == 2 + (size_t)m && ans[1] == m;
                heard(d, &ds[a].c, got);
                if (!got) break;                               /* this custodian is gone: its other records too */
                memcpy(st, ans + 2, (size_t)m);
            }
            for (int j = 0; j < m; j++)
                if (st[j] == DHT_ST_STORED || st[j] == DHT_ST_DUP || st[j] == DHT_ST_COINCIDENCE) held[ds[a].idx[o + j]]++;
        }
    }
    for (int i = 0; ok && i < n; i++) done += held[i] > 0;
    for (int a = 0; a < nd; a++) free(ds[a].idx);
    free(ds);
    free(held);
    free(req);
    return done;
}

/* the next sequence number of this owner: microseconds of the clock, and always above the last (p_mu held) */
static bool sign_locked(dht *d, record *r)
{
    const uint64_t now = wall_us();
    d->seq = now > d->seq ? now : d->seq + 1;
    r->seq = d->seq;
    memcpy(r->owner, d->me.xonly, 32);
    return record_sign(d->cfg.ctx, d->cfg.net, &d->cfg.self->kp, r);
}

int dht_publish(dht *d, record *recs, int n)
{
    pthread_mutex_lock(&d->p_mu);
    for (int i = 0; i < n; i++) {
        record held;
        int no;
        if (!sign_locked(d, &recs[i])) continue;              /* never in practice; the custodians would refuse it */
        if (store_put(d->own, &recs[i], &held, NULL, 0, &no) == STORE_DUP && held.t != recs[i].t)
            dlog(d, "this node has the same x with two types");   /* a coincidence of the home with itself: its pzsolve sees it */
    }
    const int done = send_records(d, recs, n);
    pthread_mutex_unlock(&d->p_mu);
    return done;
}

/* The records of own.bin again, signed with new sequence numbers: a custodian that joined since the last time takes them (it would refuse
 * the old numbers, which are below the last it saw of this owner); the ones that hold them already answer DUP. */
static void republish(dht *d)
{
    record *batch = malloc(DHT_STORE_MAX * sizeof *batch);
    const uint64_t total = store_count(d->own);
    uint64_t i = 0;
    while (batch && i < total && !stopping(d)) {
        int m = 0;
        pthread_mutex_lock(&d->p_mu);
        for (; i < total && m < DHT_STORE_MAX; i++)
            if (store_read(d->own, i, &batch[m]) && sign_locked(d, &batch[m])) m++;
        (void)send_records(d, batch, m);
        pthread_mutex_unlock(&d->p_mu);
    }
    free(batch);
    dlog(d, "republished %llu records", (unsigned long long)total);
}

/* ---- bootstrap, refresh, peers.txt ---- */

static void hex32(const uint8_t b[32], char out[65])
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = H[b[i] >> 4], out[2 * i + 1] = H[b[i] & 15];
    out[64] = 0;
}

static bool unhex32(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 64; i++) {
        const char c = s[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = (uint8_t)(i % 2 ? out[i / 2] | v : v << 4);
    }
    return s[64] == 0 || s[64] == '\n' || s[64] == '\r' || s[64] == ' ';
}

static void path_of(const dht *d, const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/%s", d->dir, name); }

/* peers.txt: the contacts that answered, one per line, "address identity" */
static void save_peers(dht *d)
{
    dht_contact *cs = malloc(PEERS_MAX * sizeof *cs);
    char path[4096], tmp[4100];
    int n = 0;
    if (!cs) return;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n && n < PEERS_MAX; j++)
            if (d->b[i].e[j].answered) cs[n++] = d->b[i].e[j].c;
    pthread_mutex_unlock(&d->rt_mu);
    path_of(d, "peers.txt", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = n ? fopen(tmp, "w") : NULL;                      /* no contact: the old list is kept for the next start */
    if (f) {
        for (int i = 0; i < n; i++) {
            char a[64], h[65];
            net_addr_str(&cs[i].addr, a, sizeof a);
            hex32(cs[i].xonly, h);
            fprintf(f, "%s %s\n", a, h);
        }
        if (fclose(f) == 0) (void)rename(tmp, path);
        else (void)remove(tmp);
    }
    free(cs);
}

static int boot_list(dht *d, boot_addr *out, int max)
{
    int n = 0;
    for (int i = 0; i < d->cfg.nbootstrap && n < max; i++) {
        if (net_parse_addr(d->boot[i], &out[n].a)) out[n++].has_id = false;
        else dlog(d, "bootstrap: cannot resolve %s", d->boot[i]);
    }
    char path[4096], line[256];
    path_of(d, "peers.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && n < max && fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (unhex32(sp + 1, out[n].xonly) && net_parse_addr(line, &out[n].a)) out[n++].has_id = true;
    }
    if (f) fclose(f);
    return n;
}

typedef struct {
    dht *d;
    const boot_addr *b;
    dht_contact c;
    bool ok;
} boot_ping;

/* a PING to an address, which must be a node of this network (and the expected identity, for a line of peers.txt) */
static void *boot_ping_main(void *arg)
{
    boot_ping *p = arg;
    const uint8_t req = DHT_PING;
    uint8_t ans[1], remote[32];
    size_t len;
    p->ok = net_exchange(&p->d->ncfg, &p->b->a, p->b->has_id ? p->b->xonly : NULL, &req, 1, ans, sizeof ans, &len, remote) && len == 1 &&
            ans[0] == DHT_PONG;
    if (p->ok) {
        memcpy(p->c.xonly, remote, 32);
        keys_node_id(remote, p->c.id);
        p->c.addr = p->b->a;
    }
    return NULL;
}

/* the first contacts, then a lookup of the own ID */
static int bootstrap(dht *d)
{
    boot_addr *list = malloc(BOOT_TRY * sizeof *list);
    const int n = list ? boot_list(d, list, BOOT_TRY) : 0;
    int ok = 0;
    for (int i = 0; i < n && ok < BOOT_WANT && !stopping(d); i += BOOT_BATCH) {
        boot_ping p[BOOT_BATCH];
        pthread_t th[BOOT_BATCH];
        bool started[BOOT_BATCH];
        const int m = n - i < BOOT_BATCH ? n - i : BOOT_BATCH;
        for (int j = 0; j < m; j++) {
            p[j].d = d, p[j].b = &list[i + j], p[j].ok = false;
            if (!(started[j] = spawn(&th[j], boot_ping_main, &p[j], QUERY_STACK))) boot_ping_main(&p[j]);
        }
        for (int j = 0; j < m; j++) {
            if (started[j]) pthread_join(th[j], NULL);
            if (p[j].ok && !is_me(d, p[j].c.id)) {             /* not its own address */
                rt_seen(d, &p[j].c, true);
                ok++;
            }
        }
    }
    free(list);
    if (ok) {
        dht_contact out[DHT_K];
        (void)dht_lookup(d, d->me.id, out, DHT_K);
        save_peers(d);
    }
    const int have = rt_count(d);
    dlog(d, "bootstrap: %d of %d addresses answered, %d contacts", ok, n, have);
    return have;
}

/* a lookup of a random ID in the range of each bucket from the first a node ID can have to one below the deepest that has contacts */
static void refresh(dht *d, bool all)
{
    int64_t looked[DHT_BUCKETS];
    int deepest = -1;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++) {
        if (d->b[i].n) deepest = i;
        looked[i] = d->b[i].looked;
    }
    pthread_mutex_unlock(&d->rt_mu);
    const int64_t now = now_ms();
    const int last = deepest + 1 < DHT_BUCKETS ? deepest + 1 : DHT_BUCKETS - 1;
    for (int i = (int)d->cfg.id_bits; deepest >= 0 && i <= last && !stopping(d); i++) {
        uint8_t t[32];
        dht_contact out[DHT_K];
        if (!all && now - looked[i] < d->refresh_ms) continue;
        if (!net_random(t, sizeof t)) return;
        for (int b = 0; b <= i; b++) {                         /* the first i bits of this node, then bit i flipped */
            const uint8_t mask = (uint8_t)(0x80 >> (b % 8)), bit = (uint8_t)((d->me.id[b / 8] & mask) ^ (b == i ? mask : 0));
            t[b / 8] = (uint8_t)((t[b / 8] & ~mask) | bit);
        }
        (void)dht_lookup(d, t, out, DHT_K);
    }
}

void dht_refresh(dht *d) { refresh(d, true); }

/* ---- threads ---- */

static bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c)
{
    job *j = calloc(1, sizeof *j);
    if (!j) return false;
    j->kind = kind;
    if (a) j->a = *a;
    if (b) j->b = *b;
    if (c) j->c = *c;
    pthread_mutex_lock(&d->j_mu);
    const bool ok = d->jn < JOBS_MAX && !stopping(d);
    if (ok) {
        if (d->jtail) d->jtail->next = j;
        else d->jhead = j;
        d->jtail = j;
        d->jn++;
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
            if (!d->jhead) d->jtail = NULL;
            d->jn--;
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
        if (fd == -2) {
            const struct timespec ts = {0, 50 * 1000000};       /* out of descriptors: let the handlers close some */
            nanosleep(&ts, NULL);
        }
        if (fd < 0) continue;
        pthread_mutex_lock(&d->q_mu);
        const bool room = d->qn < QUEUE_MAX && !stopping(d);
        if (room) {
            d->q[(d->qhead + d->qn) % QUEUE_MAX] = (pending){fd, from};
            d->qn++;
            pthread_cond_signal(&d->q_cv);
        }
        pthread_mutex_unlock(&d->q_mu);
        if (!room) net_close_fd(fd);                           /* busy: the dialler tries another node */
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
    }
    free(req);
    free(ans);
    return NULL;
}

static void *maint_main(void *arg)
{
    dht *d = arg;
    const int64_t t0 = now_ms();
    int64_t next_poll = t0 + d->poll_ms, next_refresh = t0 + 60000, next_repub = t0 + d->republish_ms, next_save = t0 + SAVE_MS,
            next_boot = t0 + BOOT_RETRY_MS, next_retry = t0 + 1000;
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
        const int64_t now = now_ms();
        if (now >= next_boot) {
            if (!rt_count(d)) (void)bootstrap(d);                /* alone: the bootstrap list and peers.txt again */
            next_boot = now_ms() + BOOT_RETRY_MS;
        }
        if (now >= next_retry) {
            retry_alerts(d);
            next_retry = now + 1000;
        }
        if (now >= next_poll) {
            (void)dht_mbox_poll(d);
            next_poll = now_ms() + d->poll_ms;
        }
        if (now >= next_refresh) {
            refresh(d, false);
            next_refresh = now_ms() + (d->refresh_ms < 60000 ? d->refresh_ms : 60000);
        }
        if (now >= next_save) {
            save_peers(d);
            next_save = now + SAVE_MS;
        }
        if (now >= next_repub) {
            republish(d);
            next_repub = now_ms() + d->republish_ms;
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
    d->poll_ms = 1000LL * (cfg->mbox_poll_s ? cfg->mbox_poll_s : cfg->listen_port ? 3600 : 60);
    d->retry_ms = 1000LL * (cfg->retry_s ? cfg->retry_s : 60);
    if (d->cfg.handlers <= 0) d->cfg.handlers = 8;
    memcpy(d->me.id, cfg->self->id, 32);
    memcpy(d->me.xonly, cfg->self->xonly, 32);
    pthread_mutex_init(&d->rt_mu, NULL);
    pthread_mutex_init(&d->q_mu, NULL);
    pthread_cond_init(&d->q_cv, NULL);
    pthread_mutex_init(&d->j_mu, NULL);
    pthread_cond_init(&d->j_cv, NULL);
    pthread_mutex_init(&d->a_mu, NULL);
    pthread_mutex_init(&d->p_mu, NULL);
    pthread_mutex_init(&d->m_mu, NULL);
    pthread_cond_init(&d->m_cv, NULL);
    const int64_t now = now_ms();
    for (int i = 0; i < DHT_BUCKETS; i++) d->b[i].looked = now;
    net_cfg_init(&d->ncfg, cfg->ctx, cfg->self, cfg->net->fp, cfg->id_bits, 0, cfg->timeout_ms);
    if (mkdir(d->dir, 0700) != 0) {
        struct stat sb;
        if (stat(d->dir, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            snprintf(err, errlen, "%s: cannot make the data folder", d->dir);
            dht_stop(d);
            return NULL;
        }
    }
    path_of(d, "store.bin", path, sizeof path);
    if (!(d->custody = store_open(path, cfg->ctx, cfg->net, err, errlen))) {
        dht_stop(d);
        return NULL;
    }
    path_of(d, "own.bin", path, sizeof path);
    if (!(d->own = store_open(path, cfg->ctx, cfg->net, err, errlen)) || !(d->mb = mbox_new(MBOX_CAP))) {
        if (d->own) snprintf(err, errlen, "out of memory");
        dht_stop(d);
        return NULL;
    }
    (void)store_last_seq(d->own, d->me.xonly, &d->seq);
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
        while (d->nhandlers < d->cfg.handlers && spawn(&d->th_handlers[d->nhandlers], handler_main, d, HANDLER_STACK)) d->nhandlers++;
        d->has_accept = d->nhandlers > 0 && spawn(&d->th_accept, accept_main, d, HANDLER_STACK);
    }
    d->has_jobs = spawn(&d->th_jobs, jobs_main, d, HANDLER_STACK);
    if ((d->lfd >= 0 && !d->has_accept) || !d->has_jobs) return -1;
    const int n = bootstrap(d);
    if (n) (void)dht_mbox_poll(d);                             /* the alerts that came while this node was away */
    d->has_maint = spawn(&d->th_maint, maint_main, d, HANDLER_STACK);
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
    if (d->started) save_peers(d);
    for (job *j = d->jhead, *nx; j; j = nx) nx = j->next, free(j);
    store_close(d->custody);
    store_close(d->own);
    mbox_free(d->mb);
    for (int i = 0; d->boot && d->boot[i]; i++) free(d->boot[i]);
    free(d->boot);
    free(d->th_handlers);
    free(d->alerts);
    free(d->dir);
    pthread_mutex_destroy(&d->rt_mu);
    pthread_mutex_destroy(&d->q_mu);
    pthread_cond_destroy(&d->q_cv);
    pthread_mutex_destroy(&d->j_mu);
    pthread_cond_destroy(&d->j_cv);
    pthread_mutex_destroy(&d->a_mu);
    pthread_mutex_destroy(&d->p_mu);
    pthread_mutex_destroy(&d->m_mu);
    pthread_cond_destroy(&d->m_cv);
    free(d);
}

uint16_t dht_port(const dht *d) { return d->port; }

void dht_seen(dht *d, const dht_contact *c) { rt_seen(d, c, false); }

int dht_contacts(dht *d, dht_contact *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n && n < max; j++) out[n++] = d->b[i].e[j].c;
    pthread_mutex_unlock(&d->rt_mu);
    return n;
}

int dht_local_records(dht *d, const uint8_t x[32], record *out, int max) { return store_find(d->custody, x, out, max); }

uint64_t dht_local_count(dht *d) { return store_count(d->custody); }

int dht_local_mbox(dht *d, const uint8_t owner[32], record (*pairs)[2], int max) { return mbox_get(d->mb, owner, pairs, max); }
