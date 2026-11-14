/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_table.c - the routing table (docs/en/NETWORK.md 7.4): the buckets and the limits by address block in them, the exchanges with a
 * contact and what the table learns from them, NODES, the iterative lookups, the bootstrap, the refresh of the buckets, peers.txt. */
#include "dht_priv.h"
#include "btc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define NODES_LEN (2 + DHT_K * NODE_LEN)
#define FAILS_MAX 3                /* failed exchanges in a row that drop a contact */
#define SHORT_MAX (3 * DHT_K)      /* candidates of a lookup */
#define LOOKUP_ASK_MAX 64          /* queries of one lookup: answers that keep pointing farther cannot make it endless */
#define LOOKUP_BLOCK_MAX 4         /* queries of one lookup to the nodes of one address block (answers aimed at a victim) */
#define RT_BLOCK_BUCKET 2          /* contacts of one address block in a bucket, */
#define RT_BLOCK_TABLE 4           /* ... in the table, and in one NODES answer */
#define RESP_SLACK (2 * DHT_KSTORE) /* a custodian or a mailbox that knows this many nodes closer to the key is not one of it */
#define BOOT_TRY 64                /* addresses tried at a bootstrap, BOOT_BATCH at a time (so a list of dead addresses costs a few timeouts) */
#define BOOT_BATCH 16
#define BOOT_WANT 8                /* ... until this many answered */
#define PEERS_MAX 1024

typedef struct {
    net_addr a;
    bool has_id;
    uint8_t xonly[32];
} boot_addr;


/* fn on each of n (at most BOOT_BATCH) arguments at once: the first on this thread, the others on threads of their own, or on this one
 * when a thread cannot be made. The workers of a lookup and the pings of a bootstrap. */
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

void encode_contact(const dht_contact *c, uint8_t *p)
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

static void rt_push(bucket *b, const dht_contact *c)
{
    rt_entry *e = &b->e[b->n++];
    e->c = *c;
    net_addr_block(&c->addr, e->block);
    e->fails = 0;
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

/* A node heard from. One that answered goes to the tail of its bucket, or into it when there is room. One that only asked told its port
 * itself, and a home behind a router that lets nothing in tells a port nobody reaches: it is pinged at that address and enters when it
 * answers (job_ping). A full bucket pings its weakest contact (the one that failed most, the oldest among equals) and the new one waits:
 * Kademlia keeps the contacts that have lived longest, which an attacker cannot replace by flooding the node with new ones, nor by making
 * a good contact miss one answer. One ping at a time for a bucket. A known contact that moves keeps its old address when the new one's
 * block has its share. */
static void rt_note(dht *d, const dht_contact *c, bool answered)
{
    if (is_me(d, c->id) || !addr_ok(d, &c->addr)) return;
    dht_contact ping;
    bool go = false;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, c->id)];
    const int at = rt_index(b, c->id);
    if (at >= 0 && answered) {
        rt_entry e = b->e[at];
        rt_drop(b, at);
        if ((memcmp(e.c.addr.ip, c->addr.ip, 16) || e.c.addr.port != c->addr.port) && rt_room(d, b, &c->addr, c->id)) {
            e.c.addr = c->addr;
            net_addr_block(&c->addr, e.block);
        }
        e.fails = 0;
        b->e[b->n++] = e;
    } else if (at >= 0) {
        /* a contact that asks is no news */
    } else if (!rt_room(d, b, &c->addr, c->id)) {
        /* its address block has its share of the table */
    } else if (answered && b->n < DHT_K) {
        rt_push(b, c);
    } else if (!b->pinging) {
        b->pinging = go = true;
        ping = *c;
        if (b->n == DHT_K) {
            int w = 0;
            for (int i = 1; i < b->n; i++)
                if (b->e[i].fails > b->e[w].fails) w = i;
            b->cand = *c, b->has_cand = true;
            ping = b->e[w].c;
        }
    }
    pthread_mutex_unlock(&d->rt_mu);
    if (go && !job_push(d, JOB_PING, NULL, NULL, &ping)) {
        pthread_mutex_lock(&d->rt_mu);
        b->pinging = b->has_cand = false;
        pthread_mutex_unlock(&d->rt_mu);
    }
}

