/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_priv.h - inside the node of dht.h: its state, and what its files share.
 *   dht.c        the node's life (create, start, stop), its threads (accept, handlers, jobs, maintenance), the answers to requests, the
 *                limits by address block
 *   dht_table.c  the routing table, the exchanges with a contact, NODES, the lookups, the bootstrap, the refresh, peers.txt
 *   dht_store.c  the records: what a custodian takes, what this node publishes (own.bin, own.seq)
 *   dht_alert.c  the alerts of a coincidence, the proofs of possession, the mailboxes
 *   dht_relay.c  the relay, and the channel of a session (docs/en/NETWORK.md 7.7)
 * Each shared structure has its own mutex, and no lock is held across a connection: the routing table, the limits by address block, the
 * queue of accepted connections, the jobs, the alerts, the relays, the publishing. The stores and the mailbox lock themselves. */
#ifndef PZPEER_DHT_PRIV_H
#define PZPEER_DHT_PRIV_H

#include "dht.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#define NODE_LEN 82                /* a contact in NODES: node id, xonly, ip, port */
#define PAIR_LEN (2 * RECORD_LEN)
#define MBOX_LEN (2 + DHT_MBOX_MAX * DHT_MBOX_ENTRY)
#define QUEUE_MAX 64               /* accepted connections waiting for a handler: more are closed at once */
#define SENT_MAX 256               /* pairs a custodian remembers having alerted */
#define RELAY_WAITING 32           /* homes waiting at this relay for their partner */
#define QUERY_STACK (256 << 10)

/* the limits by address block (docs/en/NETWORK.md 7.4): a bucket of tokens of each kind, refilled at a rate up to a burst */
enum { LIM_CONN, LIM_REC, LIM_ALERT, LIM_COIN, LIM_KINDS };
typedef struct lim_entry lim_entry;

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

typedef struct own_alert own_alert;

typedef struct {
    uint8_t h[32];
    int64_t at;
} sent_alert;

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

static inline int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static inline void nap(int64_t ms)
{
    const struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    if (ms > 0) nanosleep(&ts, NULL);
}

static inline bool stopping(dht *d) { return atomic_load(&d->stop); }

static inline bool is_me(const dht *d, const uint8_t id[32]) { return !memcmp(id, d->me.id, 32); }

static inline bool mine(const dht *d, const record *r) { return !memcmp(r->owner, d->me.xonly, 32); }

/* An address a node may dial and keep: any port but 0, and on a public network a public address. A NODES answer that points elsewhere
 * would make this node connect into its own LAN. */
static inline bool addr_ok(const dht *d, const net_addr *a) { return a->port != 0 && (d->cfg.allow_private || net_addr_public(a)); }

/* whether the limits by block apply to a: always on a public network; on a test network to public addresses only (its own nodes are on
 * loopback or a LAN, all in one block) */
static inline bool limited(const dht *d, const net_addr *a) { return !d->cfg.allow_private || net_addr_public(a); }

/* dht.c */
void dlog(const dht *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool spawn(pthread_t *th, void *(*fn)(void *), void *arg, size_t stack, bool detached);
bool lim_take(dht *d, const net_addr *a, int kind, double n);
void path_of(const dht *d, const char *name, char *out, size_t cap);
bool job_push(dht *d, int kind, const record *a, const record *b, const dht_contact *c);

/* dht_table.c */
void rt_seen(dht *d, const dht_contact *c, bool answered);
void rt_failed(dht *d, const uint8_t id[32]);
int rt_closest(dht *d, const uint8_t t[32], dht_contact *out, int max);
int rt_count(dht *d);
bool responsible(dht *d, const uint8_t key[32]);
bool exchange(dht *d, const dht_contact *c, const uint8_t *req, size_t rlen, uint8_t *ans, size_t cap, size_t *alen);
void heard(dht *d, const dht_contact *c, bool ok);
void job_ping(dht *d, const dht_contact *weak);
void encode_contact(const dht_contact *c, uint8_t *p);
void refresh(dht *d, bool all);
int bootstrap(dht *d);
void save_peers(dht *d);

/* dht_store.c */
int custody_put(dht *d, const record *r, const net_addr *from);
void republish(dht *d);
uint64_t load_seq(const dht *d);

/* dht_alert.c */
bool owner_ok(const dht *d, const record *r);
bool pair_ok(const dht *d, const record *a, const record *b);
void pair_hash(const record *a, const record *b, uint8_t h[32]);
bool alert_in(dht *d, const record *a, const record *b, const uint8_t *proof);
void job_alert_out(dht *d, const record *a, const record *b);
void job_alert_own(dht *d, const record *a, const record *b);
void retry_alerts(dht *d);

/* dht_relay.c */
bool relay_in(dht *d, net_conn *c, const uint8_t *m, size_t len);
void relay_sweep(dht *d);

#endif
