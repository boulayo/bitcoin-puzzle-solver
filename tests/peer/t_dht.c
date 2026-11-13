/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_dht.c - a network of N nodes in one process on 127.0.0.1, each bootstrapped from node 0 alone (identities with few zero bits, so that
 * making them is quick). Checked against a brute force over all the nodes: lookups find the true 20 closest; records published by several
 * owners are held by 4 or 5 of their 5 closest nodes. Then the coincidences: a useful one and a merge, whose owners both get on_alert
 * after the proofs of possession, with the pair in their mailboxes; an owner that does not listen, which gets its alert from its
 * mailboxes, and its partner, which gets that owner's proof in an ALERT. Then a session over dht_dial, malformed requests (and records
 * refused: of another owner, far from the custodian), a node stopped and started again (its store, its peers and its sequence number come
 * back), the eviction of the oldest contact of a full bucket. The limits: a public network refuses loopback, a NODES answer aimed at one
 * address or block is refused, the table keeps few contacts per block and answers an address once, idle connections hold few handlers,
 * a flood of alerts that will never be proved does not shut out a real one. The relay: two homes that do not listen get their alerts
 * through their mailboxes and run a stand-in session through a relay; a relay's limits (a wait without partner, a third party, relays
 * at once, bytes); a relay stopped while it copies. */
#include "dht.h"
#include "store.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fails;
static unsigned probe_s;           /* of the next node made (0: the default) */
static int timeout_ms = 2000;      /* of the next node made */
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BITS 4                     /* zero bits of a node ID in this network */
#define N 30                       /* nodes that listen */
#define U N                        /* a node that does not listen */
#define E (N + 1)                  /* a node alone, for the eviction */
#define W (N + 2)                  /* another node that does not listen: U and W meet through a relay */
#define NODES (N + 3)
#define OWNERS 4
#define RECS 2000
#define ROOT PZ_TEST_DIR "/t_dht_data"

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
static node nodes[NODES];
static record recs[RECS];

static int64_t ms(void) { return sys_now_ms(); }
static void nap(int msec) { sys_sleep_ms(msec); }

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
    static const char *const FILES[] = {"store.bin", "own.bin", "peers.txt", "peers.txt.tmp", "own.seq", "own.seq.tmp"};
    char p[128];
    for (size_t i = 0; i < sizeof FILES / sizeof *FILES; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, FILES[i]);
        remove(p);
    }
    rmdir(dir);
}

/* port: 0 for no listening, DHT_ANY_PORT or the port; boot: the index of the bootstrap node, -1 for none. A test network: loopback
 * addresses are taken; slots of relays of one second; two relays at once, of 8 MiB each. */
static dht_config node_cfg(node *n, int port)
{
    const dht_config cfg = {
        .ctx = ctx, .net = &net, .self = &n->id, .id_bits = BITS, .listen_host = "127.0.0.1", .listen_port = port, .dir = n->dir,
        .on_alert = on_alert, .own_distance = own_distance, .on_session = on_session, .user = n, .timeout_ms = timeout_ms, .handlers = 4,
        .mbox_poll_s = port ? 0 : 1, .retry_s = 1, .allow_private = true, .relay_max = 2, .relay_bytes = 8u << 20, .relay_slot_s = 1,
        .probe_s = probe_s, .verbose = getenv("T_DHT_VERBOSE") != NULL,
    };
    return cfg;
}

