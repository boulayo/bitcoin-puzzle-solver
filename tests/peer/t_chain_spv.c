/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_chain_spv.c - the chain of a session on Electrum servers, against servers written here that speak the protocol over a small chain made
 * in the test (blocks mined at the difficulty of regtest, transactions signed with btc.c). One server is honest; the others lie, each in
 * one way: they hide a coin, list one that is spent, give a proof that proves nothing, announce blocks without work, say a coin is spent
 * with a transaction that is not signed, keep a transaction to themselves, quote an absurd fee. With an honest server next to them the
 * answers are the true ones; alone, a liar gets a delay and nothing else. */
#include "peertest.h"
#include "chain_spv.h"
#include "json.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <secp256k1.h>
#include <stdatomic.h>
#include <time.h>

/* ---- the world: a chain and a mempool ---- */

typedef struct {
    uint8_t raw[600], txid[32];
    size_t len;
    int height;                    /* 0: in the mempool */
} wtx;

static struct {
    pthread_mutex_t mu;
    uint8_t hdr[64][80];
    int nb;
    wtx t[64];
    int nt;
    uint32_t t0;
} W = {.mu = PTHREAD_MUTEX_INITIALIZER};

static secp256k1_context *ctx;

static void spk_of(const char *label, uint8_t sec[32], uint8_t spk[BTC_P2PKH_LEN])
{
    secp256k1_pubkey p;
    uint8_t pub[33], h[20];
    size_t n = 33;
    pz_sha256((const uint8_t *)label, strlen(label), sec);
    CHECK(secp256k1_ec_pubkey_create(ctx, &p, sec) && secp256k1_ec_pubkey_serialize(ctx, pub, &n, &p, SECP256K1_EC_COMPRESSED));
    pz_hash160(pub, 33, h);
    btc_script_p2pkh(h, spk);
}

/* a transaction of the world, in the mempool; its index */
static int add_tx(const btc_tx *tx)
{
    wtx *t = &W.t[W.nt];
    t->len = btc_tx_serialize(tx, t->raw, sizeof t->raw);
    CHECK(t->len && btc_tx_txid(tx, t->txid));
    t->height = 0;
    return W.nt++;
}

static void merkle_root(uint8_t (*ids)[32], size_t n, size_t pos, uint8_t root[32], uint8_t (*branch)[32], size_t *depth)
{
    uint8_t cat[64];
    *depth = 0;
    while (n > 1) {
        if (n & 1) memcpy(ids[n], ids[n - 1], 32), n++;
        if (branch) memcpy(branch[(*depth)++], ids[pos ^ 1], 32);
        for (size_t i = 0; i < n; i += 2) {
            memcpy(cat, ids[i], 32), memcpy(cat + 32, ids[i + 1], 32);
            pz_sha256d(cat, 64, ids[i / 2]);
        }
        n /= 2, pos /= 2;
    }
    memcpy(root, ids[0], 32);
}

/* the ids of the transactions of a block, in their order; their number */
static size_t block_ids(int height, uint8_t (*ids)[32])
{
    size_t n = 0;
    for (int i = 0; i < W.nt; i++)
        if (W.t[i].height == height) memcpy(ids[n++], W.t[i].txid, 32);
    return n;
}

static void mine_header(uint8_t h[80])
{
    uint8_t target[32], hash[32], rev[32];
    CHECK(spv_target(0x207fffffu, target));
    for (uint32_t nonce = 0;; nonce++) {
        for (int b = 0; b < 4; b++) h[76 + b] = (uint8_t)(nonce >> (8 * b));
        pz_sha256d(h, 80, hash);
        for (int b = 0; b < 32; b++) rev[b] = hash[31 - b];
        if (memcmp(rev, target, 32) <= 0) return;
    }
}

