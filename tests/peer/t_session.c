/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_session.c - two homes run the session of a coincidence against each other, in two threads, with the real pzsign, over a channel in
 * memory and a chain in memory that checks every input of every transaction it is given (a standard P2PKH spend of an unspent output) and
 * mines a block every 200 ms. The cases: the payout, half each (with a chain that does not answer at first, and the primes made by the
 * session); a partner S that keeps the second transaction (and its last signing message) to itself, which F answers by spending its tiny
 * coin after 3 blocks; a partner F that stops, or sends garbage, instead of its last signing message, and gets nothing for it; a fee rate
 * at which F could not cancel; the limit of failed signings; a partner that copied the x of a record and cannot prove it; a pzsign that
 * hangs while the other floods it; a scan of the puzzle address that fails (the prize of the network file is paid); a chain that stops.
 * Needs PZSIGN, PZSIGN_PRIMES0 and PZSIGN_PRIMES1; without them it says it was skipped. */
#include "peertest.h"
#include "chain_rpc.h"
#include "session.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <pthread.h>
#include <secp256k1_extrakeys.h>
#include <sys/stat.h>
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
} chain = {.mu = PTHREAD_MUTEX_INITIALIZER, .tip = 1000};   /* as high as a real chain: above the waits of a session, in blocks */

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

/* a prize of the network file, looked up alone */
static int ch_coin(void *c, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    (void)c;
    pthread_mutex_lock(&chain.mu);
    const utxo *u = find(txid, vout);
    const bool ok = u && !u->spent && u->height && u->spk_len == BTC_P2PKH_LEN && !memcmp(u->spk, spk, BTC_P2PKH_LEN);
    if (ok) memcpy(out->txid, txid, 32), out->vout = vout, out->value = u->value;
    pthread_mutex_unlock(&chain.mu);
    return ok;
}

static uint64_t g_fee[2] = {2000, 3000};                     /* the two homes propose different rates: the higher is taken */
static uint64_t ch_feerate(void *c) { return g_fee[c != NULL]; }

/* a chain whose first answers about the height are lost (0): a wait must not count its blocks from there; whose scan of an address
 * fails (the answer of a node to an address with thousands of coins of dust is too long); or that stops (the height stays, and nothing
 * gets confirmed) */
typedef struct {
    session_chain in;
    int lost;
    bool scan_fails, frozen;
    uint64_t at;
} flaky;

static int fl_coins(void *c, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    flaky *f = c;
    return f->scan_fails ? -1 : f->in.coins(f->in.ctx, spk, out, max);
}
static bool fl_broadcast(void *c, const btc_tx *tx) { flaky *f = c; return f->in.broadcast(f->in.ctx, tx); }
static int fl_conf(void *c, const uint8_t txid[32], uint32_t vout)
{
    flaky *f = c;
    const int k = f->in.confirmations(f->in.ctx, txid, vout);
    return f->frozen && k > 0 ? 0 : k;
}
static uint64_t fl_feerate(void *c) { flaky *f = c; return f->in.feerate(f->in.ctx); }
static uint64_t fl_height(void *c)
{
    flaky *f = c;
    if (f->lost > 0) return f->lost--, 0;
    const uint64_t h = f->in.height(f->in.ctx);
    if (f->frozen && !f->at) f->at = h;
    return f->frozen ? f->at : h;
}
static int fl_coin(void *c, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    flaky *f = c;
    return f->in.coin ? f->in.coin(f->in.ctx, txid, vout, spk, out) : -1;
}

/* ---- or a bitcoind on regtest (PZ_REGTEST=host:port:user:password): the same cases on a real node, through chain_rpc ---- */

static bool regtest;
static struct {
    char host[64], user[64], pass[64];
    unsigned port;
} node;
static char mine_addr[100];

static bool rpc_client(btc_rpc *c)
{
    return btc_rpc_init(c, node.host, node.port, node.user, node.pass) && (c->timeout_ms = 60000, true);
}

static void addr_of(const uint8_t spk[BTC_P2PKH_LEN], char *out, size_t cap)
{
    uint8_t payload[21] = {0x6f};
    CHECK(btc_script_is_p2pkh(spk, BTC_P2PKH_LEN, payload + 1) && pz_base58check_encode(payload, 21, out, cap) > 0);
}