void rt_seen(dht *d, const dht_contact *c) { rt_note(d, c, true); }
void rt_asked(dht *d, const dht_contact *c) { rt_note(d, c, false); }

void rt_failed(dht *d, const uint8_t id[32])
{
    if (is_me(d, id)) return;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, id)];
    const int at = rt_index(b, id);
    if (at >= 0 && ++b->e[at].fails >= FAILS_MAX) rt_drop(b, at);
    pthread_mutex_unlock(&d->rt_mu);
}

/* the max contacts of the table closest to t, nearest first */
int rt_closest(dht *d, const uint8_t t[32], dht_contact *out, int max)
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

int rt_count(dht *d)
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
bool responsible(dht *d, const uint8_t key[32])
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
bool exchange(dht *d, const dht_contact *c, const uint8_t *req, size_t rlen, uint8_t *ans, size_t cap, size_t *alen)
{
    net_conn *k = dht_dial(d, c);
    const bool ok = k && net_send(k, req, rlen) && net_recv(k, ans, cap, alen) && *alen >= 1 && ans[0] == req[0] + 1;
    net_close(k);
    return ok;
}

/* what the routing table learns from an exchange whose answer was checked */
void heard(dht *d, const dht_contact *c, bool ok)
{
    if (ok) rt_seen(d, c);
    else rt_failed(d, c->id);
}

/* The ping of the weakest contact of a full bucket, or of a node that waits to enter one. A dead contact leaves its place to the node
 * that waited, once that one has answered its own ping. A node that listens answers at once: PING_MS, so that a silent address holds
 * the thread of the jobs for a moment only. */
