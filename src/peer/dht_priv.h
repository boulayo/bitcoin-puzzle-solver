/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_priv.h - inside the node of dht.h: its state, and what its files share.
 *   dht.c        the node's life (create, start, stop), its threads (accept, handlers, jobs, maintenance, and the one that meets the
 *                others in the table of BitTorrent and keeps the port open on the router), the answers to requests, the limits by
 *                address block
 *   dht_table.c  the routing table, the exchanges with a contact, NODES, the lookups, the bootstrap, the refresh, peers.txt
 *   dht_store.c  the records: what a custodian takes, what this node publishes (own.bin, own.cust)
 *   dht_alert.c  the alerts of a coincidence, the proofs of possession, the mailboxes
 *   dht_relay.c  the relay, and the channel of a session (docs/en/NETWORK.md 7.7)
 *   dht_claim.c  the claims of the blocks of a linear search (docs/en/NETWORK.md 11): those a custodian keeps (claims.c, claims.bin),
 *                CLAIM and CLAIM_GET
 * Each shared structure has its own mutex, and no lock is held across a connection: the routing table, the limits by address block, the
 * queue of accepted connections, the jobs, the alerts, the relays, own.bin and the lookups kept for publishing, the addresses found in the
 * table of BitTorrent. The stores, the claims and the mailbox lock themselves. */
#ifndef PZPEER_DHT_PRIV_H
#define PZPEER_DHT_PRIV_H

#include "dht.h"
#include "claims.h"
#include "sys.h"
#include "bt.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define NODE_LEN 82                /* a contact in NODES: node id, xonly, ip, port */
#define PAIR_LEN (2 * RECORD_LEN)
#define MBOX_LEN (2 + DHT_MBOX_MAX * DHT_MBOX_ENTRY)
#define PING_MS 3000               /* the answer to a ping: a node that listens gives it at once */
#define REACH_MS 3600000           /* a node believes its port open for this long after a connection came in */
#define QUEUE_MAX 64               /* accepted connections waiting for a handler: more are closed at once */
#define PROVE_ENTRY (RECORD_WIRE + 64)   /* a record in PROVE: its first bytes and its owner's proof */
#define PARALLEL_MAX 16            /* arguments of parallel(): the custodians a publish stores to at a time */
#define RELAY_WAITING 256          /* homes waiting at this relay for their partner: open connections, no thread (2 a block: 128 blocks to fill) */
#define QUERY_STACK (256 << 10)
#define REFRESH_MS 3600000         /* a bucket with no lookup in its range for this long gets one */

/* the limits by address block (docs/en/NETWORK.md 7.4): a bucket of tokens of each kind, refilled at a rate up to a burst */
enum { LIM_CONN, LIM_REC, LIM_KINDS };
typedef struct lim_entry lim_entry;

typedef struct {
    uint8_t id[32];                /* node ID = SHA256("pznet1/id" || xonly) */
    uint8_t xonly[32];             /* identity */
    net_addr addr;
} dht_contact;

typedef struct {
    dht_contact c;
    uint8_t block[16];             /* of its address */
    int fails;                     /* failed exchanges in a row */
} rt_entry;

typedef struct {
    rt_entry e[DHT_K];             /* the least recently seen first */
    int n;
    bool pinging, has_cand;
    dht_contact cand;              /* a node that waits for the ping of the weakest contact */
    int64_t looked;                /* the last lookup in the range of the bucket */
} bucket;

enum { JOB_ALERT, JOB_PING, JOB_KINDS };   /* the kinds take turns, the oldest job of each first */

typedef struct job {
    struct job *next;
    int kind;
    record a, b;                   /* JOB_ALERT: the pair, the record of this node first */
    dht_contact c;                 /* JOB_PING: the contact */
    bool blocked;                  /* an alert counted against block, the address block it came from */
    uint8_t block[16];
} job;

typedef struct own_alert own_alert;

/* The DHT_K nodes closest to a target that a lookup found (dht_store.c): the custodians of the keys near the target come from them. */
typedef struct {
    uint8_t t[32];
    int n;
    int64_t at;
    bool clean;                    /* in the list of a republish: every record it gave custodians to was taken by all of them */
    dht_contact c[DHT_K];
} cc_entry;

typedef struct {
    cc_entry *e;                   /* in the order of their targets */
    int n, cap;
} cc_list;

typedef struct {
    int fd;
    net_addr from;
} pending;

/* a home that waits at this relay for its partner, or the two homes of a relay under way */
typedef struct {
    bool used, limited[2];
    uint8_t name[32], from[32], to[32], block[2][16];
    net_conn *c;                   /* the waiting home's connection */
    int64_t until;
} relay_slot;