static void mine(btc_rpc *c, unsigned blocks) { CHECK(btc_rpc_generatetoaddress(c, blocks, mine_addr, NULL)); }

/* the confirmed value at a P2PKH script */
static uint64_t balance(const uint8_t spk[BTC_P2PKH_LEN])
{
    uint64_t v = 0;
    if (regtest) {
        btc_rpc c;
        chain_rpc cr = {.rpc = &c, .chain = BTC_REGTEST, .fallback_feerate = 2000};
        payout_coin coins[64];
        CHECK(rpc_client(&c));
        mine(&c, 1);                                            /* what is in the mempool, confirmed first */
        const session_chain ch = chain_rpc_iface(&cr);
        const int n = ch.coins(ch.ctx, spk, coins, 64);
        for (int i = 0; i < n; i++) v += coins[i].value;
        btc_rpc_done(&c);
        return v;
    }
    pthread_mutex_lock(&chain.mu);
    for (int j = 0; j < chain.n; j++)
        if (!chain.u[j].spent && chain.u[j].spk_len == BTC_P2PKH_LEN && !memcmp(chain.u[j].spk, spk, BTC_P2PKH_LEN)) v += chain.u[j].value;
    pthread_mutex_unlock(&chain.mu);
    return v;
}

static void fund(const uint8_t spk[BTC_P2PKH_LEN], const uint8_t txid_seed[32])
{
    if (regtest) {
        btc_rpc c;
        char addr[64];
        uint8_t txid[32];
        CHECK(rpc_client(&c) && btc_rpc_set_wallet(&c, "pz"));
        addr_of(spk, addr, sizeof addr);
        CHECK(btc_rpc_sendtoaddress(&c, addr, 100000000, txid));
        mine(&c, 1);
        btc_rpc_done(&c);
        return;
    }
    pthread_mutex_lock(&chain.mu);
    add_utxo(txid_seed, 0, 100000000, spk, BTC_P2PKH_LEN, chain.tip);
    pthread_mutex_unlock(&chain.mu);
}

