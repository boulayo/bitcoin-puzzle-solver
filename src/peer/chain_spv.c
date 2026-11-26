/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_spv.c - session_chain on Electrum servers, with proofs (see chain_spv.h). Every operation asks all the servers, one operation at a
 * time, on connections kept from one operation to the next, and keeps what is proven; each server has op_ms for its calls of an
 * operation, the servers that do not answer are left out of that operation, and one that takes no connection, or announces blocks it
 * cannot show, is left alone for ten minutes. */
#include "chain_spv.h"
#include "electrum.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define COINBASE_MATURITY 100
#define COINS_MAX 256
#define SPENDS_MAX 2048            /* the transactions whose inputs are kept: a txid names its bytes for ever */
#define SPENDS_KEEP 1024           /* inputs of one of them, at most: a larger one is looked into again every time */

#define RETRY_MS 600000            /* a server that did not answer is tried again after ten minutes */
#define OP_MS 60000                /* the calls of one server in an operation, all together (op_ms 0) */

/* the outpoints a transaction spends, 36 bytes each as it holds them (in NULL: an empty place) */
typedef struct spv_spends {
    uint8_t txid[32];
    size_t n;
    uint8_t *in;
} spends;

/* the servers of one operation: e is the connections of the chain, held until close_all */
typedef struct {
    chain_spv *c;
    electrum *e;
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
    ok = ok && pz_unhex(hex, hl, *raw) && btc_txid(*raw, hl / 2, got) && !memcmp(got, txid, 32);
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
    const json_val *b = m ? m + 1 : NULL;
    for (uint32_t i = 0; ok && i < m->n; i++, b = json_next(b)) ok = btc_hash_from_hex(json_str(b), branch[i]);
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
        ok = h && pz_unhex(hex, count * 2 * SPV_HEADER, h);
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
 * further back, where spv_add takes its branch only with more work. False when it announces blocks above ours and, answering in time,
 * gives none we keep: it lies, or is on another chain. */
static bool sync_headers(chain_spv *c, electrum *e)
{
    json_doc d;
    const json_val *res;
    uint64_t to, tip, grown;
    if (!electrum_call(e, "blockchain.headers.subscribe", NULL, &d, &res)) return true;   /* no answer: the connection is closed */
    const bool ok = json_u64(json_get(res, "height"), &to);
    json_free(&d);
    const bool had = tip_of(c, &tip);
    if (!ok || (had && to <= tip)) return true;
    if (!had) (void)fetch_headers(c, e, c->headers.base, to);
    static const uint64_t BACK[] = {0, 10, 100, 1000};
    for (size_t i = 0; had && i < sizeof BACK / sizeof *BACK; i++) {
        const uint64_t at = tip - c->headers.base > BACK[i] ? tip + 1 - BACK[i] : c->headers.base + 1;
        if (fetch_headers(c, e, at, to) || at == c->headers.base + 1) break;
    }
    return e->fd < 0 || e->budget < 0 || (tip_of(c, &grown) && (!had || grown > tip));
}

/* ---- all the servers ---- */

/* Takes the connections for one operation: those that are down and due are opened, and each brings the headers up to date. A connection
 * left from an earlier operation may have been closed by its server meanwhile: it is opened again once. Each server has op_ms for its
 * calls of the operation, whatever the others take: a slow one costs the operation its own time only. */
static void open_all(servers *s, chain_spv *c)
{
    pthread_mutex_lock(&c->io);
    s->c = c, s->e = c->conn, s->nup = 0;
    uint64_t before = 0, after = 0;
    const bool had = tip_of(c, &before);
    const int64_t now = sys_now_ms();
    for (int i = 0; i < c->nservers; i++) {
        for (int attempt = 0; attempt < 2; attempt++) {
            const bool kept = s->e[i].fd >= 0;
            if (!kept && now < c->retry[i]) break;
            if (!kept && !electrum_open(&s->e[i], c->servers[i], c->timeout_ms ? c->timeout_ms : 10000)) {
                c->retry[i] = now + RETRY_MS;
                break;
            }
            s->e[i].budget = c->op_ms ? c->op_ms : OP_MS;
            if (!sync_headers(c, &s->e[i])) {
                electrum_close(&s->e[i]), c->retry[i] = now + RETRY_MS;
                break;
            }
            if (s->e[i].fd >= 0 || !kept) break;
        }
        s->up[i] = s->e[i].fd >= 0;
        s->nup += s->up[i];
    }
    pthread_mutex_lock(&c->mu);
    c->synced = sys_now_ms();
    if (c->headers_path && spv_tip(&c->headers, &after) && (!had || after != before)) (void)spv_save(&c->headers, c->headers_path);
    pthread_mutex_unlock(&c->mu);
}

static void close_all(servers *s)
{
    for (int i = 0; i < s->c->nservers; i++) s->e[i].budget = 0;
    pthread_mutex_unlock(&s->c->io);
}

static bool is_coinbase(const uint8_t *raw, size_t len)
{
    static const uint8_t ZERO[32];
    size_t o = 4;
    if (len > 6 && raw[4] == 0 && raw[5] == 1) o = 6;          /* the marker and flag of a transaction with witness */
    return len >= o + 1 + 36 && raw[o] == 1 && !memcmp(raw + o + 1, ZERO, 32);
}

/* Whether transaction id spends the outpoint op (36 bytes, as a transaction holds it). What id spends is kept from one call to the next
 * (io held), else it comes from the server, whose bytes count only when their hash is id (false when it does not give them): every
 * input of it, however many, so that no transaction of any shape hides a spend. */
static bool spends_it(chain_spv *c, electrum *e, const uint8_t id[32], const uint8_t op[36])
{
    if (!c->spends && !(c->spends = calloc(SPENDS_MAX, sizeof *c->spends))) return false;
    spends *sp = &c->spends[(id[0] | (size_t)id[1] << 8) % SPENDS_MAX];
    uint8_t *raw, *in = sp->in;
    size_t len, n = sp->n;
    const bool kept = in && !memcmp(sp->txid, id, 32);
    if (!kept) {
        if (!get_tx(e, id, &raw, &len)) return false;
        in = btc_tx_outpoints(raw, len, &n);
        free(raw);
        if (in && n <= SPENDS_KEEP) free(sp->in), memcpy(sp->txid, id, 32), sp->n = n, sp->in = in;
    }
    bool spent = false;
    for (size_t k = 0; in && k < n && !spent; k++) spent = !memcmp(in + 36 * k, op, 36);
    if (in != sp->in) free(in);
    return spent;
}

/* The history of a script on one server, looked at for the output txid:vout. *height: where txid is confirmed, by proof. True when the
 * server shows the output spent by a confirmed transaction, with its proof. A spend in the mempool counts for nothing: one server could
 * show a transaction that was never sent (the second transaction a partner holds back), and F would not cancel. Every confirmed
 * transaction at or above txid's height is looked into, oldest first, as far as the server's time in the operation goes; what each
 * spends is kept (spends_it), so the next look goes further: dust sent to the address cannot hide the spend. */
static bool scan_history(chain_spv *c, electrum *e, const uint8_t *spk, size_t spk_len, const uint8_t txid[32], uint32_t vout, uint64_t *height)
{
    json_w w;
    json_doc d;
    const json_val *res;
    uint8_t sh[32];
    bool spent = false;
    int64_t at = 0;                                            /* the height of txid in this history */
    uint8_t op[36];
    memcpy(op, txid, 32);
    for (int b = 0; b < 4; b++) op[32 + b] = (uint8_t)(vout >> (8 * b));
    script_hash(spk, spk_len, sh);
    jw_init(&w);
    jw_arr(&w), put_hash(&w, sh), jw_end(&w);
    if (!electrum_call(e, "blockchain.scripthash.get_history", &w, &d, &res)) return false;
    const uint32_t n = res->type == JSON_ARRAY ? res->n : 0;
    const json_val *it = res + 1;
    for (uint32_t i = 0; i < n && !at; i++, it = json_next(it)) {   /* txid itself, once: a server could list it a thousand times */
        uint8_t id[32];
        int64_t h;
        if (btc_hash_from_hex(json_str(json_get(it, "tx_hash")), id) && !memcmp(id, txid, 32) && json_i64(json_get(it, "height"), &h) && h > 0) {
            at = h;
            if (get_proof(c, e, txid, (uint64_t)h)) *height = (uint64_t)h;
        }
    }
    it = res + 1;
    for (uint32_t i = 0; at && i < n && !spent; i++, it = json_next(it)) {
        uint8_t id[32];
        int64_t h;
        if (!btc_hash_from_hex(json_str(json_get(it, "tx_hash")), id) || !json_i64(json_get(it, "height"), &h) || h < at || !memcmp(id, txid, 32))
            continue;
        spent = spends_it(c, e, id, op) && get_proof(c, e, id, (uint64_t)h);
    }
    json_free(&d);
    return spent;
}

typedef struct {
    payout_coin coin;
    unsigned listed;               /* a bit for each server that lists it */
} found_coin;

typedef struct {
    uint8_t txid[32];
    uint32_t vout;
    uint64_t height, value;        /* the value the server claims */
} listed;

static int by_claimed_value(const void *a, const void *b)
{
    const listed *x = a, *y = b;
    return (x->value < y->value) - (x->value > y->value);
}

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
        /* what the server lists, the largest first by what it says (proven below): dust sent to the address cannot push a prize out */
        const uint32_t cnt = res->type == JSON_ARRAY ? res->n : 0;
        listed *l = cnt ? malloc(cnt * sizeof *l) : NULL;
        uint32_t m = 0;
        const json_val *it = res + 1;
        for (uint32_t k = 0; l && k < cnt; k++, it = json_next(it)) {
            uint64_t vout, height;
            if (btc_hash_from_hex(json_str(json_get(it, "tx_hash")), l[m].txid) && json_u64(json_get(it, "tx_pos"), &vout) &&
                json_u64(json_get(it, "height"), &height) && json_u64(json_get(it, "value"), &l[m].value) && height && height <= tip &&
                vout <= UINT32_MAX)
                l[m].vout = (uint32_t)vout, l[m].height = height, m++;   /* not in the mempool, nor above what we have verified */
        }
        if (m) qsort(l, m, sizeof *l, by_claimed_value);
        for (uint32_t k = 0; k < m && k < (uint32_t)max; k++) {
            uint8_t *raw = NULL;
            const uint8_t *ps;
            uint64_t value;
            size_t len, pl;
            int j = 0, low = 0;
            while (j < n && (memcmp(out[j].coin.txid, l[k].txid, 32) || out[j].coin.vout != l[k].vout)) j++;
            if (j < n) {
                out[j].listed |= 1u << i;
                continue;
            }
            for (int q = 1; q < n; q++) low = out[q].coin.value < out[low].coin.value ? q : low;
            if (n == max && l[k].value <= out[low].coin.value) continue;   /* full of larger coins */
            if (get_tx(&s.e[i], l[k].txid, &raw, &len) && btc_prevout(raw, len, l[k].txid, l[k].vout, &value, &ps, &pl) &&
                pl == BTC_P2PKH_LEN && !memcmp(ps, spk, pl) && tip - l[k].height + 1 >= (is_coinbase(raw, len) ? COINBASE_MATURITY : 1) &&
                get_proof(c, &s.e[i], l[k].txid, l[k].height) && (n < max || value > out[low].coin.value)) {
                const int at = n < max ? n++ : low;
                memcpy(out[at].coin.txid, l[k].txid, 32);
                out[at].coin.vout = l[k].vout, out[at].coin.value = value, out[at].listed = 1u << i;
            }
            free(raw);
        }
        free(l);
        json_free(&d);
    }
    for (int j = 0; j < n; j++)                                /* listed by some, not by others: spent, if one of those proves it */
        for (int i = 0; i < c->nservers; i++) {
            uint64_t h = 0;
            if (!(answered >> i & 1) || (out[j].listed >> i & 1)) continue;
            if (scan_history(c, &s.e[i], spk, BTC_P2PKH_LEN, out[j].coin.txid, out[j].coin.vout, &h)) {
                out[j--] = out[--n];
                break;
            }
        }
    close_all(&s);
    return answered ? n : -1;
}


