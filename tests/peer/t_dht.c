/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_dht.c - a network of N nodes in one process on 127.0.0.1, each bootstrapped from node 0 alone (identities with few zero bits, so that
 * making them is quick). Checked against a brute force over all the nodes: lookups find the true 20 closest; records published by several
 * owners are held by 4 or 5 of their 5 closest nodes. Then the coincidences: a useful one and a merge, whose owners both get on_alert
 * after the proofs of possession, with the pair in their mailboxes; an owner that does not listen, which gets its alert from its
 * mailboxes, and its partner, which gets that owner's proof in an ALERT. Then a session over dht_dial, malformed requests (and records
 * refused: false zero bits, far from the custodian), a node stopped and started again (its store and its peers come
 * back), the eviction of the oldest contact of a full bucket. The limits: a public network refuses loopback, a NODES answer aimed at one
 * address or block is refused, the table keeps few contacts per block and answers an address once, idle connections hold few handlers,
 * a flood of alerts that will never be proved does not shut out a real one, nor do forged alerts of one block. Copies of a point planted
 * at its custodians do not hide a real coincidence with it. A custodian that lies to PROVE: its answer brings only the pairs of the
 * records proved, its silence costs one timeout, and its slowness does not hold a publish behind a republish. A linear search keeps
 * claims and no records. The relay: two homes that do not listen get their alerts through their mailboxes and run a stand-in session
 * through a relay; a relay's limits (a wait without partner, a third party, relays at once, bytes); a relay stopped while it copies. */
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
static const block_net *linear;    /* of the next node made: the blocks of a linear search (NULL: a network of records) */
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BITS 4                     /* zero bits of a node ID in this network */
#define N 30                       /* nodes that listen */
#define U N                        /* a node that does not listen */
#define E (N + 1)                  /* a node alone, for the eviction */
#define W (N + 2)                  /* another node that does not listen: U and W meet through a relay */
#define LATE (N + 3)               /* a node that joins late, for the republish */
#define NODES (N + 4)
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
    point pts[64];
    int npts, alerts, slow_ms;     /* slow_ms: own_distance takes that long (the cost of a job, without a network to spend it on) */
    record mine, theirs;
    bool merge;
} node;

static secp256k1_context *ctx;
static const uint8_t FP[32] = {0xd4, 0x7};
static record_net net;
static block_net bnet;             /* the claims of a linear search (test_linear) */
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
    if (n->slow_ms) nap(n->slow_ms);
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
    static const char *const FILES[] = {"store.bin", "store.bin.owners", "own.bin", "own.cust", "own.cust.tmp", "peers.txt",
                                        "peers.txt.tmp", "claims.bin"};
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
        .probe_s = probe_s, .verbose = getenv("T_DHT_VERBOSE") != NULL, .blocks = linear,
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
    record r = {.t = t, .dp = net.dpbits};
    uint8_t nonce[32] = {1}, sig[64];
    memcpy(r.x, x, 32);
    memcpy(r.owner, n->id.xonly, 32);
    CHECK(record_prove(ctx, &net, &r, d, nodes[0].id.xonly, nonce, sig));
    pthread_mutex_lock(&n->mu);
    CHECK(n->npts < (int)(sizeof n->pts / sizeof *n->pts));
    point *p = &n->pts[n->npts++];
    memcpy(p->x, x, 32), memcpy(p->d, d, 32);
    p->t = t;
    pthread_mutex_unlock(&n->mu);
}

