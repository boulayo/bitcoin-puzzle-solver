/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_table.c - the routing table (docs/en/NETWORK.md 7.4): the buckets and the limits by address block in them, the exchanges with a
 * contact and what the table learns from them, NODES, the iterative lookups, the bootstrap, the refresh of the buckets, peers.txt. */
#include "dht_priv.h"
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
void rt_seen(dht *d, const dht_contact *c, bool answered)
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

void rt_failed(dht *d, const uint8_t id[32])
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
    net_conn *k = net_dial(&d->ncfg, &c->addr, c->xonly);
    const bool ok = k && net_send(k, req, rlen) && net_recv(k, ans, cap, alen) && *alen >= 1 && ans[0] == req[0] + 1;
    net_close(k);
    return ok;
}

/* what the routing table learns from an exchange whose answer was checked */
void heard(dht *d, const dht_contact *c, bool ok)
{
    if (ok) rt_seen(d, c, true);
    else rt_failed(d, c->id);
}

void job_ping(dht *d, const dht_contact *weak)
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

/* peers.txt: the contacts that answered, one per line, "address identity" */
void save_peers(dht *d)
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

