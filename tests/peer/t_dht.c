/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_dht.c - a network of N nodes in one process on 127.0.0.1, each bootstrapped from node 0 alone (identities with few zero bits, so that
 * making them is quick). Checked against a brute force over all the nodes: lookups find the true 20 closest; records published by several
 * owners are held by 4 or 5 of their 5 closest nodes. Then the coincidences: a useful one and a merge, whose owners both get on_alert
 * after the proofs of possession, with the pair in their mailboxes; an owner that does not listen, which gets its alert from its
 * mailboxes. Then a session over dht_dial, malformed requests, a node stopped and started again (its store and its peers come back), and
 * the eviction of the oldest contact of a full bucket. */
#include "dht.h"
#include "store.h"
#include <pzcore/hash.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BITS 4                     /* zero bits of a node ID in this network */
#define N 30                       /* nodes that listen */
#define U N                        /* a node that does not listen */
#define E (N + 1)                  /* a node alone, for the eviction */
#define OWNERS 4
#define RECS 2000
#define ROOT "build/peer/t_dht_data"

typedef struct {
    uint8_t x[32], d[32];
    int t;
} point;

typedef struct {
    int idx;
    home_identity id;
    dht *d;
    char dir[64], boot[32];
    const char *bootv[1];
    pthread_mutex_t mu;
    point pts[4];
    int npts, alerts;
    record mine, theirs;
    bool merge;
} node;

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0xd4, 0x7};
static record_net net;
static uint8_t qp[32];             /* q' = k - base, the scalar of Q' (the tests know the key of their puzzle) */
static node nodes[N + 2];
static record recs[RECS];

