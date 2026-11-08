/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.h - the node of the network (docs/en/NETWORK.md 7.3, 7.4): Kademlia (k = 20, alpha = 3, 256 buckets by XOR distance), the custody of
 * the records of others, the alerts of a coincidence and the mailboxes, over the connections of net.h.
 *
 * A record goes to the DHT_KSTORE nodes closest to K = SHA256("pznet1/key" || x). A custodian that sees the same x from two owners sends
 * ALERT(record, record) to both owners and to the DHT_KSTORE mailbox nodes of each (closest to SHA256("pznet1/mbox" || owner)). An alert
 * for this node is handed to the daemon (on_alert) only after both proofs of possession check out: this node's own (from own_distance) and
 * the other owner's (POSSESS). A node that does not listen (listen_port 0) only dials out: it gets its alerts by polling its mailboxes.
 *
 * Threads: one accepts, a bounded pool handles the connections (one request and its answer, or a session handed to on_session), one runs
 * the jobs that take time (the alerts of a custodian, the proofs of an alert, the ping of the oldest contact of a full bucket), one keeps
 * the node fresh (bucket refresh, republish of the own records, mailbox polls, the alerts that wait for a proof, peers.txt). The callbacks
 * run on those threads: they must be thread-safe and must not call dht_stop. Every other function may be called from any thread. */
#ifndef PZPEER_DHT_H
#define PZPEER_DHT_H

#include "net.h"
#include "record.h"

#define DHT_K 20                   /* contacts per bucket, nodes in an answer, nodes of a lookup */
#define DHT_ALPHA 3                /* requests of a lookup at a time */
#define DHT_KSTORE 5               /* custodians of a record, mailboxes of an owner */
#define DHT_BUCKETS 256
#define DHT_STORE_MAX 255          /* records in one STORE */
#define DHT_MBOX_MAX 64            /* pairs in one MBOX */
#define DHT_ANY_PORT (-1)          /* listen_port: any free port (tests; dht_port tells which) */

/* the messages (docs/en/NETWORK.md 7.4): an answer is its request + 1 */
enum {
    DHT_PING = 1, DHT_PONG, DHT_FIND_NODE, DHT_NODES, DHT_STORE, DHT_STORED, DHT_ALERT, DHT_OK, DHT_MBOX_GET, DHT_MBOX, DHT_POSSESS,
    DHT_PROOF, DHT_SESSION_FIRST = 16, DHT_SESSION_LAST = 31
};

/* the status of a record in STORED */
enum { DHT_ST_STORED, DHT_ST_DUP, DHT_ST_INVALID, DHT_ST_OLD, DHT_ST_COINCIDENCE, DHT_ST_FAILED };

typedef struct {
    uint8_t id[32];                /* node ID = SHA256("pznet1/id" || xonly) */
    uint8_t xonly[32];             /* identity */
    net_addr addr;
} dht_contact;

typedef struct {
    const secp256k1_context *ctx;
    const record_net *net;         /* F, the zero bits of a distinguished point, Q' */
    const home_identity *self;
    unsigned id_bits;              /* leading zero bits of a node ID: NODE_ID_ZERO_BITS on a real network */
    const char *listen_host;       /* numeric; NULL: every address */
    int listen_port;               /* 0: no listening (the node dials out and polls its mailboxes); DHT_ANY_PORT: a free one */
    const char *dir;               /* data folder: store.bin (custody), own.bin (own records), peers.txt */
    const char *const *bootstrap;  /* "host:port": the entry= lines of the network file, --peer (peers.txt is read by the node) */
    int nbootstrap;
    /* An alert for this node, after both proofs of possession: mine is this node's record, theirs the other owner's; merge when they
     * have the same type (both walkers should restart: no key comes of it). It can come again for the same pair after a restart. */
    void (*on_alert)(void *user, const record *mine, const record *theirs, bool merge);
    /* The distance d (d mod n, big-endian) of this node's point with x and type t; false when the node does not have it. */
    bool (*own_distance)(void *user, const uint8_t x[32], int t, uint8_t d[32]);
    /* A connection whose first frame (msg, valid during the call) is a session message (types 16 to 31): the callback owns the
     * connection and closes it with net_close. It runs on a handler thread: a long session goes to a thread of its own. NULL: such
     * connections are closed. */
    void (*on_session)(void *user, net_conn *c, const uint8_t *msg, size_t len);
    void *user;
    int timeout_ms;                /* 0: NET_TIMEOUT_MS */
    int handlers;                  /* threads that answer; 0: 8 */
    unsigned refresh_s;            /* bucket refresh; 0: 3600 */
    unsigned republish_s;          /* republish of the own records; 0: 86400 */
    unsigned mbox_poll_s;          /* mailbox polls; 0: 60 when not listening, 3600 when listening */
    unsigned retry_s;              /* an alert whose other owner did not answer is retried after this (for a day); 0: 60 */
    bool verbose;                  /* what the node does, on stderr */
} dht_config;