static bool start_node(node *n, int port, int boot)
{
    char err[200];
    dht_config cfg = node_cfg(n, port);
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
    pthread_t th[NODES];
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
        int who[DHT_KSTORE], held = 0;
        store_key(recs[i].x, k);
        CHECK(record_check(ctx, &net, &recs[i]));               /* dht_publish signed it */
        const int m = brute(k, DHT_KSTORE, who);
        for (int j = 0; j < m; j++) {
            record got[4];
            const int g = dht_local_records(nodes[who[j]].d, recs[i].x, got, 4);
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
    int who[DHT_KSTORE], n = 0;
    dht_mbox_key(owner, k);
    const int m = brute(k, DHT_KSTORE, who);
    for (int j = 0; j < m; j++) {
        mbox_pair pairs[8];
        const int g = dht_local_mbox(nodes[who[j]].d, owner, pairs, 8);
        bool has = false;
        for (int h = 0; h < g; h++) has = has || (same(&pairs[h].r[0], a) && same(&pairs[h].r[1], b)) || (same(&pairs[h].r[0], b) && same(&pairs[h].r[1], a));
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
    node *const want[6] = {A, B, C, D, Un, V};
    for (int done = 0; !done && ms() - t0 < 30000; nap(50)) {
        done = 1;
        for (int i = 0; i < 6; i++) done = done && alerts_of(want[i], NULL, NULL, NULL) > 0;
    }
    record mine, theirs;
    bool merge;
    CHECK(alerts_of(A, &mine, &theirs, &merge) == 1 && same(&mine, &rA) && same(&theirs, &rB) && !merge);
    CHECK(alerts_of(B, &mine, &theirs, &merge) == 1 && same(&mine, &rB) && same(&theirs, &rA) && !merge);
    CHECK(alerts_of(C, &mine, &theirs, &merge) == 1 && same(&mine, &rC) && same(&theirs, &rD) && merge);
    CHECK(alerts_of(D, &mine, &theirs, &merge) == 1 && same(&mine, &rD) && same(&theirs, &rC) && merge);
    CHECK(alerts_of(Un, &mine, &theirs, &merge) == 1 && same(&mine, &rU) && same(&theirs, &rV) && !merge);
    /* V cannot dial U for its proof: U, which can dial V, sent its own with its ALERT (before, V's alert waited for a day) */
    CHECK(alerts_of(V, &mine, &theirs, &merge) == 1 && same(&mine, &rV) && same(&theirs, &rU) && !merge);
    printf("t_dht: the six alerts in %.1f s\n", (double)(ms() - t0) / 1000);
    /* the mailboxes of each owner have the pair (the custodians send it there after the owners) */
    const int64_t t1 = ms();
    int mbA = 0, mbB = 0, mbU = 0, mbV = 0;
    for (; ms() - t1 < 15000; nap(50)) {
        mbA = in_mailboxes(A->id.xonly, &rA, &rB), mbB = in_mailboxes(B->id.xonly, &rA, &rB);
        mbU = in_mailboxes(Un->id.xonly, &rU, &rV), mbV = in_mailboxes(V->id.xonly, &rU, &rV);
        if (mbA == DHT_KSTORE && mbB == DHT_KSTORE && mbU == DHT_KSTORE && mbV == DHT_KSTORE) break;
    }
    CHECK(mbA >= 4 && mbB >= 4 && mbU >= 4 && mbV >= 4);
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
    /* well formed, refused record by record: a changed signature, an owner whose node ID has not the zero bits (sent by that owner),
     * a record of another owner (an owner stores its own: before, anyone could), a record of the sender whose key is far from node 0
     * (node 0 knows many nodes closer to it: before, it was stored); and one node 0 is the closest to, stored */
    home_identity nob;
    for (int i = 0;; i++) {
        char label[32];
        snprintf(label, sizeof label, "no bits %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, 0, &nob));
        if (!keys_id_ok(nob.id, BITS)) break;
    }
    record mk[3];                                              /* of cid: a broken signature, far from node 0, near node 0 */
    for (int i = 0, got = 0; got < 3; i++) {
        char label[32];
        uint8_t k[32], kk[32], x[32];
        secp256k1_pubkey p;
        uint8_t c33[33];
        size_t l33 = 33;
        snprintf(label, sizeof label, "cid point %d", i);
        hashed(label, k);
        CHECK(secp256k1_ec_pubkey_create(ctx, &p, k) && secp256k1_ec_pubkey_serialize(ctx, c33, &l33, &p, SECP256K1_EC_COMPRESSED));
        memcpy(x, c33 + 1, 32);
        store_key(x, kk);
        int nearer = 0;                                         /* the nodes closer to the key than node 0 */
        for (int j = 1; j < N; j++) nearer += closer(kk, nodes[j].id.id, nodes[0].id.id);
        const int want = got == 1;                              /* near, far, near: the signature is checked after the cheap checks */
        if ((want == 1 && nearer < 15) || (want == 0 && nearer != 0)) continue;
        record r = {.t = 0, .seq = 1000 + (uint64_t)got};
        memcpy(r.x, x, 32);
        memcpy(r.owner, cid.xonly, 32);
        CHECK(record_sign(ctx, &net, &cid.kp, &r));
        mk[got++] = r;
    }
    mk[0].sig[9] ^= 4;
    uint8_t big[2 + 3 * RECORD_LEN];
    big[0] = DHT_STORE, big[1] = 3;
    record_encode(&recs[0], big + 2);
    record_encode(&mk[1], big + 2 + RECORD_LEN);
    record_encode(&mk[2], big + 2 + 2 * RECORD_LEN);
    CHECK(net_exchange(&cli, &a, NULL, big, sizeof big, ans, sizeof ans, &n, NULL) && n == 5 && ans[0] == DHT_STORED && ans[1] == 3 &&
          ans[2] == DHT_ST_INVALID && ans[3] == DHT_ST_FAR && ans[4] == DHT_ST_STORED);
    record r2 = {.t = 0, .seq = 7};
    memcpy(r2.x, recs[5].x, 32);
    memcpy(r2.owner, nob.xonly, 32);
    CHECK(record_sign(ctx, &net, &nob.kp, &r2) && record_check(ctx, &net, &r2));
    big[1] = 2;
    record_encode(&mk[0], big + 2);
    record_encode(&r2, big + 2 + RECORD_LEN);
    CHECK(net_exchange(&cli, &a, NULL, big, 2 + 2 * RECORD_LEN, ans, sizeof ans, &n, NULL) && n == 4 && ans[2] == DHT_ST_INVALID &&
          ans[3] == DHT_ST_INVALID);
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

/* A full bucket: the oldest contact is pinged; alive, it stays (moved to the newest) and the newcomer is dropped; dead, it goes, and the
 * newcomer takes its place when it answers a ping of its own. */
static void test_eviction(void)
{
    node *e = &nodes[E];
    CHECK(start_node(e, DHT_ANY_PORT, -1));
    if (!e->d) return;
    const int b = BITS;                                         /* the bucket of the IDs whose first bit after the zero bits differs */
    int r = -1, r2 = -1;                                        /* two nodes of that bucket */
    for (int i = 0; i < N && r2 < 0; i++)
        if (bucket_of(e->id.id, nodes[i].id.id) == b) r2 = r < 0 ? -1 : i, r = r < 0 ? i : r;
    CHECK(r2 >= 0);
    if (r2 < 0) return;
    dht_contact live, live2, fake[DHT_K + 2];
    memcpy(live.id, nodes[r].id.id, 32), memcpy(live.xonly, nodes[r].id.xonly, 32);
    CHECK(net_parse_addr("127.0.0.1:1", &live.addr));
    live2 = live;
    memcpy(live2.id, nodes[r2].id.id, 32), memcpy(live2.xonly, nodes[r2].id.xonly, 32);
    live.addr.port = dht_port(nodes[r].d), live2.addr.port = dht_port(nodes[r2].d);
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
    dht_seen(e->d, &live2);                                     /* now the oldest is fake 0: pinged, no answer, replaced */
    for (const int64_t t0 = ms(); ms() - t0 < 5000; nap(20)) {
        n = dht_contacts(e->d, cs, DHT_K + 4);
        if (has(cs, n, live2.id, NULL)) break;
    }
    CHECK(n == DHT_K && has(cs, n, live2.id, NULL) && !has(cs, n, fake[0].id, NULL) && has(cs, n, live.id, NULL));
    dht_seen(e->d, &fake[DHT_K]);                               /* fake 1 goes the same way, for a newcomer that does not answer */
    for (const int64_t t0 = ms(); ms() - t0 < 5000 && n == DHT_K; nap(20)) n = dht_contacts(e->d, cs, DHT_K + 4);
    nap(300);
    n = dht_contacts(e->d, cs, DHT_K + 4);
    CHECK(n == DHT_K - 1 && !has(cs, n, fake[1].id, NULL) && !has(cs, n, fake[DHT_K].id, NULL));
}

/* ---- the limits ---- */

static void make_id(const char *label, home_identity *id)
{
    uint8_t seed[32];
    hashed(label, seed);
    CHECK(keys_identity(ctx, seed, FP, BITS, id));
}

/* a node of its own, with no callbacks: a public network's (allow_private false) or a test network's; started when port is not 0 */
static dht *scratch(const char *name, bool allow_private, int port, int boot, home_identity *id)
{
    char err[200], dir[64];
    static char bootv[8][32];
    static const char *bp[8];
    static int nb;
    node tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(dir, sizeof dir, ROOT "/%s", name);
    clean_dir(dir);
    make_id(name, id);
    tmp.id = *id;
    snprintf(tmp.dir, sizeof tmp.dir, "%s", dir);
    dht_config cfg = node_cfg(&tmp, port);
    cfg.self = id;
    cfg.on_alert = NULL, cfg.own_distance = NULL, cfg.on_session = NULL, cfg.user = NULL;
    cfg.allow_private = allow_private;
    if (boot >= 0) {
        snprintf(bootv[nb], sizeof bootv[nb], "127.0.0.1:%u", dht_port(nodes[boot].d));
        bp[nb] = bootv[nb];
        cfg.bootstrap = &bp[nb++];
        cfg.nbootstrap = 1;
    }
    dht *d = dht_create(&cfg, err, sizeof err);
    if (!d) printf("t_dht: %s\n", err);
    else if (port && dht_start(d) < 0) CHECK(!"started");
    return d;
}

static void scratch_end(dht *d, const char *name)
{
    char dir[64];
    dht_stop(d);
    snprintf(dir, sizeof dir, ROOT "/%s", name);
    clean_dir(dir);
}

/* a NODES answer of n contacts, the identities ids at the addresses addrs */
static size_t nodes_msg(uint8_t *m, const home_identity *ids, const char *const *addrs, int n)
{
    m[0] = DHT_NODES, m[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *p = m + 2 + 82 * (size_t)i;
        net_addr a;
        CHECK(net_parse_addr(addrs[i], &a));
        memcpy(p, ids[i].id, 32), memcpy(p + 32, ids[i].xonly, 32), memcpy(p + 64, a.ip, 16);
        p[80] = (uint8_t)(a.port >> 8), p[81] = (uint8_t)a.port;
    }
    return 2 + 82 * (size_t)n;
}

static void seen_at(dht *d, const home_identity *id, const char *addr)
{
    dht_contact c;
    memcpy(c.id, id->id, 32), memcpy(c.xonly, id->xonly, 32);
    CHECK(net_parse_addr(addr, &c.addr));
    dht_seen(d, &c);
}

/* What a public network refuses, and the limits by address block: in NODES answers (loopback and private addresses, port 0, one address
 * for two identities, more than 4 contacts of one block) and in the table (2 of a block in a bucket, 4 in the table). A test network takes
 * loopback without a limit. A node of a public network on 127.0.0.1 bootstraps from node 0 and keeps nothing: every answer points at
 * loopback. Before, all of it was taken. */
static void test_policy(void)
{
    home_identity pid, tid, ids[24];
    dht *pub = scratch("s-pub", false, 0, -1, &pid), *tst = scratch("s-test", true, 0, -1, &tid);
    CHECK(pub && tst);
    if (!pub || !tst) return;
    for (int i = 0; i < 24; i++) {
        char label[32];
        snprintf(label, sizeof label, "policy %d", i);
        make_id(label, &ids[i]);
    }
    uint8_t m[2 + 82 * DHT_K];
    dht_contact out[DHT_K];
    static const char *const LOOP[] = {"127.0.0.1:1000", "127.0.0.1:1001", "127.0.0.1:1002", "127.0.0.1:1003", "127.0.0.1:1004", "127.0.0.1:1005"};
    static const char *const PUB2[] = {"1.2.3.4:1000", "5.6.7.8:1000"};
    static const char *const LAN[] = {"5.6.7.8:1000", "192.168.1.10:80"};
    static const char *const PORT0[] = {"5.6.7.8:1000", "1.2.3.4:1"};   /* the second's port set to 0 below */
    static const char *const TWICE[] = {"8.8.4.4:5", "5.6.7.8:1000", "8.8.4.4:5"};
    static const char *const BLOCK5[] = {"1.2.3.1:1", "1.2.3.2:1", "1.2.3.3:1", "1.2.3.4:1", "1.2.3.5:1"};
    size_t l = nodes_msg(m, ids, LOOP, 2);
    CHECK(dht_parse_nodes(tst, m, l, out) == 2 && dht_parse_nodes(pub, m, l, out) == -1);
    l = nodes_msg(m, ids, LOOP, 6);
    CHECK(dht_parse_nodes(tst, m, l, out) == 6);                /* loopback on a test network: no limit */
    l = nodes_msg(m, ids, PUB2, 2);
    CHECK(dht_parse_nodes(tst, m, l, out) == 2 && dht_parse_nodes(pub, m, l, out) == 2);
    l = nodes_msg(m, ids, LAN, 2);
    CHECK(dht_parse_nodes(tst, m, l, out) == 2 && dht_parse_nodes(pub, m, l, out) == -1);
    l = nodes_msg(m, ids, PORT0, 2);
    m[2 + 82 + 80] = m[2 + 82 + 81] = 0;
    CHECK(dht_parse_nodes(tst, m, l, out) == -1 && dht_parse_nodes(pub, m, l, out) == -1);
    l = nodes_msg(m, ids, TWICE, 3);
    CHECK(dht_parse_nodes(tst, m, l, out) == -1 && dht_parse_nodes(pub, m, l, out) == -1);
    l = nodes_msg(m, ids, BLOCK5, 5);
    CHECK(dht_parse_nodes(tst, m, l, out) == -1 && dht_parse_nodes(pub, m, l, out) == -1);
    l = nodes_msg(m, ids, BLOCK5, 4);
    CHECK(dht_parse_nodes(pub, m, l, out) == 4);
    l = nodes_msg(m, ids, PUB2, 2);
    m[2 + 82 + 40] ^= 1;                                        /* the second identity does not give its ID */
    CHECK(dht_parse_nodes(pub, m, l, out) == -1);

    /* the table of the public network's node: no loopback or LAN; of one /24, two in a bucket and four in all */
    dht_contact cs[64];
    seen_at(pub, &ids[0], "127.0.0.1:5000");
    seen_at(pub, &ids[1], "192.168.1.10:5000");
    CHECK(dht_contacts(pub, cs, 64) == 0);
    int byb[DHT_BUCKETS + 1] = {0}, in_table = 0, kept = 0;
    for (int i = 0; i < 24; i++) {
        char addr[32];
        const int b = bucket_of(pid.id, ids[i].id);
        snprintf(addr, sizeof addr, "9.9.9.%d:7000", i + 1);
        seen_at(pub, &ids[i], addr);
        const int want = byb[b] < 2 && in_table < 4;
        byb[b] += want, in_table += want;
        kept += want;
    }
    const int n = dht_contacts(pub, cs, 64);
    CHECK(n == kept && n == 4);
    int per[DHT_BUCKETS + 1] = {0};
    for (int i = 0; i < n; i++) per[bucket_of(pid.id, cs[i].id)]++;
    for (int b = 0; b <= DHT_BUCKETS; b++) CHECK(per[b] <= 2);
    seen_at(pub, &ids[23], "9.9.10.1:7000");                    /* another block is welcome */
    CHECK(dht_contacts(pub, cs, 64) == 5);
    /* the test network's node: loopback in any number */
    for (int i = 0; i < 10; i++) {
        char addr[32];
        snprintf(addr, sizeof addr, "127.0.0.1:%d", 6000 + i);
        seen_at(tst, &ids[i], addr);
    }
    CHECK(dht_contacts(tst, cs, 64) == 10);
    scratch_end(pub, "s-pub");
    scratch_end(tst, "s-test");
    /* a public network's node on loopback: node 0 answers its PING (an address given by hand is dialled), but keeps none of it */
    home_identity bid;
    dht *b = scratch("s-boot", false, DHT_ANY_PORT, 0, &bid);
    CHECK(b && dht_contacts(b, cs, 64) == 0);
    if (b) scratch_end(b, "s-boot");
}

/* A table with two identities at one address (a node gone, and the one now at its address, until the old one has failed three times):
 * the NODES answer gives that address once, the nearer of the two. Before, it gave both, and every asker refused the whole answer (one
 * address, two identities) and counted it as a failure of this node. */
static void test_nodes_answer(void)
{
    home_identity sid, cid, ids[3];
    dht *sd = scratch("s-nodes", true, DHT_ANY_PORT, -1, &sid);
    CHECK(sd != NULL);
    if (!sd) return;
    make_id("nodes 0", &ids[0]), make_id("nodes 1", &ids[1]), make_id("nodes 2", &ids[2]);
    seen_at(sd, &ids[0], "127.0.0.1:7000");
    seen_at(sd, &ids[1], "127.0.0.1:7000");
    seen_at(sd, &ids[2], "127.0.0.1:7001");
    make_id("nodes client", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(sd);
    uint8_t req[33] = {DHT_FIND_NODE}, ans[2 + 82 * DHT_K];
    dht_contact out[DHT_K];
    size_t n;
    memcpy(req + 1, ids[1].id, 32);
    CHECK(net_exchange(&cli, &a, sid.xonly, req, sizeof req, ans, sizeof ans, &n, NULL));
    CHECK(dht_parse_nodes(sd, ans, n, out) == 2 && !memcmp(out[0].id, ids[1].id, 32) && !memcmp(out[1].id, ids[2].id, 32));
    scratch_end(sd, "s-nodes");
}

/* Hostile nodes near a key answer FIND_NODE with twenty contacts each that do not exist: identities that are no keys (a hash each), closer
 * to the key than any real node, at ports that take a connection and never speak. The lookup must still end with the real nodes closest
 * to the key beside the hostile ones, after a few timeouts (before, it spent all its queries on those names and returned the hostile
 * nodes alone: one address block could become the custodian of every record). */
typedef struct {
    home_identity id;
    net_cfg cfg;
    int lfd;
    uint16_t port;
    uint8_t pool[DHT_K][82];
    atomic_bool stop;
} hostile;

static void *hostile_main(void *arg)
{
    hostile *h = arg;
    uint8_t req[64], ans[2 + DHT_K * 82] = {DHT_NODES, DHT_K};
    memcpy(ans + 2, h->pool, sizeof h->pool);
    while (!atomic_load(&h->stop)) {
        net_addr from;
        size_t len;
        const int fd = net_accept(h->lfd, 100, &from);
        net_conn *c = fd < 0 ? NULL : net_handshake(&h->cfg, fd, &from);
        if (!c) continue;
        if (net_recv(c, req, sizeof req, &len) && len) {
            if (req[0] == DHT_PING) (void)net_send(c, (const uint8_t[]){DHT_PONG}, 1);
            else if (req[0] == DHT_FIND_NODE) (void)net_send(c, ans, sizeof ans);
        }
        net_close(c);
    }
    return NULL;
}

/* whether id shares `bits` bits with t after the zero bits of an identity */
static bool close_to(const uint8_t id[32], const uint8_t t[32], int bits)
{
    for (int b = BITS; b < BITS + bits; b++)
        if ((id[b / 8] ^ t[b / 8]) & (0x80 >> (b % 8))) return false;
    return true;
}

static void test_hostile_lookup(void)
{
    enum { H = 3, TAR = H * DHT_K };                           /* a port for each name: an answer with an address twice is refused whole */
    static hostile hs[H];
    pthread_t th[H];
    uint16_t tar[TAR];
    int tfd[TAR];
    uint8_t target[32];
    dht_contact base[DHT_K], got[DHT_K];
    hashed("the key of a record", target);
    const int nb = dht_lookup(nodes[2].d, target, base, DHT_KSTORE);
    CHECK(nb == DHT_KSTORE);
    for (int i = 0; i < TAR; i++) tfd[i] = net_listen("127.0.0.1", 0, &tar[i]);   /* never accepted: a connection there stays silent */
    for (int k = 0; k < H; k++) {
        hostile *h = &hs[k];
        char label[48];
        for (int c = 0;; c++) {                                 /* a real identity near the key */
            snprintf(label, sizeof label, "hostile %d try %d", k, c);
            make_id(label, &h->id);
            if (close_to(h->id.id, target, 9)) break;
        }
        for (int i = 0; i < DHT_K; i++) {                       /* the names it gives: no keys, nearer still */
            uint8_t *p = h->pool[i], m[40];
            for (uint32_t c = 0;; c++) {
                memcpy(m, target, 32), memcpy(m + 32, &(uint32_t){(uint32_t)(k * 100 + i)}, 4), memcpy(m + 36, &c, 4);
                pz_sha256(m, sizeof m, p + 32);
                keys_node_id(p + 32, p);
                if (keys_id_ok(p, BITS) && close_to(p, target, 12)) break;
            }
            memset(p + 64, 0, 10), p[74] = p[75] = 0xff, p[76] = 127, p[77] = p[78] = 0, p[79] = 1;
            p[80] = (uint8_t)(tar[k * DHT_K + i] >> 8), p[81] = (uint8_t)tar[k * DHT_K + i];
        }
        h->lfd = net_listen("127.0.0.1", 0, &h->port);
        net_cfg_init(&h->cfg, ctx, &h->id, FP, BITS, h->port, 2000);
        atomic_store(&h->stop, false);
        CHECK(pthread_create(&th[k], NULL, hostile_main, h) == 0);
        for (int i = 0; i < N; i++) {                           /* one question to each node puts it in their tables */
            net_addr a;
            uint8_t ans[1];
            size_t len;
            CHECK(net_parse_addr("127.0.0.1:1", &a));
            a.port = dht_port(nodes[i].d);
            (void)net_exchange(&h->cfg, &a, NULL, (const uint8_t[]){DHT_PING}, 1, ans, 1, &len, NULL);
        }
    }
    const int64_t t0 = ms();
    const int ng = dht_lookup(nodes[2].d, target, got, DHT_KSTORE);
    int honest = 0, evil = 0;
    for (int i = 0; i < ng; i++) {
        for (int k = 0; k < H; k++) evil += !memcmp(got[i].id, hs[k].id.id, 32);
        for (int j = 0; j < nb; j++) honest += !memcmp(got[i].id, base[j].id, 32);
    }
    printf("t_dht: a lookup among %d hostile nodes in %.1f s: %d nodes, %d of them hostile, %d of the real closest\n", H, (double)(ms() - t0) / 1000,
           ng, evil, honest);
    CHECK(ng == DHT_KSTORE && evil == H && honest == DHT_KSTORE - H && ms() - t0 < 20000);   /* evil == H: the attack was really there */
    for (int k = 0; k < H; k++) atomic_store(&hs[k].stop, true);
    for (int k = 0; k < H; k++) pthread_join(th[k], NULL), net_close_fd(hs[k].lfd);
    for (int i = 0; i < TAR; i++) net_close_fd(tfd[i]);
}

static int raw_connect(uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return sys_connect((const struct sockaddr *)&a, sizeof a, ms() + 3000);
}

/* whether the other side closed fd within ms */
static bool closed_within(int fd, int msec)
{
    uint8_t b;
    return sys_recv(fd, &b, 1, ms() + msec) == 0;
}

/* Connections that open and say nothing, from one address, on a node of a public network (whose limits apply to loopback, as to any
 * address there): four hold four handlers, the fifth is closed at once, and so is a request from that address until they go, which is
 * after two seconds. Before, the fifth waited in the queue for a handler, and every request behind it, for the timeout of each idle one. */
static void test_slowloris(void)
{
    home_identity sid, cid;
    timeout_ms = 8000;
    dht *sv = scratch("s-slow", false, DHT_ANY_PORT, -1, &sid);
    timeout_ms = 2000;
    CHECK(sv != NULL);
    if (!sv) return;
    make_id("slow client", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 1000);
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(sv);
    int idle[4];
    for (int i = 0; i < 4; i++) idle[i] = raw_connect(a.port);
    nap(200);
    const int fifth = raw_connect(a.port);
    CHECK(fifth >= 0 && closed_within(fifth, 500));
    const uint8_t ping = DHT_PING;
    uint8_t ans[4];
    size_t n;
    int64_t t0 = ms();
    CHECK(!net_exchange(&cli, &a, sid.xonly, &ping, 1, ans, sizeof ans, &n, NULL) && ms() - t0 < 500);
    CHECK(closed_within(idle[0], 3000));                        /* the node drops a silent one after 2 seconds, not after its timeout of 8 */
    for (int i = 0; i < 4; i++)
        sys_close(idle[i]);
    sys_close(fifth);
    nap(300);
    CHECK(net_exchange(&cli, &a, sid.xonly, &ping, 1, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_PONG);
    scratch_end(sv, "s-slow");
}

/* A node that asks tells its port itself. One that tells a port nobody reaches (a home behind a router that lets nothing in) stays out
 * of the table of the node it asked; one that listens there is dialled back and enters. Before, both were taken at their word, and the
 * first was given to the others as a node of the table. And a node that listens counts itself a node of the table (here: the closest
 * to its own name) only while connections come in: this one doubts its port after a second without any, until node 0 dials it back. */
static void test_asker(void)
{
    home_identity sid, cid;
    dht_contact cs[DHT_K * 8], me;
    probe_s = 1;
    dht *sv = scratch("s-ask", true, DHT_ANY_PORT, -1, &sid);
    probe_s = 0;
    CHECK(sv != NULL);
    if (!sv) return;
    make_id("deaf asker", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 1, 1000);          /* port 1: nobody listens there */
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(nodes[0].d);
    const uint8_t ping = DHT_PING;
    uint8_t ans[4];
    size_t n;
    CHECK(dht_lookup(sv, sid.id, &me, 1) == 1);
    nap(1200);
    CHECK(dht_lookup(sv, sid.id, &me, 1) == 0);
    CHECK(net_exchange(&cli, &a, nodes[0].id.xonly, &ping, 1, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_PONG);
    cli.self = &sid, cli.port = dht_port(sv);                   /* the same question from the node that listens */
    CHECK(net_exchange(&cli, &a, nodes[0].id.xonly, &ping, 1, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_PONG);
    int nc = 0;
    for (const int64_t t0 = ms(); ms() - t0 < 5000 && !has(cs, nc, sid.id, NULL); nap(20)) nc = dht_contacts(nodes[0].d, cs, DHT_K * 8);
    CHECK(has(cs, nc, sid.id, NULL) && !has(cs, nc, cid.id, NULL));
    CHECK(dht_lookup(sv, sid.id, &me, 1) == 1 && !memcmp(me.id, sid.id, 32));
    scratch_end(sv, "s-ask");
}

/* A flood of alerts that will never be proved: copies of a record of A2 by 1100 identities that are no node. A2 does not know that
 * point's distance, so they all wait, and fill its table of alerts (1024). Then a real coincidence of A2 with B2: before, it was refused
 * (the table was full of waiting alerts and kept them for a day); now the oldest waiting one goes, and the real one gets its on_alert. */
static void test_alert_flood(void)
{
    node *A2 = &nodes[17], *B2 = &nodes[19];
    uint8_t xf[32], dA[32], dB[32], xA[32], nq[32];
    record rF, rA, rB;
    hashed("flooded", dA);
    even_point(dA, xf);
    CHECK(publish_one(A2, 0, xf, &rF));                         /* no give_point: A2 does not know its distance */
    home_identity cid;
    make_id("flooder", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(A2->d);
    int sent = 0;
    const int64_t t0 = ms();
    for (int i = 0; i < 1100; i++) {
        char label[32];
        home_identity sy;
        snprintf(label, sizeof label, "sybil %d", i);
        make_id(label, &sy);
        record r = {.t = 1, .seq = 1};
        memcpy(r.x, xf, 32), memcpy(r.owner, sy.xonly, 32);
        CHECK(record_sign(ctx, &net, &sy.kp, &r));
        uint8_t req[1 + 2 * RECORD_LEN], ans[1];
        size_t n;
        req[0] = DHT_ALERT;
        record_encode(&rF, req + 1), record_encode(&r, req + 1 + RECORD_LEN);
        sent += net_exchange(&cli, &a, A2->id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_OK;
    }
    CHECK(sent == 1100);
    printf("t_dht: 1100 alerts that will never be proved sent in %.1f s\n", (double)(ms() - t0) / 1000);
    /* the real one: A2 tame, B2 wild + */
    hashed("after the flood", dA);
    even_point(dA, xA);
    memcpy(nq, qp, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nq));
    memcpy(dB, dA, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, dB, nq));
    give_point(A2, 0, xA, dA), give_point(B2, 1, xA, dB);
    CHECK(publish_one(A2, 0, xA, &rA) && publish_one(B2, 1, xA, &rB));
    const int64_t t1 = ms();
    while (ms() - t1 < 20000 && (alerts_of(A2, NULL, NULL, NULL) < 1 || alerts_of(B2, NULL, NULL, NULL) < 1)) nap(50);
    record mine, theirs;
    bool merge;
    CHECK(alerts_of(A2, &mine, &theirs, &merge) == 1 && same(&mine, &rA) && same(&theirs, &rB) && !merge);
    CHECK(alerts_of(B2, &mine, &theirs, &merge) == 1 && same(&mine, &rB) && same(&theirs, &rA));
    printf("t_dht: the real alert after the flood in %.1f s\n", (double)(ms() - t1) / 1000);
}

/* The sequence number survives a restart (own.seq): a republish signs the records with numbers that own.bin does not keep, so a node that
 * restarts with its clock behind its last republish would sign below what the custodians saw, and be refused. own.seq holds a number
 * ahead of the clock here (a clock that went back); the next record is above it. */
static void test_seq(void)
{
    node *n = &nodes[3];
    const int port = dht_port(n->d);
    char path[128];
    dht_stop(n->d);
    n->d = NULL;
    const uint64_t ahead = (uint64_t)time(NULL) * 1000000 + 1000000000000ull;   /* 11 days ahead */
    snprintf(path, sizeof path, "%s/own.seq", n->dir);
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) fprintf(f, "%llu\n", (unsigned long long)ahead), fclose(f);
    CHECK(start_node(n, port, -1));
    uint8_t d[32], x[32];
    record r;
    hashed("after a clock went back", d);
    even_point(d, x);
    CHECK(publish_one(n, 1, x, &r) && r.seq > ahead);
}

/* ---- the relay ---- */

typedef struct {
    dht *d;
    const uint8_t *partner, *name;
    net_conn *c;
    int frames, echoed;
} chan_job;

static void *chan_main(void *arg)
{
    chan_job *j = arg;
    j->c = dht_channel(j->d, j->partner, j->name, 20000, NULL, NULL);
    return NULL;
}

/* a stand-in session: the initiator sends frames of 60 000 bytes, the other side sends each back */
typedef struct {
    net_conn *c;
    bool first;
    int frames, done;
} stand_in;

static void *stand_in_main(void *arg)
{
    stand_in *s = arg;
    uint8_t *m = malloc(60000), *got = malloc(60000);
    size_t l;
    for (int i = 0; m && got && i < s->frames; i++) {
        memset(m, i * 7 + 1, 60000);
        if (s->first ? !net_send(s->c, m, 60000) || !net_recv(s->c, got, 60000, &l) || l != 60000 || memcmp(got, m, 60000)
                     : !net_recv(s->c, got, 60000, &l) || l != 60000 || got[0] != m[0] || !net_send(s->c, got, l))
            break;
        s->done++;
    }
    free(m), free(got);
    return NULL;
}

static uint64_t relays_done(void)
{
    uint64_t sum = 0;
    for (int i = 0; i < N; i++) {
        int w, a;
        uint64_t dn;
        dht_relays(nodes[i].d, &w, &a, &dn);
        sum += dn;
    }
    return sum;
}

/* Two homes that do not listen, U and W, with a useful coincidence: neither can dial the other for a proof, so each sends its own to the
 * other's mailboxes, and both get on_alert (before, both alerts waited for a day: no session could start). Then each asks dht_channel
 * for a channel with the other: through the relay of the slot, a handshake end to end, and a stand-in session of 2 x 2.4 MB on it. */
static void test_relay_session(void)
{
    node *Un = &nodes[U], *Wn = &nodes[W];
    uint8_t dU[32], dW[32], x[32], nq[32];
    record rU, rW;
    hashed("relay useful", dU);
    even_point(dU, x);
    memcpy(nq, qp, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nq));
    memcpy(dW, dU, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, dW, nq));
    give_point(Un, 0, x, dU), give_point(Wn, 1, x, dW);
    const int before = alerts_of(Un, NULL, NULL, NULL);
    CHECK(publish_one(Un, 0, x, &rU) && publish_one(Wn, 1, x, &rW));
    int64_t t0 = ms();
    while (ms() - t0 < 30000 && (alerts_of(Un, NULL, NULL, NULL) <= before || alerts_of(Wn, NULL, NULL, NULL) < 1)) nap(50);
    record mine, theirs;
    bool merge;
    CHECK(alerts_of(Un, &mine, &theirs, &merge) == before + 1 && same(&mine, &rU) && same(&theirs, &rW) && !merge);
    CHECK(alerts_of(Wn, &mine, &theirs, &merge) == 1 && same(&mine, &rW) && same(&theirs, &rU) && !merge);
    printf("t_dht: two homes that do not listen proved their points to each other through their mailboxes in %.1f s\n", (double)(ms() - t0) / 1000);
    /* the channel, both at once, as the two sessions would ask for it */
    uint8_t name[32];
    hashed("the eid of U and W", name);
    const uint64_t done0 = relays_done();
    chan_job ju = {Un->d, Wn->id.xonly, name, NULL, 0, 0}, jw = {Wn->d, Un->id.xonly, name, NULL, 0, 0};
    pthread_t tu, tw;
    t0 = ms();
    CHECK(pthread_create(&tu, NULL, chan_main, &ju) == 0);
    CHECK(pthread_create(&tw, NULL, chan_main, &jw) == 0);
    pthread_join(tu, NULL), pthread_join(tw, NULL);
    CHECK(ju.c && jw.c);
    if (ju.c && jw.c) {
        CHECK(!memcmp(ju.c->remote, Wn->id.xonly, 32) && !memcmp(jw.c->remote, Un->id.xonly, 32));
        printf("t_dht: a relayed channel in %.1f s\n", (double)(ms() - t0) / 1000);
        const bool u_first = memcmp(Un->id.xonly, Wn->id.xonly, 32) < 0;
        stand_in su = {ju.c, u_first, 40, 0}, sw = {jw.c, !u_first, 40, 0};
        t0 = ms();
        CHECK(pthread_create(&tu, NULL, stand_in_main, &su) == 0 && pthread_create(&tw, NULL, stand_in_main, &sw) == 0);
        pthread_join(tu, NULL), pthread_join(tw, NULL);
        CHECK(su.done == 40 && sw.done == 40);
        printf("t_dht: a stand-in session of 2 x 2.4 MB through the relay in %.1f s\n", (double)(ms() - t0) / 1000);
    }
    net_close(ju.c), net_close(jw.c);
    for (t0 = ms(); ms() - t0 < 5000 && relays_done() == done0; nap(20)) {}
    CHECK(relays_done() == done0 + 1);
}

/* a home that joins node r's relay by hand */
typedef struct {
    node *r;
    home_identity id;
    uint8_t name[32], partner[32];
    int wait_ms;
    net_conn *c;
    bool relayed;
    int64_t took;
} joiner;

static void *join_main(void *arg)
{
    joiner *j = arg;
    net_cfg cfg;
    net_addr a;
    uint8_t req[65], ans[4];
    size_t n;
    const int64_t t0 = ms();
    net_cfg_init(&cfg, ctx, &j->id, FP, BITS, 0, j->wait_ms);
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(j->r->d);
    j->c = net_dial(&cfg, &a, j->r->id.xonly);
    req[0] = DHT_RELAY;
    memcpy(req + 1, j->name, 32), memcpy(req + 33, j->partner, 32);
    j->relayed = j->c && net_send(j->c, req, sizeof req) && net_recv(j->c, ans, sizeof ans, &n) && n == 1 && ans[0] == DHT_RELAYED;
    j->took = ms() - t0;
    if (!j->relayed) net_close(j->c), j->c = NULL;
    return NULL;
}

static void joiner_init(joiner *j, node *r, const char *who, const char *name, const joiner *partner)
{
    memset(j, 0, sizeof *j);
    j->r = r;
    j->wait_ms = 4000;
    make_id(who, &j->id);
    hashed(name, j->name);
    if (partner) memcpy(j->partner, partner->id.xonly, 32), memcpy(((joiner *)partner)->partner, j->id.xonly, 32);
}

typedef struct {
    joiner *j;
    bool initiator, ok;
    int frames, done;
} relay_end;

static void *relay_end_main(void *arg)
{
    relay_end *e = arg;
    net_cfg cfg;
    net_cfg_init(&cfg, ctx, &e->j->id, FP, BITS, 0, 3000);
    e->ok = net_rehandshake(&cfg, e->j->c, e->initiator, e->j->partner);
    stand_in s = {e->j->c, e->initiator, e->frames, 0};
    if (e->ok) stand_in_main(&s);
    e->done = s.done;
    return NULL;
}

/* A relay's limits, at node 2, with homes that join by hand. A home whose partner never comes is closed after a slot and a quarter. A third
 * party that names the pair is not joined to anyone. Two relays at once (this network's relay_max): the third pair is refused, both of it.
 * The bytes of one relay (8 MiB here): a stand-in session that sends 9 MB is cut. */
static void test_relay_limits(void)
{
    node *R = &nodes[2];
    joiner lone, a, x, p[3], q[3];
    pthread_t th[8];
    int w, act;
    uint64_t done0, dn;
    dht_relays(R->d, &w, &act, &done0);
    joiner_init(&lone, R, "lone", "lone pair", NULL);
    hashed("nobody", lone.partner);
    join_main(&lone);
    CHECK(!lone.relayed && lone.took >= 1000 && lone.took < 3000);
    dht_relays(R->d, &w, &act, &dn);
    CHECK(w == 0 && act == 0);
    /* a waits for b; x names the same pair, with a as its partner: no match (a waits for b, not for x) */
    joiner b;
    joiner_init(&a, R, "pair a", "pair ab", NULL);
    joiner_init(&b, R, "pair b", "pair ab", &a);
    joiner_init(&x, R, "intruder", "pair ab", &a);
    memcpy(a.partner, b.id.xonly, 32);
    CHECK(pthread_create(&th[0], NULL, join_main, &a) == 0 && pthread_create(&th[1], NULL, join_main, &x) == 0);
    pthread_join(th[0], NULL), pthread_join(th[1], NULL);
    CHECK(!a.relayed && !x.relayed);
    /* two pairs relayed, the third refused */
    for (int i = 0; i < 3; i++) {
        char who[16], name[16];
        snprintf(who, sizeof who, "p%d", i), snprintf(name, sizeof name, "pq%d", i);
        joiner_init(&p[i], R, who, name, NULL);
        snprintf(who, sizeof who, "q%d", i);
        joiner_init(&q[i], R, who, name, &p[i]);
    }
    for (int i = 0; i < 2; i++) CHECK(pthread_create(&th[2 * i], NULL, join_main, &p[i]) == 0 && pthread_create(&th[2 * i + 1], NULL, join_main, &q[i]) == 0);
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    CHECK(p[0].relayed && q[0].relayed && p[1].relayed && q[1].relayed);
    dht_relays(R->d, &w, &act, &dn);
    CHECK(act == 2);
    CHECK(pthread_create(&th[4], NULL, join_main, &p[2]) == 0);
    nap(100);
    join_main(&q[2]);
    pthread_join(th[4], NULL);
    CHECK(!p[2].relayed && !q[2].relayed && q[2].took < 1000);
    /* the first pair: a session of 9 MB, cut at 8 MiB; the second ends at once */
    net_close(p[1].c), net_close(q[1].c);
    const bool p_first = memcmp(p[0].id.xonly, q[0].id.xonly, 32) < 0;
    relay_end ep = {&p[0], p_first, false, 150, 0}, eq = {&q[0], !p_first, false, 150, 0};
    CHECK(pthread_create(&th[0], NULL, relay_end_main, &ep) == 0 && pthread_create(&th[1], NULL, relay_end_main, &eq) == 0);
    pthread_join(th[0], NULL), pthread_join(th[1], NULL);
    CHECK(ep.ok && eq.ok && ep.done < 75 && ep.done > 30);
    net_close(p[0].c), net_close(q[0].c);
    for (const int64_t t0 = ms(); ms() - t0 < 3000; nap(20)) {
        dht_relays(R->d, &w, &act, &dn);
        if (act == 0) break;
    }
    CHECK(act == 0 && dn == done0 + 2);
}

/* A node stopped while it relays a pair: the relay ends within a slice of its copy, both homes see the end, and dht_stop returns. Before,
 * the thread of the relay still read the node after it had told dht_stop it was done, when dht_stop may have freed it (the thread
 * sanitizer reported it). */
static void test_relay_stop(void)
{
    node r;
    joiner a, b;
    pthread_t th;
    memset(&r, 0, sizeof r);
    r.d = scratch("s-relay", true, DHT_ANY_PORT, -1, &r.id);
    CHECK(r.d != NULL);
    if (!r.d) return;
    joiner_init(&a, &r, "stop a", "stop pair", NULL);
    joiner_init(&b, &r, "stop b", "stop pair", &a);
    CHECK(pthread_create(&th, NULL, join_main, &a) == 0);
    join_main(&b);
    pthread_join(th, NULL);
    CHECK(a.relayed && b.relayed);
    const int64_t t0 = ms();
    scratch_end(r.d, "s-relay");
    CHECK(ms() - t0 < 2000);
    if (a.c) CHECK(closed_within(a.c->fd, 1000));
    if (b.c) CHECK(closed_within(b.c->fd, 1000));
    net_close(a.c), net_close(b.c);
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
    CHECK(sys_mkdir(ROOT));
    for (int i = 0; i < NODES; i++) {
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
    if (!fails) CHECK(start_node(&nodes[W], 0, 0) && dht_port(nodes[W].d) == 0);
    if (fails) return 1;
    all(refresh_main, N);
    printf("t_dht: %d nodes up and refreshed in %.1f s\n", N + 2, (double)(ms() - t0) / 1000);
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
    test_policy();
    test_nodes_answer();
    test_slowloris();
    test_alert_flood();
    test_seq();
    t0 = ms();
    test_relay_session();
    test_relay_limits();
    test_relay_stop();
    printf("t_dht: the relay in %.1f s\n", (double)(ms() - t0) / 1000);
    /* last: they leave nodes in the tables of the others that are gone, and every dial to them costs a timeout (on Windows two seconds
     * even on loopback, where a refused connection is tried again) */
    test_asker();
    test_hostile_lookup();
    t0 = ms();
    all(stop_main, NODES);
    printf("t_dht: stopped in %.1f s\n", (double)(ms() - t0) / 1000);
    for (int i = 0; i < NODES; i++) {
        clean_dir(nodes[i].dir);
        pthread_mutex_destroy(&nodes[i].mu);
    }
    rmdir(ROOT);
    secp256k1_context_destroy(ctx);
    if (fails) printf("t_dht: %d FAILED\n", fails);
    else printf("t_dht: OK (%.1f s)\n", (double)(ms() - t_start) / 1000);
    return fails != 0;
}
