/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_session.c - two homes run the session of a coincidence against each other, in two threads, with the real pzsign, over a channel in
 * memory and a chain in memory that checks every input of every transaction it is given (a standard P2PKH spend of an unspent output) and
 * mines a block every 200 ms. The cases: the payout, half each (with a chain that does not answer at first); a partner S that keeps the second transaction (and its last signing message) to itself, which F answers by spending its tiny
 * coin after 3 blocks, also when S keeps the channel alive; a partner F that stops, or sends garbage, instead of its last signing message,
 * and gets nothing for it, or sends garbage after it, which takes nothing from S; a fee rate
 * at which F could not cancel; the limits of failed signings and of payouts; a partner that copied the x of a record and cannot prove it; a
 * pzsign that hangs while the other floods it; a scan of the puzzle address that fails (the prize of the network file is paid); a chain
 * that stops; a chain a block behind the other's; dust at F's coin key.
 * After a payout the pieces: both get the key; a partner that keeps its piece, or gives a wrong one, which leaves only this side without
 * it, and a later session that gives it; coins left at the puzzle address, which hold the exchange, and a later payout of them whose
 * second S keeps; every output of the second spent before a later session, also by a home that never saw its block; coins no payout of
 * the session takes; a second at a rate the chain no longer takes, bumped by a later session; the known coins of the prize: spent, as a
 * node shows them, not shown spent by a light chain, none known to a light chain (no piece then, and the partner gives none). Needs
 * PZSIGN; without it it says it was skipped. */
#include "peertest.h"
#include "chain_rpc.h"
#include "session.h"
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
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
    uint8_t by[32];                    /* the txid of the transaction that spent it */
    uint64_t rate;                     /* the fee rate of the transaction that made it (sat/kvB; 0: given by the test) */
} utxo;

static struct {
    pthread_mutex_t mu;
    utxo u[1024];
    int n;
    uint64_t tip;
    uint64_t mine_from;                /* the miner leaves the transactions below this rate in the mempool */
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
    if (chain.n == (int)(sizeof chain.u / sizeof *chain.u)) fprintf(stderr, "t_session: FAIL: the chain in memory is full\n"), exit(1);
    utxo *u = &chain.u[chain.n++];
    memcpy(u->txid, txid, 32), memcpy(u->spk, spk, len);
    u->vout = vout, u->value = value, u->spk_len = len, u->height = height, u->spent = false, u->rate = 0;
}

/* the fee of a transaction of the chain, from what it spent and what it made */
static uint64_t fee_of(const uint8_t id[32])
{
    uint64_t in = 0, out = 0;
    for (int i = 0; i < chain.n; i++) {
        if (chain.u[i].spent && !memcmp(chain.u[i].by, id, 32)) in += chain.u[i].value;
        if (!memcmp(chain.u[i].txid, id, 32)) out += chain.u[i].value;
    }
    return in - out;
}

/* a transaction of the mempool replaced: its coins unspent again, its outputs gone */
static void drop(const uint8_t id[32])
{
    uint8_t gone[32];
    memcpy(gone, id, 32);                                       /* id may be in the array, which moves */
    for (int i = 0; i < chain.n; i++) {
        if (chain.u[i].spent && !memcmp(chain.u[i].by, gone, 32)) chain.u[i].spent = false;
        if (!memcmp(chain.u[i].txid, gone, 32)) chain.u[i--] = chain.u[--chain.n];
    }
}

/* the coin of the case "an old copy of the first is mined": the transaction that spends it is renamed as soon as it is taken, as an old
 * copy of it (the same outputs, other signatures, so another txid) would be mined in its place */
static const uint8_t *g_copy;

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
            const utxo *u = find(tx->in[i].prev_txid, tx->in[i].prev_vout);
            ok = u && btc_verify_p2pkh(tx, i, u->spk, u->spk_len);
            if (ok) in += u->value;
        }
        for (size_t i = 0; i < tx->n_out; i++) out += tx->out[i].value;
        ok = ok && out <= in;
        for (size_t i = 0; i < tx->n_in && ok; i++) {          /* a spend in the mempool at a lower fee is replaced, as Bitcoin Core 28 does */
            const utxo *u = find(tx->in[i].prev_txid, tx->in[i].prev_vout), *by = u && u->spent ? find(u->by, 0) : NULL;
            if (by && !by->height && fee_of(u->by) < in - out) drop(u->by);
        }
        for (size_t i = 0; i < tx->n_in && ok; i++) {
            const utxo *u = find(tx->in[i].prev_txid, tx->in[i].prev_vout);
            ok = u && !u->spent;
        }
        for (size_t i = 0; i < tx->n_in && ok; i++) {
            utxo *u = find(tx->in[i].prev_txid, tx->in[i].prev_vout);
            u->spent = true, memcpy(u->by, txid, 32);
        }
        for (size_t i = 0; i < tx->n_out && ok; i++) {
            uint8_t raw[BTC_TX_MAX_SIZE];
            add_utxo(txid, (uint32_t)i, tx->out[i].value, tx->out[i].script, tx->out[i].script_len, 0);
            chain.u[chain.n - 1].rate = (in - out) * 1000 / btc_tx_serialize(tx, raw, sizeof raw);
        }
        if (ok && g_copy && !memcmp(tx->in[0].prev_txid, g_copy, 32)) {
            static const uint8_t copy[32] = {0xc0, 0x9e};
            for (size_t i = 0; i < tx->n_out; i++) memcpy(find(txid, (uint32_t)i)->txid, copy, 32);
            memcpy(find(tx->in[0].prev_txid, tx->in[0].prev_vout)->by, copy, 32), g_copy = NULL;
        }
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
    const utxo *u = find(txid, vout), *by = u && u->spent ? find(u->by, 0) : NULL;
    const int k = !u || (by && by->height) ? -1 : u->height ? (int)(chain.tip - u->height + 1) : 0;   /* spent once the spend is mined */
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

/* the spends of a script in the mempool: its outputs spent by a transaction not mined (ch_coins leaves such an output out, as an Electrum
 * server does) */
static int ch_pending(void *c, const uint8_t spk[BTC_P2PKH_LEN])
{
    (void)c;
    int n = 0;
    pthread_mutex_lock(&chain.mu);
    for (int i = 0; i < chain.n; i++) {
        const utxo *u = &chain.u[i], *by = u->spent ? find(u->by, 0) : NULL;
        if (u->spk_len == BTC_P2PKH_LEN && !memcmp(u->spk, spk, BTC_P2PKH_LEN) && by && !by->height) n++;
    }
    pthread_mutex_unlock(&chain.mu);
    return n;
}

/* an output looked up alone (a prize of the network file): a spend in the mempool is none, as for a node and a light chain */
static int ch_coin(void *c, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    (void)c;
    pthread_mutex_lock(&chain.mu);
    const utxo *u = find(txid, vout), *by = u && u->spent ? find(u->by, 0) : NULL;
    const bool ok = u && (!u->spent || (by && !by->height)) && u->height && u->spk_len == BTC_P2PKH_LEN && !memcmp(u->spk, spk, BTC_P2PKH_LEN);
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
    int lost, slow_ms;                 /* slow_ms: the scan of the coins takes this long */
    bool scan_fails, frozen;
    bool ghost;                        /* once the puzzle address is empty, it shows a coin of 1 BTC (sent there after the payout) */
    int refuse;                        /* the next sends of a second transaction it refuses (a node that is down for a moment) */
    bool stop_at_second;               /* it stops once a second transaction is sent: the home is stopped before its block */
    uint64_t fee;                      /* the rate it estimates, whatever the chain says (0: the chain's) */
    uint64_t at;
    const payout_coin *old;            /* a named coin of the address, spent long ago and older than the checkpoint of a light chain (no
                                        * scan lists it): once the address is empty its servers hide the spend, and looked up alone it
                                        * is there */
    const uint8_t *behind;             /* the txid of a coin its scan does not list yet (a chain a block behind) */
} flaky;

