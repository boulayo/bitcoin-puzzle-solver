/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht.h - the node of the network (docs/en/NETWORK.md 7.3, 7.4): Kademlia (k = 20, alpha = 3, 256 buckets by XOR distance), the custody of
 * the records of others, the alerts of a coincidence and the mailboxes, over the connections of net.h.
 *
 * A record goes to the DHT_KSTORE nodes closest to K = SHA256("pznet1/key" || x). A custodian that sees the same x from two owners tells
 * the one that stores (COINCIDENCE), and nobody else: that owner proves its record to the custodian (PROVE), gets the other owners of the
 * x in the answer, and sends ALERT(record, record, proof) with its own proof of possession to each of them and to the DHT_KSTORE mailbox
 * nodes of each (closest to SHA256("pznet1/mbox" || owner)). An alert for this node is handed to the daemon (on_alert) only after both
 * proofs of possession check out: this node's own (from own_distance) and the other owner's, which its ALERT carries or POSSESS asks for
 * (so two homes that do not listen prove to each other through their mailboxes), and once the other owner or a mailbox of it took this
 * node's proof. A node that does not listen (listen_port 0) only dials out: it gets its alerts by polling its mailboxes.
 * A node that listens is also a relay: it joins the connections of two homes that cannot dial each other (docs/en/NETWORK.md 7.7).
 *
 * Everything that comes from the network is bounded (docs/en/NETWORK.md 7.4): contacts per address block in the table, connections and
 * requests per block, the records a custodian takes (only those it may be a custodian of, from their owner, within its limits), the
 * pairs a mailbox keeps, the alerts an owner remembers, the relays.
 *
 * Threads: one accepts, a bounded pool handles the connections (one request and its answer, or a session handed to on_session), one runs
 * the jobs that take time (the proofs of an alert, the ping of the oldest contact of a full bucket: each kind in turn), one keeps the
 * node fresh (bucket refresh, republish of the own records, mailbox polls, the alerts that wait for a proof, peers.txt), one per relay
 * under way; a publish stores to 16 custodians at a time, each on a thread, and a lookup asks DHT_ALPHA nodes
 * at a time. The callbacks run on those threads: they must be thread-safe and must not call dht_stop. Every other function may be
 * called from any thread. */
#ifndef PZPEER_DHT_H
#define PZPEER_DHT_H

#include "block.h"
#include "net.h"
#include "record.h"
#include "store.h"

#define DHT_K 20                   /* contacts per bucket, nodes in an answer, nodes of a lookup */
#define DHT_ALPHA 3                /* requests of a lookup at a time */
#define DHT_KSTORE 5               /* custodians of a record, mailboxes of an owner */
#define DHT_BUCKETS 256
#define DHT_STORE_MAX 255          /* records in one STORE */
#define DHT_MBOX_MAX 64            /* pairs in one MBOX */
#define DHT_PROVE_MAX 8            /* records in one PROVE: DHT_MBOX_MAX pairs answer them, STORE_X_OWNERS each */
#define DHT_ANY_PORT (-1)          /* listen_port: any free port (tests; dht_port tells which) */
#define DHT_MBOX_ENTRY (2 * RECORD_LEN + 1 + 2 * 64)   /* a pair in MBOX: the two records, which proofs follow, the two proofs */

/* the messages (docs/en/NETWORK.md 7.4): an answer is its request + 1, but MBOX answers PROVE */
enum {
    DHT_PING = 1, DHT_PONG, DHT_FIND_NODE, DHT_NODES, DHT_STORE, DHT_STORED, DHT_ALERT, DHT_OK, DHT_MBOX_GET, DHT_MBOX, DHT_POSSESS,
    DHT_PROOF, DHT_RELAY, DHT_RELAYED, DHT_PROVE, DHT_SESSION_FIRST = 16, DHT_SESSION_LAST = 31,
    DHT_CLAIM = 32, DHT_CLAIMED, DHT_CLAIM_GET, DHT_CLAIMS   /* the claims of the blocks of a linear search (docs/en/NETWORK.md 11) */
};

/* the status of a record in STORED; COINCIDENCE: other owners have its x, and PROVE tells the owner which */
enum { DHT_ST_STORED, DHT_ST_DUP, DHT_ST_INVALID, DHT_ST_COINCIDENCE, DHT_ST_FAILED, DHT_ST_FULL, DHT_ST_FAR };

