/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_session.c - two homes run the session of a coincidence against each other, in two threads, with the real pzfirma, over a channel in
 * memory and a chain in memory that checks every input of every transaction it is given (a standard P2PKH spend of an unspent output) and
 * mines a block every 200 ms. Two cases: the payout, half each; and a partner S that keeps the second transaction (and its last signing
 * message) to itself, which F answers by spending its tiny coin after 3 blocks. Needs PZFIRMA, PZFIRMA_PRIMES0 and PZFIRMA_PRIMES1; without
 * them it says it was skipped. */
#include "peertest.h"
#include "session.h"
#include <pzcore/hash.h>
#include <pthread.h>
#include <secp256k1_extrakeys.h>
#include <time.h>
#include <unistd.h>

static secp256k1_context *ctx;

/* ---- the chain ---- */

typedef struct {
    uint8_t txid[32], spk[BTC_SPK_MAX];
    uint32_t vout;
    uint64_t value, height;            /* 0: in the mempool */
    size_t spk_len;
    bool spent;
} utxo;

static struct {
    pthread_mutex_t mu;
    utxo u[256];
    int n;
    uint64_t tip;
    bool stop;
} chain = {.mu = PTHREAD_MUTEX_INITIALIZER, .tip = 1};

static utxo *find(const uint8_t txid[32], uint32_t vout)
{
    for (int i = 0; i < chain.n; i++)
        if (!memcmp(chain.u[i].txid, txid, 32) && chain.u[i].vout == vout) return &chain.u[i];
    return NULL;
}

static void add_utxo(const uint8_t txid[32], uint32_t vout, uint64_t value, const uint8_t *spk, size_t len, uint64_t height)
{
    utxo *u = &chain.u[chain.n++];
    memcpy(u->txid, txid, 32), memcpy(u->spk, spk, len);
    u->vout = vout, u->value = value, u->spk_len = len, u->height = height, u->spent = false;
}

static bool ch_broadcast(void *c, const btc_tx *tx)
{
    (void)c;
    uint8_t txid[32];
    btc_tx_txid(tx, txid);
    pthread_mutex_lock(&chain.mu);
    bool ok = find(txid, 0) != NULL;                            /* already there: sent by the partner */
    if (!ok) {
        uint64_t in = 0, out = 0;
        ok = true;
        for (size_t i = 0; i < tx->n_in && ok; i++) {
            utxo *u = find(tx->in[i].prev_txid, tx->in[i].prev_vout);
            ok = u && !u->spent && btc_verify_p2pkh(tx, i, u->spk, u->spk_len);
            if (ok) in += u->value;
        }
        for (size_t i = 0; i < tx->n_out; i++) out += tx->out[i].value;
        ok = ok && out <= in;
        for (size_t i = 0; i < tx->n_in && ok; i++) find(tx->in[i].prev_txid, tx->in[i].prev_vout)->spent = true;
        for (size_t i = 0; i < tx->n_out && ok; i++) add_utxo(txid, (uint32_t)i, tx->out[i].value, tx->out[i].script, tx->out[i].script_len, 0);
    }
    pthread_mutex_unlock(&chain.mu);
    return ok;
}

static int ch_coins(void *c, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    (void)c;
    int n = 0;
    pthread_mutex_lock(&chain.mu);
    for (int i = 0; i < chain.n && n < max; i++) {
        const utxo *u = &chain.u[i];
        if (u->spent || !u->height || u->spk_len != BTC_P2PKH_LEN || memcmp(u->spk, spk, BTC_P2PKH_LEN)) continue;
        memcpy(out[n].txid, u->txid, 32), out[n].vout = u->vout, out[n].value = u->value, n++;
    }
    pthread_mutex_unlock(&chain.mu);
    return n;
}