static bool publish_one(node *n, int t, const uint8_t x[32], record *out)
{
    out->t = t, out->dp = net.dpbits;
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
        recs[i].t = i % 3 - 1, recs[i].dp = net.dpbits;
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
        CHECK(record_check(ctx, &net, &recs[i]));               /* dht_publish set its owner */
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

/* The estimate of the network a home prints (home.c): the nodes that listen, from the distances of a lookup, and the records of the network,
 * from those a custodian holds. Each lookup is a fifth off: the mean of twenty is checked, against the 30 nodes and the 2000 records. */
static void test_estimate(void)
{
    double nodes_sum = 0, recs_sum = 0;
    for (int i = 0; i < 20; i++) {
        const double est = dht_nodes(nodes[(i * 7) % N].d);
        nodes_sum += est;
        recs_sum += (double)dht_local_count(nodes[(i * 7) % N].d) * est / DHT_KSTORE;
    }
    printf("t_dht: about %.1f nodes (%d), about %.0f records (%d)\n", nodes_sum / 20, N, recs_sum / 20, RECS);
    CHECK(nodes_sum / 20 > 0.75 * N && nodes_sum / 20 < 1.33 * N);
    CHECK(recs_sum / 20 > 0.7 * RECS && recs_sum / 20 < 1.43 * RECS);
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
    /* the mailboxes of each owner have the pair: the other owner sent it there, with its proof */
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
    CHECK(dht_ask_proof(nodes[20].d, &fake) == -1);             /* not its record: it closes without a proof */
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

static void make_id(const char *label, home_identity *id)
{
    uint8_t seed[32];
    hashed(label, seed);
    CHECK(keys_identity(ctx, seed, FP, BITS, id));
}

/* A copy of another's point alerts nobody: a custodian alerts nobody by itself, and the copier has no proof to send. A real coincidence:
 * the owner that proves its record hears of the pair from the custodian, in the answer to PROVE, and the other owner from it, in an
 * ALERT with its proof. */
static void test_copied_point(void)
{
    node *P = &nodes[21], *Q = &nodes[23], *R = &nodes[25];
    uint8_t dP[32], xP[32], dR[32], d2[32], x2[32], nq[32], k[32];
    record rP, rQ, rR;
    hashed("copied", dP), even_point(dP, xP);
    give_point(P, 0, xP, dP);
    CHECK(publish_one(P, 0, xP, &rP));
    CHECK(publish_one(Q, 1, xP, &rQ));                         /* Q copies P's point as a wild one: it cannot prove it */
    nap(3000);
    CHECK(in_mailboxes(P->id.xonly, &rP, &rQ) == 0 && in_mailboxes(Q->id.xonly, &rP, &rQ) == 0);
    CHECK(alerts_of(P, NULL, NULL, NULL) == 0);
    home_identity cid;
    make_id("prover", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    uint8_t al[1 + 2 * RECORD_LEN + 64] = {DHT_ALERT}, ans[2 + DHT_MBOX_MAX * DHT_MBOX_ENTRY], zero[32] = {0}, h[32];
    size_t n;
    /* nor can the copier tell P itself: an ALERT without the proof of its first record, or with one that does not check out, is refused */
    a.port = dht_port(P->d);
    record_encode(&rQ, al + 1), record_encode(&rP, al + 1 + RECORD_LEN);
    CHECK(!net_exchange(&cli, &a, P->id.xonly, al, 1 + 2 * RECORD_LEN, ans, sizeof ans, &n, NULL));
    CHECK(!net_exchange(&cli, &a, P->id.xonly, al, sizeof al, ans, sizeof ans, &n, NULL));
    CHECK(alerts_of(P, NULL, NULL, NULL) == 0);
    /* the real one, by hand: R stores a wild point, then an owner of its own the same x tame, which it proves */
    hashed("proved by hand", d2), even_point(d2, x2);
    memcpy(nq, qp, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nq));
    memcpy(dR, d2, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, dR, nq));
    give_point(R, 1, x2, dR);
    CHECK(publish_one(R, 1, x2, &rR));
    record mine = {.t = 0, .dp = net.dpbits};
    memcpy(mine.x, x2, 32), memcpy(mine.owner, cid.xonly, 32);
    int who;
    store_key(x2, k);
    CHECK(brute(k, 1, &who) == 1);
    node *cu = &nodes[who];
    a.port = dht_port(cu->d);
    uint8_t req[2 + RECORD_WIRE + 64];
    req[0] = DHT_STORE, req[1] = 1, record_wire_encode(&mine, req + 2);
    CHECK(net_exchange(&cli, &a, cu->id.xonly, req, 2 + RECORD_WIRE, ans, sizeof ans, &n, NULL) && n == 3 && ans[2] == DHT_ST_COINCIDENCE);
    req[0] = DHT_PROVE;                                        /* PROVE(1, the record, the proof) */
    CHECK(record_prove(ctx, &net, &mine, d2, P->id.xonly, zero, req + 2 + RECORD_WIRE));   /* for another asker: refused */
    CHECK(!net_exchange(&cli, &a, cu->id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL));
    CHECK(record_prove(ctx, &net, &mine, d2, cu->id.xonly, zero, req + 2 + RECORD_WIRE));
    CHECK(net_exchange(&cli, &a, cu->id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 2 + DHT_MBOX_ENTRY && ans[0] == DHT_MBOX && ans[1] == 1);
    record g0, g1;
    CHECK(record_decode(ans + 2, &g0) && record_decode(ans + 2 + RECORD_LEN, &g1));
    CHECK((same(&g0, &mine) && same(&g1, &rR)) || (same(&g0, &rR) && same(&g1, &mine)));
    /* again, the same answer: the custodian keeps nothing between STORE and PROVE, and tells nobody else */
    CHECK(net_exchange(&cli, &a, cu->id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 2 + DHT_MBOX_ENTRY && ans[1] == 1);
    nap(500);
    CHECK(alerts_of(R, NULL, NULL, NULL) == 0 && in_mailboxes(R->id.xonly, &rR, &mine) == 0);
    /* the owner that proved tells R, with its proof for R over the pair: R, which holds its point, gets its alert */
    a.port = dht_port(R->d);
    record_encode(&mine, al + 1), record_encode(&rR, al + 1 + RECORD_LEN);
    pair_hash(&mine, &rR, h);
    CHECK(record_prove(ctx, &net, &mine, d2, R->id.xonly, h, al + 1 + 2 * RECORD_LEN));
    CHECK(net_exchange(&cli, &a, R->id.xonly, al, sizeof al, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_OK);
    const int64_t t0 = ms();
    while (ms() - t0 < 10000 && alerts_of(R, NULL, NULL, NULL) == 0) nap(50);
    record got, theirs;
    bool merge;
    CHECK(alerts_of(R, &got, &theirs, &merge) == 1 && same(&got, &rR) && same(&theirs, &mine) && !merge);
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
    record_wire_encode(&recs[0], req + 2);
    CHECK(!net_exchange(&cli, &a, NULL, req, 2 + RECORD_WIRE, ans, sizeof ans, &n, NULL));   /* two records announced, one sent */
    req[1] = 0;
    CHECK(!net_exchange(&cli, &a, NULL, req, 2, ans, sizeof ans, &n, NULL));                 /* none */
    req[0] = DHT_ALERT;
    record_encode(&recs[0], req + 1);
    record_encode(&recs[1], req + 1 + RECORD_LEN);
    CHECK(!net_exchange(&cli, &a, NULL, req, 1 + 2 * RECORD_LEN, ans, sizeof ans, &n, NULL));   /* two records that are no coincidence */
    req[0] = DHT_MBOX_GET;
    CHECK(!net_exchange(&cli, &a, NULL, req, 34, ans, sizeof ans, &n, NULL));
    memcpy(req + 1, nodes[1].id.xonly, 32);                                                  /* the mailbox of another owner: */
    CHECK(!net_exchange(&cli, &a, NULL, req, 33, ans, sizeof ans, &n, NULL));                /* its points are not for the asker */
    memcpy(req + 1, cid.xonly, 32);
    CHECK(net_exchange(&cli, &a, NULL, req, 33, ans, sizeof ans, &n, NULL) && n == 2 && ans[0] == DHT_MBOX && ans[1] == 0);
    memset(req + 1, 0, 32);
    req[0] = DHT_SESSION_FIRST + 1;
    CHECK(net_exchange(&cli, &a, NULL, req, 5, ans, sizeof ans, &n, NULL) && n == 5);        /* a session: the test's echo */
    /* well formed, refused record by record: zero bits that x has not, a record whose key is far from node 0 (node 0 knows many nodes
     * closer to it: before, it was stored); and one node 0 is the closest to, stored. A STORE names no owner: it is the sender, so
     * nobody stores the records of another. And an owner whose node ID has not the zero bits: refused */
    record mk[3];                                              /* of cid: false zero bits, far from node 0, near node 0 */
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
        const int want = got == 1;                              /* near, far, near: the zero bits are checked after the cheap checks */
        if ((want == 1 && nearer < 15) || (want == 0 && nearer != 0)) continue;
        record r = {.t = 0, .dp = net.dpbits};
        memcpy(r.x, x, 32);
        memcpy(r.owner, cid.xonly, 32);
        mk[got++] = r;
    }
    mk[0].dp = 40;
    uint8_t big[2 + 3 * RECORD_LEN];
    big[0] = DHT_STORE, big[1] = 3;
    for (int i = 0; i < 3; i++) record_wire_encode(&mk[i], big + 2 + i * RECORD_WIRE);
    CHECK(net_exchange(&cli, &a, NULL, big, 2 + 3 * RECORD_WIRE, ans, sizeof ans, &n, NULL) && n == 5 && ans[0] == DHT_STORED && ans[1] == 3 &&
          ans[2] == DHT_ST_INVALID && ans[3] == DHT_ST_FAR && ans[4] == DHT_ST_STORED);
    home_identity nob;
    for (int i = 0;; i++) {
        char label[32];
        snprintf(label, sizeof label, "no bits %d", i);
        hashed(label, seed);
        CHECK(keys_identity(ctx, seed, FP, 0, &nob));
        if (!keys_id_ok(nob.id, BITS)) break;
    }
    net_cfg cnob;
    net_cfg_init(&cnob, ctx, &nob, FP, 0, 0, 2000);
    big[1] = 1;
    CHECK(!net_exchange(&cnob, &a, NULL, big, 2 + RECORD_WIRE, ans, sizeof ans, &n, NULL) || (n == 3 && ans[2] == DHT_ST_INVALID));
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

static int64_t file_size(const char *dir, const char *name)
{
    char p[128];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    const long n = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
    if (f) fclose(f);
    return n;
}

/* An owner restarts, and its home gives its records again from the first: those own.bin has in the same order are not sent again, and
 * one that differs ends that, own.bin cut there (the records after it are not the home's any more). The republish: the first sends every record to its custodians, the next with no node come or gone sends
 * none, and after a node joins only the records it is now a custodian of go, to it alone. */
static void test_republish(void)
{
    node *o = &nodes[2], *r = &nodes[LATE];
    const int n = RECS / OWNERS, port = dht_port(o->d);
    const record *mine = recs + n;                              /* node 2's records (test_records) */
    record again[100];
    const int64_t size = file_size(o->dir, "own.bin");
    CHECK(size == STORE_HDR + (int64_t)n * RECORD_WIRE);
    dht_stop(o->d);
    o->d = NULL;
    CHECK(start_node(o, port, -1));
    if (!o->d) return;
    memcpy(again, mine, sizeof again);
    CHECK(dht_publish(o->d, again, 100) == 0 && file_size(o->dir, "own.bin") == size);   /* sent before the restart */
    uint8_t dk[32];
    record fresh = {.t = 0, .dp = net.dpbits};
    hashed("a point after the restart", dk);
    even_point(dk, fresh.x);
    CHECK(dht_publish(o->d, &fresh, 1) == 1 && file_size(o->dir, "own.bin") == STORE_HDR + 101 * RECORD_WIRE);
    memcpy(again, mine + 100, sizeof again);
    CHECK(dht_publish(o->d, again, 1) == 1 && file_size(o->dir, "own.bin") == STORE_HDR + 102 * RECORD_WIRE);   /* the replay is over */
    const uint64_t first = dht_republish(o->d, false), next = dht_republish(o->d, false);
    CHECK(first >= 102 * (DHT_KSTORE - 1) && next == 0);
    CHECK(start_node(r, DHT_ANY_PORT, 0));
    if (!r->d) return;
    dht_refresh(r->d);
    const uint64_t joined = dht_republish(o->d, false), held = dht_local_count(r->d);
    printf("t_dht: republish: %llu copies, then %llu, then %llu after a node joined, which holds %llu records\n", (unsigned long long)first,
           (unsigned long long)next, (unsigned long long)joined, (unsigned long long)held);
    CHECK(joined > 0 && joined == held);
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
    stop_main(e);                 /* or it names its dead contacts in every lookup of the tests that follow, a refused dial each */
}

/* ---- the limits ---- */

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

/* the node with identity id, listening at port on 127.0.0.1, becomes a contact of d */
static void seen_port(dht *d, const home_identity *id, uint16_t port)
{
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    seen_at(d, id, addr);
}

/* A known contact moves to the address it answered at only when its own failed since it last answered: a node that names it at an
 * address of its own, which passes the bytes on to it, cannot take its place in the table and then cut it out. Before, any answer
 * moved it. A contact whose address stopped answering moves. */
static void test_moved_contact(void)
{
    home_identity id;
    dht *d = scratch("s-moved", true, DHT_ANY_PORT, -1, &id);
    if (!d) return;
    dht_contact cs[64], out[DHT_K];
    int n, at = -1;
    seen_port(d, &nodes[1].id, dht_port(nodes[1].d));
    seen_at(d, &nodes[1].id, "127.0.0.1:1");                   /* answered elsewhere: it stays */
    n = dht_contacts(d, cs, 64);
    CHECK(has(cs, n, nodes[1].id.id, &at) && cs[at].addr.port == dht_port(nodes[1].d));
    seen_at(d, &nodes[2].id, "127.0.0.1:1");                   /* where nobody listens: a lookup finds it dead there */
    (void)dht_lookup(d, nodes[2].id.id, out, DHT_K);
    seen_port(d, &nodes[2].id, dht_port(nodes[2].d));          /* then it answers at its address: it moves */
    n = dht_contacts(d, cs, 64);
    CHECK(has(cs, n, nodes[2].id.id, &at) && cs[at].addr.port == dht_port(nodes[2].d));
    scratch_end(d, "s-moved");
}

/* A linear search (docs/en/NETWORK.md 11) on six nodes of its own, as its network is not the one of the records: they keep the claims of
 * its blocks, and no records. A claim published by its owner, found by another node, kept once; the same proof keys claimed by another
 * identity, which needs no scan, not kept; the claim refused from anyone but its owner. A STORE, an
 * ALERT and a PROVE are refused, and there is no store.bin: any x with the zero bits passes the checks of a record, with no walk (before,
 * anyone could fill the 5 GB store of a node of a linear search, which has no use for records). */
enum { LINEAR = 6 };

static void linear_checks(dht *const ln[LINEAR], const home_identity ids[LINEAR], const char *dir0)
{
    dht *A = ln[1], *B = ln[5];
    uint8_t key[2][32] = {{0}};
    key[0][26] = 0x08, key[0][29] = 0x06, key[0][30] = 0x8b, key[0][31] = 0x68;   /* 0x80000068b68 and 0x800003d4338 (t_block.c) */
    key[1][26] = 0x08, key[1][29] = 0x3d, key[1][30] = 0x43, key[1][31] = 0x38;
    block_claim c, copy, got[4];
    CHECK(block_claim_make(ctx, &bnet, &ids[1].kp, 0, (const uint8_t(*)[32])key, 2, &c) && c.n == 2);
    CHECK(dht_claim_publish(A, &c) >= 4);
    CHECK(dht_claims_of(B, 0, got, 4) == 1 && !memcmp(got[0].owner, ids[1].xonly, 32) && got[0].n == 2);
    CHECK(dht_claims_of(B, 1, got, 4) == 0);                    /* nobody claimed block 1 */
    CHECK(dht_claim_publish(A, &c) >= 4);                       /* again: the same claim, kept once */
    CHECK(dht_claims_of(B, 0, got, 4) == 1);
    CHECK(block_claim_make(ctx, &bnet, &ids[2].kp, 0, (const uint8_t(*)[32])key, 2, &copy) && block_claim_check(ctx, &bnet, &copy));
    CHECK(dht_claim_publish(ln[2], &copy) >= 4 && dht_claims_of(B, 0, got, 4) == 1);   /* held already: not a second claim */
    /* the same claim sent by another identity: a custodian takes a claim from its owner only */
    home_identity cid;
    make_id("claim thief", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    uint8_t k[32], req[1 + BLOCK_CLAIM_MAX], ans[8];
    size_t n;
    int who = 0;
    block_key(&bnet, 0, k);
    for (int i = 1; i < LINEAR; i++) who = closer(k, ids[i].id, ids[who].id) ? i : who;
    net_addr a;
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(ln[who]);
    req[0] = DHT_CLAIM;
    const size_t len = 1 + block_claim_encode(&c, req + 1);
    CHECK(!net_exchange(&cli, &a, ids[who].xonly, req, len, ans, sizeof ans, &n, NULL));   /* not the owner */
    /* no records: a STORE that a node of the records takes, an ALERT, a PROVE */
    record r = recs[0], r2 = recs[0];
    memcpy(r.owner, cid.xonly, 32), memcpy(r2.owner, ids[1].xonly, 32);
    r2.t = r.t == 1 ? 0 : 1;
    uint8_t m[1 + 2 * RECORD_LEN];
    m[0] = DHT_STORE, m[1] = 1, record_wire_encode(&r, m + 2);
    a.port = dht_port(nodes[0].d);
    CHECK(net_exchange(&cli, &a, nodes[0].id.xonly, m, 2 + RECORD_WIRE, ans, sizeof ans, &n, NULL) && n == 3);
    a.port = dht_port(ln[0]);
    CHECK(!net_exchange(&cli, &a, ids[0].xonly, m, 2 + RECORD_WIRE, ans, sizeof ans, &n, NULL));
    m[0] = DHT_ALERT, record_encode(&r, m + 1), record_encode(&r2, m + 1 + RECORD_LEN);
    CHECK(!net_exchange(&cli, &a, ids[0].xonly, m, 1 + 2 * RECORD_LEN, ans, sizeof ans, &n, NULL));
    m[0] = DHT_PROVE, m[1] = 1, record_wire_encode(&r, m + 2), memset(m + 2 + RECORD_WIRE, 1, 64);
    CHECK(!net_exchange(&cli, &a, ids[0].xonly, m, 2 + RECORD_WIRE + 64, ans, sizeof ans, &n, NULL));
    CHECK(dht_local_count(ln[0]) == 0 && file_size(dir0, "store.bin") < 0 && file_size(dir0, "claims.bin") > 0);
    /* nor a relay: that search has no sessions (before, a home waited there as at any node) */
    uint8_t rq[65] = {DHT_RELAY};
    int waiting, active;
    uint64_t done;
    memcpy(rq + 33, ids[2].xonly, 32);
    net_conn *rc = net_dial(&cli, &a, ids[0].xonly);
    CHECK(rc && net_send(rc, rq, sizeof rq));
    nap(300);
    dht_relays(ln[0], &waiting, &active, &done);
    CHECK(waiting == 0 && active == 0);
    net_close(rc);
    printf("t_dht: a linear search: a claim published, found, refused from another identity; no records, no relay\n");
}

static void test_linear(void)
{
    home_identity ids[LINEAR];
    dht *ln[LINEAR];
    char names[LINEAR][32], dir0[64];
    bool up = true;
    linear = &bnet;
    for (int i = 0; i < LINEAR; i++) {
        snprintf(names[i], sizeof names[i], "s-linear%d", i);
        up &= (ln[i] = scratch(names[i], true, DHT_ANY_PORT, -1, &ids[i])) != NULL;
    }
    linear = NULL;
    CHECK(up);
    for (int i = 1; up && i < LINEAR; i++) seen_port(ln[i], &ids[0], dht_port(ln[0]));
    for (const int64_t t0 = ms(); up && ms() - t0 < 10000; nap(50)) {
        int full = 0;
        for (int i = 0; i < LINEAR; i++) full += dht_count(ln[i]) == LINEAR - 1;
        if (full == LINEAR) break;
        for (int i = 1; i < LINEAR; i++) dht_refresh(ln[i]);
    }
    for (int i = 0; up && i < LINEAR; i++) CHECK(dht_count(ln[i]) == LINEAR - 1);
    snprintf(dir0, sizeof dir0, ROOT "/%s", names[0]);
    if (up) linear_checks(ln, ids, dir0);
    for (int i = 0; i < LINEAR; i++)
        if (ln[i]) scratch_end(ln[i], names[i]);
}

/* What a public network refuses, and the limits by address block: in NODES answers (loopback and private addresses, port 0) and in the
 * table (2 of a block in a bucket, 4 in the table). An answer may name one address twice and many contacts of one block: they are the
 * candidates of a lookup, which asks 4 nodes of a block at most. A test network takes loopback without a limit. A node of a public
 * network on 127.0.0.1 bootstraps from node 0 and keeps nothing: every answer points at loopback. Before, all of it was taken. */
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
    CHECK(dht_parse_nodes(tst, m, l, out) == 3 && dht_parse_nodes(pub, m, l, out) == 3);
    l = nodes_msg(m, ids, BLOCK5, 5);
    CHECK(dht_parse_nodes(tst, m, l, out) == 5 && dht_parse_nodes(pub, m, l, out) == 5);
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
 * the NODES answer gives both, and the asker takes it. Before, every asker refused the whole answer (one address, two identities) and
 * counted it as a failure of this node. */
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
    CHECK(dht_parse_nodes(sd, ans, n, out) == 3 && !memcmp(out[0].id, ids[1].id, 32));
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
    enum { H = 3, TAR = H * DHT_K };                           /* a port for each name */
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
    /* a connection that takes its time over the handshake has no more of it for its request: it is closed two seconds after it came
     * (before, two seconds after the handshake: each had its own) */
    net_conn *c = calloc(1, sizeof *c);
    const int late = raw_connect(a.port);
    nap(1200);
    CHECK(c && late >= 0);
    if (c && late >= 0) {
        c->fd = late, c->timeout_ms = 2000;
        CHECK(net_rehandshake(&cli, c, true, sid.xonly) && closed_within(late, 1500));
        net_close(c);
    }
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

/* A pair whose other side gave up long ago, started again by a home that comes back after an absence: the other side waits for
 * nothing, so a side that would only wait for its connection dials it too after one slot. Before, a side that the others reach and
 * that is party 1 of the pair waited, then went to the relays, where nobody came. */
static void test_partner_back(void)
{
    node *p0 = &nodes[3], *p1 = &nodes[4];
    if (memcmp(p0->id.xonly, p1->id.xonly, 32) > 0) p0 = &nodes[4], p1 = &nodes[3];
    uint8_t name[32], got[8];
    const uint8_t m = DHT_SESSION_FIRST;
    size_t n;
    hashed("a pair that comes back", name);
    const int64_t t0 = ms();
    net_conn *c = dht_channel(p1->d, p0->id.xonly, name, 6000, NULL, NULL);
    CHECK(c != NULL && ms() - t0 < 4000);
    CHECK(c && net_send(c, &m, 1) && net_recv(c, got, sizeof got, &n) && n == 1 && got[0] == m);   /* the partner's session answered */
    net_close(c);
}

/* The first home of a network: alone, and past the grace in which it counts its own port open, it publishes its points to nobody. They
 * go at its first contact, not at the republish of the next day: a node that joins holds them within a few seconds. Before, the
 * points of a home that had been alone waited for that republish, or for a restart. */
static void test_first_contact(void)
{
    home_identity aid, bid;
    probe_s = 1;
    dht *a = scratch("s-first-a", true, DHT_ANY_PORT, -1, &aid);
    probe_s = 0;
    dht *b = scratch("s-first-b", true, DHT_ANY_PORT, -1, &bid);
    CHECK(a && b);
    if (!a || !b) return;
    nap(1300);
    record pts[20];
    for (int i = 0; i < 20; i++) {
        char label[32];
        uint8_t dk[32];
        snprintf(label, sizeof label, "first %d", i);
        hashed(label, dk);
        even_point(dk, pts[i].x);
        pts[i].t = i & 1, pts[i].dp = net.dpbits;
    }
    CHECK(dht_publish(a, pts, 20) == 0);                       /* nobody to hold them, not even a itself */
    dht_contact cb;
    memcpy(cb.id, bid.id, 32), memcpy(cb.xonly, bid.xonly, 32);
    CHECK(net_parse_addr("127.0.0.1:1", &cb.addr));
    cb.addr.port = dht_port(b);
    const int64_t t0 = ms();
    dht_seen(a, &cb);                                            /* b answered a: its first contact */
    while (ms() - t0 < 10000 && dht_local_count(b) < 20) nap(50);
    CHECK(dht_local_count(b) == 20);
    printf("t_dht: the points of a home that was alone held by the first node it met in %.1f s\n", (double)(ms() - t0) / 1000);
    scratch_end(a, "s-first-a");
    scratch_end(b, "s-first-b");
}

/* A flood of alerts that will never end: a point that A2 does not hold, in a record with its name, and the same point under 1100
 * identities, each with its proof (who holds a point signs for any name). A2 does not know that point's distance, so they all wait, and
 * fill its table of alerts (1024). Then a real coincidence of A2 with B2: before, it was refused (the table was full of waiting alerts
 * and kept them for a day); now the oldest waiting one goes, and the real one gets its on_alert. */
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
        record r = {.t = 0, .dp = net.dpbits};
        memcpy(r.x, xf, 32), memcpy(r.owner, sy.xonly, 32);
        uint8_t req[1 + 2 * RECORD_LEN + 64], ans[1], h[32];
        size_t n;
        req[0] = DHT_ALERT;
        record_encode(&r, req + 1), record_encode(&rF, req + 1 + RECORD_LEN);
        pair_hash(&r, &rF, h);
        CHECK(record_prove(ctx, &net, &r, dA, A2->id.xonly, h, req + 1 + 2 * RECORD_LEN));
        sent += net_exchange(&cli, &a, A2->id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 1 && ans[0] == DHT_OK;
    }
    CHECK(sent == 1100);
    printf("t_dht: 1100 alerts of a point the node does not hold sent in %.1f s\n", (double)(ms() - t0) / 1000);
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

/* the relays done by the nodes: all of them, also E and LATE, which listen and may be the relay of a slot (before, only the first N
 * were counted, and a channel relayed by one of those two failed the count, as seen once on a Mac) */
static uint64_t relays_done(void)
{
    uint64_t sum = 0;
    for (int i = 0; i < NODES; i++) {
        int w, a;
        uint64_t dn = 0;
        if (nodes[i].d) dht_relays(nodes[i].d, &w, &a, &dn);
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
    /* the relay of this channel ends when its homes close it. Counted from here: a relay that joined one of them to a partner already
     * gone at the end of a slot, or whose end-to-end handshake failed, ended long before */
    const uint64_t done0 = relays_done();
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

/* ---- coincidences against the attacks of custodians and senders ---- */

/* d = d0 - q': the distance of the wild + point at the x of the tame point d0*G */
static void wild_of(const uint8_t d0[32], uint8_t d[32])
{
    uint8_t nq[32];
    memcpy(nq, qp, 32);
    CHECK(secp256k1_ec_seckey_negate(ctx, nq));
    memcpy(d, d0, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, d, nq));
}

static record rec_of(int t, const uint8_t x[32], const uint8_t owner[32])
{
    record r = {.t = t, .dp = net.dpbits};
    memcpy(r.x, x, 32), memcpy(r.owner, owner, 32);
    return r;
}

/* Copies of a point planted at all its custodians before its owner's record, under as many identities as a custodian keeps owners of
 * one x (a custodian that saw the owner's STORE can plant them at the others before the owner's republish reaches them): the owner's
 * record, refused for a full x, is kept once it is proved, and a real coincidence with that point is still alerted to both owners.
 * Before, two copies kept the pair from the other owner for ever (the custodians gave it the first two of the x), and then seven (the
 * first seven), and eight kept the owner's record out. */
static void test_planted_copies(void)
{
    node *A = &nodes[10], *B = &nodes[12];
    uint8_t dA[32], dB[32], x[32], k[32];
    int who[DHT_KSTORE];
    hashed("planted", dA), even_point(dA, x), wild_of(dA, dB);
    give_point(A, 0, x, dA), give_point(B, 1, x, dB);
    store_key(x, k);
    dht_contact cs[DHT_KSTORE];                                 /* the custodians A finds: node E of the eviction may be one */
    const int m = dht_lookup(A->d, k, cs, DHT_KSTORE);
    bool known = m == DHT_KSTORE;
    for (int j = 0; j < m; j++) {
        for (who[j] = 0; who[j] < NODES && (!nodes[who[j]].d || memcmp(nodes[who[j]].id.id, cs[j].id, 32)); who[j]++) {}
        known &= who[j] < NODES;
    }
    CHECK(known);
    if (!known) return;
    for (int c = 0; c < STORE_X_OWNERS; c++) {
        char label[32];
        home_identity pid;
        net_cfg cli;
        snprintf(label, sizeof label, "planter %d", c);
        make_id(label, &pid);
        net_cfg_init(&cli, ctx, &pid, FP, BITS, 0, 2000);
        const record cp = rec_of(1, x, pid.xonly);
        uint8_t req[2 + RECORD_WIRE] = {DHT_STORE, 1}, ans[4];
        size_t n;
        record_wire_encode(&cp, req + 2);
        for (int j = 0; j < m; j++) {
            net_addr a;
            CHECK(net_parse_addr("127.0.0.1:1", &a));
            a.port = dht_port(nodes[who[j]].d);
            CHECK(net_exchange(&cli, &a, nodes[who[j]].id.xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 3 &&
                  ans[2] == (c ? DHT_ST_COINCIDENCE : DHT_ST_STORED));
        }
    }
    record rA, rB, mine, theirs;
    bool merge;
    CHECK(publish_one(A, 0, x, &rA) && publish_one(B, 1, x, &rB));
    const int64_t t0 = ms();
    while (ms() - t0 < 20000 && (alerts_of(A, NULL, NULL, NULL) < 1 || alerts_of(B, NULL, NULL, NULL) < 1)) nap(50);
    CHECK(alerts_of(A, &mine, &theirs, &merge) == 1 && same(&mine, &rA) && same(&theirs, &rB) && !merge);
    CHECK(alerts_of(B, &mine, &theirs, &merge) == 1 && same(&mine, &rB) && same(&theirs, &rA) && !merge);
    printf("t_dht: a coincidence behind %d copies planted at every custodian, alerted in %.1f s\n", STORE_X_OWNERS, (double)(ms() - t0) / 1000);
}

/* Copies planted on eight other points of an owner, at the custodians of a point where it has a real coincidence, and all nine sent
 * in one STORE, that point last: the owner proves every record a custodian says COINCIDENCE for, and hears of the real one. Before, it
 * proved the first eight of a STORE (and a custodian said COINCIDENCE for eight records of an address block at once): the copies took
 * those places, at every republish again, and the owner never heard of the real one from its custodians. */
static void test_starved_coincidence(void)
{
    enum { COPIES = DHT_PROVE_MAX };
    node *A = &nodes[14], *B = &nodes[16];
    uint8_t d[COPIES + 1][32], dB[32], k[32];
    record batch[COPIES + 1], rB, mine, theirs;
    dht_contact cs[DHT_K], cc[DHT_K];                           /* the custodians as a publish finds them: the closest of a lookup of 20 */
    bool merge;
    home_identity pid;
    net_cfg cli;
    make_id("starver", &pid);
    net_cfg_init(&cli, ctx, &pid, FP, BITS, 0, 2000);
    const int m = DHT_KSTORE;
    bool found = false;
    for (int i = 0; i < 100 && !found; i++) {                   /* the real point: A is no custodian of it */
        char label[32];
        snprintf(label, sizeof label, "starved %d", i);
        hashed(label, d[COPIES]), even_point(d[COPIES], batch[COPIES].x);
        store_key(batch[COPIES].x, k);
        found = dht_lookup(A->d, k, cs, DHT_K) >= m && !has(cs, m, A->id.id, NULL);
    }
    CHECK(found);
    int got = 0;
    for (int i = 0; found && got < COPIES && i < 200000; i++) { /* eight points with the same custodians, a copy planted at each */
        char label[32];
        uint8_t ki[32], req[2 + RECORD_WIRE] = {DHT_STORE, 1}, ans[4];
        size_t n;
        snprintf(label, sizeof label, "starving %d", i);
        hashed(label, d[got]), even_point(d[got], batch[got].x);
        store_key(batch[got].x, ki);
        if (ki[0] != k[0] || dht_lookup(A->d, ki, cc, DHT_K) < m) continue;
        int shared = 0;
        for (int j = 0; j < m; j++) shared += has(cc, m, cs[j].id, NULL);
        if (shared != m) continue;
        const record cp = rec_of(1, batch[got].x, pid.xonly);
        record_wire_encode(&cp, req + 2);
        for (int j = 0; j < m; j++)
            CHECK(net_exchange(&cli, &cs[j].addr, cs[j].xonly, req, sizeof req, ans, sizeof ans, &n, NULL) && n == 3 && ans[2] == DHT_ST_STORED);
        got++;
    }
    CHECK(got == COPIES);
    if (got < COPIES) return;
    for (int i = 0; i <= COPIES; i++) batch[i].t = 0, batch[i].dp = net.dpbits, give_point(A, 0, batch[i].x, d[i]);
    wild_of(d[COPIES], dB);
    give_point(B, 1, batch[COPIES].x, dB);
    CHECK(publish_one(B, 1, batch[COPIES].x, &rB) && dht_publish(A->d, batch, COPIES + 1) == COPIES + 1);
    const int64_t t0 = ms();
    while (ms() - t0 < 20000 && (alerts_of(A, NULL, NULL, NULL) < 1 || alerts_of(B, NULL, NULL, NULL) < 1)) nap(50);
    CHECK(alerts_of(A, &mine, &theirs, &merge) == 1 && same(&mine, &batch[COPIES]) && same(&theirs, &rB) && !merge);
    CHECK(alerts_of(B, &mine, &theirs, &merge) == 1 && same(&mine, &rB) && same(&theirs, &batch[COPIES]) && !merge);
    printf("t_dht: a coincidence behind %d copies planted on other points of its owner, alerted in %.1f s\n", COPIES, (double)(ms() - t0) / 1000);
}

/* A node of the table at an address of its own that answers as a custodian would, except: every record of a STORE is a coincidence, and
 * PROVE and MBOX_GET get the answer the test set (LIAR_PAIRS) or, PROVE, none (LIAR_SILENT: it holds the connection past the timeout
 * of the asker); or every record of a STORE is stored, at once (LIAR_OK) or after a second (LIAR_SLOW). Each connection on a thread
 * of its own. */
enum { LIAR_OK, LIAR_PAIRS, LIAR_SILENT, LIAR_SLOW };

typedef struct {
    home_identity id;
    net_cfg cfg;
    int lfd;
    uint16_t port;
    pthread_t th;
    atomic_int mode, proves, live;
    atomic_bool stop;
    pthread_mutex_t mu;
    uint8_t mbox[2 + DHT_MBOX_MAX * DHT_MBOX_ENTRY];   /* LIAR_PAIRS: the answer to PROVE */
    size_t mbox_len;
} liar;

typedef struct {
    liar *l;
    int fd;
    net_addr from;
} liar_conn;

static void *liar_conn_main(void *arg)
{
    liar_conn *lc = arg;
    liar *l = lc->l;
    uint8_t *req = malloc(NOISE_FRAME_MAX), *ans = malloc(sizeof l->mbox);
    size_t len, n = 0;
    net_conn *c = net_handshake(&l->cfg, lc->fd, &lc->from);
    const int mode = atomic_load(&l->mode);
    if (c && req && ans && net_recv(c, req, NOISE_FRAME_MAX, &len) && len) {
        if (req[0] == DHT_PING) ans[0] = DHT_PONG, n = 1;
        else if (req[0] == DHT_FIND_NODE) ans[0] = DHT_NODES, ans[1] = 0, n = 2;
        else if (req[0] == DHT_ALERT) ans[0] = DHT_OK, n = 1;
        else if (req[0] == DHT_MBOX_GET && mode != LIAR_PAIRS) ans[0] = DHT_MBOX, ans[1] = 0, n = 2;
        else if (req[0] == DHT_STORE && len >= 2) {
            if (mode == LIAR_SLOW) nap(1000);
            ans[0] = DHT_STORED, ans[1] = req[1], n = 2 + (size_t)req[1];
            memset(ans + 2, mode == LIAR_OK || mode == LIAR_SLOW ? DHT_ST_STORED : DHT_ST_COINCIDENCE, req[1]);
        } else if (req[0] == DHT_PROVE || req[0] == DHT_MBOX_GET) {
            atomic_fetch_add(&l->proves, req[0] == DHT_PROVE);
            if (mode == LIAR_SILENT) nap(timeout_ms + 500);
            pthread_mutex_lock(&l->mu);
            if (mode == LIAR_PAIRS) memcpy(ans, l->mbox, l->mbox_len), n = l->mbox_len;
            pthread_mutex_unlock(&l->mu);
        }
        if (n) (void)net_send(c, ans, n);
    }
    net_close(c);
    free(req), free(ans), free(lc);
    atomic_fetch_sub(&l->live, 1);
    return NULL;
}

static void *liar_main(void *arg)
{
    liar *l = arg;
    while (!atomic_load(&l->stop)) {
        net_addr from;
        pthread_t th;
        const int fd = net_accept(l->lfd, 100, &from);
        liar_conn *lc = fd < 0 ? NULL : malloc(sizeof *lc);
        if (fd >= 0 && !lc) net_close_fd(fd);
        if (!lc) continue;
        *lc = (liar_conn){l, fd, from};
        atomic_fetch_add(&l->live, 1);
        if (pthread_create(&th, NULL, liar_conn_main, lc) == 0) pthread_detach(th);
        else atomic_fetch_sub(&l->live, 1), net_close_fd(fd), free(lc);
    }
    return NULL;
}

/* the answer of the liar to PROVE and to MBOX_GET: an MBOX with these pairs, without proofs */
static void liar_answer(liar *l, const mbox_pair *p, int n)
{
    pthread_mutex_lock(&l->mu);
    l->mbox[0] = DHT_MBOX, l->mbox[1] = (uint8_t)n;
    memset(l->mbox + 2, 0, (size_t)n * DHT_MBOX_ENTRY);
    for (int i = 0; i < n; i++) {
        record_encode(&p[i].r[0], l->mbox + 2 + (size_t)i * DHT_MBOX_ENTRY);
        record_encode(&p[i].r[1], l->mbox + 2 + (size_t)i * DHT_MBOX_ENTRY + RECORD_LEN);
    }
    l->mbox_len = 2 + (size_t)n * DHT_MBOX_ENTRY;
    pthread_mutex_unlock(&l->mu);
}

/* a pair as an MBOX carries it */
static mbox_pair pair_of(const record *a, const record *b)
{
    mbox_pair p;
    memset(&p, 0, sizeof p);
    pair_order(a, b, p.r);
    return p;
}

typedef struct {
    dht *d;
    int64_t end;
} repub_job;

static void *repub_main(void *arg)
{
    repub_job *j = arg;
    (void)dht_republish(j->d, true);
    j->end = ms();
    return NULL;
}

static void lying_checks(node *O, node *P, liar *l)
{
    /* the answer to PROVE: O takes the pair of the record it proved (before, O refused every answer to PROVE: it waited for type 16), and
     * not the pair of another point of O, which P could prove too (before, a custodian could bring 64 pairs of any records of O) */
    uint8_t d[4][32], x[4][32], w[4][32];
    for (int i = 0; i < 4; i++) {
        char label[32];
        snprintf(label, sizeof label, "lied %d", i);
        hashed(label, d[i]), even_point(d[i], x[i]), wild_of(d[i], w[i]);
        give_point(O, 0, x[i], d[i]), give_point(P, 1, x[i], w[i]);
    }
    record rO[4], rP[4], got, mine, theirs;
    for (int i = 0; i < 4; i++) rO[i] = rec_of(0, x[i], O->id.xonly), rP[i] = rec_of(1, x[i], P->id.xonly);
    mbox_pair pp[STORE_X_OWNERS + 1] = {pair_of(&rO[0], &rP[0]), pair_of(&rO[1], &rP[1])};
    bool merge;
    liar_answer(l, pp, 2);
    atomic_store(&l->mode, LIAR_PAIRS);
    CHECK(publish_one(O, 0, x[0], &got));
    int64_t t0 = ms();
    while (ms() - t0 < 10000 && (alerts_of(O, NULL, NULL, NULL) < 1 || alerts_of(P, NULL, NULL, NULL) < 1)) nap(50);
    CHECK(alerts_of(O, &mine, &theirs, &merge) == 1 && same(&mine, &rO[0]) && same(&theirs, &rP[0]));
    CHECK(alerts_of(P, &mine, &theirs, &merge) == 1 && same(&mine, &rP[0]) && same(&theirs, &rO[0]));
    /* the liar as a mailbox of O: the pair of another point of O, which no owner proved, is no alert (before, any mailbox made one) */
    liar_answer(l, pp + 1, 1);
    CHECK(dht_mbox_poll(O->d) == 0);
    /* more pairs for one record than a custodian keeps owners of its x: the answer is refused whole */
    pp[0] = pair_of(&rO[2], &rP[2]);
    for (int i = 1; i <= STORE_X_OWNERS; i++) {
        char label[32];
        home_identity sy;
        snprintf(label, sizeof label, "lied owner %d", i);
        make_id(label, &sy);
        const record q = rec_of(1, x[2], sy.xonly);
        pp[i] = pair_of(&rO[2], &q);
    }
    liar_answer(l, pp, STORE_X_OWNERS + 1);
    CHECK(publish_one(O, 0, x[2], &got));
    nap(1500);
    CHECK(alerts_of(O, NULL, NULL, NULL) == 1 && alerts_of(P, NULL, NULL, NULL) == 1);
    printf("t_dht: a custodian's answer to PROVE: the pair of the record proved taken, not one of another record, nor too many\n");

    /* a custodian that leaves PROVE unanswered costs one timeout for a batch (before: one for each record, one after the other) */
    record r20[20];
    for (int i = 0; i < 20; i++) {
        char label[32];
        uint8_t di[32];
        snprintf(label, sizeof label, "silent %d", i);
        hashed(label, di), even_point(di, r20[i].x);
        give_point(O, 0, r20[i].x, di);
        r20[i].t = 0, r20[i].dp = net.dpbits;
    }
    atomic_store(&l->mode, LIAR_SILENT);
    const int proves = atomic_load(&l->proves);
    t0 = ms();
    CHECK(dht_publish(O->d, r20, 20) == 20);
    const int64_t took = ms() - t0;
    printf("t_dht: 20 records to a custodian that does not answer PROVE in %.1f s\n", (double)took / 1000);
    CHECK(took < 2 * timeout_ms + 1500 && atomic_load(&l->proves) == proves + 1);
    /* a PROVE that failed during a republish: its records go to that custodian again at the next one, though it said it holds them
     * (before, they counted as held, and the custodian was left out until the weekly republish to all) */
    (void)dht_republish(O->d, true);
    atomic_store(&l->mode, LIAR_OK);
    CHECK(dht_republish(O->d, false) > 0 && dht_republish(O->d, false) == 0);
    /* every record a custodian says COINCIDENCE for is proved, DHT_PROVE_MAX at a time: 20 records, three PROVEs */
    liar_answer(l, pp, 0);
    atomic_store(&l->mode, LIAR_PAIRS);
    for (int i = 0; i < 20; i++) {
        char label[32];
        uint8_t di[32];
        snprintf(label, sizeof label, "proved %d", i);
        hashed(label, di), even_point(di, r20[i].x);
        give_point(O, 0, r20[i].x, di);
    }
    CHECK(dht_publish(O->d, r20, 20) == 20 && atomic_load(&l->proves) == proves + 2 + (20 + DHT_PROVE_MAX - 1) / DHT_PROVE_MAX);

    /* a publish while a republish waits for a slow custodian: it does not wait for the republish (before, it waited for all of it) */
    record *bulk = malloc(600 * sizeof *bulk), one;
    for (int i = 0; bulk && i < 600; i++) {
        char label[32];
        uint8_t di[32];
        snprintf(label, sizeof label, "bulk %d", i);
        hashed(label, di), even_point(di, bulk[i].x);
        bulk[i].t = i % 3 - 1, bulk[i].dp = net.dpbits;
    }
    atomic_store(&l->mode, LIAR_OK);
    CHECK(bulk && dht_publish(O->d, bulk, 600) == 600);
    free(bulk);
    atomic_store(&l->mode, LIAR_SLOW);
    repub_job rj = {O->d, 0};
    pthread_t th;
    const int64_t r0 = ms();
    CHECK(pthread_create(&th, NULL, repub_main, &rj) == 0);
    nap(300);
    t0 = ms();
    CHECK(publish_one(O, 1, x[3], &one));
    const int64_t pub_end = ms();
    pthread_join(th, NULL);
    printf("t_dht: a publish of %.1f s during a republish of %.1f s to a slow custodian\n", (double)(pub_end - t0) / 1000, (double)(rj.end - r0) / 1000);
    CHECK(pub_end - t0 < 2500 && rj.end > pub_end);
}

/* PROVE against a custodian that lies, on a network of three of its own: O publishes, P is the other owner of the points (it never
 * publishes them: only the liar tells of the pairs), and the liar is a custodian of every key. */
static void test_lying_custodian(void)
{
    static node tiny[2];
    static liar l;
    node *O = &tiny[0], *P = &tiny[1];
    for (int i = 0; i < 2; i++) {
        memset(&tiny[i], 0, sizeof tiny[i]);
        pthread_mutex_init(&tiny[i].mu, NULL);
        make_id(i ? "tiny P" : "tiny O", &tiny[i].id);
        snprintf(tiny[i].dir, sizeof tiny[i].dir, ROOT "/%s", i ? "t-p" : "t-o");
        clean_dir(tiny[i].dir);
    }
    memset(&l, 0, sizeof l);
    pthread_mutex_init(&l.mu, NULL);
    make_id("liar", &l.id);
    l.lfd = net_listen("127.0.0.1", 0, &l.port);
    net_cfg_init(&l.cfg, ctx, &l.id, FP, BITS, l.port, 2000);
    const bool up = start_node(P, DHT_ANY_PORT, -1) && start_node(O, DHT_ANY_PORT, -1) && l.lfd >= 0 && pthread_create(&l.th, NULL, liar_main, &l) == 0;
    CHECK(up);
    if (up) {
        seen_port(O->d, &P->id, dht_port(P->d)), seen_port(P->d, &O->id, dht_port(O->d));
        for (int i = 0; i < 2; i++) {                           /* the liar asks each, which pings it and takes it */
            net_addr a;
            uint8_t ans[1];
            size_t len;
            CHECK(net_parse_addr("127.0.0.1:1", &a));
            a.port = dht_port(tiny[i].d);
            CHECK(net_exchange(&l.cfg, &a, tiny[i].id.xonly, (const uint8_t[]){DHT_PING}, 1, ans, 1, &len, NULL));
        }
        for (const int64_t t0 = ms(); ms() - t0 < 5000 && (dht_count(O->d) < 2 || dht_count(P->d) < 2); nap(20)) {}
        CHECK(dht_count(O->d) == 2 && dht_count(P->d) == 2);
        lying_checks(O, P, &l);
    }
    atomic_store(&l.stop, true);
    if (up) pthread_join(l.th, NULL);
    while (atomic_load(&l.live)) nap(20);
    net_close_fd(l.lfd);
    for (int i = 0; i < 2; i++) {
        dht_stop(tiny[i].d);
        clean_dir(tiny[i].dir);
        pthread_mutex_destroy(&tiny[i].mu);
    }
    pthread_mutex_destroy(&l.mu);
}

/* An exchange from the source address src of this machine (127.0.0.2: another address block than the one of 127.0.0.1), as
 * net_exchange makes it; false also when the system has no such address. */
static bool exchange_from(const char *src, const net_cfg *cfg, uint16_t port, const uint8_t expect[32], const uint8_t *req, size_t rlen,
                          uint8_t *ans, size_t cap, size_t *alen)
{
    struct sockaddr_in s, t;
    memset(&s, 0, sizeof s), memset(&t, 0, sizeof t);
    s.sin_family = t.sin_family = AF_INET;
    t.sin_port = htons(port);
    t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int fd = sys_socket(AF_INET, SOCK_STREAM);
    net_conn *c = calloc(1, sizeof *c);
    bool ok = fd >= 0 && c && inet_pton(AF_INET, src, &s.sin_addr) == 1 && bind(fd, (const struct sockaddr *)&s, sizeof s) == 0;
    if (ok) (void)connect(fd, (const struct sockaddr *)&t, sizeof t);   /* non-blocking: connected once it can be written */
    ok = ok && sys_wait(fd, true, ms() + 2000) > 0;
    if (!ok) {
        sys_close(fd), free(c);
        return false;
    }
    c->fd = fd, c->timeout_ms = cfg->timeout_ms;
    ok = net_rehandshake(cfg, c, true, expect) && net_send(c, req, rlen) && net_recv(c, ans, cap, alen);
    net_close(c);
    return ok;
}

/* V, a node alone on a public network (whose limits apply to loopback as to any address), started; its jobs of an alert of its own
 * points cost about 2 * slow_ms (own_distance sleeps: on a real network it is the lookups and connections of the job) */
static bool victim(node *V, const char *name, int slow_ms)
{
    char err[200];
    memset(V, 0, sizeof *V);
    pthread_mutex_init(&V->mu, NULL);
    make_id(name, &V->id);
    snprintf(V->dir, sizeof V->dir, ROOT "/%s", name);
    clean_dir(V->dir);
    V->slow_ms = slow_ms;
    dht_config cfg = node_cfg(V, DHT_ANY_PORT);
    cfg.allow_private = false;
    V->d = dht_create(&cfg, err, sizeof err);
    CHECK(V->d && dht_start(V->d) >= 0);
    return V->d != NULL;
}

static void victim_end(node *V)
{
    dht_stop(V->d);
    clean_dir(V->dir);
    pthread_mutex_destroy(&V->mu);
}

/* ALERT(a, rV, proof) to V from the loopback address src (each one a block of its own): a is the record of type t at the x of rV under
 * the identity named who, with its proof for V, made with d, the distance of that point for that type. Whether V took it; *out: a. */
static bool alert_from(const char *src, const net_cfg *cli, const node *V, const char *who, int t, const uint8_t d[32], const record *rV,
                       record *out)
{
    uint8_t req[1 + 2 * RECORD_LEN + 64], ans[4], h[32];
    size_t n;
    home_identity id;
    make_id(who, &id);
    const record a = rec_of(t, rV->x, id.xonly);
    if (out) *out = a;
    req[0] = DHT_ALERT, record_encode(&a, req + 1), record_encode(rV, req + 1 + RECORD_LEN);
    pair_hash(&a, rV, h);
    CHECK(record_prove(ctx, &net, &a, d, V->id.xonly, h, req + 1 + 2 * RECORD_LEN));
    return exchange_from(src, cli, dht_port(V->d), V->id.xonly, req, sizeof req, ans, sizeof ans, &n) && n == 1 && ans[0] == DHT_OK;
}

/* Alerts of a point that a node does not hold, in a record with its name, under 64 identities that prove it (who holds a point signs
 * for any name), from one address block: they hold ALERT_JOBS_BLOCK (8) places of the queue of jobs, also when tried again, so a real
 * alert from another block is checked after a few of them (150 ms each here). Before, the 64 alerts that one address sends at once
 * were all queued before the real one: 10 s, and a flood that goes on kept it waiting. */
static void test_alert_blocks(void)
{
    static node V;
    if (!victim(&V, "s-blocks", 150)) return;
    uint8_t dV[32], x[32], dF[32], xf[32];
    hashed("blocks", dV), even_point(dV, x);
    hashed("blocks flood", dF), even_point(dF, xf);
    give_point(&V, 0, x, dV);
    const record rV = rec_of(0, x, V.id.xonly), rF = rec_of(0, xf, V.id.xonly);
    uint8_t dW[32];
    record rB;
    wild_of(dV, dW);
    home_identity cid;
    make_id("blocks client", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    int sent = 0;
    for (int i = 0; i < 64 && sent == i; i++) {
        char label[32];
        snprintf(label, sizeof label, "blocks sybil %d", i);
        sent += alert_from("127.0.0.2", &cli, &V, label, 0, dF, &rF, NULL);
    }
    if (!sent) printf("t_dht: no address 127.0.0.2 on this machine: the alerts of two blocks not tried\n");
    if (sent) {
        CHECK(sent == 64);
        const int64_t t0 = ms();
        CHECK(alert_from("127.0.0.1", &cli, &V, "blocks partner", 1, dW, &rV, &rB));
        while (ms() - t0 < 20000 && alerts_of(&V, NULL, NULL, NULL) < 1) nap(20);
        record mine, theirs;
        bool merge;
        const int64_t took = ms() - t0;
        CHECK(alerts_of(&V, &mine, &theirs, &merge) == 1 && same(&mine, &rV) && same(&theirs, &rB) && took < 5000);
        printf("t_dht: a real alert behind 64 of another block that never end in %.1f s\n", (double)took / 1000);
    }
    victim_end(&V);
}

/* A real alert whose job waits behind slow ones (8 alerts from one block, 0.4 s each) while 1152 alerts of 18 other blocks fill the
 * table of alerts (1024), all of them of a point V does not hold, proved under other names: its job in the queue keeps it there, and
 * it is checked when its turn comes. Before, the flood pushed it out first, its job found nothing, and V never got the alert. */
static void test_alert_queue(void)
{
    static node V;
    if (!victim(&V, "s-queue", 400)) return;
    uint8_t dV[32], x[32], dF[32], xf[32];
    hashed("queue", dV), even_point(dV, x);
    hashed("queue flood", dF), even_point(dF, xf);             /* a point with V's name that V does not hold */
    give_point(&V, 0, x, dV);
    const record rV = rec_of(0, x, V.id.xonly), rF = rec_of(0, xf, V.id.xonly);
    uint8_t dW[32];
    record rB;
    wild_of(dV, dW);
    home_identity cid;
    make_id("queue client", &cid);
    net_cfg cli;
    net_cfg_init(&cli, ctx, &cid, FP, BITS, 0, 2000);
    bool ok = true;
    for (int i = 0; i < 8 && ok; i++) {
        char label[32];
        snprintf(label, sizeof label, "queue slow %d", i);
        ok = alert_from("127.0.0.20", &cli, &V, label, 0, dF, &rF, NULL);
    }
    if (!ok) {
        printf("t_dht: no address 127.0.0.20 on this machine: the alert behind a flood not tried\n");
        victim_end(&V);
        return;
    }
    const int64_t t0 = ms();
    CHECK(alert_from("127.0.0.1", &cli, &V, "queue partner", 1, dW, &rV, &rB));
    int sent = 0;
    for (int b = 2; b < 20; b++)
        for (int i = 0; i < 64; i++) {
            char label[48], src[24];
            snprintf(label, sizeof label, "queue flood %d %d", b, i), snprintf(src, sizeof src, "127.0.0.%d", b);
            sent += alert_from(src, &cli, &V, label, 0, dF, &rF, NULL);
        }
    const int64_t flood = ms() - t0;
    CHECK(sent == 18 * 64);
    while (ms() - t0 < 30000 && alerts_of(&V, NULL, NULL, NULL) < 1) nap(20);
    record mine, theirs;
    bool merge;
    CHECK(alerts_of(&V, &mine, &theirs, &merge) == 1 && same(&mine, &rV) && same(&theirs, &rB));
    printf("t_dht: a real alert behind 8 slow jobs and %d alerts of 18 blocks (sent in %.1f s), checked in %.1f s\n", sent,
           (double)flood / 1000, (double)(ms() - t0) / 1000);
    victim_end(&V);
}

/* STORE of the n records of r by the identity of cli to V, from 127.0.0.1: the status of each into st; false when V did not answer */
static bool store_to(const net_cfg *cli, const node *V, const record *r, int n, uint8_t *st)
{
    uint8_t req[2 + DHT_STORE_MAX * RECORD_WIRE] = {DHT_STORE, (uint8_t)n}, ans[2 + DHT_STORE_MAX];
    size_t len;
    net_addr a;
    for (int i = 0; i < n; i++) record_wire_encode(&r[i], req + 2 + i * RECORD_WIRE);
    CHECK(net_parse_addr("127.0.0.1:1", &a));
    a.port = dht_port(V->d);
    const bool ok = net_exchange(cli, &a, V->id.xonly, req, 2 + (size_t)n * RECORD_WIRE, ans, sizeof ans, &len, NULL) && len == 2 + (size_t)n;
    if (ok) memcpy(st, ans + 2, (size_t)n);
    return ok;
}

/* The owners of a custodian. A store has room for one owner in 256 records, keeps an owner for ever, and an identity costs little: so
 * an owner the store does not know costs its address block 256 of its record tokens (100 000 at once, 50 a second), also when its
 * record is refused. A block that spent them gets a new owner refused, and an owner the store knows served. Before, a new owner cost
 * one token: one address block filled the owners of a custodian in 7 hours, and no new home could store there, for ever. */
static void test_owner_cost(void)
{
    static node V;
    static record valid[DHT_STORE_MAX + 2], bad[DHT_STORE_MAX];
    if (!victim(&V, "s-owners", 0)) return;
    home_identity kid, xid, yid;
    net_cfg known, burner, fresh;
    uint8_t st[DHT_STORE_MAX], d[32];
    make_id("owner known", &kid), make_id("owner burner", &xid), make_id("owner new", &yid);
    net_cfg_init(&known, ctx, &kid, FP, BITS, 0, 2000), net_cfg_init(&burner, ctx, &xid, FP, BITS, 0, 2000);
    net_cfg_init(&fresh, ctx, &yid, FP, BITS, 0, 2000);
    for (int i = 0; i < DHT_STORE_MAX + 2; i++) {
        char label[32];
        snprintf(label, sizeof label, "owner point %d", i);
        hashed(label, d), even_point(d, valid[i].x);
        valid[i].t = 0, valid[i].dp = net.dpbits;
        if (i < DHT_STORE_MAX) bad[i] = valid[i], bad[i].dp = 40;   /* zero bits that x has not: refused after the tokens are taken */
    }
    CHECK(store_to(&known, &V, valid, 1, st) && st[0] == DHT_ST_STORED);
    /* 255 refused records of an owner the store does not know: 65 280 tokens; the next 255 find 34 464 left, enough for 134 */
    CHECK(store_to(&burner, &V, bad, DHT_STORE_MAX, st) && st[0] == DHT_ST_INVALID && st[DHT_STORE_MAX - 1] == DHT_ST_INVALID);
    CHECK(store_to(&burner, &V, bad, DHT_STORE_MAX, st) && st[0] == DHT_ST_INVALID && st[DHT_STORE_MAX - 1] == DHT_ST_FULL);
    CHECK(store_to(&known, &V, valid + 1, DHT_STORE_MAX, st) && st[0] == DHT_ST_STORED);   /* the rest of them, one a record */
    CHECK(store_to(&fresh, &V, valid, 1, st) && st[0] == DHT_ST_FULL);                    /* a new owner: 256, five seconds away */
    nap(200);
    CHECK(store_to(&known, &V, valid + DHT_STORE_MAX + 1, 1, st) && st[0] == DHT_ST_STORED);
    CHECK(dht_local_count(V.d) >= 3 && dht_local_count(V.d) <= 2 + DHT_STORE_MAX);
    victim_end(&V);
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
    /* blocks of 2^42 keys from 2^43, four of them; the target has bytes 8 to 12 of the two proof keys of t_block.c, in block 0 */
    memcpy(bnet.fp, FP, 32), bnet.bits = 42, bnet.start[26] = 0x08, memset(bnet.end + 26, 0xff, 6), bnet.end[26] = 0x17;
    memset(bnet.h160, 0x11, 20), memcpy(bnet.h160 + 8, "\x08\xe1\xd9\xa1\x9a", 5);
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
    test_estimate();
    printf("t_dht: %d records published by %d owners in %.1f s\n", RECS, OWNERS, (double)(ms() - t0) / 1000);
    test_coincidences();
    test_copied_point();
    test_malformed();
    test_restart();
    test_republish();
    test_eviction();
    test_moved_contact();
    test_planted_copies();                                     /* after the eviction: its alerts that wait make lookups */
    test_starved_coincidence();
    test_policy();
    test_nodes_answer();
    test_slowloris();
    test_alert_flood();
    test_alert_blocks();
    test_alert_queue();
    test_owner_cost();
    test_lying_custodian();
    test_linear();
    t0 = ms();
    test_relay_session();
    test_relay_limits();
    test_relay_stop();
    printf("t_dht: the relay in %.1f s\n", (double)(ms() - t0) / 1000);
    test_partner_back();
    test_first_contact();
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