/* a new block with the transactions of the mempool (and one of its own, so that it is never empty) */
static void mine_block(void)
{
    pthread_mutex_lock(&W.mu);
    btc_tx filler;
    uint8_t prev[32], ids[66][32], spk[BTC_P2PKH_LEN], sec[32], *h = W.hdr[W.nb];
    size_t depth;
    const int height = W.nb;
    btc_tx_init(&filler);
    pz_sha256((const uint8_t *)&height, sizeof height, prev);
    spk_of("filler", sec, spk);
    CHECK(btc_tx_add_in(&filler, prev, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&filler, 1000, spk, BTC_P2PKH_LEN));
    add_tx(&filler);
    if (height > 0)
        for (int i = 0; i < W.nt; i++)
            if (W.t[i].height == 0) W.t[i].height = height;
    if (height == 0) W.t[W.nt - 1].height = -1;                /* the first block: its transactions are never asked for */
    memset(h, 0, 80);
    h[0] = 2;
    if (height > 0) pz_sha256d(W.hdr[height - 1], 80, h + 4);
    merkle_root(ids, block_ids(height ? height : -1, ids), 0, h + 36, NULL, &depth);
    const uint32_t t = W.t0 + 600u * (uint32_t)height, bits = 0x207fffffu;
    for (int b = 0; b < 4; b++) h[68 + b] = (uint8_t)(t >> (8 * b)), h[72 + b] = (uint8_t)(bits >> (8 * b));
    mine_header(h);
    W.nb++;
    pthread_mutex_unlock(&W.mu);
}

/* ---- a server ---- */

typedef struct {
    bool hide_unspent;             /* lists no coin */
    bool list_spent;               /* lists the coins that are spent too, and shows no transaction that spends */
    bool bad_proof;                /* its Merkle branches lead nowhere */
    bool no_work;                  /* announces five blocks more, linked but without proof of work */
    bool drop_broadcast;           /* says it took a transaction and keeps it to itself */
    bool blind_mempool;            /* does not show the mempool */
    uint64_t fee;                  /* its estimate, satoshis per 1000 bytes */
} lies;

typedef struct {
    lies l;
    int lfd;
    uint16_t port;
    pthread_t th;
    atomic_bool stop;
    char addr[32];
    int broadcasts;
} server;

static bool tx_script(const wtx *t, size_t out, const uint8_t **spk, size_t *len)
{
    uint64_t v;
    return btc_prevout(t->raw, t->len, t->txid, (uint32_t)out, &v, spk, len);
}

static void sh_of(const uint8_t *spk, size_t len, char hex[65])
{
    uint8_t sh[32];
    pz_sha256(spk, len, sh);
    btc_hash_to_hex(sh, hex);
}

/* whether transaction i pays to, or spends from, the script hash; *spent_by: for output o of i, the transaction that spends it */
static bool touches(const server *s, int i, const char *sh, int spent_by[16])
{
    btc_tx tx;
    bool hit = false;
    if (!btc_tx_parse(&tx, W.t[i].raw, W.t[i].len)) return false;
    for (size_t o = 0; o < tx.n_out; o++) {
        char hex[65];
        sh_of(tx.out[o].script, tx.out[o].script_len, hex);
        hit |= !strcmp(hex, sh);
        if (spent_by) {
            spent_by[o] = -1;
            for (int j = 0; j < W.nt; j++) {
                btc_tx sp;
                if (j == i || (s->l.blind_mempool && W.t[j].height == 0) || !btc_tx_parse(&sp, W.t[j].raw, W.t[j].len)) continue;
                for (size_t k = 0; k < sp.n_in; k++)
                    if (!memcmp(sp.in[k].prev_txid, W.t[i].txid, 32) && sp.in[k].prev_vout == o) spent_by[o] = j;
            }
        }
    }
    for (size_t k = 0; k < tx.n_in; k++)
        for (int j = 0; j < W.nt; j++) {
            const uint8_t *spk;
            size_t len;
            char hex[65];
            if (memcmp(tx.in[k].prev_txid, W.t[j].txid, 32) || !tx_script(&W.t[j], tx.in[k].prev_vout, &spk, &len)) continue;
            sh_of(spk, len, hex);
            hit |= !strcmp(hex, sh);
        }
    return hit;
}

static int find_tx(const char *hex)
{
    uint8_t id[32];
    if (!btc_hash_from_hex(hex, id)) return -1;
    for (int i = 0; i < W.nt; i++)
        if (!memcmp(W.t[i].txid, id, 32)) return i;
    return -1;
}

static void hexcat(char *out, const uint8_t *p, size_t n) { btc_hex(p, n, out + strlen(out)); }