static int ch_conf(void *c, const uint8_t txid[32], uint32_t vout)
{
    (void)c;
    pthread_mutex_lock(&chain.mu);
    const utxo *u = find(txid, vout);
    const int k = !u || u->spent ? -1 : u->height ? (int)(chain.tip - u->height + 1) : 0;
    pthread_mutex_unlock(&chain.mu);
    return k;
}

static uint64_t ch_height(void *c)
{
    (void)c;
    pthread_mutex_lock(&chain.mu);
    const uint64_t h = chain.tip;
    pthread_mutex_unlock(&chain.mu);
    return h;
}

static uint64_t ch_feerate(void *c) { return c ? 3000 : 2000; }   /* the two homes propose different rates: the higher is taken */

static void *miner(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(200000);
        pthread_mutex_lock(&chain.mu);
        const bool stop = chain.stop;
        chain.tip++;
        for (int i = 0; i < chain.n; i++)
            if (!chain.u[i].height) chain.u[i].height = chain.tip;
        pthread_mutex_unlock(&chain.mu);
        if (stop) return NULL;
    }
}

/* ---- the channel: two queues ---- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint8_t *msg[4096];
    size_t len[4096];
    int head, tail;
} queue;

typedef struct {
    queue *in, *out;
    bool drop_last_b;                  /* the side S of the second case: its last signing message of the second transaction is lost */
    int plans, msgs_a, msgs;
} end;

static bool q_send(void *c, const uint8_t *m, size_t len)
{
    end *e = c;
    if (e->drop_last_b && len >= 1) {
        if (m[0] == S_PLAN) {
            if (++e->plans == 2) e->msgs_a = e->msgs;          /* the second plan: the first signing is over, its messages counted */
            e->msgs = 0;
        } else if (m[0] == S_SIGNER && len >= 2 && m[1] == 0) {
            if (++e->msgs == e->msgs_a && e->plans == 2) return true;   /* the same count as the first signing: the last one */
        } else if (m[0] == S_SIGNED && e->plans == 2) return true;
    }
    queue *q = e->out;
    pthread_mutex_lock(&q->mu);
    q->msg[q->tail % 4096] = malloc(len), memcpy(q->msg[q->tail % 4096], m, len), q->len[q->tail % 4096] = len, q->tail++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return true;
}

static bool q_recv(void *c, uint8_t *m, size_t cap, size_t *len, int timeout_ms)
{
    queue *q = ((end *)c)->in;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000, until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    pthread_mutex_lock(&q->mu);
    while (q->head == q->tail)
        if (pthread_cond_timedwait(&q->cv, &q->mu, &until) != 0) {
            pthread_mutex_unlock(&q->mu);
            return false;
        }
    uint8_t *p = q->msg[q->head % 4096];
    const size_t n = q->len[q->head % 4096];
    q->head++;
    pthread_mutex_unlock(&q->mu);
    const bool ok = n <= cap;
    if (ok) memcpy(m, p, n), *len = n;
    free(p);
    return ok;
}

/* ---- the two homes ---- */

static void hashed(const char *s, uint8_t out[32]) { pz_sha256((const uint8_t *)s, strlen(s), out); }

typedef struct {
    session_cfg cfg;
    secp256k1_keypair id;
    session_result res;
    char why[300];
} home;

static void *run_home(void *arg)
{
    home *h = arg;
    h->res = session_run(&h->cfg, h->why, sizeof h->why);
    return NULL;
}

static void p2pkh_of(const uint8_t sec[32], uint8_t spk[BTC_P2PKH_LEN])
{
    secp256k1_pubkey p;
    uint8_t pub[33], h[20];
    size_t n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, sec) && secp256k1_ec_pubkey_serialize(ctx, pub, &n, &p, SECP256K1_EC_COMPRESSED));
    pz_hash160(pub, 33, h);
    btc_script_p2pkh(h, spk);
}