static int64_t ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void nap(int msec)
{
    const struct timespec ts = {msec / 1000, (long)(msec % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static void hashed(const char *s, uint8_t out[32]) { pz_sha256((const uint8_t *)s, strlen(s), out); }

static bool closer(const uint8_t t[32], const uint8_t a[32], const uint8_t b[32])
{
    for (int i = 0; i < 32; i++)
        if ((a[i] ^ t[i]) != (b[i] ^ t[i])) return (a[i] ^ t[i]) < (b[i] ^ t[i]);
    return false;
}

static int bucket_of(const uint8_t a[32], const uint8_t b[32])
{
    for (int i = 0; i < 256; i++)
        if ((a[i / 8] ^ b[i / 8]) & (0x80 >> (i % 8))) return i;
    return 256;
}

/* the want nodes (of the N that listen) closest to t, by brute force */
static int brute(const uint8_t t[32], int want, int *out)
{
    int n = 0;
    for (int i = 0; i < N; i++) {
        int at = n;
        while (at > 0 && closer(t, nodes[i].id.id, nodes[out[at - 1]].id.id)) at--;
        if (at >= want) continue;
        if (n == want) n--;
        memmove(&out[at + 1], &out[at], (size_t)(n - at) * sizeof *out);
        out[at] = i;
        n++;
    }
    return n;
}

static bool same(const record *a, const record *b)
{
    uint8_t ea[RECORD_LEN], eb[RECORD_LEN];
    record_encode(a, ea), record_encode(b, eb);
    return !memcmp(ea, eb, RECORD_LEN);
}

/* ---- the callbacks of a node ---- */

static void on_alert(void *user, const record *mine, const record *theirs, bool merge)
{
    node *n = user;
    pthread_mutex_lock(&n->mu);
    n->alerts++;
    n->mine = *mine, n->theirs = *theirs, n->merge = merge;
    pthread_mutex_unlock(&n->mu);
}

static bool own_distance(void *user, const uint8_t x[32], int t, uint8_t d[32])
{
    node *n = user;
    bool ok = false;
    pthread_mutex_lock(&n->mu);
    for (int i = 0; i < n->npts && !ok; i++)
        if (!memcmp(n->pts[i].x, x, 32) && n->pts[i].t == t) memcpy(d, n->pts[i].d, 32), ok = true;
    pthread_mutex_unlock(&n->mu);
    return ok;
}

/* a session: the first frame comes back, then the requests of the table are answered on the same connection until the other side closes */
static void on_session(void *user, net_conn *c, const uint8_t *msg, size_t len)
{
    node *n = user;
    uint8_t buf[512];
    size_t l;
    if (net_send(c, msg, len))
        while (net_recv(c, buf, sizeof buf, &l) && l && buf[0] < DHT_SESSION_FIRST && dht_serve(n->d, c, buf, l)) {}
    net_close(c);
}

/* ---- nodes ---- */

static void clean_dir(const char *dir)
{
    static const char *const FILES[] = {"store.bin", "own.bin", "peers.txt", "peers.txt.tmp"};
    char p[128];
    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, FILES[i]);
        remove(p);
    }
    rmdir(dir);
}

/* port: 0 for no listening, DHT_ANY_PORT or the port; boot: the index of the bootstrap node, -1 for none */
static bool start_node(node *n, int port, int boot)
{
    char err[200];
    dht_config cfg = {
        .ctx = ctx, .net = &net, .self = &n->id, .id_bits = BITS, .listen_host = "127.0.0.1", .listen_port = port, .dir = n->dir,
        .on_alert = on_alert, .own_distance = own_distance, .on_session = on_session, .user = n, .timeout_ms = 2000, .handlers = 4,
        .mbox_poll_s = port ? 0 : 1, .retry_s = 1, .verbose = getenv("T_DHT_VERBOSE") != NULL,
    };
    if (boot >= 0) {
        snprintf(n->boot, sizeof n->boot, "127.0.0.1:%u", dht_port(nodes[boot].d));
        n->bootv[0] = n->boot;
        cfg.bootstrap = n->bootv;
        cfg.nbootstrap = 1;
    }
    n->d = dht_create(&cfg, err, sizeof err);
    if (!n->d) printf("t_dht: %s\n", err);
    const int got = n->d ? dht_start(n->d) : -1;
    return got >= (boot >= 0 ? 1 : 0);
}

static void *refresh_main(void *arg)
{
    dht_refresh(((node *)arg)->d);
    return NULL;
}

static void *stop_main(void *arg)
{
    node *n = arg;
    dht_stop(n->d);
    n->d = NULL;
    return NULL;
}

/* on every node at once, each on a thread */
static void all(void *(*fn)(void *), int count)
{
    pthread_t th[N + 2];
    for (int i = 0; i < count; i++) CHECK(pthread_create(&th[i], NULL, fn, &nodes[i]) == 0);
    for (int i = 0; i < count; i++) pthread_join(th[i], NULL);
}

static int alerts_of(node *n, record *mine, record *theirs, bool *merge)
{
    pthread_mutex_lock(&n->mu);
    const int a = n->alerts;
    if (mine) *mine = n->mine, *theirs = n->theirs, *merge = n->merge;
    pthread_mutex_unlock(&n->mu);
    return a;
}

/* the x of the even-y point d*G (d is negated when its point has an odd y) */
static void even_point(uint8_t d[32], uint8_t x[32])
{
    secp256k1_pubkey p;
    uint8_t c[33];
    size_t n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, d) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &p, SECP256K1_EC_COMPRESSED));
    if (c[0] == 3) {
        CHECK(secp256k1_ec_seckey_negate(ctx, d));
        CHECK(secp256k1_ec_pubkey_create(ctx, &p, d) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &p, SECP256K1_EC_COMPRESSED));
    }
    memcpy(x, c + 1, 32);
}

/* the point (t, x) with distance d becomes one of node n's; the distance must fit (a proof made with it checks out) */
static void give_point(node *n, int t, const uint8_t x[32], const uint8_t d[32])
{
    record r = {.t = t, .seq = 1};
    uint8_t nonce[32] = {1}, sig[64];
    memcpy(r.x, x, 32);
    memcpy(r.owner, n->id.xonly, 32);
    CHECK(record_prove(ctx, &net, &r, d, nodes[0].id.xonly, nonce, sig));
    pthread_mutex_lock(&n->mu);
    point *p = &n->pts[n->npts++];
    memcpy(p->x, x, 32), memcpy(p->d, d, 32);
    p->t = t;
    pthread_mutex_unlock(&n->mu);
}