static void *miner(void *arg)
{
    (void)arg;
    btc_rpc c;
    if (regtest) CHECK(rpc_client(&c));
    for (;;) {
        usleep(regtest ? 1000000 : 200000);
        if (regtest) {
            pthread_mutex_lock(&chain.mu);
            const bool stop = chain.stop;
            pthread_mutex_unlock(&chain.mu);
            if (stop) return NULL;
            mine(&c, 1);
            continue;
        }
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

/* what an end does to its own messages of the second signing (the counts of the first tell which one is the last) */
enum { LAST_SENT, LAST_DROPPED, LAST_ABORT, LAST_DAMAGED };

typedef struct {
    queue *in, *out;
    int last_b;                        /* LAST_*: the last signing message of the second transaction */
    int plans, msgs_a, msgs, alives;
} end;

static void q_put(queue *q, const uint8_t *m, size_t len)
{
    pthread_mutex_lock(&q->mu);
    q->msg[q->tail % 4096] = malloc(len), memcpy(q->msg[q->tail % 4096], m, len), q->len[q->tail % 4096] = len, q->tail++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static bool q_send(void *c, const uint8_t *m, size_t len)
{
    end *e = c;
    if (len >= 1 && m[0] == S_ALIVE) e->alives++;
    if (len >= 1 && (m[0] == S_PLAN || m[0] == S_SIGNER)) q_put(e->out, (const uint8_t[]){S_ALIVE}, 1);   /* the reader must skip it */
    if (e->last_b != LAST_SENT && len >= 1) {
        if (m[0] == S_PLAN) {
            if (++e->plans == 2) e->msgs_a = e->msgs;          /* the second plan: the first signing is over, its messages counted */
            e->msgs = 0;
        } else if (m[0] == S_SIGNER && len >= 2 && m[1] == 0) {
            if (++e->msgs == e->msgs_a && e->plans == 2) {     /* the same count as the first signing: the last one */
                if (e->last_b == LAST_DROPPED) return true;
                if (e->last_b == LAST_ABORT) return q_put(e->out, (const uint8_t[]){S_ABORT, 'n', 'o'}, 3), true;
                uint8_t *bad = malloc(len);
                memcpy(bad, m, len), bad[len - 1] ^= 1;
                q_put(e->out, bad, len);
                return free(bad), true;
            }
        } else if (m[0] == S_SIGNED && e->plans == 2 && e->last_b == LAST_DROPPED) return true;
    }
    q_put(e->out, m, len);
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

/* the messages nobody read (the end of a session that stopped early) */
static void q_drain(queue *q)
{
    for (; q->head != q->tail; q->head++) free(q->msg[q->head % 4096]);
}

/* ---- the files of a case: a folder of its own ---- */

static char g_dir[64];

static const char *in_dir(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
    return out;
}

static void write_text(const char *path, const char *text, bool exec)
{
    FILE *f = fopen(path, "w");
    CHECK(f && fputs(text, f) >= 0);
    if (f) fclose(f);
    if (exec) CHECK(chmod(path, 0700) == 0);
}

/* the failures that a file of the limit counts for key (0 when it has no line for it) */
static unsigned fails_of(const char *path, const uint8_t key[32])
{
    char want[65], hex[80];
    unsigned v, n = 0;
    pt_hex(key, 32, want);
    FILE *f = fopen(path, "r");
    while (f && fscanf(f, "%79s %u", hex, &v) == 2)
        if (!strcmp(hex, want)) n = v;
    if (f) fclose(f);
    return n;
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

enum { PAYOUT, CANCEL, NOTHING, FIRST };   /* what the chain must show at the end (FIRST: the first transaction alone) */

typedef struct {
    const char *name;
    int expect;
    int s_last, f_last;               /* LAST_*: what S and F do to their last message of the second signing */
    bool s_withholds;                 /* S keeps the second transaction */
    uint64_t fee;                     /* the fee rate home 1 proposes (0: the usual) */
    int lost_heights;                 /* answers about the height each chain loses at first */
    bool make_primes;                 /* home 0 has no primes: its session makes them */
    unsigned limit[2];                /* failures already counted on the network by home 0, and for the pair by home 1 */
    unsigned fails_F, fails_S;        /* the failed signings F and S count in the case */
    const char *bin[2];               /* the pzsign of each home (NULL: the real one) */
    const char *why;                  /* in the reason of both homes, when not NULL */
    bool scan_fails;                  /* the scan of the puzzle address fails: the prize of the network file is given */
    bool frozen;                      /* the chain stops, and a wait gives up after a while of block_ms a block */
    int block_ms;
} tcase;

/* a puzzle, its coin on the chain, two homes whose records meet on the same point; then the session of both */
static void run_case(const tcase *tc, const char *bin, const char *const primes[2])
{
    /* on a node that keeps its chain the keys are new at every run: coins left by an earlier run would change the first transaction (and
     * the count of messages some cases rely on) */
    char label[96];
    snprintf(label, sizeof label, "%s %ld", tc->name, regtest ? (long)time(NULL) : 0L);
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
    hashed(label, coin_txid);
    fund(puzzle_spk, coin_txid);                                /* 1 BTC on the puzzle address */
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
    flaky chn[2];
    btc_rpc rpcs[2];
    chain_rpc crpc[2];
    session_limit lim[2];
    bool proved[2] = {false, false};
    char limpath[2][96], mine_primes[96];
    uint8_t xo[2][32];
    for (int i = 0; i < 2; i++) {
        uint8_t sec[32];
        char l[128];
        secp256k1_xonly_pubkey xpk;
        snprintf(l, sizeof l, "%s home %d", label, i), hashed(l, sec);
        CHECK(secp256k1_keypair_create(ctx, &h[i].id, sec) && secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &h[i].id));
        CHECK(secp256k1_xonly_pubkey_serialize(ctx, xo[i], &xpk));
        memcpy(r[i].x, c + 1, 32), memcpy(r[i].owner, xo[i], 32), r[i].seq = 7;
        CHECK(record_sign(ctx, &net, &h[i].id, &r[i]));
    }
    record pair[2];
    uint8_t eid[32];
    CHECK(pieces_order(&r[0], &r[1], pair));
    pieces_eid(&net, pair, eid);
    const int F_home = !memcmp(pair[eid[31] & 1].owner, xo[0], 32) ? 0 : 1, S_home = 1 - F_home;
    for (int i = 0; i < 2; i++) {
        session_cfg *g = &h[i].cfg;
        memset(g, 0, sizeof *g);
        g->ctx = ctx, g->net = &net, g->identity = &h[i].id;
        memcpy(g->base, base, 32), memcpy(g->pubkey33, p33, 33), memcpy(g->partner, xo[1 - i], 32);
        g->mine = r[i], g->theirs = r[1 - i];
        memcpy(g->d, d[i], 32);
        char l[128];
        snprintf(l, sizeof l, "%s coin %d", label, i), hashed(l, g->coin_sec);
        snprintf(l, sizeof l, "%s pay %d", label, i);
        uint8_t ps[32];
        hashed(l, ps);
        p2pkh_of(ps, g->pay_spk), g->pay_len = BTC_P2PKH_LEN;
        g->max_feerate = 200000, g->pzsign = tc->bin[i] ? tc->bin[i] : bin, g->primes = primes[i];
        chan[i] = (session_chan){&e[i], q_send, q_recv};
        if (regtest) {                                          /* each home its own client of the node */
            CHECK(rpc_client(&rpcs[i]));
            crpc[i] = (chain_rpc){.rpc = &rpcs[i], .chain = BTC_REGTEST, .fallback_feerate = i && tc->fee ? tc->fee : g_fee[i]};
            chn[i].in = chain_rpc_iface(&crpc[i]);
        } else chn[i].in = (session_chain){i ? (void *)1 : NULL, ch_coins, ch_broadcast, ch_conf, ch_height, ch_feerate, ch_coin};
        chn[i].lost = tc->lost_heights, chn[i].frozen = tc->frozen, chn[i].scan_fails = tc->scan_fails, chn[i].at = 0;
        g->block_ms = tc->block_ms;
        if (tc->scan_fails) {                                   /* the prize, as a line of the network file gives it */
            payout_coin pc[64];
            CHECK(chn[i].in.coins(chn[i].in.ctx, puzzle_spk, pc, 64) == 1);
            g->prizes[0] = pc[0], g->nprizes = 1;
        }
        g->chan = &chan[i];
        g->timeout_ms = 20000, g->poll_ms = 50, g->cancel_blocks = 3, g->alive_ms = 1;
        /* each home its limit, in a file of the case */
        snprintf(l, sizeof l, "limit%d", i);
        in_dir(l, limpath[i], sizeof limpath[i]);
        remove(limpath[i]);
        lim[i] = (session_limit){.path = limpath[i], .max = 10, .pair_max = 3};
        pthread_mutex_init(&lim[i].mu, NULL);
        pthread_cond_init(&lim[i].cv, NULL);
        g->limit = &lim[i], g->proved = &proved[i];
        if (tc->limit[i]) {
            char hex[65], text[200];
            pt_hex(i ? eid : net.fp, 32, hex);
            snprintf(text, sizeof text, "%s %u\n", hex, tc->limit[i]);
            write_text(limpath[i], text, false);
        }
    }
    session_chain ifc[2];
    for (int i = 0; i < 2; i++) {
        ifc[i] = (session_chain){&chn[i], fl_coins, fl_broadcast, fl_conf, fl_height, fl_feerate, fl_coin};
        h[i].cfg.chain = &ifc[i];
    }
    if (tc->fee) g_fee[1] = tc->fee;
    if (tc->make_primes) remove(in_dir("primes-made", mine_primes, sizeof mine_primes)), h[0].cfg.primes = mine_primes;
    h[S_home].cfg.withhold = tc->s_withholds;
    e[S_home].last_b = tc->s_last, e[F_home].last_b = tc->f_last;
    pthread_t th[2];
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, run_home, &h[i]);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    g_fee[1] = 3000;
    q_drain(&q[0]), q_drain(&q[1]);
    static const char *const RES[] = {"paid", "cancelled", "failed"};
    printf("  %s: F %s (%s), S %s (%s)\n", tc->name, RES[h[F_home].res], h[F_home].why, RES[h[S_home].res], h[S_home].why);
    /* what the chain holds now */
    uint64_t pay[2], at_puzzle = balance(puzzle_spk);
    bool coin_left[2];
    for (int i = 0; i < 2; i++) {
        uint8_t coin_spk[BTC_P2PKH_LEN];
        p2pkh_of(h[i].cfg.coin_sec, coin_spk);
        pay[i] = balance(h[i].cfg.pay_spk);
        coin_left[i] = balance(coin_spk) > 0;
        if (regtest) btc_rpc_done(&rpcs[i]);
    }
    printf("    paid: F %llu, S %llu sat; left on the puzzle address %llu sat\n", (unsigned long long)pay[F_home], (unsigned long long)pay[S_home],
           (unsigned long long)at_puzzle);
    if (tc->expect == PAYOUT) {
        CHECK(h[0].res == SESSION_PAID && h[1].res == SESSION_PAID);
        CHECK(at_puzzle == 0 && pay[S_home] > 49985000);            /* half of 1 BTC less the two tiny coins and two small fees */
        CHECK(pay[F_home] - PAYOUT_TINY_MIN - pay[S_home] <= 1);                       /* F also has its tiny coin back */
        CHECK(coin_left[S_home] && !coin_left[F_home]);                            /* S keeps its tiny coin for the next time */
    } else if (tc->expect == CANCEL) {
        CHECK(h[F_home].res == SESSION_CANCELLED && h[S_home].res == SESSION_FAILED);
        CHECK(at_puzzle > 99800000 && pay[S_home] == 0);                           /* the prize is still under the joint key */
        CHECK(pay[F_home] > 0 && pay[F_home] < 100000 && !coin_left[F_home]);      /* F spent its tiny coin: the held transaction is void */
    } else if (tc->expect == FIRST) {
        CHECK(h[0].res == SESSION_FAILED && h[1].res == SESSION_FAILED);
        CHECK(at_puzzle > 99900000 && at_puzzle < 100000000 && pay[0] == 0 && pay[1] == 0 && coin_left[0] && coin_left[1]);
    } else {
        CHECK(h[0].res == SESSION_FAILED && h[1].res == SESSION_FAILED);
        CHECK(at_puzzle == 100000000 && pay[0] == 0 && pay[1] == 0 && !coin_left[0] && !coin_left[1]);   /* nothing was signed */
    }
    if (tc->why) CHECK(strstr(h[0].why, tc->why) || strstr(h[1].why, tc->why));
    if (tc->expect == CANCEL) CHECK(e[F_home].alives > 0);      /* sent while F waited its blocks */
    CHECK(proved[0] && proved[1]);
    /* the limits: a failed signing counts for the network and the pair, a good one nothing, a refused one nothing */
    for (int i = 0; i < 2; i++) {
        const unsigned failed = i == F_home ? tc->fails_F : tc->fails_S;
        CHECK(fails_of(limpath[i], net.fp) == (i ? 0 : tc->limit[0]) + failed);
        CHECK(fails_of(limpath[i], eid) == (i ? tc->limit[1] : 0) + failed);
        pthread_mutex_destroy(&lim[i].mu);
        pthread_cond_destroy(&lim[i].cv);
        remove(limpath[i]);
    }
    if (tc->make_primes) {
        struct stat st;
        CHECK(stat(mine_primes, &st) == 0 && (st.st_mode & 077) == 0);   /* made by the session, for its owner only */
        remove(mine_primes);
    }
}

/* A partner that copied the x of home 0's record into a record of its own type, under its own identity, and cannot prove it: home 0 makes
 * no primes and runs no pzsign for it (the primes file it would make stays missing). */
static void run_copied(const char *bin)
{
    static record_net net;
    uint8_t base[32] = {0}, k[32] = {0}, p33[33], d0[32], c[33];
    char prim[96];
    base[23] = 0x80, k[20] = 1, k[31] = 7;
    secp256k1_pubkey P, R;
    size_t n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &P, k) && secp256k1_ec_pubkey_serialize(ctx, p33, &n, &P, SECP256K1_EC_COMPRESSED));
    const uint8_t fp[32] = {0x5f};
    CHECK(record_net_init(ctx, &net, fp, 0, p33, base));
    hashed("copied distance", d0);
    n = 33;
    CHECK(secp256k1_ec_pubkey_create(ctx, &R, d0) && secp256k1_ec_pubkey_serialize(ctx, c, &n, &R, SECP256K1_EC_COMPRESSED));
    if (c[0] == 3) CHECK(secp256k1_ec_seckey_negate(ctx, d0));
    home h;
    secp256k1_keypair thief;
    uint8_t sec[32], xo[2][32];
    secp256k1_xonly_pubkey xpk;
    hashed("copied home", sec);
    CHECK(secp256k1_keypair_create(ctx, &h.id, sec) && secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &h.id) && secp256k1_xonly_pubkey_serialize(ctx, xo[0], &xpk));
    hashed("copied thief", sec);
    CHECK(secp256k1_keypair_create(ctx, &thief, sec) && secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &thief) && secp256k1_xonly_pubkey_serialize(ctx, xo[1], &xpk));
    record mine = {.t = 0, .seq = 1}, theirs = {.t = 1, .seq = 1};
    memcpy(mine.x, c + 1, 32), memcpy(theirs.x, c + 1, 32), memcpy(mine.owner, xo[0], 32), memcpy(theirs.owner, xo[1], 32);
    CHECK(record_sign(ctx, &net, &h.id, &mine) && record_sign(ctx, &net, &thief, &theirs) && record_check(ctx, &net, &theirs));
    queue q[2] = {{.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}, {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}};
    end e[2] = {{.in = &q[0], .out = &q[1]}, {.in = &q[1], .out = &q[0]}};
    session_chan chan = {&e[0], q_send, q_recv};
    session_chain chn = {NULL, ch_coins, ch_broadcast, ch_conf, ch_height, ch_feerate, ch_coin};
    session_cfg *g = &h.cfg;
    memset(g, 0, sizeof *g);
    g->ctx = ctx, g->net = &net, g->identity = &h.id, g->mine = mine, g->theirs = theirs;
    memcpy(g->base, base, 32), memcpy(g->pubkey33, p33, 33), memcpy(g->partner, xo[1], 32), memcpy(g->d, d0, 32);
    hashed("copied coin", g->coin_sec);
    p2pkh_of(g->coin_sec, g->pay_spk), g->pay_len = BTC_P2PKH_LEN;
    g->max_feerate = 200000, g->pzsign = bin, g->primes = in_dir("primes-copied", prim, sizeof prim);
    remove(prim);
    g->chan = &chan, g->chain = &chn, g->timeout_ms = 20000, g->poll_ms = 50, g->cancel_blocks = 3;
    bool proved = false;
    g->proved = &proved;
    pthread_t th;
    pthread_create(&th, NULL, run_home, &h);
    /* the thief: its hello, then a proof it could not make */
    uint8_t m[1 + 2 * RECORD_LEN + 32 + 8 + BTC_P2PKH_LEN + 1 + BTC_P2PKH_LEN] = {S_HELLO}, proof[1 + 64] = {S_PROOF};
    record_encode(&theirs, m + 1), record_encode(&mine, m + 1 + RECORD_LEN);
    m[1 + 2 * RECORD_LEN + 32 + 6] = 0x0b, m[1 + 2 * RECORD_LEN + 32 + 7] = 0xb8;   /* 3000 */
    p2pkh_of(sec, m + 1 + 2 * RECORD_LEN + 40);
    m[1 + 2 * RECORD_LEN + 40 + BTC_P2PKH_LEN] = BTC_P2PKH_LEN;
    p2pkh_of(sec, m + 1 + 2 * RECORD_LEN + 41 + BTC_P2PKH_LEN);
    hashed("not a proof", proof + 1), hashed("really not", proof + 33);
    CHECK(q_send(&e[1], m, sizeof m) && q_send(&e[1], proof, sizeof proof));
    pthread_join(th, NULL);
    q_drain(&q[0]), q_drain(&q[1]);
    printf("  copied x: %s (%s)\n", h.res == SESSION_FAILED ? "failed" : "not failed", h.why);
    CHECK(h.res == SESSION_FAILED && strstr(h.why, "did not prove") && !proved);
    CHECK(access(prim, F_OK) != 0);
}