static int coins(void *ctx, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    found_coin *f = malloc(COINS_MAX * sizeof *f);
    if (!f) return -1;
    int n = gather(ctx, spk, f, COINS_MAX);
    if (n > 0) qsort(f, (size_t)n, sizeof *f, payout_by_value);   /* the largest first, as payout_first takes them (the coin begins f) */
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = f[i].coin;
    free(f);
    return n;
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

/* Output txid:vout: -1 spent by a confirmed transaction, with its proof; -2 when no server gives the transaction (its bytes count only with
 * the hash of txid); else its confirmations, 0 when no kept header proves its block (the mempool, or below the checkpoint). With spk,
 * *pays tells whether it pays that P2PKH script, and *value is its value */
static int look(chain_spv *c, const uint8_t txid[32], uint32_t vout, const uint8_t *spk, bool *pays, uint64_t *value)
{
    servers s;
    uint8_t *raw = NULL;
    const uint8_t *out;
    size_t len = 0, out_len;
    uint64_t v, tip, height = 0;
    bool spent = false;
    open_all(&s, c);
    for (int i = 0; i < c->nservers && !raw; i++)
        if (s.up[i]) (void)get_tx(&s.e[i], txid, &raw, &len);
    if (!raw || !tip_of(c, &tip) || !btc_prevout(raw, len, txid, vout, &v, &out, &out_len)) {
        free(raw);
        close_all(&s);
        return -2;                                             /* no server shows the transaction: that says nothing */
    }
    if (spk) *pays = out_len == BTC_P2PKH_LEN && !memcmp(out, spk, BTC_P2PKH_LEN), *value = v;
    for (int i = 0; i < c->nservers && !spent; i++)
        if (s.up[i]) spent = scan_history(c, &s.e[i], out, out_len, txid, vout, &height);
    free(raw);
    close_all(&s);
    return spent ? -1 : height && height <= tip ? (int)(tip - height + 1) : 0;
}

static int confirmations(void *ctx, const uint8_t txid[32], uint32_t vout) { return look(ctx, txid, vout, NULL, NULL, NULL); }

/* A coin of the prize (prize= of the network file, or of the program for its networks), looked up alone: the creator's coins are older than
 * the checkpoint, where no kept header proves a block, so no scan of the address shows them. The txid is the file's or the program's, not
 * a server's: the bytes hashed to it are the chain's, value and script. 1 when it pays the script and no server proves it spent; a
 * server that hides a spend only makes the first transaction spend a coin that is gone, which the network refuses: a delay. 0 when it is
 * spent or pays another script, -1 when no server gives the transaction */
static int coin(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    bool pays = false;
    uint64_t value = 0;
    const int k = look(ctx, txid, vout, spk, &pays, &value);
    if (k == -2) return -1;
    if (k == -1 || !pays) return 0;
    memcpy(out->txid, txid, 32), out->vout = vout, out->value = value;
    return 1;
}

/* the tip of our headers, the servers asked when tip_ms has gone by since they were last (sessions poll it, each every few seconds) */
static uint64_t height(void *ctx)
{
    chain_spv *c = ctx;
    uint64_t tip = 0;
    pthread_mutex_lock(&c->mu);
    const bool fresh = c->tip_ms && c->synced && sys_now_ms() - c->synced < c->tip_ms;
    pthread_mutex_unlock(&c->mu);
    if (!fresh) {
        servers s;
        open_all(&s, c);
        close_all(&s);
    }
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
    return f[(n - 1) / 2];                                     /* the lower median: servers that raise the fee win only as a majority */
}

/* The spends of a script waiting in the mempool: a transaction some server lists there and gives (its bytes hashed to its id), with an
 * input validly signed for the script, which only its key makes. A spend takes its coin out of listunspent, so a coin about to be spent
 * looks gone already; what is only sent to the script counts for nothing (a node's scan does not show it either), nor does an id a server
 * lists and does not give. When no server could look into all it lists (no time left), 1: it cannot be told. -1 when none answered. */
static int pending(void *ctx, const uint8_t spk[BTC_P2PKH_LEN])
{
    chain_spv *c = ctx;
    servers s;
    uint8_t sh[32];
    int found = 0, answered = 0, whole = 0;
    btc_tx *tx = malloc(sizeof *tx);
    if (!tx) return -1;
    open_all(&s, c);
    script_hash(spk, BTC_P2PKH_LEN, sh);
    for (int i = 0; i < c->nservers; i++) {
        json_w w;
        json_doc d;
        const json_val *res;
        if (!s.up[i]) continue;
        jw_init(&w);
        jw_arr(&w), put_hash(&w, sh), jw_end(&w);
        if (!electrum_call(&s.e[i], "blockchain.scripthash.get_mempool", &w, &d, &res)) continue;
        bool all = res->type == JSON_ARRAY;
        const json_val *it = res + 1;
        for (uint32_t j = 0; all && j < res->n; j++, it = json_next(it)) {
            uint8_t id[32], *raw;
            size_t len;
            if (!btc_hash_from_hex(json_str(json_get(it, "tx_hash")), id)) continue;
            if (!get_tx(&s.e[i], id, &raw, &len)) {
                all = s.e[i].fd >= 0 && s.e[i].budget >= 0;    /* it did not give it, or there is no time left to ask */
                continue;
            }
            bool signed_for = false;
            const bool parsed = btc_tx_parse(tx, raw, len);
            for (size_t k = 0; parsed && k < tx->n_in && !signed_for; k++) signed_for = btc_verify_p2pkh(tx, k, spk, BTC_P2PKH_LEN);
            found += signed_for;
            free(raw);
        }
        answered++, whole += all;
        json_free(&d);
    }
    close_all(&s);
    free(tx);
    return !answered ? -1 : found ? found : !whole;
}

static int ready(void *ctx, char *why, size_t cap) { return chain_spv_ready(ctx, why, cap); }

session_chain chain_spv_iface(chain_spv *c) { return (session_chain){c, coins, broadcast, confirmations, height, feerate, coin, pending, ready}; }

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

int chain_spv_defaults(const char **out, uint64_t *cp_height, uint8_t cp_hash[32])
{
    static const char *const MAIN[] = {"electrum.blockstream.info:50001", "electrum.bitaroo.net:50001", "bitcoin.aranguren.org:50001",
                                       "bitcoin.lu.ke:50001",             "electrum.qtornado.com:50001", "e.keff.org:50001",
                                       "tardis.bauerj.eu:50001",          "xtrum.com:50001"};
    const int n = (int)(sizeof MAIN / sizeof *MAIN);
    for (int i = 0; out && i < n; i++) out[i] = MAIN[i];
    *cp_height = 969696;
    (void)btc_hash_from_hex("000000000000000000001e39df127cbab82a824ac43cfcdbf62e59f6a08b9f0b", cp_hash);
    return n;
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
    for (int i = 0; i < nservers; i++) c->servers[i] = srv[i], c->conn[i].fd = -1;
    pthread_mutex_init(&c->mu, NULL);
    pthread_mutex_init(&c->io, NULL);
    if (headers_path && !spv_load(&c->headers, headers_path, (uint64_t)time(NULL)) && c->headers.n == 0) spv_free(&c->headers);
    return true;
}

void chain_spv_free(chain_spv *c)
{
    for (int i = 0; i < c->nservers; i++) electrum_close(&c->conn[i]);
    for (int i = 0; c->spends && i < SPENDS_MAX; i++) free(c->spends[i].in);
    free(c->spends);
    spv_free(&c->headers);
    pthread_mutex_destroy(&c->mu);
    pthread_mutex_destroy(&c->io);
}