struct dht {
    dht_config cfg;
    net_cfg ncfg, ncfg0;           /* ncfg0: the same, telling no port (dial_cfg) */
    atomic_llong in_ms;            /* when the last connection of another node came in */
    int64_t t0;
    dht_contact me;
    char *dir;
    store *custody;
    claims *claims;                /* a linear search: the claims kept as a custodian (NULL: none) */
    mbox *mb;
    atomic_bool stop;
    atomic_bool unplaced;          /* an own record went to no other node: the first contacts after none get all of them again */
    int lfd;
    uint16_t port;
    int64_t retry_ms, alert_ms, slot_ms, probe_ms;
    pthread_mutex_t rt_mu;         /* the routing table */
    bucket b[DHT_BUCKETS];
    pthread_mutex_t l_mu;          /* the limits by address block */
    lim_entry *lim;
    uint8_t lim_key[32];           /* of the places of the blocks in lim */
    pthread_mutex_t q_mu;          /* accepted connections */
    pthread_cond_t q_cv;
    pending q[QUEUE_MAX];
    int qhead, qn;
    pthread_mutex_t j_mu;          /* jobs */
    pthread_cond_t j_cv;
    job *jhead;                    /* the oldest first */
    int jn, jturn;                 /* the jobs; the kind whose turn it is */
    pthread_mutex_t a_mu;          /* the alerts of this node */
    own_alert *alerts;
    int nalerts, calerts;
    pthread_mutex_t r_mu;          /* the relays */
    pthread_cond_t r_cv;
    relay_slot waiting[RELAY_WAITING], *active;
    int nactive;
    pthread_mutex_t p_mu;          /* publishing: own.bin and what follows, the lookups kept (never across an exchange) */
    int ownfd;                     /* own.bin */
    uint64_t nown, replay, replay_end;   /* its records; those a restart may give again (dht_publish) */
    uint64_t ncut;                 /* the times own.bin was cut (a republish under way does not save own.cust then) */
    cc_list cc;                    /* the lookups of the last hour */
    pthread_mutex_t f_mu;          /* the addresses found in the table of BitTorrent, for the next bootstrap */
    net_addr found[BT_PEERS_MAX];
    int nfound;
    pthread_t th_accept, th_jobs, th_maint, th_meet, *th_handlers;
    int nhandlers;
    bool has_accept, has_jobs, has_maint, has_meet, started;
};

/* whether a is closer to t than b (XOR distance) */
static inline bool closer(const uint8_t t[32], const uint8_t a[32], const uint8_t b[32])
{
    for (int i = 0; i < 32; i++)
        if ((a[i] ^ t[i]) != (b[i] ^ t[i])) return (a[i] ^ t[i]) < (b[i] ^ t[i]);
    return false;
}

static inline uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static inline bool stopping(dht *d) { return atomic_load(&d->stop); }

static inline bool is_me(const dht *d, const uint8_t id[32]) { return !memcmp(id, d->me.id, 32); }

static inline bool mine(const dht *d, const record *r) { return !memcmp(r->owner, d->me.xonly, 32); }

/* whether a and b are one record, whatever their zero bits */
static inline bool same_record(const record *a, const record *b)
{
    uint8_t ea[RECORD_LEN], eb[RECORD_LEN];
    record_encode(a, ea), record_encode(b, eb);
    return record_same(ea, eb);
}

/* An address a node may dial and keep: any port but 0, and on a public network a public address. A NODES answer that points elsewhere
 * would make this node connect into its own LAN. */
static inline bool addr_ok(const dht *d, const net_addr *a) { return a->port != 0 && (d->cfg.allow_private || net_addr_public(a)); }

/* whether the limits by block apply to a: always on a public network; on a test network to public addresses only (its own nodes are on
 * loopback or a LAN, all in one block) */
static inline bool limited(const dht *d, const net_addr *a) { return !d->cfg.allow_private || net_addr_public(a); }

/* Whether the others reach the port of this node: a connection of another node came in within REACH_MS, or the node started less than
 * probe_ms ago. A home behind a router that lets nothing in listens in vain: it lives as a node that does not listen (no place in the
 * table, its mailboxes asked every minute, its partner dialled or met at a relay). */
static inline bool reached(const dht *d) { return d->port && sys_now_ms() - atomic_load(&d->in_ms) < REACH_MS; }

/* What a connection this node opens tells: its port while the others reach it, and for probe_ms of every REACH_MS all the same. A node
 * asked that does not know this one dials it back, and that connection is how this one learns that its port is open. */
static inline const net_cfg *dial_cfg(const dht *d)
{
    return reached(d) || (d->port && (sys_now_ms() - d->t0) % REACH_MS < d->probe_ms) ? &d->ncfg : &d->ncfg0;
}