int main(void)
{
    const char *bin = getenv("PZSIGN"), *primes[2] = {getenv("PZSIGN_PRIMES0"), getenv("PZSIGN_PRIMES1")};
    if (!bin || !primes[0] || !primes[1]) {
        printf("t_session: skipped (set PZSIGN, PZSIGN_PRIMES0 and PZSIGN_PRIMES1)\n");
        return 0;
    }
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    snprintf(g_dir, sizeof g_dir, "/tmp/t_session.XXXXXX");
    CHECK(mkdtemp(g_dir) != NULL);
    const char *rt = getenv("PZ_REGTEST");
    if (rt && *rt) {                                            /* host:port:user:password */
        regtest = sscanf(rt, "%63[^:]:%u:%63[^:]:%63s", node.host, &node.port, node.user, node.pass) == 4;
        btc_rpc c;
        CHECK(regtest && rpc_client(&c));
        if (!btc_rpc_createwallet(&c, "pz")) {                  /* it is there already: loaded, or to load after a restart of the node */
            json_w w;
            json_doc d;
            const json_val *res;
            jw_init(&w), jw_arr(&w), jw_str(&w, "pz"), jw_end(&w);
            if (btc_rpc_call(&c, false, "loadwallet", &w, &d, &res)) json_free(&d);
            jw_free(&w);
        }
        CHECK(btc_rpc_set_wallet(&c, "pz") && btc_rpc_getnewaddress(&c, "legacy", mine_addr, sizeof mine_addr));
        mine(&c, 101);                                          /* coinbase coins mature after 100 blocks */
        btc_rpc_done(&c);
        printf("  on the regtest node %s:%u\n", node.host, node.port);
    }
    /* the programs of some cases: a pzsign whose primes come from a copy, one that hangs and reads nothing, one that floods */
    char maker[96], hang[96], flood[96], text[1200];
    snprintf(text, sizeof text, "#!/bin/sh\nif [ \"$1\" = primes ]; then cp '%s' \"$2\" && chmod 600 \"$2\"; exit $?; fi\nexec '%s' \"$@\"\n", primes[0], bin);
    write_text(in_dir("maker", maker, sizeof maker), text, true);
    write_text(in_dir("hang", hang, sizeof hang), "#!/bin/sh\nexec sleep 600\n", true);
    write_text(in_dir("flood", flood, sizeof flood),
               "#!/bin/sh\ni=0\nwhile [ $i -lt 5 ]; do printf '\\000\\000\\352\\141\\201'; head -c 60000 /dev/zero; i=$((i+1)); done\nexec sleep 600\n", true);
    pthread_t m;
    pthread_create(&m, NULL, miner, NULL);
    const tcase cases[] = {
        {.name = "payout", .expect = PAYOUT, .lost_heights = 1, .make_primes = true, .bin = {maker}},
        {.name = "held back", .expect = CANCEL, .s_last = LAST_DROPPED, .s_withholds = true, .fails_F = 1},
        /* F stops, or sends garbage, instead of its last message: S's last one must not come back (it did before: F had the signature) */
        {.name = "F stops at its last message", .expect = CANCEL, .f_last = LAST_ABORT, .fails_F = 1, .fails_S = 1},
        {.name = "F damages its last message", .expect = CANCEL, .f_last = LAST_DAMAGED, .fails_F = 1, .fails_S = 1},
        /* S proposes a high rate and holds back: the tiny coins are larger, and F can still cancel (with coins of a fixed size it could not) */
        {.name = "held back at a high fee rate", .expect = CANCEL, .s_last = LAST_DROPPED, .s_withholds = true, .fee = 60000, .fails_F = 1},
        /* one home at its limit (on the network, or for the pair): it stops before the plan, and the other counts no failure */
        {.name = "limit of the network reached", .expect = NOTHING, .limit = {10, 0}, .why = "failed signings on this network"},
        {.name = "limit of the pair reached", .expect = NOTHING, .limit = {0, 3}, .why = "failed signings of this pair"},
        {.name = "a pzsign that hangs", .expect = NOTHING, .bin = {hang, flood}, .fails_F = 1, .fails_S = 1},
        /* the scan fails (dust): the prize of the network file is paid all the same (before, no coin of the puzzle to spend) */
        {.name = "the scan fails", .expect = PAYOUT, .scan_fails = true},
        /* the chain stops after the first transaction: the wait for its block ends (before, never) */
        {.name = "the chain stops", .expect = FIRST, .frozen = true, .block_ms = 10, .why = "not confirmed"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) run_case(&cases[i], bin, primes);
    run_copied(maker);
    pthread_mutex_lock(&chain.mu);
    chain.stop = true;
    pthread_mutex_unlock(&chain.mu);
    pthread_join(m, NULL);
    char p[96];
    const char *const files[] = {"maker", "hang", "flood"};
    for (int i = 0; i < 3; i++) remove(in_dir(files[i], p, sizeof p));
    CHECK(rmdir(g_dir) == 0);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_session");
}