/* the result of a call, as JSON text in out; false: an error */
static bool answer(server *s, const char *method, const json_val *p, char *out, size_t cap)
{
    const char *a0 = json_str(json_at(p, 0));
    uint64_t u0 = 0, u1 = 0;
    (void)json_u64(json_at(p, 0), &u0), (void)json_u64(json_at(p, 1), &u1);
    const int top = W.nb - 1 + (s->l.no_work ? 5 : 0);
    out[0] = 0;
    if (!strcmp(method, "server.version")) return snprintf(out, cap, "[\"test\", \"1.4\"]"), true;
    if (!strcmp(method, "blockchain.headers.subscribe")) {
        snprintf(out, cap, "{\"height\": %d, \"hex\": \"", top);
        hexcat(out, W.hdr[W.nb - 1], 80);
        return strcat(out, "\"}"), true;
    }
    if (!strcmp(method, "blockchain.block.headers")) {
        int n = 0;
        snprintf(out, cap, "{\"hex\": \"");
        for (uint64_t h = u0; h < u0 + u1 && (int)h <= top; h++, n++) {
            uint8_t fake[80] = {2};
            if ((int)h < W.nb) hexcat(out, W.hdr[h], 80);
            else {                                             /* linked to the real tip, with its time and target, and no work */
                pz_sha256d(W.hdr[W.nb - 1], 80, fake + 4);
                memcpy(fake + 68, W.hdr[W.nb - 1] + 68, 8);
                fake[68] += (uint8_t)(h - (uint64_t)W.nb + 1);
                for (uint8_t hash[32];; fake[76]++) {          /* at this difficulty one hash in two would do: one that does not */
                    pz_sha256d(fake, 80, hash);
                    if (hash[31] >= 0x80) break;
                }
                hexcat(out, fake, 80);
            }
        }
        return snprintf(out + strlen(out), cap - strlen(out), "\", \"count\": %d, \"max\": 2016}", n), n > 0;
    }
    if (!strcmp(method, "blockchain.transaction.get")) {
        const int i = a0 ? find_tx(a0) : -1;
        if (i < 0 || (s->l.blind_mempool && W.t[i].height == 0)) return false;
        strcat(out, "\""), hexcat(out, W.t[i].raw, W.t[i].len), strcat(out, "\"");
        return true;
    }
    if (!strcmp(method, "blockchain.transaction.get_merkle")) {
        const int i = a0 ? find_tx(a0) : -1;
        uint8_t ids[66][32], root[32], branch[8][32];
        size_t depth, pos = 0;
        if (i < 0 || W.t[i].height <= 0 || (uint64_t)W.t[i].height != u1) return false;
        const size_t n = block_ids(W.t[i].height, ids);
        while (memcmp(ids[pos], W.t[i].txid, 32)) pos++;
        merkle_root(ids, n, pos, root, branch, &depth);
        snprintf(out, cap, "{\"block_height\": %d, \"pos\": %zu, \"merkle\": [", W.t[i].height, pos);
        for (size_t k = 0; k < depth; k++) {
            char hex[65];
            if (s->l.bad_proof) branch[k][3] ^= 1;
            btc_hash_to_hex(branch[k], hex);
            snprintf(out + strlen(out), cap - strlen(out), "%s\"%s\"", k ? ", " : "", hex);
        }
        return strcat(out, "]}"), true;
    }
    if (!strcmp(method, "blockchain.scripthash.listunspent") || !strcmp(method, "blockchain.scripthash.get_history")) {
        const bool unspent = method[22] == 'l';
        int n = 0;
        strcat(out, "[");
        for (int pass = 0; a0 && pass < 2; pass++)             /* the confirmed ones, then the mempool */
            for (int i = 0; i < W.nt; i++) {
                int by[16];
                char hex[65];
                btc_tx tx;
                if ((W.t[i].height > 0) == (pass == 1) || W.t[i].height < 0 || (s->l.blind_mempool && W.t[i].height == 0)) continue;
                if (!touches(s, i, a0, by) || !btc_tx_parse(&tx, W.t[i].raw, W.t[i].len)) continue;
                btc_hash_to_hex(W.t[i].txid, hex);
                if (!unspent) {
                    bool spends = false;                       /* a liar that lists spent coins shows no transaction that spends */
                    for (size_t k = 0; k < tx.n_in; k++)
                        for (int j = 0; j < W.nt; j++) spends |= !memcmp(tx.in[k].prev_txid, W.t[j].txid, 32);
                    if (s->l.list_spent && spends) continue;
                    snprintf(out + strlen(out), cap - strlen(out), "%s{\"tx_hash\": \"%s\", \"height\": %d}", n++ ? ", " : "", hex, W.t[i].height);
                    continue;
                }
                if (s->l.hide_unspent) continue;
                for (size_t o = 0; o < tx.n_out; o++) {
                    char sh[65];
                    sh_of(tx.out[o].script, tx.out[o].script_len, sh);
                    if (strcmp(sh, a0) || (by[o] >= 0 && !s->l.list_spent)) continue;
                    snprintf(out + strlen(out), cap - strlen(out), "%s{\"tx_hash\": \"%s\", \"tx_pos\": %zu, \"height\": %d, \"value\": %llu}",
                             n++ ? ", " : "", hex, o, W.t[i].height, (unsigned long long)tx.out[o].value);
                }
            }
        return strcat(out, "]"), true;
    }
    if (!strcmp(method, "blockchain.transaction.broadcast")) {
        btc_tx tx;
        uint8_t raw[600], id[32];
        char hex[65];
        const size_t hl = a0 ? strlen(a0) : 0;
        if (!hl || hl / 2 > sizeof raw || !btc_unhex(a0, hl, raw) || !btc_tx_parse(&tx, raw, hl / 2) || !btc_tx_txid(&tx, id)) return false;
        s->broadcasts++;
        btc_hash_to_hex(id, hex);
        if (!s->l.drop_broadcast && find_tx(hex) < 0) add_tx(&tx);
        return snprintf(out, cap, "\"%s\"", hex), true;
    }
    if (!strcmp(method, "blockchain.estimatefee")) return snprintf(out, cap, "%.8f", (double)s->l.fee / 1e8), true;
    return false;
}

