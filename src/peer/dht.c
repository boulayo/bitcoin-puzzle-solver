/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.c - the node (see dht.h). Each shared structure has its own mutex, and no lock is held across a connection: the routing table, the
 * limits by address block, the queue of accepted connections, the jobs, the alerts, the relays, the publishing. The stores and the
 * mailbox lock themselves. */
#include "dht.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <inttypes.h>
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
#define MBOX_LEN (2 + DHT_MBOX_MAX * DHT_MBOX_ENTRY)
#define ANS_MAX MBOX_LEN           /* the longest answer */
#define QUEUE_MAX 64               /* accepted connections waiting for a handler: more are closed at once */
#define JOBS_MAX 4096
#define ALERT_JOBS_MAX 256         /* of them, the alerts a custodian sends */
#define FAILS_MAX 3                /* failed exchanges in a row that drop a contact */
#define SHORT_MAX (3 * DHT_K)      /* candidates of a lookup */
#define LOOKUP_ASK_MAX 64          /* queries of one lookup: answers that keep pointing farther cannot make it endless */
#define LOOKUP_BLOCK_MAX 4         /* queries of one lookup to the nodes of one address block (answers aimed at a victim) */
#define RT_BLOCK_BUCKET 2          /* contacts of one address block in a bucket, */
#define RT_BLOCK_TABLE 4           /* ... in the table, and in one NODES answer */
#define RESP_SLACK (2 * DHT_KSTORE) /* a custodian or a mailbox that knows this many nodes closer to the key is not one of it */
#define CONN_PER_BLOCK 4           /* connections of one address block queued or being answered at a time */
#define LIM_SLOTS 4096             /* address blocks whose limits are remembered */
#define OWNER_SHARE 16             /* an owner holds at most 1/16 of a custodian's records */
#define MBOX_CAP 4096              /* alert pairs a mailbox keeps, */
#define MBOX_PER_OWNER DHT_MBOX_MAX /* ... and of one owner */
#define SENT_MAX 256               /* pairs a custodian remembers having alerted */
#define ALERTS_MAX 1024            /* alerts of this node remembered */
#define RESEND_MS (3600 * 1000LL)  /* a custodian alerts the same pair at most once an hour (a republished record alerts again) */
#define POST_MS (3600 * 1000LL)    /* this node's proof of an alert, sent again to the other owner while the alert waits */
#define BACKOFF_MAX_MS (3600 * 1000LL)
#define ALERT_TTL_MS (24 * 3600 * 1000LL)
#define RELAY_WAITING 32           /* homes waiting at this relay for their partner */
#define RELAY_PER_BLOCK 2          /* relays (waiting or under way) of the homes of one address block, */
#define RELAY_PER_ID 2             /* ... and waits of one identity */
#define BOOT_TRY 64                /* addresses tried at a bootstrap, BOOT_BATCH at a time (so a list of dead addresses costs a few timeouts) */
#define BOOT_BATCH 16
#define BOOT_WANT 8                /* ... until this many answered */
#define BOOT_RETRY_MS 30000
#define SAVE_MS 300000
#define PEERS_MAX 1024
#define HANDLER_STACK (1 << 20)
#define QUERY_STACK (256 << 10)

/* the limits by address block (docs/en/NETWORK.md 7.4): a bucket of tokens of each kind, refilled at a rate up to a burst */
enum { LIM_CONN, LIM_REC, LIM_ALERT, LIM_COIN, LIM_KINDS };
static const double LIM_RATE[LIM_KINDS] = {20, 50, 2, 1.0 / 60};      /* per second: connections, records, alerts in, coincidences */
static const double LIM_BURST[LIM_KINDS] = {100, 100000, 64, 8};       /* a republish sends all of an owner's records at once */

typedef struct {
    uint8_t block[16];
    bool used;
    int conns;                     /* connections queued or being answered */
    double tok[LIM_KINDS];
    int64_t at;                    /* the last refill */
} lim_entry;

typedef struct {
    dht_contact c;
    uint8_t block[16];             /* of its address */
    int fails;                     /* failed exchanges in a row */
    bool answered;                 /* it answered a request of this node (peers.txt keeps those) */
} rt_entry;