static int fl_coins(void *c, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    flaky *f = c;
    if (f->slow_ms) usleep((useconds_t)f->slow_ms * 1000);
    int n = f->scan_fails ? -1 : f->in.coins(f->in.ctx, spk, out, max);
    for (int i = 0; i < n; i++)
        if (f->behind && !memcmp(out[i].txid, f->behind, 32)) out[i--] = out[--n];
    if (n == 0 && f->ghost && max > 0) return memset(&out[0], 0x77, sizeof out[0]), out[0].value = 100000000, 1;
    return n;
}
static bool fl_broadcast(void *c, const btc_tx *tx)
{
    flaky *f = c;
    const bool second = tx->n_in == 2 && tx->n_out == 3;
    if (second && f->refuse > 0) return f->refuse--, false;
    if (second && f->stop_at_second) f->frozen = true;
    return f->in.broadcast(f->in.ctx, tx);
}
static int fl_conf(void *c, const uint8_t txid[32], uint32_t vout)
{
    flaky *f = c;
    const int k = f->in.confirmations(f->in.ctx, txid, vout);
    return f->frozen && k > 0 ? 0 : k;
}
static uint64_t fl_feerate(void *c) { flaky *f = c; return f->fee ? f->fee : f->in.feerate(f->in.ctx); }
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
    if (f->old && !memcmp(f->old->txid, txid, 32) && f->old->vout == vout) {
        payout_coin there;
        return f->in.coins(f->in.ctx, spk, &there, 1) ? 0 : (*out = *f->old, 1);
    }
    return f->in.coin ? f->in.coin(f->in.ctx, txid, vout, spk, out) : -1;
}
static int fl_pending(void *c, const uint8_t spk[BTC_P2PKH_LEN]) { flaky *f = c; return f->in.pending ? f->in.pending(f->in.ctx, spk) : 0; }

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

/* a call of the test to the node, its parameters as the JSON text of an array; a result that is a string goes to out (cap bytes) */
static bool node_call(btc_rpc *c, const char *method, const char *params, char *out, size_t cap)
{
    json_w w;
    json_doc d;
    const json_val *res;
    jw_init(&w), jw_raw(&w, params, strlen(params));
    if (!btc_rpc_call(c, method, &w, &d, &res)) return false;
    if (out) snprintf(out, cap, "%s", json_str(res) ? json_str(res) : "");
    json_free(&d);
    return true;
}

static void mine(btc_rpc *c, unsigned blocks)
{
    char params[160];
    snprintf(params, sizeof params, "[%u,\"%s\"]", blocks, mine_addr);
    CHECK(node_call(c, "generatetoaddress", params, NULL, 0));
}

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
        char addr[64], params[100];
        CHECK(rpc_client(&c) && strcpy(c.wallet, "/wallet/pz"));
        addr_of(spk, addr, sizeof addr);
        snprintf(params, sizeof params, "[\"%s\",1.00000000]", addr);
        CHECK(node_call(&c, "sendtoaddress", params, NULL, 0));
        /* until the coin is in a block: the block of the miner of this test, made before the transaction came, can take the place of
         * the one mined here, and the coin then waits for the next (the session would find the address empty) */
        chain_rpc cr = {.rpc = &c, .chain = BTC_REGTEST};
        const session_chain ch = chain_rpc_iface(&cr);
        payout_coin pc[2];
        int there = 0;
        for (int k = 0; k < 20 && there < 1; k++) mine(&c, 1), there = ch.coins(ch.ctx, spk, pc, 2);
        CHECK(there == 1);
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
            if (!chain.u[i].height && chain.u[i].rate >= chain.mine_from) chain.u[i].height = chain.tip;
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
    bool pump;                         /* the sender keeps the channel alive and says nothing else: an S_ALIVE every 100 ms when it is empty */
} queue;

/* what an end does to its own messages of the second signing (the counts of the first tell which one is the last): sends it, drops it (and
 * its S_SIGNED), drops it and keeps the channel alive for ever, aborts instead, damages it, or sends it and then garbage and an abort */
enum { LAST_SENT, LAST_DROPPED, LAST_PUMP, LAST_ABORT, LAST_DAMAGED, LAST_JUNK };
/* and to its piece after the payout: gives it, keeps it, or gives a wrong one */
enum { PIECE_GIVEN, PIECE_KEPT, PIECE_WRONG };