static void *serve(void *arg)
{
    server *s = arg;
    static const size_t CAP = 1 << 20;
    char *in = malloc(CAP), *out = malloc(CAP), *msg = malloc(CAP + 200);
    while (!atomic_load(&s->stop)) {                           /* every wait in slices: the stop is looked at */
        const int fd = sys_wait(s->lfd, false, sys_now_ms() + 100) > 0 ? sys_accept(s->lfd, NULL, NULL) : -1;
        if (fd < 0) continue;
        size_t len = 0;
        while (!atomic_load(&s->stop)) {
            char *nl = memchr(in, '\n', len);
            if (!nl) {
                const long r = len < CAP ? sys_recv(fd, in + len, CAP - len, sys_now_ms() + 100) : 0;
                if (r == SYS_AGAIN) continue;
                if (r <= 0) break;
                len += (size_t)r;
                continue;
            }
            json_doc d;
            uint64_t id = 0;
            const bool parsed = json_parse(&d, in, (size_t)(nl - in));
            memmove(in, nl + 1, len - (size_t)(nl + 1 - in));
            len -= (size_t)(nl + 1 - in);
            if (!parsed) break;
            (void)json_u64(json_get(d.v, "id"), &id);
            pthread_mutex_lock(&W.mu);
            const bool ok = answer(s, json_str(json_get(d.v, "method")), json_get(d.v, "params"), out, CAP);
            pthread_mutex_unlock(&W.mu);
            if (ok) snprintf(msg, CAP + 200, "{\"jsonrpc\": \"2.0\", \"id\": %llu, \"result\": %s}\n", (unsigned long long)id, out);
            else snprintf(msg, CAP + 200, "{\"jsonrpc\": \"2.0\", \"id\": %llu, \"error\": {\"code\": 1, \"message\": \"no\"}}\n", (unsigned long long)id);
            json_free(&d);
            if (!sys_send_all(fd, msg, strlen(msg), sys_now_ms() + 5000)) break;
        }
        sys_close(fd);
    }
    free(in), free(out), free(msg);
    return NULL;
}