typedef struct {
    rt_entry e[DHT_K];             /* the least recently seen first */
    int n;
    bool pinging, has_cand, cand_answered;
    dht_contact cand;              /* a contact that waits for the ping of the weakest contact */
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
    record mine, theirs;
    int state;
    bool mine_ok, theirs_ok;       /* the proofs checked: this node's own (it knows the distance), the other owner's */
    bool busy;                     /* a job works on it */
    int tries;
    int64_t until, next, posted;   /* retried at next (later each time) until until; this node's proof last sent to the other owner */
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

/* a home that waits at this relay for its partner, or the two homes of a relay under way */
typedef struct {
    bool used, limited[2];
    uint8_t name[32], from[32], to[32], block[2][16];
    net_conn *c;                   /* the waiting home's connection */
    int64_t until;
} relay_slot;

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
    int64_t refresh_ms, republish_ms, poll_ms, retry_ms, relay_ms, slot_ms;
    pthread_mutex_t rt_mu;         /* the routing table */
    bucket b[DHT_BUCKETS];
    pthread_mutex_t l_mu;          /* the limits by address block */
    lim_entry *lim;
    pthread_mutex_t q_mu;          /* accepted connections */
    pthread_cond_t q_cv;
    pending q[QUEUE_MAX];
    int qhead, qn;
    pthread_mutex_t j_mu;          /* jobs */
    pthread_cond_t j_cv;
    job *jhead, *jtail;
    int jn, jn_alert;
    pthread_mutex_t a_mu;          /* alerts of this node, alerts sent as a custodian */
    own_alert *alerts;
    int nalerts, calerts;
    sent_alert sent[SENT_MAX];
    int nsent;
    pthread_mutex_t r_mu;          /* the relays */
    pthread_cond_t r_cv;
    relay_slot waiting[RELAY_WAITING], *active;
    int nactive;
    uint64_t relays_done;
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

static void nap(int64_t ms)
{
    const struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    if (ms > 0) nanosleep(&ts, NULL);
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

static bool spawn(pthread_t *th, void *(*fn)(void *), void *arg, size_t stack, bool detached)
{
    pthread_attr_t at;
    if (pthread_attr_init(&at) != 0) return false;
    (void)pthread_attr_setstacksize(&at, stack);
    if (detached) (void)pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const bool ok = pthread_create(th, &at, fn, arg) == 0;
    pthread_attr_destroy(&at);
    return ok;
}

/* fn on each of n (at most BOOT_BATCH) arguments at once: the first on this thread, the others on threads of their own, or on this one
 * when a thread cannot be made. The rounds of a lookup and the pings of a bootstrap. */
static void parallel(void *(*fn)(void *), void *args, size_t size, int n)
{
    pthread_t th[BOOT_BATCH];
    bool started[BOOT_BATCH] = {false};
    for (int i = 1; i < n; i++) started[i] = spawn(&th[i], fn, (char *)args + (size_t)i * size, QUERY_STACK, false);
    if (n > 0) fn(args);
    for (int i = 1; i < n; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else fn((char *)args + (size_t)i * size);
    }
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

/* SHA256("pznet1/pair" || the two records in the order of their owners): the name of a pair, whichever order it came in; also the nonce
 * of the proof an owner sends for a pair (docs/en/NETWORK.md 7.3) */
static void pair_hash(const record *a, const record *b, uint8_t h[32])
{
    uint8_t m[11 + PAIR_LEN] = "pznet1/pair";
    const bool swap = memcmp(a->owner, b->owner, 32) > 0;
    record_encode(swap ? b : a, m + 11);
    record_encode(swap ? a : b, m + 11 + RECORD_LEN);
    pz_sha256(m, sizeof m, h);
}

/* ---- addresses and the limits by block ---- */

/* An address a node may dial and keep: any port but 0, and on a public network a public address. A NODES answer that points elsewhere
 * would make this node connect into its own LAN. */
static bool addr_ok(const dht *d, const net_addr *a) { return a->port != 0 && (d->cfg.allow_private || net_addr_public(a)); }

/* whether the limits by block apply to a: always on a public network; on a test network to public addresses only (its own nodes are on
 * loopback or a LAN, all in one block) */
static bool limited(const dht *d, const net_addr *a) { return !d->cfg.allow_private || net_addr_public(a); }

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
    best->at = now_ms();
    return best;
}

static void lim_refill(lim_entry *e)
{
    const int64_t now = now_ms();
    const double dt = (double)(now - e->at) / 1000;
    for (int k = 0; k < LIM_KINDS; k++) e->tok[k] = e->tok[k] + dt * LIM_RATE[k] < LIM_BURST[k] ? e->tok[k] + dt * LIM_RATE[k] : LIM_BURST[k];
    e->at = now;
}

/* n tokens of a kind from the block of a: false when it has used its share */
static bool lim_take(dht *d, const net_addr *a, int kind, double n)
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

static void encode_contact(const dht_contact *c, uint8_t *p)
{
    memcpy(p, c->id, 32);
    memcpy(p + 32, c->xonly, 32);
    memcpy(p + 64, c->addr.ip, 16);
    p[80] = (uint8_t)(c->addr.port >> 8), p[81] = (uint8_t)c->addr.port;
}

/* NODES: every contact must be whole (its ID from its identity, with the zero bits, an address this node may dial), no address twice,
 * at most RT_BLOCK_TABLE in one block (an honest table has no more), or the answer is refused */
static bool parse_nodes(const dht *d, const uint8_t *m, size_t len, dht_contact *out, int *n)
{
    if (len < 2 || m[0] != DHT_NODES || m[1] > DHT_K || len != 2 + (size_t)m[1] * NODE_LEN) return false;
    uint8_t blk[DHT_K][16];
    for (int i = 0; i < m[1]; i++) {
        const uint8_t *p = m + 2 + (size_t)i * NODE_LEN;
        dht_contact *c = &out[i];
        uint8_t id[32];
        memcpy(c->id, p, 32);
        memcpy(c->xonly, p + 32, 32);
        memcpy(c->addr.ip, p + 64, 16);
        c->addr.port = (uint16_t)(p[80] << 8 | p[81]);
        keys_node_id(c->xonly, id);
        if (memcmp(id, c->id, 32) || !keys_id_ok(id, d->cfg.id_bits) || !addr_ok(d, &c->addr)) return false;
        net_addr_block(&c->addr, blk[i]);
        int inblock = 0;
        for (int j = 0; j < i; j++) {
            if (!memcmp(out[j].addr.ip, c->addr.ip, 16) && out[j].addr.port == c->addr.port) return false;   /* one address, two identities */
            inblock += !memcmp(blk[j], blk[i], 16);
        }
        if (limited(d, &c->addr) && inblock >= RT_BLOCK_TABLE) return false;
    }
    *n = m[1];
    return true;
}

int dht_parse_nodes(const dht *d, const uint8_t *msg, size_t len, dht_contact *out)
{
    int n;
    return parse_nodes(d, msg, len, out, &n) ? n : -1;
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
    net_addr_block(&c->addr, e->block);
    e->fails = 0;
    e->answered = answered;
}

/* Room for a contact at address a in bucket b (rt_mu held), not counting the contact id itself: at most RT_BLOCK_BUCKET of one address
 * block in a bucket and RT_BLOCK_TABLE in the table. One attacker's nodes, on its few networks, cannot fill a bucket or the table (the
 * rule of Bitcoin, and of the project, 4.2). */
static bool rt_room(const dht *d, const bucket *b, const net_addr *a, const uint8_t id[32])
{
    if (!limited(d, a)) return true;
    uint8_t blk[16];
    net_addr_block(a, blk);
    int inb = 0, all = 0;
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n; j++) {
            const rt_entry *e = &d->b[i].e[j];
            if (memcmp(e->c.id, id, 32) && !memcmp(e->block, blk, 16)) all++, inb += &d->b[i] == b;
        }
    return inb < RT_BLOCK_BUCKET && all < RT_BLOCK_TABLE;
}

/* the waiting candidate of a bucket into it, when there is room for it now (rt_mu held) */
static void rt_take_cand(dht *d, bucket *b)
{
    if (b->has_cand && b->n < DHT_K && rt_index(b, b->cand.id) < 0 && rt_room(d, b, &b->cand.addr, b->cand.id)) rt_push(b, &b->cand, b->cand_answered);
    b->has_cand = false;
}

/* A contact heard from goes to the tail of its bucket. A full bucket pings its weakest contact (the one that failed most, the oldest
 * among equals) and the new one waits: Kademlia keeps the contacts that have lived longest, which an attacker cannot replace by flooding
 * the node with new ones, nor by making a good contact miss one answer. A candidate that answered a request of this node is not pushed
 * aside by one that only asked. A known contact that moves keeps its old address when the new one's block has its share. */
static void rt_seen(dht *d, const dht_contact *c, bool answered)
{
    if (is_me(d, c->id) || !addr_ok(d, &c->addr)) return;
    dht_contact weak;
    bool ping = false;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, c->id)];
    const int at = rt_index(b, c->id);
    if (at >= 0) {
        rt_entry e = b->e[at];
        rt_drop(b, at);
        if ((memcmp(e.c.addr.ip, c->addr.ip, 16) || e.c.addr.port != c->addr.port) && rt_room(d, b, &c->addr, c->id)) {
            e.c.addr = c->addr;
            net_addr_block(&c->addr, e.block);
        }
        e.fails = 0;
        e.answered = e.answered || answered;
        b->e[b->n++] = e;
    } else if (!rt_room(d, b, &c->addr, c->id)) {
        /* its address block has its share of the table */
    } else if (b->n < DHT_K) {
        rt_push(b, c, answered);
    } else {
        if (!b->has_cand || answered || !b->cand_answered) {
            b->cand = *c;
            b->cand_answered = answered;
            b->has_cand = true;
        }
        if (!b->pinging) {
            int w = 0;
            for (int i = 1; i < b->n; i++)
                if (b->e[i].fails > b->e[w].fails) w = i;
            b->pinging = ping = true;
            weak = b->e[w].c;
        }
    }
    pthread_mutex_unlock(&d->rt_mu);
    if (ping && !job_push(d, JOB_PING, NULL, NULL, &weak)) {
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
        if (!b->pinging) rt_take_cand(d, b);
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

/* Whether this node may be one of the DHT_KSTORE nodes closest to key: it knows fewer than RESP_SLACK nodes closer. A contact of bucket j
 * (the first bit where it differs from this node is j) is closer to key exactly when bit j of (this ID XOR key) is 1, so the count is a
 * sum over buckets. A custodian and a mailbox take only what they may be responsible for: records and pairs sprayed at random nodes are
 * refused, and to aim them at one node costs about (nodes / RESP_SLACK) tries each. */
static bool responsible(dht *d, const uint8_t key[32])
{
    int nearer = 0;
    pthread_mutex_lock(&d->rt_mu);
    for (int j = 0; j < DHT_BUCKETS && nearer < RESP_SLACK; j++)
        if ((d->me.id[j / 8] ^ key[j / 8]) & (0x80 >> (j % 8))) nearer += d->b[j].n;
    pthread_mutex_unlock(&d->rt_mu);
    return nearer < RESP_SLACK;
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

static void job_ping(dht *d, const dht_contact *weak)
{
    const uint8_t req = DHT_PING;
    uint8_t ans[1];
    size_t n;
    const bool alive = exchange(d, weak, &req, 1, ans, sizeof ans, &n) && n == 1;
    heard(d, weak, alive);                                     /* alive: to the tail of its bucket */
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, weak->id)];
    b->pinging = false;
    if (!alive) {
        const int at = rt_index(b, weak->id);
        if (at >= 0) rt_drop(b, at);
        rt_take_cand(d, b);
    }
    b->has_cand = false;
    pthread_mutex_unlock(&d->rt_mu);
    dlog(d, alive ? "the weakest contact of a full bucket answered: kept" : "the weakest contact of a full bucket did not answer: replaced");
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

/* how many of the queries so far went to the block of c's address (a lookup asks at most LOOKUP_BLOCK_MAX there) */
static int asked_in_block(const dht *d, const net_addr *a, uint8_t (*blocks)[16], int nasked)
{
    if (!limited(d, a)) return 0;
    uint8_t blk[16];
    int k = 0;
    net_addr_block(a, blk);
    for (int i = 0; i < nasked; i++) k += !memcmp(blocks[i], blk, 16);
    return k;
}

int dht_lookup(dht *d, const uint8_t target[32], dht_contact *out, int want)
{
    if (want > DHT_K) want = DHT_K;
    cand s[SHORT_MAX];
    dht_contact seed[DHT_K];
    uint8_t blocks[LOOKUP_ASK_MAX][16];
    int n = 0, nasked = 0;
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
     * when those want have all answered, or after LOOKUP_ASK_MAX queries. A candidate in a block that had its queries counts as failed:
     * answers that all point at one network (a victim, or the attacker's own) cannot make this node dial it more. */
    while (!stopping(d) && nasked < LOOKUP_ASK_MAX) {
        int nq = 0, live = 0;
        for (int i = 0; i < n && live < want && nq < DHT_ALPHA && nasked + nq < LOOKUP_ASK_MAX; i++) {
            if (s[i].state == C_FAIL) continue;
            if (s[i].state == C_NEW && asked_in_block(d, &s[i].c.addr, blocks, nasked + nq) >= LOOKUP_BLOCK_MAX) {
                s[i].state = C_FAIL;
                continue;
            }
            live++;
            if (s[i].state != C_NEW) continue;
            q[nq].d = d, q[nq].c = s[i].c, q[nq].target = target, q[nq].ngot = 0, q[nq].ok = false;
            net_addr_block(&s[i].c.addr, blocks[nasked + nq]);
            nq++;
        }
        if (!nq) break;
        nasked += nq;
        parallel(query_main, q, sizeof *q, nq);
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

/* ---- custody ---- */

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

/* A record for the custody of this node: its status for STORED. from: the address of the owner that sent it, NULL for this node's own.
 * A record of another node is taken only when this node may be one of its custodians, and within the rate of the sender's block. Each
 * coincidence becomes a job that alerts the owners and their mailboxes, also for a duplicate (a republished record alerts again, at most
 * once an hour: an alert lost on the way comes back). Each alert costs this node lookups and a dozen connections, so the coincidences of
 * one block are limited too, and each record alerts at most two pairs: more owners of one x are a flood, not walks. */
static int custody_put(dht *d, const record *r, const net_addr *from)
{
    uint8_t k[32];
    record held, others[2];
    int no;
    if (!owner_ok(d, r)) return DHT_ST_INVALID;
    store_key(r->x, k);
    if (from && !responsible(d, k)) return DHT_ST_FAR;
    if (from && !lim_take(d, from, LIM_REC, 1)) return DHT_ST_FULL;
    const store_status s = store_put(d->custody, r, &held, others, 2, &no);
    if (s == STORE_FULL) held = *r;                            /* not kept, but its coincidences are alerted all the same */
    for (int i = 0; i < no; i++) {
        uint8_t h[32];
        pair_hash(&held, &others[i], h);
        if (sent_check(d, h) && (!from || lim_take(d, from, LIM_COIN, 1)) && job_push(d, JOB_ALERT_OUT, &held, &others[i], NULL))
            dlog(d, "coincidence (%s): alerting the owners", held.t == others[i].t ? "merge" : "useful");
    }
    switch (s) {
    case STORE_OK: return no ? DHT_ST_COINCIDENCE : DHT_ST_STORED;
    case STORE_DUP: return DHT_ST_DUP;
    case STORE_INVALID: return DHT_ST_INVALID;
    case STORE_OLD: return DHT_ST_OLD;
    case STORE_FULL: return DHT_ST_FULL;
    default: return DHT_ST_FAILED;
    }
}

/* ---- alerts ---- */

static own_alert *alert_find(dht *d, const uint8_t h[32])   /* a_mu held */
{
    for (int i = 0; i < d->nalerts; i++)
        if (!memcmp(d->alerts[i].h, h, 32)) return &d->alerts[i];
    return NULL;
}

/* the state of an alert of this node (-1: unknown), and whether the other owner's proof checked out */
static int alert_state(dht *d, const uint8_t h[32], bool *theirs_ok)
{
    pthread_mutex_lock(&d->a_mu);
    const own_alert *e = alert_find(d, h);
    const int st = e ? e->state : -1;
    if (theirs_ok) *theirs_ok = e && e->theirs_ok;
    pthread_mutex_unlock(&d->a_mu);
    return st;
}

/* The alert of a pair with a record m of this node (a_mu held): found, or added (*fresh). When the table is full the oldest one that is
 * not waiting goes, else the oldest waiting one no job holds: a flood of alerts that will never be proved (copies of this node's points
 * by owners that do not answer) cannot shut out the next real one, which is tried at once; an alert pushed out comes back with the
 * next republish or mailbox poll. NULL only without memory. */
static own_alert *alert_add_locked(dht *d, const record *m, const record *t, const uint8_t h[32], bool *fresh)
{
    *fresh = false;
    own_alert *e = alert_find(d, h);
    if (e) return e;
    if (d->nalerts == ALERTS_MAX) {
        int at = 0;
        while (at < d->nalerts && d->alerts[at].state == AL_WAIT) at++;
        if (at == d->nalerts)
            for (at = 0; at < d->nalerts && d->alerts[at].busy; at++) {}
        if (at == d->nalerts) return NULL;
        memmove(&d->alerts[at], &d->alerts[at + 1], (size_t)(d->nalerts - at - 1) * sizeof *d->alerts);
        d->nalerts--;
    }
    if (d->nalerts == d->calerts) {
        const int nc = d->calerts ? 2 * d->calerts : 16;
        own_alert *g = realloc(d->alerts, (size_t)nc * sizeof *g);
        if (!g) return NULL;
        d->alerts = g, d->calerts = nc;
    }
    e = &d->alerts[d->nalerts++];
    memset(e, 0, sizeof *e);
    memcpy(e->h, h, 32);
    e->mine = *m, e->theirs = *t;
    e->state = AL_WAIT;
    e->until = now_ms() + ALERT_TTL_MS;
    e->next = now_ms() + d->retry_ms;
    *fresh = true;
    return e;
}

/* A checked pair that came to this node (an ALERT, a mailbox, its own custody), with the proof of the owner of a for the owner of b when
 * one came (checked): kept as a mailbox when this node may be a mailbox of one of the owners, and an alert of this node when one record
 * is its own. True for a new alert of this node. */
static bool alert_in(dht *d, const record *a, const record *b, const uint8_t *proof)
{
    uint8_t ka[32], kb[32], h[32];
    dht_mbox_key(a->owner, ka), dht_mbox_key(b->owner, kb);
    if ((responsible(d, ka) || responsible(d, kb)) && mbox_put(d->mb, a, b, proof)) dlog(d, "an alert pair in the mailbox");
    if (!mine(d, a) && !mine(d, b)) return false;
    const record *m = mine(d, a) ? a : b, *t = m == a ? b : a;
    bool fresh, go = false;
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *e = alert_add_locked(d, m, t, h, &fresh);
    if (e && e->state == AL_WAIT && proof && t == a && !e->theirs_ok) e->theirs_ok = go = true;   /* the other owner proved its record */
    pthread_mutex_unlock(&d->a_mu);
    if (fresh) dlog(d, "an alert for this node: checking the proofs");
    if (fresh || go) (void)job_push(d, JOB_ALERT_OWN, m, t, NULL);   /* if the queue is full, the maintenance thread retries it */
    return fresh;
}

/* an ALERT (with or without a proof) to a contact; to this node itself, what it would do with it */
static void send_alert(dht *d, const dht_contact *c, const record *a, const record *b, const uint8_t *proof)
{
    if (is_me(d, c->id)) {
        (void)alert_in(d, a, b, proof);
        return;
    }
    uint8_t req[1 + PAIR_LEN + 64], ans[1];
    size_t n;
    req[0] = DHT_ALERT;
    record_encode(a, req + 1);
    record_encode(b, req + 1 + RECORD_LEN);
    if (proof) memcpy(req + 1 + PAIR_LEN, proof, 64);
    heard(d, c, exchange(d, c, req, 1 + PAIR_LEN + (proof ? 64 : 0), ans, sizeof ans, &n) && n == 1);
}

/* the alert to an owner (found by its node ID) and to its mailboxes */
static void alert_owner(dht *d, const uint8_t owner[32], const record *a, const record *b, const uint8_t *proof)
{
    dht_contact c, mb[DHT_KSTORE];
    uint8_t k[32];
    if (!memcmp(owner, d->me.xonly, 32)) (void)alert_in(d, a, b, proof);
    else if (dht_find(d, owner, &c)) send_alert(d, &c, a, b, proof);
    dht_mbox_key(owner, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    for (int i = 0; i < m && !stopping(d); i++) send_alert(d, &mb[i], a, b, proof);
}

/* the job of a custodian: ALERT to both owners and to the mailboxes of each */
static void job_alert_out(dht *d, const record *a, const record *b)
{
    for (int w = 0; w < 2 && !stopping(d); w++) alert_owner(d, (w ? b : a)->owner, a, b, NULL);
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

/* This node's proof of possession for the other owner of an alert, over the pair: the pair's name is the nonce, so the proof can wait in
 * a mailbox, and it binds the other owner as the asker, so it proves nothing to anyone else (docs/en/NETWORK.md 7.3). To the other owner
 * when it can be reached, and to its mailboxes, where it finds it when it does not listen. False when this node cannot make it. */
static bool post_proof(dht *d, const own_alert *e)
{
    uint8_t dd[32], proof[64];
    const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, e->mine.x, e->mine.t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, &e->mine, dd, e->theirs.owner, e->h, proof);
    pz_secure_zero(dd, sizeof dd);
    if (ok) alert_owner(d, e->theirs.owner, &e->mine, &e->theirs, proof);
    return ok;
}

static int64_t backoff_ms(const dht *d, int tries)
{
    int64_t w = d->retry_ms;
    for (int i = 1; i < tries && w < BACKOFF_MAX_MS; i++) w *= 2;
    return w < BACKOFF_MAX_MS ? w : BACKOFF_MAX_MS;
}

/* The job of an alert of this node: its own proof (the record is signed by this node, so when its distance is missing the daemon has not
 * read that point yet: the alert waits), then this node's proof sent to the other owner, then the other owner's proof, asked with POSSESS
 * unless it came already (an ALERT with its proof, a mailbox). on_alert when both check out; only a wrong proof of the other owner
 * refuses it. An alert that waits is tried again later each time (backoff_ms) for a day. */
static void job_alert_own(dht *d, const record *a, const record *b)
{
    uint8_t h[32];
    own_alert e;
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *p = alert_find(d, h);
    const bool go = p && p->state == AL_WAIT && !p->busy;
    if (go) p->busy = true, e = *p;
    pthread_mutex_unlock(&d->a_mu);
    if (!go) return;                                           /* done already, or a job on it now */
    if (!e.mine_ok) e.mine_ok = own_proof(d, &e.mine);
    if (e.mine_ok && (!e.posted || now_ms() - e.posted >= POST_MS) && post_proof(d, &e)) e.posted = now_ms();
    const int asked = e.mine_ok && !e.theirs_ok ? dht_ask_proof(d, &e.theirs) : -2;
    int st = AL_WAIT;
    pthread_mutex_lock(&d->a_mu);
    if ((p = alert_find(d, h))) {
        p->busy = false;
        p->mine_ok = e.mine_ok, p->posted = e.posted;
        p->theirs_ok = p->theirs_ok || asked == 1;             /* or its proof came while this job asked */
        if (asked == 0) p->state = AL_REFUSED;
        else if (p->mine_ok && p->theirs_ok) p->state = AL_DONE;
        else p->tries++, p->next = now_ms() + backoff_ms(d, p->tries);
        st = p->state;
        e = *p;
    }
    pthread_mutex_unlock(&d->a_mu);
    dlog(d, st == AL_DONE      ? "alert: both proofs check out"
            : st == AL_REFUSED ? "alert refused: the other owner's proof does not check out"
            : !e.mine_ok       ? "alert: this node does not know the distance of its point (yet), retried later"
                               : "alert: the other owner's proof has not come, retried later");
    if (st == AL_DONE && d->cfg.on_alert) d->cfg.on_alert(d->cfg.user, &e.mine, &e.theirs, e.mine.t == e.theirs.t);
}

static void retry_alerts(dht *d)
{
    record pairs[16][2];
    int n = 0;
    const int64_t now = now_ms();
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts; i++) {
        own_alert *e = &d->alerts[i];
        if (e->state != AL_WAIT || e->busy || now < e->next) continue;
        if (now >= e->until) e->state = AL_REFUSED;
        else if (n < 16) e->next = now + backoff_ms(d, e->tries + 1), pairs[n][0] = e->mine, pairs[n][1] = e->theirs, n++;
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
                        len == 2 + (size_t)ans[1] * DHT_MBOX_ENTRY;
        heard(d, &mb[i], ok);
        for (int p = 0; ok && p < ans[1]; p++) {
            const uint8_t *q = ans + 2 + (size_t)p * DHT_MBOX_ENTRY, flags = q[PAIR_LEN];
            record r[2];
            uint8_t h[32];
            bool theirs_ok;
            if (!record_decode(q, &r[0]) || !record_decode(q + RECORD_LEN, &r[1]) || flags > 3) continue;
            const int me = mine(d, &r[0]) ? 0 : mine(d, &r[1]) ? 1 : -1;
            if (me < 0) continue;
            pair_hash(&r[0], &r[1], h);
            const int st = alert_state(d, h, &theirs_ok);
            if (st > AL_WAIT || (st < 0 && !pair_ok(d, &r[0], &r[1]))) continue;   /* the signatures only of a new pair */
            /* the other owner's proof for this node, when the mailbox has it */
            const uint8_t *pf = flags & (1 << (1 - me)) ? q + PAIR_LEN + 1 + (size_t)(1 - me) * 64 : NULL;
            if (pf && (theirs_ok || !record_proof_ok(d->cfg.ctx, d->cfg.net, &r[1 - me], d->me.xonly, h, pf))) pf = NULL;
            if (st < 0 || pf) fresh += alert_in(d, &r[1 - me], &r[me], pf);
        }
    }
    free(ans);
    if (fresh) dlog(d, "%d alerts from the mailboxes", fresh);
    return fresh;
}

/* ---- the relay (docs/en/NETWORK.md 7.7) ---- */

typedef struct {
    dht *d;
    relay_slot *slot;
    net_conn *a, *b;
} relay_job;

static void *relay_main(void *arg)
{
    relay_job *j = arg;
    dht *d = j->d;
    const uint64_t n = net_splice(j->a, j->b, d->cfg.relay_bytes, d->relay_ms, &d->stop);
    pthread_mutex_lock(&d->r_mu);
    j->slot->used = false;
    d->nactive--;
    d->relays_done++;
    pthread_cond_broadcast(&d->r_cv);
    pthread_mutex_unlock(&d->r_mu);
    dlog(d, "a relay ended: %" PRIu64 " bytes", n);
    free(j);
    return NULL;
}

/* the relays (waiting or under way) that count against a home at block blk with identity id (r_mu held) */
static void relay_count(dht *d, const uint8_t blk[16], const uint8_t id[32], int *in_block, int *of_id)
{
    *in_block = *of_id = 0;
    for (int i = 0; i < RELAY_WAITING; i++) {
        const relay_slot *w = &d->waiting[i];
        if (!w->used) continue;
        *in_block += w->limited[0] && !memcmp(w->block[0], blk, 16);
        *of_id += !memcmp(w->from, id, 32);
    }
    for (int i = 0; i < d->cfg.relay_max; i++)
        for (int k = 0; k < 2; k++) *in_block += d->active[i].used && d->active[i].limited[k] && !memcmp(d->active[i].block[k], blk, 16);
}

/* the waits that are over, closed (r_mu held) */
static void relay_sweep(dht *d)
{
    const int64_t now = now_ms();
    for (int i = 0; i < RELAY_WAITING; i++)
        if (d->waiting[i].used && now >= d->waiting[i].until) {
            net_close(d->waiting[i].c);
            d->waiting[i].used = false;
        }
}

/* RELAY(name, partner) from a home on c: it waits here for the partner's RELAY(name, this home), within the limits; when the partner is
 * waiting, both get RELAYED and a thread of their own copies their bytes. True when this node keeps c (waiting, or relayed). */
static bool relay_in(dht *d, net_conn *c, const uint8_t *m, size_t len)
{
    if (len != 65 || !d->port || !memcmp(m + 33, c->remote, 32)) return false;
    const uint8_t *name = m + 1, *to = m + 33;
    uint8_t blk[16];
    net_addr_block(&c->addr, blk);
    const bool lim = limited(d, &c->addr);
    net_conn *other = NULL;
    relay_slot *act = NULL;
    bool kept = false;
    pthread_mutex_lock(&d->r_mu);
    relay_sweep(d);
    for (int i = 0; i < RELAY_WAITING && !other; i++) {
        relay_slot *w = &d->waiting[i];
        if (!w->used || memcmp(w->name, name, 32) || memcmp(w->from, to, 32) || memcmp(w->to, c->remote, 32)) continue;
        w->used = false;
        if (!net_alive(w->c)) {                                /* the partner went away while it waited */
            net_close(w->c);
            continue;
        }
        other = w->c;
        for (int k = 0; k < d->cfg.relay_max && !act; k++)
            if (!d->active[k].used) act = &d->active[k];
        if (act) {
            *act = *w;
            act->used = true;
            act->limited[1] = lim;
            memcpy(act->block[1], blk, 16);
            act->c = NULL;
            d->nactive++;
        }
    }
    if (!other) {
        int in_block, of_id, free_at = -1;
        relay_count(d, blk, c->remote, &in_block, &of_id);
        for (int i = 0; i < RELAY_WAITING; i++) {
            relay_slot *w = &d->waiting[i];
            if (w->used && !memcmp(w->name, name, 32) && !memcmp(w->from, c->remote, 32)) {   /* the same home again: the new join wins */
                in_block -= w->limited[0] && !memcmp(w->block[0], blk, 16);
                of_id--;
                net_close(w->c);
                w->used = false;
            }
            if (!w->used && free_at < 0) free_at = i;
        }
        if (free_at >= 0 && (!lim || in_block < RELAY_PER_BLOCK) && of_id < RELAY_PER_ID) {
            relay_slot *w = &d->waiting[free_at];
            memset(w, 0, sizeof *w);
            w->used = kept = true;
            memcpy(w->name, name, 32), memcpy(w->from, c->remote, 32), memcpy(w->to, to, 32), memcpy(w->block[0], blk, 16);
            w->limited[0] = lim;
            w->c = c;
            w->until = now_ms() + d->slot_ms + d->slot_ms / 4;  /* a slot, and a quarter for clocks a little apart */
        }
    }
    pthread_mutex_unlock(&d->r_mu);
    if (!other) return kept;
    if (!act) {                                                /* full: both go, and look for the relay of the next slot */
        net_close(other);
        return false;
    }
    const uint8_t go = DHT_RELAYED;
    relay_job *j = malloc(sizeof *j);
    pthread_t th;
    if (j) *j = (relay_job){d, act, other, c};
    if (j && net_send(other, &go, 1) && net_send(c, &go, 1) && spawn(&th, relay_main, j, QUERY_STACK, true)) {
        dlog(d, "relaying a pair");
        return true;
    }
    free(j);
    net_close(other);
    net_close(c);
    pthread_mutex_lock(&d->r_mu);
    act->used = false;
    d->nactive--;
    pthread_cond_broadcast(&d->r_cv);
    pthread_mutex_unlock(&d->r_mu);
    return true;
}

void dht_relays(dht *d, int *waiting, int *active, uint64_t *done)
{
    pthread_mutex_lock(&d->r_mu);
    relay_sweep(d);
    int w = 0;
    for (int i = 0; i < RELAY_WAITING; i++) w += d->waiting[i].used;
    *waiting = w, *active = d->nactive, *done = d->relays_done;
    pthread_mutex_unlock(&d->r_mu);
}

/* The relay of a pair in a slot of time: the node closest to SHA256("pznet1/relay" || name || slot) that is neither of the two homes.
 * Both homes find the same one without a word between them; a relay that fails or lies costs one slot, and the next slot has a relay of
 * its own (another key), so no node near one key can stop a pair. */
static bool relay_of(dht *d, const uint8_t name[32], const uint8_t partner[32], uint64_t slot, dht_contact *out)
{
    uint8_t m[12 + 32 + 8] = "pznet1/relay", k[32], pid[32];
    dht_contact cs[DHT_KSTORE];
    memcpy(m + 12, name, 32);
    for (int b = 0; b < 8; b++) m[44 + b] = (uint8_t)(slot >> (56 - 8 * b));
    pz_sha256(m, sizeof m, k);
    keys_node_id(partner, pid);
    const int n = dht_lookup(d, k, cs, DHT_KSTORE);
    for (int i = 0; i < n; i++)
        if (!is_me(d, cs[i].id) && memcmp(cs[i].id, pid, 32)) {
            *out = cs[i];
            return true;
        }
    return false;
}

/* joins the relay r and waits there for the partner until until: the channel, its handshake end to end done (party 0 initiates) */
static net_conn *relay_join(dht *d, const dht_contact *r, const uint8_t partner[32], const uint8_t name[32], int64_t until)
{
    uint8_t req[65], ans[1];
    size_t n;
    net_conn *c = dht_dial(d, r);
    if (!c) {
        rt_failed(d, r->id);
        return NULL;
    }
    req[0] = DHT_RELAY;
    memcpy(req + 1, name, 32), memcpy(req + 33, partner, 32);
    bool ok = net_send(c, req, sizeof req);
    while (ok && !stopping(d) && now_ms() < until && !net_readable(c, until - now_ms() < 200 ? (int)(until - now_ms()) : 200)) {}
    ok = ok && !stopping(d) && net_readable(c, 0) && net_recv(c, ans, sizeof ans, &n) && n == 1 && ans[0] == DHT_RELAYED &&
         net_rehandshake(&d->ncfg, c, memcmp(d->me.xonly, partner, 32) < 0, partner);
    if (!ok) {
        net_close(c);
        return NULL;
    }
    dlog(d, "a channel through a relay");
    return c;
}

static net_conn *await(void *arg, dht_wait_fn wait, int64_t ms, bool *give_up)
{
    if (wait) return wait(arg, (int)ms, give_up);
    nap(ms);
    return NULL;
}

net_conn *dht_channel(dht *d, const uint8_t partner[32], const uint8_t name[32], int wait_ms, dht_wait_fn wait, void *arg)
{
    const int64_t until = now_ms() + wait_ms;
    bool give_up = false;
    dht_contact pc;
    /* direct: party 0 dials the partner it finds, and so does a side that does not listen (nobody can dial it) */
    if ((memcmp(d->me.xonly, partner, 32) < 0 || !d->port) && dht_find(d, partner, &pc)) {
        net_conn *c = dht_dial(d, &pc);
        if (c) return c;
        rt_failed(d, pc.id);
    }
    for (int round = 0; !stopping(d) && now_ms() < until; round++) {
        /* the partner's own connection: a side that listens gives it the first slot, then looks between the tries of the relay */
        net_conn *c = await(arg, wait, round == 0 && d->port ? (d->slot_ms < until - now_ms() ? d->slot_ms : until - now_ms()) : 0, &give_up);
        if (c || give_up) return c;
        const uint64_t wall = wall_us() / 1000, slot = wall / (uint64_t)d->slot_ms;
        const int64_t left = (int64_t)((slot + 1) * (uint64_t)d->slot_ms - wall), end = now_ms() + left < until ? now_ms() + left : until;
        dht_contact r;
        if (relay_of(d, name, partner, slot, &r) && (c = relay_join(d, &r, partner, name, end))) return c;
        while (!stopping(d) && now_ms() < end) {               /* this slot's relay did not serve: the next slot has another */
            if ((c = await(arg, wait, end - now_ms() < 200 ? end - now_ms() : 200, &give_up)) || give_up) return c;
        }
    }
    return NULL;
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
    if (req[0] == DHT_RELAY) {                                 /* the relay keeps it, waiting or relayed */
        if (!relay_in(d, c, req, len)) net_close(c);
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

/* own.seq: the last sequence number this node signed. own.bin keeps the records as they were first published, and a republish signs
 * them again with higher numbers: after a restart with a clock that went back, the next records would be refused by every custodian
 * until the clock caught up with the last republish. */
static void save_seq(dht *d)
{
    char path[4096], tmp[4100];
    snprintf(path, sizeof path, "%s/own.seq", d->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    const bool ok = fprintf(f, "%" PRIu64 "\n", d->seq) > 0;
    if (fclose(f) == 0 && ok) (void)rename(tmp, path);
    else (void)remove(tmp);
}

static uint64_t load_seq(const dht *d)
{
    char path[4096];
    uint64_t v = 0;
    snprintf(path, sizeof path, "%s/own.seq", d->dir);
    FILE *f = fopen(path, "r");
    if (f && fscanf(f, "%" SCNu64, &v) != 1) v = 0;
    if (f) fclose(f);
    return v;
}

/* At most DHT_STORE_MAX records, in the order of their sequence numbers: a lookup for each, then one STORE per custodian with all its
 * records in that order. The records that at least one custodian holds. */
static int send_batch(dht *d, const record *recs, int n)
{
    dht_contact (*cs)[DHT_KSTORE] = malloc((size_t)n * sizeof *cs);   /* the custodians of each record */
    int *ncs = calloc((size_t)n, sizeof *ncs), done = 0;
    bool (*sent)[DHT_KSTORE] = calloc((size_t)n, sizeof *sent);
    uint8_t *held = calloc((size_t)n, 1), *req = malloc(2 + DHT_STORE_MAX * RECORD_LEN);
    int *idx = malloc((size_t)n * sizeof *idx);
    const bool ok = cs && ncs && sent && held && req && idx;
    for (int i = 0; ok && i < n && !stopping(d); i++) {
        uint8_t k[32];
        store_key(recs[i].x, k);
        ncs[i] = dht_lookup(d, k, cs[i], DHT_KSTORE);
    }
    for (int i = 0; ok && i < n && !stopping(d); i++)
        for (int j = 0; j < ncs[i]; j++) {
            if (sent[i][j]) continue;
            const dht_contact t = cs[i][j];                    /* a custodian not served yet: all its records, in order */
            int m = 0;
            for (int i2 = i; i2 < n; i2++)
                for (int j2 = 0; j2 < ncs[i2]; j2++)
                    if (!sent[i2][j2] && !memcmp(cs[i2][j2].id, t.id, 32)) sent[i2][j2] = true, idx[m++] = i2;
            uint8_t st[DHT_STORE_MAX], ans[2 + DHT_STORE_MAX];
            if (is_me(d, t.id)) {
                for (int q = 0; q < m; q++) st[q] = (uint8_t)custody_put(d, &recs[idx[q]], NULL);
            } else {
                size_t len;
                req[0] = DHT_STORE, req[1] = (uint8_t)m;
                for (int q = 0; q < m; q++) record_encode(&recs[idx[q]], req + 2 + (size_t)q * RECORD_LEN);
                const bool got = exchange(d, &t, req, 2 + (size_t)m * RECORD_LEN, ans, sizeof ans, &len) && len == 2 + (size_t)m && ans[1] == m;
                heard(d, &t, got);
                if (!got) continue;
                memcpy(st, ans + 2, (size_t)m);
            }
            for (int q = 0; q < m; q++) held[idx[q]] |= st[q] == DHT_ST_STORED || st[q] == DHT_ST_DUP || st[q] == DHT_ST_COINCIDENCE;
        }
    for (int i = 0; ok && i < n; i++) done += held[i];
    free(cs), free(ncs), free(sent), free(held), free(req), free(idx);
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
    int done = 0;
    pthread_mutex_lock(&d->p_mu);
    for (int i = 0; i < n; i++) {
        record held;
        int no;
        if (!sign_locked(d, &recs[i])) continue;              /* never in practice; the custodians would refuse it */
        if (store_put(d->own, &recs[i], &held, NULL, 0, &no) == STORE_DUP && held.t != recs[i].t)
            dlog(d, "this node has the same x with two types");   /* a coincidence of the home with itself: its pzsolve sees it */
    }
    save_seq(d);
    for (int o = 0; o < n && !stopping(d); o += DHT_STORE_MAX) done += send_batch(d, recs + o, n - o < DHT_STORE_MAX ? n - o : DHT_STORE_MAX);
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
        save_seq(d);
        (void)send_batch(d, batch, m);
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
        const int m = n - i < BOOT_BATCH ? n - i : BOOT_BATCH;
        for (int j = 0; j < m; j++) p[j].d = d, p[j].b = &list[i + j], p[j].ok = false;
        parallel(boot_ping_main, p, sizeof *p, m);
        for (int j = 0; j < m; j++)
            if (p[j].ok && !is_me(d, p[j].c.id)) {             /* not its own address */
                rt_seen(d, &p[j].c, true);
                ok++;
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
    const bool ok = d->jn < JOBS_MAX && (kind != JOB_ALERT_OUT || d->jn_alert < ALERT_JOBS_MAX) && !stopping(d);
    if (ok) {
        if (d->jtail) d->jtail->next = j;
        else d->jhead = j;
        d->jtail = j;
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
            if (!d->jhead) d->jtail = NULL;
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
        if (fd == -2) nap(50);                                 /* out of descriptors: let the handlers close some */
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
            pthread_mutex_lock(&d->r_mu);
            relay_sweep(d);                                     /* the homes that waited at this relay in vain */
            pthread_mutex_unlock(&d->r_mu);
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
    pthread_cond_init(&d->m_cv, NULL);
    const int64_t now = now_ms();
    for (int i = 0; i < DHT_BUCKETS; i++) d->b[i].looked = now;
    net_cfg_init(&d->ncfg, cfg->ctx, cfg->self, cfg->net->fp, cfg->id_bits, 0, cfg->timeout_ms);
    if (!(d->lim = calloc(LIM_SLOTS, sizeof *d->lim)) || !(d->active = calloc((size_t)d->cfg.relay_max, sizeof *d->active))) {
        snprintf(err, errlen, "out of memory");
        dht_stop(d);
        return NULL;
    }
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

int dht_local_mbox(dht *d, const uint8_t owner[32], mbox_pair *pairs, int max) { return mbox_get(d->mb, owner, pairs, max); }