static bool publish_one(node *n, int t, const uint8_t x[32], record *out)
{
    out->t = t;
    memcpy(out->x, x, 32);
    return dht_publish(n->d, out, 1) == 1;
}

/* ---- the checks ---- */

static void test_lookups(void)
{
    int bad = 0;
    for (int k = 0; k < 40; k++) {
        char label[32];
        uint8_t t[32];
        dht_contact got[DHT_K];
        int want[DHT_K];
        snprintf(label, sizeof label, "target %d", k);
        hashed(label, t);
        if (k >= 30) memcpy(t, nodes[k - 30].id.id, 32);       /* also the IDs of nodes themselves */
        const int n = dht_lookup(nodes[(k * 7) % N].d, t, got, DHT_K);
        const int m = brute(t, DHT_K, want);
        bool ok = n == m;
        for (int i = 0; i < n && ok; i++) ok = !memcmp(got[i].id, nodes[want[i]].id.id, 32);
        bad += !ok;
    }
    CHECK(bad == 0);
    if (bad) printf("t_dht: %d of 40 lookups not the true 20 closest\n", bad);
}

typedef struct {
    node *n;
    record *r;
    int count, held;
} pub_job;

static void *publish_main(void *arg)
{
    pub_job *j = arg;
    for (int o = 0; o < j->count; o += 100) j->held += dht_publish(j->n->d, j->r + o, j->count - o < 100 ? j->count - o : 100);
    return NULL;
}

static void test_records(void)
{
    pthread_t th[OWNERS];
    pub_job jobs[OWNERS];
    for (int i = 0; i < RECS; i++) {
        char label[32];
        uint8_t k[32];
        secp256k1_pubkey p;
        uint8_t c[33];
        size_t n = 33;
        snprintf(label, sizeof label, "record %d", i);
        hashed(label, k);
        CHECK(secp256k1_ec_pubkey_create(ctx, &p, k) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &p, SECP256K1_EC_COMPRESSED));
        memcpy(recs[i].x, c + 1, 32);
        recs[i].t = i % 3 - 1;
    }
    for (int o = 0; o < OWNERS; o++) {
        jobs[o] = (pub_job){&nodes[1 + o], recs + o * (RECS / OWNERS), RECS / OWNERS, 0};
        CHECK(pthread_create(&th[o], NULL, publish_main, &jobs[o]) == 0);
    }
    for (int o = 0; o < OWNERS; o++) {
        pthread_join(th[o], NULL);
        CHECK(jobs[o].held == RECS / OWNERS);
    }
    int by_count[DHT_KSTORE + 1] = {0};
    for (int i = 0; i < RECS; i++) {
        uint8_t k[32];
        int near[DHT_KSTORE], held = 0;
        store_key(recs[i].x, k);
        CHECK(record_check(ctx, &net, &recs[i]));               /* dht_publish signed it */
        const int m = brute(k, DHT_KSTORE, near);
        for (int j = 0; j < m; j++) {
            record got[4];
            const int g = dht_local_records(nodes[near[j]].d, recs[i].x, got, 4);
            for (int h = 0; h < g; h++) held += same(&got[h], &recs[i]);
        }
        by_count[held]++;
    }
    printf("t_dht: %d records held by 5 of their 5 closest nodes, %d by 4, %d by fewer\n", by_count[5], by_count[4],
           RECS - by_count[5] - by_count[4]);
    CHECK(by_count[5] + by_count[4] == RECS);
}

/* the 5 mailboxes of an owner that hold the pair */
static int in_mailboxes(const uint8_t owner[32], const record *a, const record *b)
{
    uint8_t k[32];
    int near[DHT_KSTORE], n = 0;
    dht_mbox_key(owner, k);
    const int m = brute(k, DHT_KSTORE, near);
    for (int j = 0; j < m; j++) {
        record pairs[8][2];
        const int g = dht_local_mbox(nodes[near[j]].d, owner, pairs, 8);
        bool has = false;
        for (int h = 0; h < g; h++) has = has || (same(&pairs[h][0], a) && same(&pairs[h][1], b)) || (same(&pairs[h][0], b) && same(&pairs[h][1], a));
        n += has;
    }
    return n;
}