static void start(server *s, lies l)
{
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    memset(s, 0, sizeof *s);
    s->l = l;
    s->lfd = sys_socket(AF_INET, SOCK_STREAM);
    CHECK(s->lfd >= 0 && bind(s->lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(s->lfd, 8) == 0 &&
          getsockname(s->lfd, (struct sockaddr *)&a, &al) == 0);
    s->port = ntohs(a.sin_port);
    snprintf(s->addr, sizeof s->addr, "127.0.0.1:%u", s->port);
    pthread_create(&s->th, NULL, serve, s);
}

static void stop(server *s)
{
    atomic_store(&s->stop, true);
    pthread_join(s->th, NULL);
    sys_close(s->lfd);
}

/* the chain of a session on the given servers (at most 3) */
static void view(chain_spv *c, session_chain *ch, server *a, server *b, server *d)
{
    const char *srv[3];
    uint8_t cp[32];
    int n = 0;
    if (a) srv[n++] = a->addr;
    if (b) srv[n++] = b->addr;
    if (d) srv[n++] = d->addr;
    pz_sha256d(W.hdr[0], 80, cp);
    CHECK(chain_spv_init(c, BTC_REGTEST, 0, cp, srv, n, NULL));
    c->timeout_ms = 3000, c->fallback_feerate = 1234;
    *ch = chain_spv_iface(c);
}

int main(void)
{
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    W.t0 = (uint32_t)time(NULL) - 200000;
    uint8_t psec[32], pspk[BTC_P2PKH_LEN], asec[32], aspk[BTC_P2PKH_LEN], other[32];
    spk_of("puzzle", psec, pspk), spk_of("someone", asec, aspk);
    mine_block();                                              /* block 0, the checkpoint */
    /* the prize: 1 BTC to the puzzle script in block 1; a second coin in block 2, spent in block 3 */
    btc_tx fund, fund2, spend2;
    pz_sha256((const uint8_t *)"elsewhere", 9, other);
    btc_tx_init(&fund), btc_tx_init(&fund2), btc_tx_init(&spend2);
    CHECK(btc_tx_add_in(&fund, other, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&fund, 100000000, pspk, BTC_P2PKH_LEN));
    const int ifund = add_tx(&fund);
    mine_block();
    CHECK(btc_tx_add_in(&fund2, other, 1, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&fund2, 5000000, pspk, BTC_P2PKH_LEN));
    const int ifund2 = add_tx(&fund2);
    mine_block();
    CHECK(btc_tx_add_in(&spend2, W.t[ifund2].txid, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&spend2, 4990000, aspk, BTC_P2PKH_LEN) &&
          btc_sign_p2pkh(&spend2, 0, psec));
    add_tx(&spend2);
    mine_block();
    mine_block();                                              /* the tip is block 4 */

    server honest, hider, stale, noproof, nowork, keeper, blind;
    start(&honest, (lies){.fee = 3000});
    start(&hider, (lies){.hide_unspent = true, .fee = 2000});
    start(&stale, (lies){.list_spent = true, .fee = 900000000});
    start(&noproof, (lies){.bad_proof = true});
    start(&nowork, (lies){.no_work = true, .fee = 4000});
    start(&keeper, (lies){.drop_broadcast = true});
    start(&blind, (lies){.blind_mempool = true});

    chain_spv c;
    session_chain ch;
    payout_coin coins[8], one;
    char why[200];
    uint8_t fid[32], f2id[32];
    memcpy(fid, W.t[ifund].txid, 32), memcpy(f2id, W.t[ifund2].txid, 32);

    /* an honest server: the prize with its value, not the spent coin; the height; the fee */
    view(&c, &ch, &honest, NULL, NULL);
    CHECK(chain_spv_ready(&c, why, sizeof why) == 1 && ch.height(ch.ctx) == 4);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 1 && coins[0].value == 100000000 && !memcmp(coins[0].txid, fid, 32) && coins[0].vout == 0);
    CHECK(ch.coin(ch.ctx, fid, 0, pspk, &one) == 1 && one.value == 100000000 && ch.coin(ch.ctx, f2id, 0, pspk, &one) == 0);
    CHECK(ch.confirmations(ch.ctx, fid, 0) == 4 && ch.confirmations(ch.ctx, f2id, 0) == -1 && ch.confirmations(ch.ctx, other, 0) == -2);
    CHECK(ch.feerate(ch.ctx) == 3000);
    c.max_tip_age = 3600;                                      /* the last block is two days old */
    CHECK(chain_spv_ready(&c, why, sizeof why) == 0);
    chain_spv_free(&c);

    /* a server that hides the coins: alone it gives none (a delay); next to an honest one the prize is there */
    view(&c, &ch, &hider, NULL, NULL);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 0);
    chain_spv_free(&c);
    view(&c, &ch, &hider, &honest, NULL);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 1 && coins[0].value == 100000000);
    CHECK(ch.feerate(ch.ctx) == 3000);                        /* the median of 2000 and 3000 is the upper one */
    chain_spv_free(&c);

    /* a server that lists a spent coin and hides who spent it: alone it is believed (the first transaction would be refused by the
     * network: a delay); the honest one proves the spend, and the coin is dropped. Its absurd fee does not win the median of three */
    view(&c, &ch, &stale, NULL, NULL);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 2);
    chain_spv_free(&c);
    view(&c, &ch, &stale, &honest, &hider);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 1 && !memcmp(coins[0].txid, fid, 32));
    CHECK(ch.feerate(ch.ctx) == 3000);
    CHECK(ch.confirmations(ch.ctx, f2id, 0) == -1);
    chain_spv_free(&c);

    /* proofs that prove nothing: no coin, no confirmation */
    view(&c, &ch, &noproof, NULL, NULL);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 0 && ch.confirmations(ch.ctx, fid, 0) == 0 && ch.confirmations(ch.ctx, f2id, 0) == 0);
    chain_spv_free(&c);

    /* blocks without work: not taken, the chain ends where the work ends */
    view(&c, &ch, &nowork, NULL, NULL);
    CHECK(ch.height(ch.ctx) == 4);
    CHECK(ch.coins(ch.ctx, pspk, coins, 8) == 1);
    CHECK(ch.confirmations(ch.ctx, fid, 0) == 4);
    chain_spv_free(&c);

    /* no server at all */
    {
        const char *nobody[1] = {"127.0.0.1:1"};
        uint8_t cp[32];
        pz_sha256d(W.hdr[0], 80, cp);
        CHECK(chain_spv_init(&c, BTC_REGTEST, 0, cp, nobody, 1, NULL));
        c.timeout_ms = 500;
        ch = chain_spv_iface(&c);
        CHECK(chain_spv_ready(&c, why, sizeof why) == 0 && ch.coins(ch.ctx, pspk, coins, 8) == -1 && ch.confirmations(ch.ctx, fid, 0) == -2);
        chain_spv_free(&c);
    }

    /* the payout: a transaction that spends the prize. Sent through a server that keeps it: not sent. Through it and an honest one: sent */
    btc_tx pay, forged;
    btc_tx_init(&pay);
    CHECK(btc_tx_add_in(&pay, fid, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_out(&pay, 99990000, aspk, BTC_P2PKH_LEN));
    forged = pay;
    CHECK(btc_sign_p2pkh(&forged, 0, asec));                   /* signed by the wrong key */
    CHECK(btc_sign_p2pkh(&pay, 0, psec));
    view(&c, &ch, &keeper, NULL, NULL);
    CHECK(!ch.broadcast(ch.ctx, &pay) && keeper.broadcasts == 1);
    chain_spv_free(&c);
    /* a transaction that spends the prize without a valid signature, shown from a mempool: the prize has not moved */
    pthread_mutex_lock(&W.mu);
    const int iforged = add_tx(&forged);
    pthread_mutex_unlock(&W.mu);
    view(&c, &ch, &honest, NULL, NULL);
    CHECK(ch.confirmations(ch.ctx, fid, 0) == 4);
    pthread_mutex_lock(&W.mu);
    W.t[iforged].height = -1;                                  /* gone from the mempool */
    pthread_mutex_unlock(&W.mu);
    chain_spv_free(&c);
    view(&c, &ch, &keeper, &honest, NULL);
    CHECK(ch.broadcast(ch.ctx, &pay) && honest.broadcasts == 1 && ch.broadcast(ch.ctx, &pay));   /* and again: known already */
    /* in the mempool, validly signed: the prize has moved. A server that does not show its mempool says it has not: next to the honest
     * one, the spend is seen */
    CHECK(ch.confirmations(ch.ctx, fid, 0) == -1);
    chain_spv_free(&c);
    view(&c, &ch, &blind, NULL, NULL);
    CHECK(ch.confirmations(ch.ctx, fid, 0) == 4);
    chain_spv_free(&c);
    view(&c, &ch, &blind, &honest, NULL);
    CHECK(ch.confirmations(ch.ctx, fid, 0) == -1 && ch.coins(ch.ctx, pspk, coins, 8) == 0);
    /* confirmed: spent by proof; what it paid has one confirmation, then two; the headers grew by themselves */
    mine_block();
    uint8_t pid[32];
    CHECK(btc_tx_txid(&pay, pid));
    CHECK(ch.confirmations(ch.ctx, fid, 0) == -1 && ch.confirmations(ch.ctx, pid, 0) == 1);
    mine_block();
    CHECK(ch.height(ch.ctx) == 6 && ch.confirmations(ch.ctx, pid, 0) == 2);
    chain_spv_free(&c);

    stop(&honest), stop(&hider), stop(&stale), stop(&noproof), stop(&nowork), stop(&keeper), stop(&blind);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_chain_spv");
}