typedef struct {
    const secp256k1_context *ctx;
    const record_net *net;         /* F, the zero bits of a distinguished point, Q' */
    const home_identity *self;
    unsigned id_bits;              /* leading zero bits of a node ID: NODE_ID_ZERO_BITS on a real network */
    const char *listen_host;       /* numeric; NULL: every address */
    int listen_port;               /* 0: no listening (the node dials out and polls its mailboxes); DHT_ANY_PORT: a free one */
    const char *dir;               /* data folder: store.bin (custody; claims.bin on a linear search), own.bin and own.cust (own records),
                                    * peers.txt */
    const char *const *bootstrap;  /* "host:port": the entry= lines of the network file, --peer (peers.txt is read by the node). They must
                                    * stay valid while the node runs, as bt_routers */
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
    unsigned mbox_poll_s;          /* mailbox polls; 0: 3600 while the others reach this node, 60 otherwise */
    unsigned probe_s;              /* how long a node that listens goes without a connection coming in before it doubts its port; 0: 120 */
    unsigned retry_s;              /* an alert whose other owner did not answer is retried after this, then twice as late each time up to an
                                    * hour, for alert_s; 0: 60 */
    unsigned alert_s;              /* the life of an alert that waits, after which it is forgotten (its pair is new when it comes again);
                                    * 0: a day */
    bool allow_private;            /* a test network (or --allow-private): loopback and private addresses are taken, and not limited.
                                    * A public network refuses them in answers and in the table, so no answer sends it into the LAN */
    uint64_t store_max;            /* records this node keeps as a custodian; 0: DHT_STORE_DEFAULT */
    int relay_max;                 /* relays at once (a node that listens is a relay); 0: 32 */
    uint64_t relay_bytes;          /* bytes of one relay, both ways together; 0: 32 MiB */
    unsigned relay_slot_s;         /* the slot of time that picks the relay of a pair (docs/en/NETWORK.md 7.7); 0: 60 */
    const char *const *bt_routers; /* "host:port" of nodes of the table of BitTorrent: the node announces itself there and looks for the others
                                    * (bt.h), with no server of this network. They must stay valid while the node runs. NULL: not used */
    int nbt_routers;
    bool portmap;                  /* ask the router of the LAN to let the listening port in (portmap.h) */
    bool verbose;                  /* what the node does, on stderr */
    const block_net *blocks;       /* a linear search: the claims of its blocks are kept (claims.bin) and answered, and no records (any x
                                    * with the zero bits passes their checks, with no walk); NULL: a search of records, without claims */
} dht_config;

#define DHT_STORE_DEFAULT (1ull << 27) /* records of a custodian: 5 GB on disk, an index of 2 GB in memory at most */

typedef struct dht dht;

/* Opens the stores of the data folder (made if missing). NULL, with the reason in err, when they cannot be opened. */
dht *dht_create(const dht_config *cfg, char *err, size_t errlen);
/* Listens, starts the threads and bootstraps (the bootstrap list, then peers.txt; then a lookup of the own ID): the contacts in the routing
 * table when it returns, -1 when the port cannot be listened on or a thread cannot start (why, on stderr). A node with no contact keeps
 * trying in the background. */
int dht_start(dht *d);
/* Stops the threads, saves peers.txt and frees the node (started or not). */
void dht_stop(dht *d);
uint16_t dht_port(const dht *d);    /* 0 when the node does not listen */

/* Publishes points of this node: the caller sets t, dp and x of each record, the node sets the owner. They go to the DHT_KSTORE nodes
 * closest to their K, found with a lookup of the last hour near K when one shows them, else with a new one, and are kept in own.bin for
 * the republish, 16 custodians at a time (a slow one holds one thread). After a restart the caller may give its records again from the
 * first: those own.bin has in the same order are not sent again (nor counted). The records sent now that at least one custodian holds.
 * It does not wait for a republish under way. */
int dht_publish(dht *d, record *recs, int n);

/* A claim of a block of this node: to the DHT_KSTORE custodians of the key of its block (block_key). The custodians that took it. */
int dht_claim_publish(dht *d, const block_claim *c);
/* The claims of a block the custodians of its key hold, checked, one an owner, at most max; -1 when no custodian answered. */
int dht_claims_of(dht *d, uint64_t index, block_claim *out, int max);
/* The claims this node keeps as a custodian. */
uint64_t dht_claims_held(dht *d);

/* The daemon's side of a channel: waits up to ms (0: only a look) for the connection the partner opened to this node, the one on_session
 * got; NULL when none came. *give_up: set when the daemon stops waiting (it is stopping). */
typedef net_conn *(*dht_wait_fn)(void *arg, int ms, bool *give_up);

/* A channel with the home partner (x-only identity) for the pair named name (a session's eid), within wait_ms (docs/en/NETWORK.md 7.7):
 * dialled when this side dials (party 0, the smaller identity, when it finds the partner; a side that does not listen), the partner's own
 * connection when it comes (wait), else through a relay, with a handshake end to end that checks the partner. NULL when none could be
 * made in time, or when wait gives up. wait may be NULL. Close the channel with net_close. */
net_conn *dht_channel(dht *d, const uint8_t partner[32], const uint8_t name[32], int wait_ms, dht_wait_fn wait, void *arg);

int dht_count(dht *d);              /* how many contacts the routing table has */
/* An estimate of the nodes that listen (the custodians), from one lookup of a random point: about a fifth of it off, so worth averaging. */
double dht_nodes(dht *d);
/* The records this node holds as a custodian, and the jumps they stand for: the sum of 2^(their zero bits). */
uint64_t dht_local_count(dht *d);
double dht_local_weight(dht *d);

#endif