static void test_coincidences(void)
{
    /* useful: A tame (R = dA*G), B wild + (R = Q' + dB*G, dB = dA - q'). merge: C and D tame with the same distance. U, which does not
     * listen, tame (R = dU*G), and V wild - (R = -Q' + dV*G, dV = dU + q'). */
    node *A = &nodes[5], *B = &nodes[9], *C = &nodes[11], *D = &nodes[13], *Un = &nodes[U], *V = &nodes[15];
    uint8_t dA[32], dB[32], xA[32], dC[32], xC[32], dU[32], dV[32], xU[32], nq[32];
    hashed("useful", dA), hashed("merge", dC), hashed("unreachable", dU);
    even_point(dA, xA), even_point(dC, xC), even_point(dU, xU);
    memcpy(nq, qp, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nq));
    memcpy(dB, dA, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, dB, nq));
    memcpy(dV, dU, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, dV, qp));
    give_point(A, 0, xA, dA), give_point(B, 1, xA, dB);
    give_point(C, 0, xC, dC), give_point(D, 0, xC, dC);
    give_point(Un, 0, xU, dU), give_point(V, -1, xU, dV);
    record rA, rB, rC, rD, rU, rV;
    CHECK(publish_one(A, 0, xA, &rA) && publish_one(C, 0, xC, &rC) && publish_one(Un, 0, xU, &rU));
    CHECK(publish_one(B, 1, xA, &rB) && publish_one(D, 0, xC, &rD) && publish_one(V, -1, xU, &rV));
    const int64_t t0 = ms();
    node *const want[5] = {A, B, C, D, Un};
    for (int done = 0; !done && ms() - t0 < 30000; nap(50)) {
        done = 1;
        for (int i = 0; i < 5; i++) done = done && alerts_of(want[i], NULL, NULL, NULL) > 0;
    }
    record mine, theirs;
    bool merge;
    CHECK(alerts_of(A, &mine, &theirs, &merge) == 1 && same(&mine, &rA) && same(&theirs, &rB) && !merge);
    CHECK(alerts_of(B, &mine, &theirs, &merge) == 1 && same(&mine, &rB) && same(&theirs, &rA) && !merge);
    CHECK(alerts_of(C, &mine, &theirs, &merge) == 1 && same(&mine, &rC) && same(&theirs, &rD) && merge);
    CHECK(alerts_of(D, &mine, &theirs, &merge) == 1 && same(&mine, &rD) && same(&theirs, &rC) && merge);
    CHECK(alerts_of(Un, &mine, &theirs, &merge) == 1 && same(&mine, &rU) && same(&theirs, &rV) && !merge);
    printf("t_dht: the five alerts in %.1f s\n", (double)(ms() - t0) / 1000);
    /* the mailboxes of each owner have the pair (the custodians send it there after the owners) */
    const int64_t t1 = ms();
    int mbA = 0, mbB = 0, mbU = 0, mbV = 0;
    for (; ms() - t1 < 15000; nap(50)) {
        mbA = in_mailboxes(A->id.xonly, &rA, &rB), mbB = in_mailboxes(B->id.xonly, &rA, &rB);
        mbU = in_mailboxes(Un->id.xonly, &rU, &rV), mbV = in_mailboxes(V->id.xonly, &rU, &rV);
        if (mbA == DHT_KSTORE && mbB == DHT_KSTORE && mbU == DHT_KSTORE && mbV == DHT_KSTORE) break;
    }
    CHECK(mbA >= 4 && mbB >= 4 && mbU >= 4 && mbV >= 4);
    /* V reached U only through the mailboxes: it cannot dial U for its proof, so its alert waits (the session will come from U) */
    CHECK(alerts_of(V, NULL, NULL, NULL) == 0);
    /* a proof asked by hand, and one of a record whose owner cannot prove it (it has no such point) */
    CHECK(dht_ask_proof(nodes[20].d, &rB) == 1 && dht_ask_proof(A->d, &rA) == 1);
    record fake = rA;
    fake.t = 1;
    memcpy(fake.owner, nodes[3].id.xonly, 32);
    CHECK(dht_ask_proof(nodes[20].d, &fake) == -1);             /* not its record (unsigned by it): it closes without a proof */
    CHECK(dht_ask_proof(nodes[20].d, &rU) == -1);               /* U does not listen */

    /* a session to B: the first frame comes back, then a proof over the same connection */
    dht_contact cb;
    CHECK(dht_find(A->d, B->id.xonly, &cb) && !memcmp(cb.id, B->id.id, 32));
    net_conn *c = dht_dial(A->d, &cb);
    CHECK(c != NULL);
    if (c) {
        const uint8_t hello[3] = {DHT_SESSION_FIRST, 'h', 'i'};
        uint8_t back[8];
        size_t n;
        CHECK(net_send(c, hello, 3) && net_recv(c, back, sizeof back, &n) && n == 3 && !memcmp(back, hello, 3));
        CHECK(dht_proof_on(A->d, c, &rB) == 1);
        CHECK(dht_proof_on(A->d, c, &rA) == 0);                 /* not the owner at the other end */
        net_close(c);
    }
}

