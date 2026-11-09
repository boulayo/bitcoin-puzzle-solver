/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_spv.c - session_chain on Electrum servers, with proofs (see chain_spv.h). Every operation opens a connection to each server, asks
 * all of them and keeps what is proven; the servers that do not answer are left out of that operation. */
#include "chain_spv.h"
#include "electrum.h"
#include <pzcore/hash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COINBASE_MATURITY 100
#define HISTORY_MAX 256            /* the newest transactions of a script that are looked at */
#define COINS_MAX 256

typedef struct {
    chain_spv *c;
    electrum e[CHAIN_SPV_SERVERS];
    bool up[CHAIN_SPV_SERVERS];
    int nup;
} servers;

/* ---- one server ---- */

static void put_hash(json_w *w, const uint8_t h[32])
{
    char hex[65];
    btc_hash_to_hex(h, hex);
    jw_str(w, hex);
}

/* the script hash of the protocol: SHA-256 of the script, shown reversed like a txid */
static void script_hash(const uint8_t *spk, size_t len, uint8_t sh[32]) { pz_sha256(spk, len, sh); }

/* The bytes of a transaction (malloc'd), accepted only when their hash is txid. A "transaction" of 64 bytes is refused: an inner node of
 * a Merkle tree is 64 bytes, and could be passed off as a leaf. */
static bool get_tx(electrum *e, const uint8_t txid[32], uint8_t **raw, size_t *len)
{
    json_w w;
    json_doc d;
    const json_val *res;
    uint8_t got[32];
    *raw = NULL;
    jw_init(&w);
    jw_arr(&w), put_hash(&w, txid), jw_end(&w);
    if (!electrum_call(e, "blockchain.transaction.get", &w, &d, &res)) return false;
    const char *hex = json_str(res);
    const size_t hl = hex ? strlen(hex) : 0;
    bool ok = hl >= 2 && hl % 2 == 0 && hl / 2 != 64 && (*raw = malloc(hl / 2)) != NULL;
    ok = ok && btc_unhex(hex, hl, *raw) && btc_txid(*raw, hl / 2, got) && !memcmp(got, txid, 32);
    json_free(&d);
    if (!ok) {
        free(*raw);
        *raw = NULL;
    }
    *len = hl / 2;
    return ok;
}

/* whether the server proves that txid is in the block at `height` of our chain of headers */
static bool get_proof(chain_spv *c, electrum *e, const uint8_t txid[32], uint64_t height)
{
    json_w w;
    json_doc d;
    const json_val *res;
    uint8_t branch[SPV_BRANCH_MAX][32];
    uint64_t pos;
    jw_init(&w);
    jw_arr(&w), put_hash(&w, txid), jw_u64(&w, height), jw_end(&w);
    if (!electrum_call(e, "blockchain.transaction.get_merkle", &w, &d, &res)) return false;
    const json_val *m = json_get(res, "merkle");
    bool ok = m && m->type == JSON_ARRAY && m->n <= SPV_BRANCH_MAX && json_u64(json_get(res, "pos"), &pos) && pos <= UINT32_MAX;
    for (uint32_t i = 0; ok && i < m->n; i++) ok = btc_hash_from_hex(json_str(json_at(m, i)), branch[i]);
    if (ok) {
        pthread_mutex_lock(&c->mu);
        ok = spv_proof(&c->headers, height, txid, (const uint8_t(*)[32])branch, m->n, (uint32_t)pos);
        pthread_mutex_unlock(&c->mu);
    }
    json_free(&d);
    return ok;
}