/* dht.c */
void dlog(const dht *d, const char *fmt, ...) __attribute__((format(SYS_PRINTF, 2, 3)));
bool spawn(pthread_t *th, void *(*fn)(void *), void *arg, size_t stack, bool detached);
bool lim_take(dht *d, const net_addr *a, int kind, double n);
void path_of(const dht *d, const char *name, char *out, size_t cap);
/* A job at the end of the queue: the ping of c, or the alert of the pair (a, b) that came from the address block `block` (NULL: from
 * this node, or from an address that is not limited), which holds at most ALERT_JOBS_BLOCK of the queued alerts. False when the queue
 * has no room for it. */
bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c, const uint8_t *block);

/* dht_table.c */
/* fn on each of n (at most PARALLEL_MAX) arguments at once: the first on this thread, the others on threads of their own */
void parallel(void *(*fn)(void *), void *args, size_t size, int n);
void rt_seen(dht *d, const dht_contact *c);     /* it answered */
void rt_asked(dht *d, const dht_contact *c);    /* it asked */
void rt_failed(dht *d, const dht_contact *c);   /* an exchange with it failed */
int rt_closest(dht *d, const uint8_t t[32], dht_contact *out, int max);
int dht_contacts(dht *d, dht_contact *out, int max);   /* the contacts of the routing table, at most max; their number */
bool responsible(dht *d, const uint8_t key[32]);
/* The check of a NODES answer, as a lookup makes it: its contacts, -1 when the answer is refused whole. */
int parse_nodes(const dht *d, const uint8_t *m, size_t len, dht_contact *out);
/* One request to a contact, dialled with its identity, and its answer, whose type must be the one of that request: its own + 1, MBOX
 * for PROVE. */
bool exchange(dht *d, const dht_contact *c, const uint8_t *req, size_t rlen, uint8_t *ans, size_t cap, size_t *alen);
void heard(dht *d, const dht_contact *c, bool ok);
void job_ping(dht *d, const dht_contact *c);
void encode_contact(const dht_contact *c, uint8_t *p);
/* The want (at most DHT_K) nodes closest to target, this one included when it listens, nearest first: an iterative lookup. */
int dht_lookup(dht *d, const uint8_t target[32], dht_contact *out, int want);
/* dht_lookup, and *whole: no node closer than the farthest of the result was left unasked (the lookup did not stop at its query limit), so
 * the result is the closest of all the nodes that answer; a whole result of fewer than want is every such node */
int lookup_whole(dht *d, const uint8_t target[32], dht_contact *out, int want, bool *whole);
/* The contact of an identity: from the routing table, or by a lookup of its node ID. False when no node answers for it (it is offline,
 * or it does not listen). */
bool dht_find(dht *d, const uint8_t xonly[32], dht_contact *out);
/* A connection to a contact, checked to be its identity, for the frames of a session (types 16 to 31; the other side gets the first in
 * on_session). Close it with net_close. */
net_conn *dht_dial(dht *d, const dht_contact *c);
/* A lookup of a random ID in the range of each bucket (all: also of those looked up within REFRESH_MS) */
void refresh(dht *d, bool all);
int bootstrap(dht *d);
void save_peers(dht *d);

/* dht_store.c */
int custody_put(dht *d, const record *r, const net_addr *from);
int custody_proved(dht *d, const record *r, const uint8_t sig[64], const net_addr *from, mbox_pair *pairs);
bool own_open(dht *d, char *err, size_t errlen);
/* The own records again now (the maintenance thread does it every day), each to the custodians it did not have at the last republish
 * (all: to every custodian, as at the first contacts after none). The copies sent. */
uint64_t dht_republish(dht *d, bool all);

/* dht_alert.c */
/* SHA256("pznet1/mbox" || owner): the mailboxes of an owner are the DHT_KSTORE nodes closest to it. */
void dht_mbox_key(const uint8_t owner[32], uint8_t k[32]);
bool owner_ok(const dht *d, const record *r);
bool pair_ok(const dht *d, const record *a, const record *b);
bool alert_in(dht *d, const record *a, const record *b, const uint8_t *proof, const net_addr *from);
extern const uint8_t CUSTODY_NONCE[32];
bool prove_to(dht *d, const dht_contact *c, const record *r, int n);
/* The proof of possession of r from its owner at its contact c (POSSESS): 1 it checks out, 0 a wrong proof, -1 none (offline,
 * unreachable, or it closed without one). */
int ask_proof(dht *d, const dht_contact *c, const record *r);
void job_alert(dht *d, const record *a, const record *b);
void retry_alerts(dht *d);
/* Asks this node's mailboxes for its alerts now (the maintenance thread does it on its own): the new pairs found. */
int dht_mbox_poll(dht *d);

/* dht_claim.c */
size_t claim_answer(dht *d, const net_conn *c, const uint8_t *m, size_t len, uint8_t *ans);

/* dht_relay.c */
bool relay_in(dht *d, net_conn *c, const uint8_t *m, size_t len);
void relay_sweep(dht *d);

#endif