static void test_malformed(void)
{
    home_identity cid;
    uint8_t seed[32];
    hashed("a client", seed);
    CHECK(keys_identity(ctx, seed, FP, BITS, &cid));
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(nodes[0].d);
    uint8_t req[1 + 2 * RECORD_LEN + 8] = {0}, ans[2 + DHT_MBOX_MAX * 2 * RECORD_LEN];
    size_t n;
    req[0] = DHT_PING;
    CHECK(net_exchange(&cli, &a, nodes[0].id.xonly, req, 1, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_PONG);
    CHECK(!net_exchange(&cli, &a, NULL, req, 2, ans, sizeof ans, &n, NULL));                 /* PING with a byte too many */
    req[0] = DHT_FIND_NODE;
    CHECK(!net_exchange(&cli, &a, NULL, req, 32, ans, sizeof ans, &n, NULL));                /* a target one byte short */
    req[0] = 200;
    CHECK(!net_exchange(&cli, &a, NULL, req, 1, ans, sizeof ans, &n, NULL));                 /* an unknown type */
    req[0] = DHT_STORE, req[1] = 2;
    record_encode(&recs[0], req + 2);
    CHECK(!net_exchange(&cli, &a, NULL, req, 2 + RECORD_LEN, ans, sizeof ans, &n, NULL));    /* two records announced, one sent */
    req[1] = 0;
    CHECK(!net_exchange(&cli, &a, NULL, req, 2, ans, sizeof ans, &n, NULL));                 /* none */
    req[0] = DHT_ALERT;
    record_encode(&recs[0], req + 1);
    record_encode(&recs[1], req + 1 + RECORD_LEN);
    CHECK(!net_exchange(&cli, &a, NULL, req, 1 + 2 * RECORD_LEN, ans, sizeof ans, &n, NULL));   /* two records that are no coincidence */
    req[0] = DHT_MBOX_GET;
    CHECK(!net_exchange(&cli, &a, NULL, req, 34, ans, sizeof ans, &n, NULL));
    req[0] = DHT_SESSION_FIRST + 1;
    CHECK(net_exchange(&cli, &a, NULL, req, 5, ans, sizeof ans, &n, NULL) && n == 5);        /* a session: the test's echo */
    /* well formed, refused record by record: a changed signature, an owner whose node ID has not the zero bits */
    record bad = recs[0];
    bad.sig[9] ^= 4;
    req[0] = DHT_STORE, req[1] = 2;
    record_encode(&bad, req + 2);
    home_identity nob;
    for (int i = 0;; i++) {
        char label[32];
        snprintf(label, sizeof label, "no bits %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, 0, &nob));
        if (!keys_id_ok(nob.id, BITS)) break;
    }
    record r2 = {.t = 0, .seq = 7};
    memcpy(r2.x, recs[5].x, 32);
    memcpy(r2.owner, nob.xonly, 32);
    CHECK(record_sign(ctx, &net, &nob.kp, &r2) && record_check(ctx, &net, &r2));
    record_encode(&r2, req + 2 + RECORD_LEN);
    uint8_t big[2 + 2 * RECORD_LEN];
    memcpy(big, req, sizeof big);
    CHECK(net_exchange(&cli, &a, NULL, big, sizeof big, ans, sizeof ans, &n, NULL) && n == 4 && ans[0] == DHT_STORED && ans[1] == 2 &&
          ans[2] == DHT_ST_INVALID && ans[3] == DHT_ST_INVALID);
}

static void test_restart(void)
{
    node *n = &nodes[7];
    const uint64_t count = dht_local_count(n->d);
    const int port = dht_port(n->d);
    dht_contact cs[256];
    CHECK(count > 0);
    dht_stop(n->d);
    n->d = NULL;
    const int64_t t0 = ms();
    CHECK(start_node(n, port, -1));                             /* no bootstrap address: peers.txt alone */
    CHECK(dht_local_count(n->d) == count);
    const int nc = dht_contacts(n->d, cs, 256);
    CHECK(nc >= 10);
    uint8_t t[32];
    dht_contact got[DHT_K];
    int want[DHT_K];
    hashed("after the restart", t);
    const int g = dht_lookup(n->d, t, got, DHT_K);
    CHECK(g == brute(t, DHT_K, want));
    for (int i = 0; i < g; i++) CHECK(!memcmp(got[i].id, nodes[want[i]].id.id, 32));
    printf("t_dht: node 7 restarted in %.1f s with its %llu records and %d contacts\n", (double)(ms() - t0) / 1000,
           (unsigned long long)count, nc);
}

static bool has(const dht_contact *cs, int n, const uint8_t id[32], int *at)
{
    for (int i = 0; i < n; i++)
        if (!memcmp(cs[i].id, id, 32)) {
            if (at) *at = i;
            return true;
        }
    return false;
}

/* A full bucket: the oldest contact is pinged; alive, it stays (moved to the newest) and the newcomer is dropped; dead, it goes. */
static void test_eviction(void)
{
    node *e = &nodes[E];
    CHECK(start_node(e, DHT_ANY_PORT, -1));
    if (!e->d) return;
    const int b = BITS;                                         /* the bucket of the IDs whose first bit after the zero bits differs */
    int r = -1;
    for (int i = 0; i < N && r < 0; i++)
        if (bucket_of(e->id.id, nodes[i].id.id) == b) r = i;
    CHECK(r >= 0);
    if (r < 0) return;
    dht_contact live, fake[DHT_K + 2];
    memcpy(live.id, nodes[r].id.id, 32), memcpy(live.xonly, nodes[r].id.xonly, 32);
    CHECK(net_parse_addr("127.0.0.1:1", &live.addr));
    live.addr.port = dht_port(nodes[r].d);
    for (int i = 0, k = 0; k < DHT_K + 1; i++) {                /* identities in that bucket, at an address where nobody listens */
        char label[32];
        uint8_t seed[32];
        home_identity id;
        snprintf(label, sizeof label, "fake %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, BITS, &id));
        if (bucket_of(e->id.id, id.id) != b) continue;
        memcpy(fake[k].id, id.id, 32), memcpy(fake[k].xonly, id.xonly, 32);
        CHECK(net_parse_addr("127.0.0.1:1", &fake[k].addr));
        k++;
    }
    dht_seen(e->d, &live);
    for (int i = 0; i < DHT_K - 1; i++) dht_seen(e->d, &fake[i]);
    dht_contact cs[DHT_K + 4];
    CHECK(dht_contacts(e->d, cs, DHT_K + 4) == DHT_K && !memcmp(cs[0].id, live.id, 32));   /* full, the live one oldest */
    dht_seen(e->d, &fake[DHT_K - 1]);                           /* the live one is pinged, answers, and stays */
    int n = 0, at = -1;
    for (const int64_t t0 = ms(); ms() - t0 < 5000; nap(20)) {
        n = dht_contacts(e->d, cs, DHT_K + 4);
        if (has(cs, n, live.id, &at) && at == n - 1) break;
    }
    CHECK(n == DHT_K && at == DHT_K - 1 && !has(cs, n, fake[DHT_K - 1].id, NULL));
    dht_seen(e->d, &fake[DHT_K]);                               /* now the oldest is fake 0: pinged, no answer, replaced */
    for (const int64_t t0 = ms(); ms() - t0 < 5000; nap(20)) {
        n = dht_contacts(e->d, cs, DHT_K + 4);
        if (has(cs, n, fake[DHT_K].id, NULL)) break;
    }
    CHECK(n == DHT_K && has(cs, n, fake[DHT_K].id, NULL) && !has(cs, n, fake[0].id, NULL) && has(cs, n, live.id, NULL));
}

int main(void)
{
    const int64_t t_start = ms();
    setvbuf(stdout, NULL, _IOLBF, 0);
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t k[32], base[32] = {0}, p[33], nb[32];
    secp256k1_pubkey pk;
    size_t len = 33;
    hashed("dht puzzle", k);
    base[30] = 0x01;
    CHECK(secp256k1_ec_pubkey_create(ctx, &pk, k) && secp256k1_ec_pubkey_serialize(ctx, p, &len, &pk, SECP256K1_EC_COMPRESSED));
    CHECK(record_net_init(ctx, &net, FP, 0, p, base));
    memcpy(qp, k, 32), memcpy(nb, base, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nb) && secp256k1_ec_seckey_tweak_add(ctx, qp, nb));
    mkdir("build/peer", 0755);
    mkdir(ROOT, 0755);
    for (int i = 0; i < N + 2; i++) {
        char label[32];
        uint8_t seed[32];
        node *n = &nodes[i];
        n->idx = i;
        pthread_mutex_init(&n->mu, NULL);
        snprintf(label, sizeof label, "node %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, BITS, &n->id));
        snprintf(n->dir, sizeof n->dir, ROOT "/%02d", i);
        clean_dir(n->dir);
    }
    /* node 0 alone, then each of the others from node 0 */
    int64_t t0 = ms();
    CHECK(start_node(&nodes[0], DHT_ANY_PORT, -1));
    for (int i = 1; i < N && !fails; i++) CHECK(start_node(&nodes[i], DHT_ANY_PORT, 0));
    if (!fails) CHECK(start_node(&nodes[U], 0, 0) && dht_port(nodes[U].d) == 0);
    if (fails) return 1;
    all(refresh_main, N);
    printf("t_dht: %d nodes up and refreshed in %.1f s\n", N + 1, (double)(ms() - t0) / 1000);
    if (fails) return 1;
    t0 = ms();
    test_lookups();
    printf("t_dht: lookups in %.1f s\n", (double)(ms() - t0) / 1000);
    t0 = ms();
    test_records();
    printf("t_dht: %d records published by %d owners in %.1f s\n", RECS, OWNERS, (double)(ms() - t0) / 1000);
    test_coincidences();
    test_malformed();
    test_restart();
    test_eviction();
    t0 = ms();
    all(stop_main, N + 2);
    printf("t_dht: stopped in %.1f s\n", (double)(ms() - t0) / 1000);
    for (int i = 0; i < N + 2; i++) {
        clean_dir(nodes[i].dir);
        pthread_mutex_destroy(&nodes[i].mu);
    }
    rmdir(ROOT);
    secp256k1_context_destroy(ctx);
    if (fails) printf("t_dht: %d FAILED\n", fails);
    else printf("t_dht: OK (%.1f s)\n", (double)(ms() - t_start) / 1000);
    return fails != 0;
}