typedef struct {
    queue *in, *out;
    int last_b;                        /* LAST_*: the last signing message of the second transaction */
    int piece;                         /* PIECE_* */
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
    if (len == 33 && m[0] == S_PIECE && e->piece != PIECE_GIVEN) {
        if (e->piece == PIECE_KEPT) return true;
        uint8_t bad[33];
        memcpy(bad, m, 33), bad[32] ^= 1;
        return q_put(e->out, bad, 33), true;
    }
    if (len >= 1 && (m[0] == S_PLAN || m[0] == S_SIGNER)) q_put(e->out, (const uint8_t[]){S_ALIVE}, 1);   /* the reader must skip it */
    if (e->last_b != LAST_SENT && len >= 1) {
        if (m[0] == S_PLAN) {
            if (++e->plans == 2) e->msgs_a = e->msgs;          /* the second plan: the first signing is over, its messages counted */
            e->msgs = 0;
        } else if (m[0] == S_SIGNER && len >= 2 && m[1] == 0) {
            if (++e->msgs == e->msgs_a && e->plans == 2) {     /* the same count as the first signing: the last one */
                if (e->last_b == LAST_DROPPED) return true;
                if (e->last_b == LAST_PUMP) {
                    pthread_mutex_lock(&e->out->mu);
                    e->out->pump = true;
                    pthread_cond_signal(&e->out->cv);
                    pthread_mutex_unlock(&e->out->mu);
                    return true;
                }
                if (e->last_b == LAST_ABORT) return q_put(e->out, (const uint8_t[]){S_ABORT, 'n', 'o'}, 3), true;
                if (e->last_b == LAST_JUNK)
                    return q_put(e->out, m, len), q_put(e->out, (const uint8_t[]){0xee, 0xde, 0xad}, 3),   /* of no type */
                           q_put(e->out, (const uint8_t[]){S_ABORT, 'n', 'o'}, 3), true;
                uint8_t *bad = malloc(len);
                memcpy(bad, m, len), bad[len - 1] ^= 1;
                q_put(e->out, bad, len);
                return free(bad), true;
            }
        } else if (m[0] == S_SIGNED && e->plans == 2 && (e->last_b == LAST_DROPPED || e->last_b == LAST_PUMP)) return true;
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
    while (q->head == q->tail) {
        if (q->pump) {
            pthread_mutex_unlock(&q->mu);
            usleep(100000);
            return m[0] = S_ALIVE, *len = 1, true;
        }
        if (pthread_cond_timedwait(&q->cv, &q->mu, &until) != 0) {
            pthread_mutex_unlock(&q->mu);
            return false;
        }
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
static const char *g_case = "";                               /* the case under way, for the watchdog */

/* a case that hangs fails with its name, instead of holding the run until the CI stops it */
static void watchdog(int sig)
{
    static const char m[] = "t_session: FAIL: still running after 5 minutes: ";
    (void)sig;
    (void)!write(2, m, sizeof m - 1), (void)!write(2, g_case, strlen(g_case)), (void)!write(2, "\n", 1);
    _exit(1);
}

static void watch(const char *name) { g_case = name, alarm(300); }

static const char *in_dir(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", g_dir, name);
    return out;
}

static void write_script(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    CHECK(f && fputs(text, f) >= 0);
    if (f) fclose(f);
    CHECK(chmod(path, 0700) == 0);
}

/* a line of a file of the limit: key, its count, and the time of its first failure (0: none) */
static void add_line(const char *path, const uint8_t key[32], unsigned count, long long since)
{
    char hex[65];
    FILE *f = fopen(path, "a");
    pt_hex(key, 32, hex);
    CHECK(f && (since ? fprintf(f, "%s %u %lld\n", hex, count, since) : fprintf(f, "%s %u\n", hex, count)) > 0);
    if (f) fclose(f);
}

/* the failures that a file of the limit counts for key (0 when it has no line for it) */
static unsigned fails_of(const char *path, const uint8_t key[32])
{
    char want[65], hex[80], line[200];
    unsigned v, n = 0;
    pt_hex(key, 32, want);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "%79s %u", hex, &v) == 2 && !strcmp(hex, want)) n = v;
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

/* the partner of a home proved its distance */
static bool on_proved(void *user) { return *(bool *)user = true; }

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

enum { PAYOUT, CANCEL, NOTHING, FIRST, TAKEN, OTHER, COINS, TWICE };   /* what the chain must show at the end (FIRST: the first
                                                  * transaction alone; TAKEN: the prize spent by someone else before; COINS: the payout, and
                                                  * the coin that came after it at the address, F's cancel having voided the second of its
                                                  * payout; TWICE: the payout, and the payout of a coin that came after it) */

typedef struct {
    const char *name;
    int expect;
    int s_last, f_last;               /* LAST_*: what S and F do to their last message of the second signing */
    uint64_t fee;                     /* the fee rate the chains of both homes estimate (0: the usual) */
    uint64_t fee_far;                 /* the fee rate the chain of home 1 estimates, home 0's the usual */
    bool unproved;                    /* the session ends before the proofs */
    int lost_heights;                 /* answers about the height each chain loses at first */
    unsigned limit[2];                /* failures already counted on the network by home 0, and for the pair by home 1 */
    bool limit_old;                   /* those on the network were counted from 31 days ago: they are forgotten */
    unsigned limit_ever;              /* failures already counted by home 0 on the network in all, which are never forgotten */
    unsigned pays;                    /* payouts that did not pay already counted by home 0 on the network in all, never forgotten */
    unsigned fails_F, fails_S;        /* the failed signings F and S count in the case */
    const char *bin[2];               /* the pzsign of each home (NULL: the real one) */
    const char *why;                  /* in the reason of both homes, when not NULL */
    bool scan_fails;                  /* the scan of the puzzle address fails: the prize of the network file is given */
    bool frozen;                      /* the chain stops, and a wait gives up after a while of block_ms a block */
    int slow_ms, timeout_ms;          /* the scan of home 0 takes slow_ms; the wait for a message (0: 20 s) */
    bool silent0;                     /* the pzsign of home 0 sends nothing: home 1, which never heard it, counts no failure */
    int block_ms;
    bool pieces;                      /* after the payout, the exchange of the pieces */
    int piece1;                       /* PIECE_*: what home 1 does to its piece */
    bool ghost;                       /* a coin comes to the puzzle address after the payout: no exchange */
    bool again;                       /* then a session of the pair that only exchanges the pieces, both giving theirs */
    bool taken;                       /* the prize was taken before the session: the address is empty */
    bool other;                       /* another pair's first transaction takes the prize output of this pair's first */
    bool copy;                        /* an old copy of the first transaction, with other signatures, is mined in its place */
    bool node;                        /* a chain of a node of its own (no pending: an empty scan is the truth), not of servers */
    bool node1;                       /* only home 1's is of a node, home 0's of servers */
    bool no_prizes;                   /* no coin of the prize is known (a network file without prize= lines) */
    bool hide_prize;                  /* home 0 knows one more coin of the prize, which its chain shows unspent (a server hides it) */
    int refuse_s;                     /* the sends of the second transaction S's chain refuses */
    int cancel_blocks;                /* the blocks F waits before its cancel (0: 3) */
    bool restart_s;                   /* S's chain stops once S sends the second: then a later session, the chain back, settles it */
    bool f_forgets;                   /* F loses its record of the payout before the later session */
    bool f_moves;                     /* and has another payout address in it */
    bool dust;                        /* twelve coins too small for any payout come to the puzzle address once the second is in a block */
    bool spend_payouts;               /* both homes spend their payout before the later session */
    bool spend_author;                /* and the output of the author is spent too */
    bool coin_again;                  /* and a coin of 1 BTC comes to the puzzle address before the later session, which pays it out */
    bool slow_F;                      /* the slow scan is F's, not home 0's */
    bool coins_after;                 /* a coin comes to the puzzle address once the second is in a block; a later session pays it out, and
                                       * S keeps that second and its last message */
    bool behind;                      /* a coin of half a BTC came to the puzzle address that home 0's chain does not show yet */
    bool tiny_dust;                   /* F's coin key holds a tiny coin of an earlier try and 15 coins of dust, each output 1 of a
                                       * transaction whose output 0 is spent in a block */
    bool bump;                        /* the chain takes no rate below 50 sat/vB once the first is in a block: the second waits; a later
                                       * session, at twice an estimate of 30, signs it again */
    bool memory;                      /* only on the chain in memory, not on a node */
} tcase;

/* another pair of the same puzzle, whose key the test knows: once this pair's first transaction is in a block, its own first transaction
 * spends the prize output back to the puzzle address, before this pair's second gets there; or (rise) the chain takes no rate below 50
 * sat/vB from then on, and the second waits in the mempool */
typedef struct { const uint8_t *coin, *spk, *k; atomic_bool done; bool rise; } other_arg;
static void *other_pair(void *arg)
{
    other_arg *a = arg;
    uint8_t first[32];
    for (bool in = false; !in; usleep(2000)) {
        if (atomic_load(&a->done)) return NULL;                 /* the sessions ended before this pair's first was in a block */
        pthread_mutex_lock(&chain.mu);
        const utxo *c = find(a->coin, 0), *f = c && c->spent ? find(c->by, 0) : NULL;
        if ((in = f && f->height)) memcpy(first, c->by, 32), chain.mine_from = a->rise ? 50000 : 0;
        pthread_mutex_unlock(&chain.mu);
    }
    if (a->rise) return NULL;
    pthread_mutex_lock(&chain.mu);
    const uint64_t v = find(first, 0)->value;
    pthread_mutex_unlock(&chain.mu);
    btc_tx t;
    btc_tx_init(&t);
    CHECK(btc_tx_add_in(&t, first, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&t, v - 2000, a->spk, BTC_P2PKH_LEN) && btc_sign_p2pkh(&t, 0, a->k));
    CHECK(ch_broadcast(NULL, &t));
    return NULL;
}

/* the coin that comes to the puzzle address after the payout: sent as soon as the second transaction of the pair is in a block */
typedef struct { const uint8_t *coin, *spk, *seed; atomic_bool done; bool dust; } after_arg;
static void *coin_after(void *arg)
{
    after_arg *a = arg;
    for (bool in = false; !in; usleep(2000)) {
        if (atomic_load(&a->done)) return NULL;
        pthread_mutex_lock(&chain.mu);
        const utxo *c = find(a->coin, 0), *f = c && c->spent ? find(c->by, 0) : NULL, *b = f && f->spent ? find(f->by, 0) : NULL;
        in = b && b->height;
        pthread_mutex_unlock(&chain.mu);
    }
    if (!a->dust) return fund(a->spk, a->seed), NULL;
    pthread_mutex_lock(&chain.mu);                              /* 108 000 satoshis in all, none worth an input of a payout */
    for (uint32_t i = 0; i < 12; i++) add_utxo(a->seed, i, 9000, a->spk, BTC_P2PKH_LEN, chain.tip);
    pthread_mutex_unlock(&chain.mu);
    return NULL;
}

/* each home spends its output of the second transaction (to its own address again), mined at the next block; with author, the output of
 * the author is spent too, by a transaction of its own */
static void spend_payouts(const session_paid *p, const home h[2], const char *label, bool author)
{
    btc_tx b, t;
    uint8_t bid[32];
    CHECK(btc_tx_parse(&b, p->second, p->second_len) && btc_tx_txid(&b, bid));
    for (uint32_t v = 0; v < 2; v++) {
        const int i = !memcmp(b.out[v].script, h[0].cfg.pay_spk, BTC_P2PKH_LEN) ? 0 : 1;
        uint8_t ps[32];
        char l[160];
        snprintf(l, sizeof l, "%s pay %d", label, i), hashed(l, ps);
        btc_tx_init(&t);
        CHECK(btc_tx_add_in(&t, bid, v, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&t, b.out[v].value - 1000, h[i].cfg.pay_spk, BTC_P2PKH_LEN) &&
              btc_sign_p2pkh(&t, 0, ps) && ch_broadcast(NULL, &t));
    }
    pthread_mutex_lock(&chain.mu);
    utxo *u = find(bid, 2);
    if (author) u->spent = true, memset(u->by, 0xa7, 32), add_utxo(u->by, 0, 1, (const uint8_t[]){0x6a}, 1, chain.tip);
    pthread_mutex_unlock(&chain.mu);
    usleep(500000);                                             /* mined */
}

/* a puzzle, its coin on the chain, two homes whose records meet on the same point; then the session of both */
static void run_case(const tcase *tc, const char *bin)
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
    if (tc->taken) {                                            /* spent in a block by someone else, before the session */
        pthread_mutex_lock(&chain.mu);
        utxo *u = find(coin_txid, 0);
        if (u) u->spent = true, memset(u->by, 0x99, 32), add_utxo(u->by, 0, 1, (const uint8_t[]){0x6a}, 1, chain.tip);
        pthread_mutex_unlock(&chain.mu);
    }
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
    session_paid paid[2];
    memset(paid, 0, sizeof paid);
    bool proved[2] = {false, false};
    char limpath[2][96];
    uint8_t xo[2][32];
    for (int i = 0; i < 2; i++) {
        uint8_t sec[32];
        char l[128];
        secp256k1_xonly_pubkey xpk;
        snprintf(l, sizeof l, "%s home %d", label, i), hashed(l, sec);
        CHECK(secp256k1_keypair_create(ctx, &h[i].id, sec) && secp256k1_keypair_xonly_pub(ctx, &xpk, NULL, &h[i].id));
        CHECK(secp256k1_xonly_pubkey_serialize(ctx, xo[i], &xpk));
        memcpy(r[i].x, c + 1, 32), memcpy(r[i].owner, xo[i], 32), r[i].dp = net.dpbits;
    }
    record pair[2];
    CHECK(pieces_order(&r[0], &r[1], pair));
    uint8_t fm[11 + 64], own[32], paid_key[32], ever[2][32], pays[2][32];   /* the keys of the pair in the files of the limit: its point, the same for both; */
    memcpy(fm, "pznet1/fail", 11), memcpy(fm + 11, net.fp, 32), memcpy(fm + 43, r[0].x, 32);
    pz_sha256(fm, sizeof fm, own);
    memcpy(fm, "pznet1/paid", 11);
    pz_sha256(fm, sizeof fm, paid_key);
    for (int i = 0; i < 2; i++) {                               /* and each home on the network, for ever */
        memcpy(fm, "pznet1/ever", 11), memcpy(fm + 43, xo[i], 32);
        pz_sha256(fm, sizeof fm, ever[i]);
        memcpy(fm, "pznet1/pays", 11);
        pz_sha256(fm, sizeof fm, pays[i]);
    }
    const int F_home = !memcmp(pair[pieces_first(&net, pair)].owner, xo[0], 32) ? 0 : 1, S_home = 1 - F_home;
    for (int i = 0; i < 2; i++) {
        session_cfg *g = &h[i].cfg;
        memset(g, 0, sizeof *g);
        g->ctx = ctx, g->net = &net, g->identity = &h[i].id;
        memcpy(g->base, base, 32), memcpy(g->pubkey33, p33, 33);
        g->mine = r[i], g->theirs = r[1 - i];
        memcpy(g->d, d[i], 32);
        char l[128];
        snprintf(l, sizeof l, "%s coin %d", label, i), hashed(l, g->coin_sec);
        snprintf(l, sizeof l, "%s pay %d", label, i);
        uint8_t ps[32];
        hashed(l, ps);
        p2pkh_of(ps, g->pay_spk), g->pay_len = BTC_P2PKH_LEN;
        g->max_feerate = 200000, g->pzsign = tc->bin[i] ? tc->bin[i] : bin;
        g->min_feerate = tc->dust || tc->tiny_dust ? 20000 : 0;   /* the lowest rate of pzpeer (else that of the chain) */
        chan[i] = (session_chan){&e[i], q_send, q_recv};
        if (regtest) {                                          /* each home its own client of the node */
            CHECK(rpc_client(&rpcs[i]));
            crpc[i] = (chain_rpc){.rpc = &rpcs[i], .chain = BTC_REGTEST, .fallback_feerate = tc->fee ? tc->fee : g_fee[i]};
            chn[i].in = chain_rpc_iface(&crpc[i]);
        } else chn[i].in = (session_chain){i ? (void *)1 : NULL, ch_coins, ch_broadcast, ch_conf, ch_height, ch_feerate, ch_coin, ch_pending, NULL};
        chn[i].lost = tc->lost_heights, chn[i].frozen = tc->frozen, chn[i].scan_fails = tc->scan_fails, chn[i].at = 0, chn[i].ghost = tc->ghost;
        chn[i].old = NULL, chn[i].behind = NULL;
        chn[i].refuse = i == S_home ? tc->refuse_s : 0, chn[i].stop_at_second = i == S_home && tc->restart_s;
        chn[i].fee = tc->fee_far ? (i ? tc->fee_far : g_fee[0]) : 0;   /* the two rates, also on a node with an estimate of its own */
        if (tc->pieces || tc->restart_s || tc->bump) g->paid = &paid[i], g->reveal_conf = 2;
        chn[i].slow_ms = (tc->slow_F ? i == F_home : i == 0) ? tc->slow_ms : 0;
        g->block_ms = i == S_home && tc->restart_s ? 10 : tc->block_ms;   /* a stopped chain ends S's waits soon */
        if (!tc->taken && !tc->no_prizes) {                     /* the prize, as a line of the network file gives it */
            payout_coin pc[64];
            CHECK(chn[i].in.coins(chn[i].in.ctx, puzzle_spk, pc, 64) == 1);
            g->prizes[0] = pc[0], g->nprizes = 1;
        }
        if (!i && tc->hide_prize) {                             /* below the checkpoint; its spend is hidden once the payout is made */
            g->prizes[g->nprizes++] = (payout_coin){.txid = {0x61, 0xde}, .vout = 3, .value = 50000000};
            chn[i].old = &g->prizes[1];
        }
        g->chan = &chan[i];
        g->timeout_ms = tc->timeout_ms ? tc->timeout_ms : 20000, g->poll_ms = 50, g->cancel_blocks = tc->cancel_blocks ? tc->cancel_blocks : 3, g->alive_ms = 1;
        /* each home its limit, in a file of the case */
        snprintf(l, sizeof l, "limit%d", i);
        in_dir(l, limpath[i], sizeof limpath[i]);
        remove(limpath[i]);
        lim[i] = (session_limit){.path = limpath[i], .max = 10, .pair_max = 3, .ever_max = 64, .pay_max = 5, .pay_ever_max = 10};
        pthread_mutex_init(&lim[i].mu, NULL);
        pthread_cond_init(&lim[i].cv, NULL);
        g->limit = &lim[i], g->on_proved = on_proved, g->user = &proved[i];
        /* the network with the time of its first failure; the others for ever */
        if (!i && tc->limit[0]) add_line(limpath[0], net.fp, tc->limit[0], (long long)time(NULL) - (tc->limit_old ? 31 * 86400LL : 0));
        if (i && tc->limit[1]) add_line(limpath[1], own, tc->limit[1], 0);
        if (!i && tc->limit_ever) add_line(limpath[0], ever[0], tc->limit_ever, 0);
        if (!i && tc->pays) add_line(limpath[0], pays[0], tc->pays, 0);
    }
    uint8_t behind[32];
    char bl[128];
    snprintf(bl, sizeof bl, "%s behind", label), hashed(bl, behind);
    pthread_mutex_lock(&chain.mu);
    if (tc->behind) add_utxo(behind, 0, 50000000, puzzle_spk, BTC_P2PKH_LEN, chain.tip), chn[0].behind = behind;
    if (tc->tiny_dust) {                                        /* their outputs 0 spent by one transaction, in a block */
        uint8_t coin_spk[BTC_P2PKH_LEN], by[32];
        p2pkh_of(h[F_home].cfg.coin_sec, coin_spk);
        snprintf(bl, sizeof bl, "%s spends the dust", label), hashed(bl, by);
        add_utxo(by, 0, 1, (const uint8_t[]){0x6a}, 1, chain.tip);
        for (int j = 0; j < 16; j++) {
            uint8_t id[32];
            snprintf(bl, sizeof bl, "%s dust %d", label, j), hashed(bl, id);
            add_utxo(id, 0, 1000, (const uint8_t[]){0x6a}, 1, chain.tip);
            chain.u[chain.n - 1].spent = true, memcpy(chain.u[chain.n - 1].by, by, 32);
            add_utxo(id, 1, j ? 546 : 10000, coin_spk, BTC_P2PKH_LEN, chain.tip);
        }
    }
    pthread_mutex_unlock(&chain.mu);
    session_chain ifc[2];
    for (int i = 0; i < 2; i++) {
        ifc[i] = (session_chain){&chn[i], fl_coins, fl_broadcast, fl_conf, fl_height, fl_feerate, fl_coin, tc->node || (i && tc->node1) ? NULL : fl_pending,
                                 NULL};
        h[i].cfg.chain = &ifc[i];
    }
    if (tc->fee) g_fee[0] = g_fee[1] = tc->fee;
    e[S_home].last_b = tc->s_last, e[F_home].last_b = tc->f_last;
    e[1].piece = tc->piece1;
    pthread_t th[2], ot;
    other_arg oa = {coin_txid, puzzle_spk, k, false, tc->bump};
    if (tc->other || tc->bump) pthread_create(&ot, NULL, other_pair, &oa);
    uint8_t after_txid[32];
    char al[128];
    snprintf(al, sizeof al, "%s after", label), hashed(al, after_txid);
    after_arg aa = {coin_txid, puzzle_spk, after_txid, false, tc->dust};
    if (tc->coins_after || tc->dust) pthread_create(&ot, NULL, coin_after, &aa);
    if (tc->copy) g_copy = coin_txid;
    for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, run_home, &h[i]);
    for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
    if (tc->other || tc->bump) atomic_store(&oa.done, true), pthread_join(ot, NULL);
    if (tc->coins_after || tc->dust) atomic_store(&aa.done, true), pthread_join(ot, NULL);
    g_copy = NULL;
    g_fee[0] = 2000, g_fee[1] = 3000;
    q_drain(&q[0]), q_drain(&q[1]);
    static const char *const RES[] = {"paid", "cancelled", "failed"};
    printf("  %s: F %s (%s), S %s (%s)\n", tc->name, RES[h[F_home].res], h[F_home].why, RES[h[S_home].res], h[S_home].why);
    if (tc->other) {                                            /* nothing was paid: no payout is recorded, no piece given */
        for (int i = 0; i < 2; i++) CHECK(h[i].res == SESSION_FAILED && !paid[i].paid_at && !paid[i].have_key && strstr(h[i].why, "not by the second"));
    } else if (tc->taken) {                                     /* taken for gone only on a node: servers may hide the coins */
        for (int i = 0; i < 2; i++) CHECK(h[i].res == SESSION_FAILED && paid[i].empty == tc->node && !paid[i].paid_at);
        const char *w = tc->node ? "the prize has been taken" : "proves nothing";
        CHECK(strstr(h[0].why, w) || strstr(h[1].why, w));
    } else if (tc->pieces) {
        /* home 1 has home 0's piece whatever it does with its own; home 0 has the key only when home 1 gave the right piece */
        const bool later = tc->ghost || tc->coins_after, none = later || tc->hide_prize || tc->no_prizes;
        const bool want[2] = {!none && tc->piece1 == PIECE_GIVEN, !none};
        for (int i = 0; i < 2; i++) {
            CHECK(paid[i].paid_at > 0 && paid[i].have_key == want[i] && paid[i].refused == tc->no_prizes);
            if (want[i]) CHECK(!memcmp(paid[i].key, k, 32));
        }
        if (tc->piece1 == PIECE_KEPT) CHECK(strstr(h[0].why, "no piece from the partner") != NULL);
        if (tc->piece1 == PIECE_WRONG) CHECK(strstr(h[0].why, "not the right one") != NULL);
        if (later) CHECK(strstr(h[0].why, "paid out first") && strstr(h[1].why, "paid out first"));
        /* a named coin that the servers show unspent counts in what is left: no piece, and the partner, whose word gets no answer, gives none */
        if (tc->hide_prize) CHECK(strstr(h[0].why, "paid out first") && strstr(h[1].why, "no piece from the partner"));
        if (tc->no_prizes) CHECK(strstr(h[0].why, "--rpc") && strstr(h[1].why, "the partner gives no piece"));
    }
    if (tc->coins_after) {                                      /* a later session pays out the coin that came; S keeps its second */
        e[S_home].last_b = LAST_DROPPED, e[S_home].plans = e[S_home].msgs = 0, chn[S_home].refuse = 1000;
        for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, run_home, &h[i]);
        for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
        q_drain(&q[0]), q_drain(&q[1]);
        printf("  %s, later: F %s (%s), S %s (%s)\n", tc->name, RES[h[F_home].res], h[F_home].why, RES[h[S_home].res], h[S_home].why);
        CHECK(!paid[S_home].paid_at && paid[S_home].second_len);   /* the record of S: that second, in no block (its exchange sees it void) */
    }
    if (tc->again) {                                            /* a later session of the pair: no payout, only the pieces */
        e[1].piece = PIECE_GIVEN;
        chn[S_home].frozen = chn[S_home].stop_at_second = false, chn[S_home].at = 0, h[S_home].cfg.block_ms = tc->block_ms;
        if (tc->f_forgets) memset(&paid[F_home], 0, sizeof paid[F_home]);
        uint8_t was[BTC_P2PKH_LEN], ps[32];
        memcpy(was, h[F_home].cfg.pay_spk, sizeof was);
        if (tc->f_moves) hashed("another payout address", ps), p2pkh_of(ps, h[F_home].cfg.pay_spk);
        if (tc->spend_payouts) spend_payouts(&paid[0], h, label, tc->spend_author);
        if (tc->coin_again) fund(puzzle_spk, after_txid);
        if (tc->bump) chn[0].fee = chn[1].fee = 30000;
        for (int i = 0; i < 2; i++) pthread_create(&th[i], NULL, run_home, &h[i]);
        for (int i = 0; i < 2; i++) pthread_join(th[i], NULL);
        q_drain(&q[0]), q_drain(&q[1]);
        printf("  %s, again: %s (%s), %s (%s)\n", tc->name, RES[h[0].res], h[0].why, RES[h[1].res], h[1].why);
        for (int i = 0; i < 2; i++) CHECK(h[i].res == SESSION_PAID && paid[i].paid_at && paid[i].have_key && !memcmp(paid[i].key, k, 32));
        memcpy(h[F_home].cfg.pay_spk, was, sizeof was);
    }
    pthread_mutex_lock(&chain.mu);
    chain.mine_from = 0;                                        /* the second of the case bump, mined at last if it waits still */
    pthread_mutex_unlock(&chain.mu);
    /* what the chain holds now */
    uint64_t pay[2], at_puzzle = balance(puzzle_spk);
    uint64_t coin_left[2];                                                          /* the satoshis of its tiny coin still unspent */
    for (int i = 0; i < 2; i++) {
        uint8_t coin_spk[BTC_P2PKH_LEN];
        p2pkh_of(h[i].cfg.coin_sec, coin_spk);
        pay[i] = balance(h[i].cfg.pay_spk);
        coin_left[i] = balance(coin_spk);
        if (regtest) btc_rpc_done(&rpcs[i]);
    }
    printf("    paid: F %llu, S %llu sat; left on the puzzle address %llu sat; tiny coins left: F %llu, S %llu sat\n", (unsigned long long)pay[F_home],
           (unsigned long long)pay[S_home], (unsigned long long)at_puzzle, (unsigned long long)coin_left[F_home], (unsigned long long)coin_left[S_home]);
    if (tc->expect == PAYOUT) {
        CHECK(h[0].res == SESSION_PAID && h[1].res == SESSION_PAID);
        /* 45% each of 1 BTC less two fees (on a regtest node with a history, at twice its own estimate), 10% to the author */
        CHECK(at_puzzle == (tc->dust ? 12 * 9000u : 0) + (tc->behind ? 50000000u : 0) && pay[S_home] > 44900000 && pay[S_home] < 45000000);
        /* half each of the rest of the prize and of F's tiny coin, which came from it: no tiny coin is left (the odd satoshi goes to S); F
         * swept the tiny coin of its earlier try, less the fee of one input and one output at 20 sat/vB, and left the dust */
        const uint64_t f = pay[F_home] - (tc->tiny_dust ? 10000 - btc_fee(192, 20000) : 0);
        CHECK(pay[S_home] >= f && pay[S_home] - f <= 1 && coin_left[F_home] == (tc->tiny_dust ? 15 * 546u : 0) && !coin_left[S_home]);
    } else if (tc->expect == CANCEL) {
        CHECK(h[F_home].res == SESSION_CANCELLED && h[S_home].res == SESSION_FAILED);
        CHECK(at_puzzle > 99800000 && pay[S_home] == 0);                           /* the prize is still under the joint key */
        CHECK(pay[F_home] > 0 && pay[F_home] < 100000 && !coin_left[F_home]);      /* F spent its tiny coin: the held transaction is void */
    } else if (tc->expect == FIRST) {
        CHECK(h[0].res == SESSION_FAILED && h[1].res == SESSION_FAILED);
        CHECK(at_puzzle > 99900000 && at_puzzle < 100000000 && pay[0] == 0 && pay[1] == 0 && coin_left[F_home] && !coin_left[S_home]);
    } else if (tc->expect == OTHER) {
        CHECK(at_puzzle > 99900000 && pay[0] == 0 && pay[1] == 0 && coin_left[F_home]);   /* the prize is back at the address */
    } else if (tc->expect == COINS) {                           /* the first payout, and the coin that came later under the joint key */
        CHECK(h[F_home].res == SESSION_CANCELLED && h[S_home].res == SESSION_FAILED && !coin_left[F_home]);
        CHECK(at_puzzle > 99800000 && at_puzzle < 100000000 && pay[S_home] > 44900000 && pay[S_home] < 45000000);
    } else if (tc->expect == TWICE) {                           /* 45% each of both coins, the first share moved on since */
        CHECK(h[0].res == SESSION_PAID && h[1].res == SESSION_PAID && at_puzzle == 0 && !coin_left[0] && !coin_left[1]);
        CHECK(pay[0] > 89800000 && pay[0] < 90000000 && pay[1] > 89800000 && pay[1] < 90000000);
    } else if (tc->expect == NOTHING) {
        CHECK(h[0].res == SESSION_FAILED && h[1].res == SESSION_FAILED);
        CHECK(at_puzzle == 100000000 && pay[0] == 0 && pay[1] == 0 && !coin_left[0] && !coin_left[1]);   /* nothing was signed */
    } else CHECK(at_puzzle == 0 && pay[0] == 0 && pay[1] == 0);
    if (tc->why) CHECK(strstr(h[0].why, tc->why) || strstr(h[1].why, tc->why));
    if (tc->expect == CANCEL) CHECK(e[F_home].alives > 0);      /* sent while F waited its blocks */
    CHECK(tc->unproved || (proved[0] && proved[1]));
    /* the limits: a failed signing counts for the network and the pair, a good one nothing, a refused one nothing */
    for (int i = 0; i < 2; i++) {
        const unsigned failed = tc->silent0 && i == 1 ? 0 : i == F_home ? tc->fails_F : tc->fails_S;
        CHECK(fails_of(limpath[i], net.fp) == (i || tc->limit_old ? 0 : tc->limit[0]) + failed);
        CHECK(fails_of(limpath[i], own) == (i ? tc->limit[1] : 0) + failed);
        CHECK(fails_of(limpath[i], ever[i]) == (i ? 0 : tc->limit_ever) + failed);
        /* a payout counts from the signing of its first transaction until it pays, for the point and the home */
        const unsigned tries = tc->expect == CANCEL || tc->expect == FIRST || tc->expect == OTHER || tc->expect == COINS || (tc->bin[0] && failed);
        CHECK(fails_of(limpath[i], paid_key) == tries && fails_of(limpath[i], pays[i]) == (i ? 0 : tc->pays) + tries);
        pthread_mutex_destroy(&lim[i].mu);
        pthread_cond_destroy(&lim[i].cv);
        remove(limpath[i]);
    }
}