void job_ping(dht *d, const dht_contact *c)
{
    const uint8_t req = DHT_PING;
    uint8_t ans[1];
    size_t n;
    dht_contact next;
    net_cfg quick = *dial_cfg(d);
    if (quick.timeout_ms > PING_MS) quick.timeout_ms = PING_MS;
    const bool alive = net_exchange(&quick, &c->addr, c->xonly, &req, 1, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_PONG;
    pthread_mutex_lock(&d->rt_mu);
    bucket *b = &d->b[bucket_of(d->me.id, c->id)];
    const int at = rt_index(b, c->id);
    if (at >= 0 && !alive) rt_drop(b, at);
    const bool more = at >= 0 && !alive && b->has_cand;
    if (more) next = b->cand;
    b->has_cand = false;
    b->pinging = more;
    pthread_mutex_unlock(&d->rt_mu);
    if (alive) rt_seen(d, c);
    if (more && !job_push(d, JOB_PING, NULL, NULL, &next)) {
        pthread_mutex_lock(&d->rt_mu);
        b->pinging = false;
        pthread_mutex_unlock(&d->rt_mu);
    }
    dlog(d, "ping: the contact %s", alive ? "answered" : "did not answer");
}

/* ---- lookups ---- */

enum { C_NEW, C_ASKED, C_OK, C_FAIL };   /* a candidate: not asked yet, asked and not answered yet, answered, failed */

typedef struct {
    dht_contact c;
    int state;
    uint8_t by[32];                /* the node whose answer named it; all zero for a contact of this node's own table */
} cand;

/* Into the candidates, sorted by distance to t, at most SHORT_MAX. In a full list a new one takes the place of one that failed, or of
 * the farthest that another node named and that was not asked yet, when it is closer; never of one that answered, nor of a contact of
 * the own table: names that nobody checked cannot push out the nodes this node knows to be there. */
static void cand_add(cand *s, int *n, const uint8_t t[32], const dht_contact *c, int state, const uint8_t *by)
{
    static const uint8_t own[32];
    for (int i = 0; i < *n; i++)
        if (!memcmp(s[i].c.id, c->id, 32)) return;
    if (*n == SHORT_MAX) {
        int out = *n - 1;
        while (out >= 0 && s[out].state != C_FAIL) out--;
        if (out < 0) {
            for (out = *n - 1; out >= 0 && (s[out].state != C_NEW || !memcmp(s[out].by, own, 32)); out--) {}
            if (out < 0 || !closer(t, c->id, s[out].c.id)) return;
        }
        memmove(&s[out], &s[out + 1], (size_t)(*n - 1 - out) * sizeof *s);
        (*n)--;
    }
    int at = *n;
    while (at > 0 && closer(t, c->id, s[at - 1].c.id)) at--;
    memmove(&s[at + 1], &s[at], (size_t)(*n - at) * sizeof *s);
    s[at].c = *c;
    s[at].state = state;
    memcpy(s[at].by, by ? by : own, 32);
    (*n)++;
}

/* A node is believed while the nodes it names answer. When three more of them failed than answered, the rest of what it named is given
 * up: answers full of closer identities that do not exist (they cost a hash each) would spend every query of the lookup on timeouts
 * and keep it from the nodes that are there. The contacts of the own table are not judged this way. */
static void cand_distrust(cand *s, int n, const uint8_t failed[32])
{
    static const uint8_t own[32];
    const uint8_t *by = NULL;
    int bad = 0;
    for (int i = 0; i < n && !by; i++)
        if (!memcmp(s[i].c.id, failed, 32)) by = s[i].by;
    if (!by || !memcmp(by, own, 32)) return;
    for (int i = 0; i < n; i++)
        if (!memcmp(s[i].by, by, 32)) bad += (s[i].state == C_FAIL) - (s[i].state == C_OK);
    for (int i = 0; bad >= 3 && i < n; i++)
        if (s[i].state == C_NEW && !memcmp(s[i].by, by, 32)) s[i].state = C_FAIL;
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

/* A lookup: DHT_ALPHA workers (this thread and DHT_ALPHA - 1 others) each take the closest candidate not asked yet among the want closest
 * that did not fail, ask it, and take the next as soon as it answers: a slow or dead node holds one worker, not the lookup. It ends when
 * those want have all answered, or after LOOKUP_ASK_MAX queries. A candidate in a block that had its queries counts as failed: answers
 * that all point at one network (a victim, or the attacker's own) cannot make this node dial it more. */
typedef struct {
    dht *d;
    const uint8_t *target;
    int want, n, nasked, busy;
    cand s[SHORT_MAX];
    uint8_t blocks[LOOKUP_ASK_MAX][16];
    pthread_mutex_t mu;
    pthread_cond_t cv;
} lookup;

/* the candidate to ask now (mu held), -1 when there is none */
static int lookup_next(lookup *L)
{
    for (int i = 0, live = 0; i < L->n && live < L->want && L->nasked < LOOKUP_ASK_MAX; i++) {
        cand *c = &L->s[i];
        if (c->state == C_NEW && asked_in_block(L->d, &c->c.addr, L->blocks, L->nasked) >= LOOKUP_BLOCK_MAX) c->state = C_FAIL;
        if (c->state == C_FAIL) continue;
        live++;
        if (c->state == C_NEW) return i;
    }
    return -1;
}

static void *lookup_main(void *arg)
{
    lookup *L = *(lookup **)arg;
    uint8_t req[33], ans[NODES_LEN];
    req[0] = DHT_FIND_NODE;
    memcpy(req + 1, L->target, 32);
    pthread_mutex_lock(&L->mu);
    while (!stopping(L->d)) {
        const int i = lookup_next(L);
        if (i < 0) {
            if (!L->busy) break;                                /* nothing to ask, and no answer to wait for: done */
            pthread_cond_wait(&L->cv, &L->mu);
            continue;
        }
        const dht_contact c = L->s[i].c;
        L->s[i].state = C_ASKED;
        net_addr_block(&c.addr, L->blocks[L->nasked++]);
        L->busy++;
        pthread_mutex_unlock(&L->mu);
        dht_contact got[DHT_K];
        int ngot = 0;
        size_t len;
        const bool ok = exchange(L->d, &c, req, sizeof req, ans, sizeof ans, &len) && parse_nodes(L->d, ans, len, got, &ngot);
        heard(L->d, &c, ok);
        pthread_mutex_lock(&L->mu);
        L->busy--;
        for (int j = 0; j < L->n; j++)
            if (!memcmp(L->s[j].c.id, c.id, 32)) L->s[j].state = ok ? C_OK : C_FAIL;
        if (!ok) cand_distrust(L->s, L->n, c.id);
        for (int g = 0; g < ngot; g++)
            if (!is_me(L->d, got[g].id)) cand_add(L->s, &L->n, L->target, &got[g], C_NEW, c.id);
        pthread_cond_broadcast(&L->cv);
    }
    pthread_cond_broadcast(&L->cv);
    pthread_mutex_unlock(&L->mu);
    return NULL;
}

int dht_lookup(dht *d, const uint8_t target[32], dht_contact *out, int want)
{
    lookup *L = calloc(1, sizeof *L), *w[DHT_ALPHA];
    dht_contact seed[DHT_K];
    if (!L) return 0;
    L->d = d, L->target = target, L->want = want < DHT_K ? want : DHT_K;
    pthread_mutex_init(&L->mu, NULL);
    pthread_cond_init(&L->cv, NULL);
    const int ns = rt_closest(d, target, seed, DHT_K);
    for (int i = 0; i < ns; i++) cand_add(L->s, &L->n, target, &seed[i], C_NEW, NULL);
    if (reached(d)) cand_add(L->s, &L->n, target, &d->me, C_OK, NULL);   /* a node the others reach is a node of the table like them */
    const int bk = bucket_of(d->me.id, target);
    if (bk < DHT_BUCKETS) {
        pthread_mutex_lock(&d->rt_mu);
        d->b[bk].looked = sys_now_ms();
        pthread_mutex_unlock(&d->rt_mu);
    }
    for (int i = 0; i < DHT_ALPHA; i++) w[i] = L;
    parallel(lookup_main, w, sizeof *w, DHT_ALPHA);
    int m = 0;
    for (int i = 0; i < L->n && m < L->want; i++)
        if (L->s[i].state == C_OK) out[m++] = L->s[i].c;
    pthread_mutex_destroy(&L->mu);
    pthread_cond_destroy(&L->cv);
    free(L);
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

net_conn *dht_dial(dht *d, const dht_contact *c) { return net_dial(dial_cfg(d), &c->addr, c->xonly); }

/* ---- bootstrap, refresh, peers.txt ---- */

/* peers.txt: the contacts, one per line, "address identity" */
void save_peers(dht *d)
{
    dht_contact *cs = malloc(PEERS_MAX * sizeof *cs);
    char path[4096], tmp[4100];
    int n = 0;
    if (!cs) return;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n && n < PEERS_MAX; j++) cs[n++] = d->b[i].e[j].c;
    pthread_mutex_unlock(&d->rt_mu);
    path_of(d, "peers.txt", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = n ? fopen(tmp, "w") : NULL;                      /* no contact: the old list is kept for the next start */
    if (f) {
        for (int i = 0; i < n; i++) {
            char a[64], h[65];
            net_addr_str(&cs[i].addr, a, sizeof a);
            btc_hex(cs[i].xonly, 32, h);
            fprintf(f, "%s %s\n", a, h);
        }
        if (fclose(f) == 0) (void)sys_replace(tmp, path);
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
    /* the contacts of the last run, with their identities, up to half of what is left: addresses that anyone can announce in the table of
     * BitTorrent do not push them out, nor do they push those out */
    const int half = n + (max - n) / 2;
    char path[4096], line[256];
    path_of(d, "peers.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && n < half && fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        if (strcspn(sp + 1, " \r\n") == 64 && btc_unhex(sp + 1, 64, out[n].xonly) && net_parse_addr(line, &out[n].a)) out[n++].has_id = true;
    }
    if (f) fclose(f);
    pthread_mutex_lock(&d->f_mu);
    for (int i = 0; i < d->nfound && n < max; i++) out[n].a = d->found[i], out[n++].has_id = false;
    pthread_mutex_unlock(&d->f_mu);
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
    p->ok = net_exchange(dial_cfg(p->d), &p->b->a, p->b->has_id ? p->b->xonly : NULL, &req, 1, ans, sizeof ans, &len, remote) && len == 1 &&
            ans[0] == DHT_PONG;
    if (p->ok) {
        memcpy(p->c.xonly, remote, 32);
        keys_node_id(remote, p->c.id);
        p->c.addr = p->b->a;
    }
    return NULL;
}

/* the first contacts, then a lookup of the own ID */
int bootstrap(dht *d)
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
                rt_seen(d, &p[j].c);
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
void refresh(dht *d, bool all)
{
    int64_t looked[DHT_BUCKETS];
    int deepest = -1;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++) {
        if (d->b[i].n) deepest = i;
        looked[i] = d->b[i].looked;
    }
    pthread_mutex_unlock(&d->rt_mu);
    const int64_t now = sys_now_ms();
    const int last = deepest + 1 < DHT_BUCKETS ? deepest + 1 : DHT_BUCKETS - 1;
    for (int i = (int)d->cfg.id_bits; deepest >= 0 && i <= last && !stopping(d); i++) {
        uint8_t t[32];
        dht_contact out[DHT_K];
        if (!all && now - looked[i] < d->refresh_ms) continue;
        if (!sys_random(t, sizeof t)) return;
        for (int b = 0; b <= i; b++) {                         /* the first i bits of this node, then bit i flipped */
            const uint8_t mask = (uint8_t)(0x80 >> (b % 8)), bit = (uint8_t)((d->me.id[b / 8] & mask) ^ (b == i ? mask : 0));
            t[b / 8] = (uint8_t)((t[b / 8] & ~mask) | bit);
        }
        (void)dht_lookup(d, t, out, DHT_K);
    }
}

void dht_refresh(dht *d) { refresh(d, true); }

void dht_seen(dht *d, const dht_contact *c) { rt_seen(d, c); }

int dht_count(dht *d) { return rt_count(d); }

/* The j-th closest of N nodes spread evenly over the IDs lies at about j / (N + 1) of their space from any point: a least-squares fit of
 * the distances of the DHT_K nodes closest to a random point gives N (a lookup that finds fewer knows them all). */
double dht_nodes(dht *d)
{
    uint8_t t[32];
    dht_contact c[DHT_K];
    const unsigned zero = d->cfg.id_bits < 64 ? d->cfg.id_bits : 64;
    if (!sys_random(t, sizeof t)) return 0;
    for (unsigned b = 0; b < zero; b++) t[b / 8] &= (uint8_t)~(0x80 >> (b % 8));   /* in the space of the IDs */
    const int n = dht_lookup(d, t, c, DHT_K);
    if (n < DHT_K) return n;
    long double sjj = 0, sjx = 0;
    for (int j = 0; j < n; j++) {
        long double x = 0;                                     /* the distance, as a share of the space of the IDs */
        for (int b = 0; b < 16; b++) x = x * 256 + (c[j].id[b] ^ t[b]);
        x = ldexpl(x, (int)zero - 128);
        sjj += (long double)(j + 1) * (j + 1), sjx += (j + 1) * x;
    }
    return sjx > 0 ? (double)(sjj / sjx) - 1 : n;
}

int dht_contacts(dht *d, dht_contact *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&d->rt_mu);
    for (int i = 0; i < DHT_BUCKETS; i++)
        for (int j = 0; j < d->b[i].n && n < max; j++) out[n++] = d->b[i].e[j].c;
    pthread_mutex_unlock(&d->rt_mu);
    return n;
}