/* a puzzle, its coin on the chain, two homes whose records meet on the same point; then the session of both */
static void run_case(const char *label, bool withhold, const char *bin, const char *const primes[2])
{
    static record_net net;
    uint8_t base[32] = {0}, k[32], x[32] = {0}, hb[32], p33[33];
    base[23] = 0x80;
    hashed(label, hb);
    memcpy(x + 16, hb, 16);
    memcpy(k, x, 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, k, base));
    secp256k1_pubkey P;
    size_t n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &P, k) && secp256k1_ec_pubkey_serialize(ctx, p33, &n, &P, SECP256K1_EC_COMPRESSED));
    const uint8_t fp[32] = {0x5e};
    CHECK(record_net_init(ctx, &net, fp, 0, p33, base));
    uint8_t puzzle_spk[BTC_P2PKH_LEN], coin_txid[32];
    p2pkh_of(k, puzzle_spk);
    hashed("funding", coin_txid), coin_txid[0] ^= (uint8_t)withhold;
    pthread_mutex_lock(&chain.mu);
    add_utxo(coin_txid, 0, 100000000, puzzle_spk, BTC_P2PKH_LEN, chain.tip);   /* 1 BTC on the puzzle address */
    pthread_mutex_unlock(&chain.mu);
    /* the pair: a tame point d_a*G and a wild one Q' + d_b*G, with d_a = x + d_b */
    uint8_t d[2][32], c[33];
    hashed("wild distance", d[1]), d[1][0] = 0;
    memcpy(d[0], d[1], 32);
    CHECK(secp256k1_ec_seckey_tweak_add(ctx, d[0], x));
    record r[2] = {{.t = 0}, {.t = 1}};
    secp256k1_pubkey R;
    n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &R, d[0]) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &R, SECP256K1_EC_COMPRESSED));
    if (c[0] == 3) {
        r[0].t = 0, r[1].t = -1;
        CHECK(secp256k1_ec_seckey_negate(ctx, d[0]) && secp256k1_ec_seckey_negate(ctx, d[1]));
    }
    home h[2];
    queue q[2] = {{.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}, {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}};
    end e[2] = {{.in = &q[0], .out = &q[1]}, {.in = &q[1], .out = &q[0]}};
    session_chan chan[2];
    session_chain chn[2];
    uint8_t xo[2][32];
    for (int i = 0; i < 2; i++) {
        uint8_t s[32];
        char l[64];
        secp256k1_xonly_pubkey xpk;
        snprintf(l, sizeof l, "%s home %d", label, i), hashed(l, s);
        CHECK(secp256k1_keypair_create(ctx, &h[i].id, s) && secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &h[i].id));
        CHECK(secp256k1_xonly_pubkey_serialize(ctx, xo[i], &xpk));
        memcpy(r[i].x, c + 1, 32), memcpy(r[i].owner, xo[i], 32), r[i].seq = 7;
        CHECK(record_sign(ctx, &net, &h[i].id, &r[i]));
    }
    for (int i = 0; i < 2; i++) {
        session_cfg *g = &h[i].cfg;
        memset(g, 0, sizeof *g);
        g->ctx = ctx, g->net = &net, g->identity = &h[i].id;
        memcpy(g->base, base, 32), memcpy(g->pubkey33, p33, 33), memcpy(g->partner, xo[1 - i], 32);
        g->mine = r[i], g->theirs = r[1 - i];
        memcpy(g->d, d[i], 32);
        char l[64];
        snprintf(l, sizeof l, "%s coin %d", label, i), hashed(l, g->coin_sec);
        snprintf(l, sizeof l, "%s pay %d", label, i);
        uint8_t ps[32];
        hashed(l, ps);
        p2pkh_of(ps, g->pay_spk), g->pay_len = BTC_P2PKH_LEN;
        g->max_feerate = 10000, g->pzfirma = bin, g->primes = primes[i];
        chan[i] = (session_chan){&e[i], q_send, q_recv};
        chn[i] = (session_chain){i ? (void *)1 : NULL, ch_coins, ch_broadcast, ch_conf, ch_height, ch_feerate};
        g->chan = &chan[i], g->chain = &chn[i];
        g->timeout_ms = 20000, g->poll_ms = 50, g->cancel_blocks = 3;
    }
    /* who is F: the last bit of the session id; in the second case the other one, S, keeps the second transaction */
    record pair[2];
    uint8_t eid[32];
    CHECK(pieces_order(&r[0], &r[1], pair));
    pieces_eid(&net, pair, eid);
    const int F_home = !memcmp(pair[eid[31] & 1].owner, xo[0], 32) ? 0 : 1, S_home = 1 - F_home;
    if (withhold) h[S_home].cfg.withhold = true, e[S_home].drop_last_b = true;
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, run_home, &h[i]);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    printf("  %s: F %s (%s), S %s (%s)\n", label, h[F_home].res == SESSION_PAID ? "paid" : h[F_home].res == SESSION_CANCELLED ? "cancelled" : "failed",
           h[F_home].why, h[S_home].res == SESSION_PAID ? "paid" : "failed", h[S_home].why);
    /* what the chain holds now */
    uint64_t pay[2] = {0, 0}, at_puzzle = 0;
    uint8_t coin_spk[2][BTC_P2PKH_LEN];
    bool coin_left[2] = {false, false};
    for (int i = 0; i < 2; i++) p2pkh_of(h[i].cfg.coin_sec, coin_spk[i]);
    pthread_mutex_lock(&chain.mu);
    for (int j = 0; j < chain.n; j++) {
        const utxo *u = &chain.u[j];
        if (u->spent) continue;
        for (int i = 0; i < 2; i++) {
            if (u->spk_len == h[i].cfg.pay_len && !memcmp(u->spk, h[i].cfg.pay_spk, u->spk_len)) pay[i] += u->value;
            if (!memcmp(u->spk, coin_spk[i], BTC_P2PKH_LEN)) coin_left[i] = true;
        }
        if (!memcmp(u->spk, puzzle_spk, BTC_P2PKH_LEN)) at_puzzle += u->value;
    }
    pthread_mutex_unlock(&chain.mu);
    printf("    paid: F %llu, S %llu sat; left on the puzzle address %llu sat\n", (unsigned long long)pay[F_home], (unsigned long long)pay[S_home],
           (unsigned long long)at_puzzle);
    if (!withhold) {
        CHECK(h[0].res == SESSION_PAID && h[1].res == SESSION_PAID);
        CHECK(at_puzzle == 0 && pay[S_home] > 49985000);            /* half of 1 BTC less the two tiny coins and two small fees */
        CHECK(pay[F_home] - PAYOUT_TINY - pay[S_home] <= 1);                       /* F also has its tiny coin back */
        CHECK(coin_left[S_home] && !coin_left[F_home]);                            /* S keeps its tiny coin for the next time */
    } else {
        CHECK(h[F_home].res == SESSION_CANCELLED && h[S_home].res == SESSION_FAILED);
        CHECK(at_puzzle > 99900000 && pay[S_home] == 0);                           /* the prize is still under the joint key */
        CHECK(pay[F_home] > 0 && pay[F_home] < PAYOUT_TINY && !coin_left[F_home]);  /* F spent its tiny coin: the held transaction is void */
    }
}

int main(void)
{
    const char *bin = getenv("PZFIRMA"), *primes[2] = {getenv("PZFIRMA_PRIMES0"), getenv("PZFIRMA_PRIMES1")};
    if (!bin || !primes[0] || !primes[1]) {
        printf("t_session: skipped (set PZFIRMA, PZFIRMA_PRIMES0 and PZFIRMA_PRIMES1)\n");
        return 0;
    }
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    pthread_t m;
    pthread_create(&m, NULL, miner, NULL);
    run_case("payout", false, bin, primes);
    run_case("held back", true, bin, primes);
    pthread_mutex_lock(&chain.mu);
    chain.stop = true;
    pthread_mutex_unlock(&chain.mu);
    pthread_join(m, NULL);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_session");
}