/* A partner that copied the x of home 0's record into a record of its own type, under its own identity, and cannot prove it: home 0 runs
 * no pzsign for it (the one it would run leaves a mark). With terms 0 its hello names no version of the terms of the payout. */
static void run_copied(const char *bin, unsigned terms)
{
    static record_net net;
    uint8_t base[32] = {0}, k[32] = {0}, p33[33], d0[32], c[33];
    char mark[96];
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
    record mine = {.t = 0, .dp = net.dpbits}, theirs = {.t = 1, .dp = net.dpbits};
    memcpy(mine.x, c + 1, 32), memcpy(theirs.x, c + 1, 32), memcpy(mine.owner, xo[0], 32), memcpy(theirs.owner, xo[1], 32);
    CHECK(record_check(ctx, &net, &mine) && record_check(ctx, &net, &theirs));
    queue q[2] = {{.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}, {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER}};
    end e[2] = {{.in = &q[0], .out = &q[1]}, {.in = &q[1], .out = &q[0]}};
    session_chan chan = {&e[0], q_send, q_recv};
    session_chain chn = {NULL, ch_coins, ch_broadcast, ch_conf, ch_height, ch_feerate, ch_coin, ch_pending, NULL};
    session_cfg *g = &h.cfg;
    memset(g, 0, sizeof *g);
    g->ctx = ctx, g->net = &net, g->identity = &h.id, g->mine = mine, g->theirs = theirs;
    memcpy(g->base, base, 32), memcpy(g->pubkey33, p33, 33), memcpy(g->d, d0, 32);
    hashed("copied coin", g->coin_sec);
    p2pkh_of(g->coin_sec, g->pay_spk), g->pay_len = BTC_P2PKH_LEN;
    g->max_feerate = 200000, g->pzsign = bin;
    remove(in_dir("copied-mark", mark, sizeof mark));
    g->chan = &chan, g->chain = &chn, g->timeout_ms = 20000, g->poll_ms = 50, g->cancel_blocks = 3;
    bool proved = false;
    g->on_proved = on_proved, g->user = &proved;
    pthread_t th;
    pthread_create(&th, NULL, run_home, &h);
    /* the thief: its hello, then a proof it could not make */
    uint8_t m[1 + 2 * RECORD_LEN + 32 + BTC_P2PKH_LEN + 2 + BTC_P2PKH_LEN] = {S_HELLO}, proof[1 + 64] = {S_PROOF};
    record_encode(&theirs, m + 1), record_encode(&mine, m + 1 + RECORD_LEN);
    p2pkh_of(sec, m + 1 + 2 * RECORD_LEN + 32);
    m[1 + 2 * RECORD_LEN + 32 + BTC_P2PKH_LEN] = (uint8_t)terms, m[1 + 2 * RECORD_LEN + 33 + BTC_P2PKH_LEN] = BTC_P2PKH_LEN;
    p2pkh_of(sec, m + 1 + 2 * RECORD_LEN + 34 + BTC_P2PKH_LEN);
    hashed("not a proof", proof + 1), hashed("really not", proof + 33);
    CHECK(q_send(&e[1], m, sizeof m) && q_send(&e[1], proof, sizeof proof));
    pthread_join(th, NULL);
    q_drain(&q[0]), q_drain(&q[1]);
    printf("  %s: %s (%s)\n", terms ? "copied x" : "no terms of the payout", h.res == SESSION_FAILED ? "failed" : "not failed", h.why);
    CHECK(h.res == SESSION_FAILED && strstr(h.why, terms ? "did not prove" : "does not know") && !proved);
    CHECK(access(mark, F_OK) != 0);
}