typedef struct dht dht;

/* Opens the stores of the data folder (made if missing). NULL, with the reason in err, when they cannot be opened. */
dht *dht_create(const dht_config *cfg, char *err, size_t errlen);
/* Listens, starts the threads and bootstraps (the bootstrap list, then peers.txt; then a lookup of the own ID): the contacts in the routing
 * table when it returns, -1 when the port cannot be listened on. A node with no contact keeps trying in the background. */
int dht_start(dht *d);
/* Stops the threads, saves peers.txt and frees the node (started or not). */
void dht_stop(dht *d);
uint16_t dht_port(const dht *d);    /* 0 when the node does not listen */

/* Publishes points of this node: the caller sets t and x of each record, the node sets the owner, the next sequence numbers and the
 * signature (the records come back signed). They are kept in own.bin for the republish and sent to the DHT_KSTORE nodes closest to
 * their K. The node signs and sends under one lock, so every custodian sees the sequence numbers of this owner in order, as it must
 * (it refuses a new record whose number is not above the last). The records that at least one custodian holds now. */
int dht_publish(dht *d, record *recs, int n);

/* The want (at most DHT_K) nodes closest to target, this one included when it listens, nearest first: an iterative lookup. */
int dht_lookup(dht *d, const uint8_t target[32], dht_contact *out, int want);
/* The contact of an identity: from the routing table, or by a lookup of its node ID. False when no node answers for it (it is offline,
 * or it does not listen). */
bool dht_find(dht *d, const uint8_t xonly[32], dht_contact *out);
/* A connection to a contact, checked to be its identity, for the frames of a session (types 16 to 31; the other side gets the first in
 * on_session). Close it with net_close. */
net_conn *dht_dial(dht *d, const dht_contact *c);

/* The proof of possession of a record by its owner (found and dialled; this node's own record is proved from own_distance): 1 the proof
 * checks out, 0 the owner answered with a wrong proof, -1 no answer (offline, unreachable, or it closed without a proof). */
int dht_ask_proof(dht *d, const record *r);
/* The same over an open connection to the owner (a session); 0 when the connection is not with the owner of r. */
int dht_proof_on(dht *d, net_conn *c, const record *r);
/* Answers a request of the table (types 1 to 12) that came on a session connection; false for a malformed one (close the connection). */
bool dht_serve(dht *d, net_conn *c, const uint8_t *msg, size_t len);

/* Asks this node's mailboxes for its alerts now (the maintenance thread does it on its own): the new pairs found. */
int dht_mbox_poll(dht *d);
/* A lookup in the range of every bucket now (the maintenance thread does it every refresh_s). */
void dht_refresh(dht *d);
/* A contact heard from, as the routing table takes it (a full bucket pings its oldest contact). */
void dht_seen(dht *d, const dht_contact *c);
/* The contacts of the routing table, at most max; their number. */
int dht_contacts(dht *d, dht_contact *out, int max);
/* What the node holds: the records of the network with x, the number of them, the alert pairs of an owner in its mailbox. */
int dht_local_records(dht *d, const uint8_t x[32], record *out, int max);
uint64_t dht_local_count(dht *d);
int dht_local_mbox(dht *d, const uint8_t owner[32], record (*pairs)[2], int max);
/* SHA256("pznet1/mbox" || owner): the mailboxes of an owner are the DHT_KSTORE nodes closest to it. */
void dht_mbox_key(const uint8_t owner[32], uint8_t k[32]);

#endif