static bool tip_of(chain_spv *c, uint64_t *tip)
{
    pthread_mutex_lock(&c->mu);
    const bool ok = spv_tip(&c->headers, tip);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

/* the headers from `at` up to the height the server announced; false as soon as some do not fit our chain */
static bool fetch_headers(chain_spv *c, electrum *e, uint64_t at, uint64_t to)
{
    while (at <= to) {
        json_w w;
        json_doc d;
        const json_val *res;
        uint64_t count;
        jw_init(&w);
        jw_arr(&w), jw_u64(&w, at), jw_u64(&w, to - at + 1 < SPV_PERIOD ? to - at + 1 : SPV_PERIOD), jw_end(&w);
        if (!electrum_call(e, "blockchain.block.headers", &w, &d, &res)) return false;
        const char *hex = json_str(json_get(res, "hex"));
        bool ok = hex && json_u64(json_get(res, "count"), &count) && count >= 1 && count <= SPV_PERIOD && strlen(hex) == count * 2 * SPV_HEADER;
        uint8_t *h = ok ? malloc(count * SPV_HEADER) : NULL;
        ok = h && btc_unhex(hex, count * 2 * SPV_HEADER, h);
        if (ok) {                                              /* all of them, or else the valid ones up to the first that is not */
            const uint64_t now = (uint64_t)time(NULL);
            pthread_mutex_lock(&c->mu);
            ok = spv_add(&c->headers, at, h, count, now);
            for (uint64_t i = 0; !ok && i < count && spv_add(&c->headers, at + i, h + i * SPV_HEADER, 1, now); i++) {}
            pthread_mutex_unlock(&c->mu);
        }
        free(h);
        json_free(&d);
        if (!ok) return false;
        at += count;
    }
    return true;
}

/* Brings our headers up to the server's. When its chain does not continue ours it may have left it some blocks back: asked again from
 * further back, where spv_add takes its branch only with more work. */
static void sync_headers(chain_spv *c, electrum *e)
{
    json_doc d;
    const json_val *res;
    uint64_t to, tip;
    if (!electrum_call(e, "blockchain.headers.subscribe", NULL, &d, &res)) return;
    const bool ok = json_u64(json_get(res, "height"), &to);
    json_free(&d);
    if (!ok) return;
    if (!tip_of(c, &tip)) {
        (void)fetch_headers(c, e, c->headers.base, to);
        return;
    }
    if (to <= tip) return;
    static const uint64_t BACK[] = {0, 10, 100, 1000};
    for (size_t i = 0; i < sizeof BACK / sizeof *BACK; i++) {
        const uint64_t at = tip - c->headers.base > BACK[i] ? tip + 1 - BACK[i] : c->headers.base + 1;
        if (fetch_headers(c, e, at, to) || at == c->headers.base + 1) return;
    }
}

/* ---- all the servers ---- */

static void open_all(servers *s, chain_spv *c)
{
    s->c = c, s->nup = 0;
    uint64_t before = 0, after = 0;
    const bool had = tip_of(c, &before);
    for (int i = 0; i < c->nservers; i++) {
        s->up[i] = electrum_open(&s->e[i], c->servers[i], c->timeout_ms ? c->timeout_ms : 20000);
        if (!s->up[i]) continue;
        s->nup++;
        sync_headers(c, &s->e[i]);
    }
    if (c->headers_path && tip_of(c, &after) && (!had || after != before)) {
        pthread_mutex_lock(&c->mu);
        (void)spv_save(&c->headers, c->headers_path);
        pthread_mutex_unlock(&c->mu);
    }
}

static void close_all(servers *s)
{
    for (int i = 0; i < s->c->nservers; i++)
        if (s->up[i]) electrum_close(&s->e[i]);
}

static bool is_coinbase(const uint8_t *raw, size_t len)
{
    static const uint8_t ZERO[32];
    size_t o = 4;
    if (len > 6 && raw[4] == 0 && raw[5] == 1) o = 6;          /* the marker and flag of a transaction with witness */
    return len >= o + 1 + 36 && raw[o] == 1 && !memcmp(raw + o + 1, ZERO, 32);
}

/* The history of a script on one server, looked at for the output txid:vout, whose transaction is raw. *height: where txid is confirmed,
 * by proof. True when the server shows the output spent: by a confirmed transaction with its proof, or by one in the mempool whose inputs
 * are all outputs of raw and all validly signed (the second transaction of a session, complete).
 * Transactions with more inputs or outputs than a btc_tx holds are not looked into: ours are small. */
static bool scan_history(chain_spv *c, electrum *e, const uint8_t *spk, size_t spk_len, const uint8_t *raw, size_t rawlen,
                         const uint8_t txid[32], uint32_t vout, uint64_t *height)
{
    json_w w;
    json_doc d;
    const json_val *res;
    uint8_t sh[32];
    bool spent = false;
    script_hash(spk, spk_len, sh);
    jw_init(&w);
    jw_arr(&w), put_hash(&w, sh), jw_end(&w);
    if (!electrum_call(e, "blockchain.scripthash.get_history", &w, &d, &res)) return false;
    const uint32_t n = res->type == JSON_ARRAY ? res->n : 0;
    for (uint32_t i = n > HISTORY_MAX ? n - HISTORY_MAX : 0; i < n && !spent; i++) {
        const json_val *it = json_at(res, i);
        uint8_t id[32], *tr = NULL;
        size_t tl;
        int64_t h;
        if (!btc_hash_from_hex(json_str(json_get(it, "tx_hash")), id) || !json_i64(json_get(it, "height"), &h)) continue;
        if (!memcmp(id, txid, 32)) {
            if (h > 0 && get_proof(c, e, txid, (uint64_t)h)) *height = (uint64_t)h;
            continue;
        }
        btc_tx *tx = malloc(sizeof *tx);
        bool spends = tx && get_tx(e, id, &tr, &tl) && btc_tx_parse(tx, tr, tl), ours = spends;
        if (spends) {
            spends = false;
            for (size_t k = 0; k < tx->n_in; k++) {
                spends |= !memcmp(tx->in[k].prev_txid, txid, 32) && tx->in[k].prev_vout == vout;
                ours &= !memcmp(tx->in[k].prev_txid, txid, 32);
            }
        }
        if (spends && h > 0) spent = get_proof(c, e, id, (uint64_t)h);
        else if (spends && ours) {                             /* in the mempool: believed only when each signature is valid */
            spent = true;
            for (size_t k = 0; k < tx->n_in && spent; k++) {
                uint64_t value;
                const uint8_t *ps;
                size_t pl;
                spent = btc_prevout(raw, rawlen, txid, tx->in[k].prev_vout, &value, &ps, &pl) && btc_verify_p2pkh(tx, k, ps, pl);
            }
        }
        free(tr);
        free(tx);
    }
    json_free(&d);
    return spent;
}

typedef struct {
    payout_coin coin;
    unsigned listed;               /* a bit for each server that lists it */
} found_coin;

/* The proven, confirmed coins of a P2PKH script: the union of what the servers list. A coin some server does not list is dropped when that
 * server proves it spent. -1 when no server answered. */
static int gather(chain_spv *c, const uint8_t spk[BTC_P2PKH_LEN], found_coin *out, int max)
{
    servers s;
    uint8_t sh[32];
    uint64_t tip = 0;
    unsigned answered = 0;
    int n = 0;
    open_all(&s, c);
    script_hash(spk, BTC_P2PKH_LEN, sh);
    if (!tip_of(c, &tip)) s.nup = 0;
    for (int i = 0; i < c->nservers && s.nup; i++) {
        json_w w;
        json_doc d;
        const json_val *res;
        if (!s.up[i]) continue;
        jw_init(&w);
        jw_arr(&w), put_hash(&w, sh), jw_end(&w);
        if (!electrum_call(&s.e[i], "blockchain.scripthash.listunspent", &w, &d, &res)) continue;
        answered |= 1u << i;
        for (uint32_t k = 0; res->type == JSON_ARRAY && k < res->n; k++) {
            const json_val *it = json_at(res, k);
            uint8_t txid[32], *raw = NULL;
            const uint8_t *ps;
            uint64_t vout, height, value;
            size_t len, pl;
            int j = 0;
            if (!btc_hash_from_hex(json_str(json_get(it, "tx_hash")), txid) || !json_u64(json_get(it, "tx_pos"), &vout) ||
                !json_u64(json_get(it, "height"), &height) || height == 0 || height > tip || vout > UINT32_MAX)
                continue;                                      /* in the mempool, or above what we have verified */
            while (j < n && (memcmp(out[j].coin.txid, txid, 32) || out[j].coin.vout != vout)) j++;
            if (j < n) {
                out[j].listed |= 1u << i;
                continue;
            }
            if (n < max && get_tx(&s.e[i], txid, &raw, &len) && btc_prevout(raw, len, txid, (uint32_t)vout, &value, &ps, &pl) &&
                pl == BTC_P2PKH_LEN && !memcmp(ps, spk, pl) && tip - height + 1 >= (is_coinbase(raw, len) ? COINBASE_MATURITY : 1) &&
                get_proof(c, &s.e[i], txid, height)) {
                memcpy(out[n].coin.txid, txid, 32);
                out[n].coin.vout = (uint32_t)vout, out[n].coin.value = value, out[n].listed = 1u << i;
                n++;
            }
            free(raw);
        }
        json_free(&d);
    }
    for (int j = 0; j < n; j++)                                /* listed by some, not by others: spent, if one of those proves it */
        for (int i = 0; i < c->nservers; i++) {
            uint8_t *raw;
            size_t len;
            uint64_t h = 0;
            if (!(answered >> i & 1) || (out[j].listed >> i & 1) || !get_tx(&s.e[i], out[j].coin.txid, &raw, &len)) continue;
            const bool spent = scan_history(c, &s.e[i], spk, BTC_P2PKH_LEN, raw, len, out[j].coin.txid, out[j].coin.vout, &h);
            free(raw);
            if (spent) {
                out[j--] = out[--n];
                break;
            }
        }
    close_all(&s);
    return answered ? n : -1;
}

static int by_value(const void *a, const void *b)
{
    const payout_coin *x = &((const found_coin *)a)->coin, *y = &((const found_coin *)b)->coin;
    if (x->value != y->value) return x->value > y->value ? -1 : 1;
    const int c = memcmp(x->txid, y->txid, 32);
    return c ? c : (x->vout > y->vout) - (x->vout < y->vout);
}

static int coins(void *ctx, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    found_coin *f = malloc(COINS_MAX * sizeof *f);
    if (!f) return -1;
    int n = gather(ctx, spk, f, COINS_MAX);
    if (n > 0) qsort(f, (size_t)n, sizeof *f, by_value);       /* the largest first, as payout_first takes them */
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = f[i].coin;
    free(f);
    return n;
}

static int coin(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    found_coin *f = malloc(COINS_MAX * sizeof *f);
    if (!f) return -1;
    const int n = gather(ctx, spk, f, COINS_MAX);
    int r = n < 0 ? -1 : 0;
    for (int i = 0; i < n; i++)
        if (!memcmp(f[i].coin.txid, txid, 32) && f[i].coin.vout == vout) *out = f[i].coin, r = 1;
    free(f);
    return r;
}

static bool broadcast(void *ctx, const btc_tx *tx)
{
    chain_spv *c = ctx;
    servers s;
    uint8_t raw[BTC_TX_MAX_SIZE], txid[32], *back;
    size_t bl;
    const size_t n = btc_tx_serialize(tx, raw, sizeof raw);
    bool ok = false;
    if (!n || !btc_tx_txid(tx, txid)) return false;
    open_all(&s, c);
    for (int i = 0; i < c->nservers; i++) {                    /* to every server: one that keeps it to itself changes nothing */
        json_w w;
        json_doc d;
        const json_val *res;
        if (!s.up[i]) continue;
        jw_init(&w);
        jw_arr(&w), jw_hex(&w, raw, n), jw_end(&w);
        if (electrum_call(&s.e[i], "blockchain.transaction.broadcast", &w, &d, &res)) json_free(&d);
        if (get_tx(&s.e[i], txid, &back, &bl)) {               /* taken now, or known already (sent by the partner): the server has it */
            ok = true;
            free(back);
        }
    }
    close_all(&s);
    return ok;
}

static int confirmations(void *ctx, const uint8_t txid[32], uint32_t vout)
{
    chain_spv *c = ctx;
    servers s;
    uint8_t *raw = NULL;
    const uint8_t *spk;
    size_t len = 0, spk_len;
    uint64_t value, tip, height = 0;
    bool spent = false;
    open_all(&s, c);
    for (int i = 0; i < c->nservers && !raw; i++)
        if (s.up[i]) (void)get_tx(&s.e[i], txid, &raw, &len);
    if (!raw || !tip_of(c, &tip) || !btc_prevout(raw, len, txid, vout, &value, &spk, &spk_len)) {
        free(raw);
        close_all(&s);
        return -2;                                             /* no server shows the transaction: that says nothing */
    }
    for (int i = 0; i < c->nservers && !spent; i++)
        if (s.up[i]) spent = scan_history(c, &s.e[i], spk, spk_len, raw, len, txid, vout, &height);
    free(raw);
    close_all(&s);
    return spent ? -1 : height && height <= tip ? (int)(tip - height + 1) : 0;
}

static uint64_t height(void *ctx)
{
    chain_spv *c = ctx;
    servers s;
    uint64_t tip = 0;
    open_all(&s, c);
    close_all(&s);
    (void)tip_of(c, &tip);
    return tip;
}

static int by_u64(const void *a, const void *b) { return (*(const uint64_t *)a > *(const uint64_t *)b) - (*(const uint64_t *)a < *(const uint64_t *)b); }

static uint64_t feerate(void *ctx)
{
    chain_spv *c = ctx;
    servers s;
    uint64_t f[CHAIN_SPV_SERVERS];
    int n = 0;
    open_all(&s, c);
    for (int i = 0; i < c->nservers; i++) {
        json_w w;
        json_doc d;
        const json_val *res;
        if (!s.up[i]) continue;
        jw_init(&w);
        jw_arr(&w), jw_u64(&w, 2), jw_end(&w);                 /* within 2 blocks (project 4.5) */
        if (!electrum_call(&s.e[i], "blockchain.estimatefee", &w, &d, &res)) continue;
        if (json_sats(res, &f[n]) && f[n] > 0) n++;            /* BTC per 1000 bytes: its satoshis are the rate; -1 means no estimate */
        json_free(&d);
    }
    close_all(&s);
    if (!n) return c->fallback_feerate;
    qsort(f, (size_t)n, sizeof *f, by_u64);
    return f[n / 2];                                           /* the median: one server cannot set the fee */
}

session_chain chain_spv_iface(chain_spv *c) { return (session_chain){c, coins, broadcast, confirmations, height, feerate, coin}; }

int chain_spv_ready(chain_spv *c, char *why, size_t cap)
{
    servers s;
    uint64_t tip;
    open_all(&s, c);
    close_all(&s);
    if (!s.nup) return snprintf(why, cap, "no Electrum server answers"), 0;
    if (!tip_of(c, &tip)) return snprintf(why, cap, "no server gave headers that start at the checkpoint"), 0;
    pthread_mutex_lock(&c->mu);
    const uint8_t *h = spv_header(&c->headers, tip);
    const uint32_t t = (uint32_t)h[68] | (uint32_t)h[69] << 8 | (uint32_t)h[70] << 16 | (uint32_t)h[71] << 24;
    pthread_mutex_unlock(&c->mu);
    const uint64_t now = (uint64_t)time(NULL);
    if (c->max_tip_age && now > t && now - t > c->max_tip_age)
        return snprintf(why, cap, "the last block the servers give (%llu) is %llu minutes old", (unsigned long long)tip, (unsigned long long)((now - t) / 60)), 0;
    snprintf(why, cap, "block %llu, %d server%s", (unsigned long long)tip, s.nup, s.nup == 1 ? "" : "s");
    return 1;
}

bool chain_spv_init(chain_spv *c, btc_chain chain, uint64_t cp_height, const uint8_t *cp_hash, const char *const *srv, int nservers,
                    const char *headers_path)
{
    uint8_t g[32];
    memset(c, 0, sizeof *c);
    if (nservers < 1 || nservers > CHAIN_SPV_SERVERS || (!cp_hash && cp_height)) return false;
    if (!cp_hash) spv_genesis(chain, g);
    if (!spv_init(&c->headers, chain, cp_height, cp_hash ? cp_hash : g)) return false;
    c->chain = chain, c->nservers = nservers, c->headers_path = headers_path;
    for (int i = 0; i < nservers; i++) c->servers[i] = srv[i];
    pthread_mutex_init(&c->mu, NULL);
    if (headers_path && !spv_load(&c->headers, headers_path, (uint64_t)time(NULL)) && c->headers.n == 0) spv_free(&c->headers);
    return true;
}

void chain_spv_free(chain_spv *c)
{
    spv_free(&c->headers);
    pthread_mutex_destroy(&c->mu);
}