int main(void)
{
    const char *bin = getenv("PZSIGN");
    if (!bin) {
        printf("t_session: skipped (set PZSIGN)\n");
        return 0;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);                           /* each case shows when it ends, also through a pipe */
    signal(SIGALRM, watchdog);
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    snprintf(g_dir, sizeof g_dir, "/tmp/t_session.XXXXXX");
    CHECK(mkdtemp(g_dir) != NULL);
    const char *rt = getenv("PZ_REGTEST");
    if (rt && *rt) {                                            /* host:port:user:password */
        regtest = sscanf(rt, "%63[^:]:%u:%63[^:]:%63s", node.host, &node.port, node.user, node.pass) == 4;
        btc_rpc c;
        CHECK(regtest && rpc_client(&c));
        /* the wallet of the tests: made, or there already, loaded or to load after a restart of the node */
        if (!node_call(&c, "createwallet", "[\"pz\"]", NULL, 0)) (void)node_call(&c, "loadwallet", "[\"pz\"]", NULL, 0);
        CHECK(strcpy(c.wallet, "/wallet/pz") && node_call(&c, "getnewaddress", "[\"\",\"legacy\"]", mine_addr, sizeof mine_addr) && mine_addr[0]);
        c.wallet[0] = 0;
        mine(&c, 101);                                          /* coinbase coins mature after 100 blocks */
        btc_rpc_done(&c);
        printf("  on the regtest node %s:%u\n", node.host, node.port);
    }
    /* the programs of some cases: a pzsign that leaves a mark when it runs, one that hangs and reads nothing, one that floods */
    char marker[96], hang[96], flood[96], text[1200], mark[96];
    snprintf(text, sizeof text, "#!/bin/sh\ntouch '%s'\nexec '%s' \"$@\"\n", in_dir("copied-mark", mark, sizeof mark), bin);
    write_script(in_dir("marker", marker, sizeof marker), text);
    write_script(in_dir("hang", hang, sizeof hang), "#!/bin/sh\nexec sleep 600\n");
    write_script(in_dir("flood", flood, sizeof flood),
                 "#!/bin/sh\ni=0\nwhile [ $i -lt 5 ]; do printf '\\000\\000\\352\\141\\201'; head -c 60000 /dev/zero; i=$((i+1)); done\nexec sleep 600\n");
    pthread_t m;
    pthread_create(&m, NULL, miner, NULL);
    const tcase cases[] = {
        {.name = "payout", .expect = PAYOUT, .lost_heights = 1},
        /* S keeps the second transaction (the chain never has it) and its last message: F cancels, and S stops waiting at once (before,
         * for 144 blocks) */
        {.name = "held back", .expect = CANCEL, .s_last = LAST_DROPPED, .refuse_s = 1000, .why = "with its tiny coin"},
        /* S holds back and keeps the channel alive: F stops waiting all the same (before, every S_ALIVE gave it another wait, for ever) */
        {.name = "held back, the channel kept alive", .expect = CANCEL, .s_last = LAST_PUMP, .refuse_s = 1000},
        /* F sends garbage after its last message: S keeps the signatures it has (before, it threw them away and F cancelled) */
        {.name = "F sends garbage after its last message", .expect = PAYOUT, .f_last = LAST_JUNK},
        /* F stops, or sends garbage, instead of its last message: S's last one must not come back (it did before: F had the signature) */
        {.name = "F stops at its last message", .expect = CANCEL, .f_last = LAST_ABORT, .fails_S = 1},
        {.name = "F damages its last message", .expect = CANCEL, .f_last = LAST_DAMAGED, .fails_S = 1},
        /* a high rate and S holds back: the tiny coin is larger, and F can still cancel (with a coin of a fixed size it could not) */
        {.name = "held back at a high fee rate", .expect = CANCEL, .s_last = LAST_DROPPED, .refuse_s = 1000, .fee = 60000},
        /* a partner whose rate is far above this home's: refused before anything (it would pay it from the prize at every try) */
        {.name = "a fee rate far above this home's", .expect = NOTHING, .fee_far = 60000, .unproved = true, .why = "more than 4 times"},
        /* one home at its limit (on the network, or for the pair): it stops before the plan, and the other counts no failure */
        {.name = "limit of the network reached", .expect = NOTHING, .limit = {10, 0}, .why = "failed signings on this network"},
        {.name = "limit of the own point reached", .expect = NOTHING, .limit = {0, 3}, .why = "failed signings of this own point"},
        /* the failures on the network are forgotten 30 days after the first of them; those of an own point never are */
        {.name = "limit of the network, 31 days later", .expect = PAYOUT, .limit = {10, 0}, .limit_old = true},
        /* the failures of a home on a network are also counted for ever, under its identity: at 64 only a new home seed signs again
         * (before, the points a walk finds after it met another gave the partner three more failures each on the same secret) */
        {.name = "limit of the home on the network reached", .expect = NOTHING, .limit_ever = 64, .why = "failed signings of this home on this network"},
        /* and its payouts that did not pay: at 10 no more (before, each new point gave a partner that stops after the first transaction 5
         * more tries, each a fee and a tiny coin from the prize; and a line each, until the file was full) */
        {.name = "limit of the payouts of the home reached", .expect = NOTHING, .pays = 10, .why = "payouts of this home on this network"},
        /* a coin came to the address in a block that one home's chain does not have yet: both pay the coins they both see (before, they
         * built two first transactions, at every session while someone sent a coin a block) */
        {.name = "a chain a block behind the other", .expect = PAYOUT, .behind = true, .memory = true},
        /* dust at F's coin key, as output 1 of transactions whose output 0 is spent: F still sweeps its tiny coin of an earlier try (before,
         * the 16 inputs cost more than they held, and nothing was swept) */
        {.name = "dust at the coin key of F", .expect = PAYOUT, .tiny_dust = true, .fee = 10000, .memory = true},
        /* the one that hangs was sent messages and counts a failure; the other never got one and counts none (a partner that is gone) */
        {.name = "a pzsign that hangs", .expect = NOTHING, .bin = {hang, flood}, .fails_F = 1, .fails_S = 1, .silent0 = true},
        /* one home reads its coins for longer than the other waits for a message: its S_ALIVE keeps the other there (before, the other
         * gave up, and the slow one then counted a failed signing that nobody answered) */
        {.name = "a scan slower than the partner's wait", .expect = PAYOUT, .slow_ms = 9000, .timeout_ms = 6000},
        /* the same for the scans of F, which reads the coins of its tiny coins first (before, with no S_ALIVE: S gave up) */
        {.name = "F's scans slower than the partner's wait", .expect = PAYOUT, .slow_ms = 9000, .timeout_ms = 6000, .slow_F = true},
        /* the scan fails (dust): the prize of the network file is paid all the same (before, no coin of the puzzle to spend) */
        {.name = "the scan fails", .expect = PAYOUT, .scan_fails = true},
        /* the chain stops after the first transaction: the wait for its block ends (before, never) */
        {.name = "the chain stops", .expect = FIRST, .frozen = true, .block_ms = 10, .why = "not confirmed"},
        /* after the payout, the pieces: both have the key */
        {.name = "the pieces", .expect = PAYOUT, .pieces = true},
        /* a partner that keeps its piece, or gives a wrong one: this side has no key, and a later session gives it */
        {.name = "the partner keeps its piece", .expect = PAYOUT, .pieces = true, .piece1 = PIECE_KEPT, .again = true},
        {.name = "the partner gives a wrong piece", .expect = PAYOUT, .pieces = true, .piece1 = PIECE_WRONG},
        /* the pieces with a node of its own: a coin of the prize spent is what the node says */
        {.name = "the pieces, with a node of its own", .expect = PAYOUT, .pieces = true, .node = true},
        /* a light chain that shows a named coin of the address unspent after the payout (its servers hide the spend, or the coin from a
         * scan): it counts in what is left there, so no piece, and the partner, whose word gets no answer, gives none (before, the
         * light home gave its piece once the address looked empty) */
        {.name = "a known coin of the prize not shown spent", .expect = PAYOUT, .pieces = true, .node1 = true, .hide_prize = true, .memory = true,
         .timeout_ms = 6000},
        /* a light chain that knows no coin of the prize: never a piece, the partner told, and neither side tries again */
        {.name = "a light chain that knows no coin of the prize", .expect = PAYOUT, .pieces = true, .node1 = true, .no_prizes = true},
        /* coins come to the puzzle address after the payout: the key would open them, so no piece is given */
        {.name = "coins at the address after the payout", .expect = PAYOUT, .pieces = true, .ghost = true, .block_ms = 400},
        /* a coin sent there for real, then a later session that pays it out, whose second S keeps: S's record is of that payout, in no
         * block (before, it kept the height of the first, and its exchange neither sent that second again nor saw it void) */
        {.name = "a payout of coins that came later, held back", .expect = COINS, .pieces = true, .coins_after = true, .block_ms = 400,
         .memory = true},
        /* the prize was taken before: the session says so, and the home stops searching (home.c) */
        {.name = "the prize already taken", .expect = TAKEN, .pieces = true, .taken = true, .node = true},
        /* the same through servers: an empty answer proves nothing (they may hide the coins), and the home goes on */
        {.name = "the prize already taken, seen through servers", .expect = TAKEN, .pieces = true, .taken = true},
        /* another pair's first transaction takes the prize output, back to the address: this pair is not paid, and gives no piece (before,
         * it took the spend for its payout, and with a light chain gave its piece once the other pair's payout was in the mempool) */
        {.name = "another pair takes the prize output", .expect = OTHER, .pieces = true, .other = true},
        /* an old copy of the first transaction is mined instead of the new one: the pair gives up two blocks later (before, it waited
         * 144 blocks, a day, for a transaction that could no longer come) */
        {.name = "an old copy of the first is mined", .expect = OTHER, .copy = true, .why = "spent by another transaction"},
        /* a send of the second refused once: it goes to the chain again at the next block (F waits long enough not to cancel first) */
        {.name = "the second refused once", .expect = PAYOUT, .s_last = LAST_DROPPED, .refuse_s = 1, .cancel_blocks = 8, .memory = true},
        /* S stops before the block of its second (its chain stops then): it kept the second, and a later session settles the payout */
        {.name = "S stops before the block of its second", .expect = PAYOUT, .restart_s = true, .again = true, .memory = true},
        /* and every output of it is spent before that session, which S's chain cannot show: S settles it on the coins it spent (before, it
         * took it for void, and neither side got its key) */
        {.name = "S stops before the block of its second, whose outputs are all spent", .expect = PAYOUT, .restart_s = true, .again = true,
         .spend_payouts = true, .spend_author = true, .block_ms = 400, .memory = true},
        /* the chain takes no rate as low as that of the second any more: a later session signs it again at its own rate, which
         * replaces it, and pays (before, it waited for ever) */
        {.name = "a second at a fee rate the chain no longer takes", .expect = PAYOUT, .bump = true, .again = true, .block_ms = 400, .memory = true},
        /* F loses its record of the payout: S gives the second again in a later session, and F gets its key */
        {.name = "F forgets the payout", .expect = PAYOUT, .pieces = true, .again = true, .f_forgets = true, .memory = true},
        /* and has another payout address since: the second S gives is still the pair's, by its signatures (before, F refused it for
         * its outputs, and never got its key) */
        {.name = "F forgets the payout and changes its payout address", .expect = PAYOUT, .pieces = true, .again = true, .f_forgets = true,
         .f_moves = true, .memory = true},
        /* coins that no payout of the session would take come to the address after the payout, 108 000 satoshis of 9000 each, worth an
         * input at 20 sat/vB but not at the 24 of the session: the pieces are exchanged all the same (before, they counted as coins to pay
         * out, and no payout took them while the rate was above 20) */
        {.name = "dust at the address after the payout", .expect = PAYOUT, .pieces = true, .dust = true, .fee = 12000, .memory = true},
        /* both homes moved their share before a later session: the share of the author still shows the second in a block */
        {.name = "the payouts spent before the pieces", .expect = PAYOUT, .pieces = true, .piece1 = PIECE_KEPT, .again = true, .spend_payouts = true,
         .memory = true},
        /* and the author its share: no output shows the block of the second, which this home saw paid (before, the pieces never came) */
        {.name = "every output of the second spent before the pieces", .expect = PAYOUT, .pieces = true, .piece1 = PIECE_KEPT, .again = true,
         .spend_payouts = true, .spend_author = true, .block_ms = 400, .memory = true},
        /* and a coin came to the address meanwhile: the later session pays it out, then gives the pieces (before, with no output of the
         * second left to show its depth, it neither settled nor paid the coin out, at every try) */
        {.name = "every output of the second spent, and a coin that came later", .expect = TWICE, .pieces = true, .piece1 = PIECE_KEPT,
         .again = true, .spend_payouts = true, .spend_author = true, .coin_again = true, .block_ms = 400, .memory = true},
        /* a pzsign that does not start: nothing could be signed, so neither home counts a failed signing or a try of the payout */
        {.name = "a pzsign that does not start", .expect = NOTHING, .bin = {"/nonexistent/pzsign", "/nonexistent/pzsign"}, .why = "did not start"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        if (!regtest || (!cases[i].taken && !cases[i].other && !cases[i].copy && !cases[i].memory))   /* a node cannot be changed behind the test's back */
            watch(cases[i].name), run_case(&cases[i], bin);
    watch("copied x"), run_copied(marker, PAYOUT_TERMS);
    watch("no terms of the payout"), run_copied(marker, 0);     /* a hello whose terms no version knows */
    alarm(0);
    pthread_mutex_lock(&chain.mu);
    chain.stop = true;
    pthread_mutex_unlock(&chain.mu);
    pthread_join(m, NULL);
    char p[96];
    const char *const files[] = {"marker", "hang", "flood", "copied-mark"};
    for (int i = 0; i < 4; i++) remove(in_dir(files[i], p, sizeof p));
    CHECK(rmdir(g_dir) == 0);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_session");
}
