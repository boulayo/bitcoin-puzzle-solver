/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* payout.c - the transactions of a pair (see payout.h). */
#include "payout.h"
#include <stdlib.h>
#include <string.h>

/* the terms by version (payout.h, docs/en/NETWORK.md 7.5) */
static const struct {
    unsigned pct;
    const char *address;
} AUTHOR[PAYOUT_TERMS] = {{10, "bc1qfw2y5xzvsy6w70rwqfq6n65tahk906678c7tvz"}};

bool payout_author(unsigned version, payout_terms *t)
{
    if (version < 1 || version > PAYOUT_TERMS) return false;
    t->author_pct = AUTHOR[version - 1].pct, t->author_address = AUTHOR[version - 1].address;
    t->author_len = btc_address_script(BTC_MAIN, AUTHOR[version - 1].address, t->author_spk);
    return t->author_len > 0;
}

static int by_value(const void *a, const void *b)
{
    const payout_coin *x = a, *y = b;
    if (x->value != y->value) return x->value > y->value ? -1 : 1;
    const int c = memcmp(x->txid, y->txid, 32);
    return c ? c : (x->vout > y->vout) - (x->vout < y->vout);
}

size_t payout_largest(payout_coin *coins, size_t n, size_t max, uint64_t least)
{
    size_t k = 0;
    qsort(coins, n, sizeof *coins, by_value);
    while (k < n && k < max && coins[k].value >= least) k++;
    return k;
}

/* the output of F's cancel and its fee */
static uint64_t cancel_out(const payout_terms *t, btc_out *out)
{
    *out = (btc_out){.script_len = (uint8_t)t->pay_len[t->first]};
    memcpy(out->script, t->pay_spk[t->first], t->pay_len[t->first]);
    return btc_fee(btc_vsize_p2pkh(1, out, 1), t->feerate);
}

/* The outputs of the second transaction from what it spends: of what the fee leaves, the share of the author at 2 and the rest half each,
 * the odd satoshi to S. False when one of them would be dust. */
static bool second_values(const payout_terms *t, uint64_t all, uint64_t v[3])
{
    btc_out out[3] = {{0}};
    for (int p = 0; p < 2; p++) out[p].script_len = (uint8_t)t->pay_len[p], memcpy(out[p].script, t->pay_spk[p], t->pay_len[p]);
    out[2].script_len = (uint8_t)t->author_len, memcpy(out[2].script, t->author_spk, t->author_len);
    const uint64_t fee = btc_fee(btc_vsize_p2pkh(2, out, 3), t->feerate);
    if (!t->author_len || t->author_pct > 50 || all < fee) return false;
    const uint64_t left = all - fee, cut = left * t->author_pct / 100, half = (left - cut) / 2;   /* left: below 2^51 */
    v[t->first] = half, v[1 - t->first] = left - cut - half, v[2] = cut;
    return half >= 546 && cut >= 546;
}

uint64_t payout_tiny(const payout_terms *t)
{
    btc_out out;
    const uint64_t need = 2 * cancel_out(t, &out) + 546;
    return need > PAYOUT_TINY_MIN ? need : PAYOUT_TINY_MIN;
}

bool payout_first(const payout_terms *t, const payout_coin *coins, size_t n, btc_tx *tx, size_t *used)
{
    payout_coin take[64];
    if (t->first != 0 && t->first != 1) return false;
    const uint64_t tiny = payout_tiny(t);
    if (n > 64) n = 64;
    memcpy(take, coins, n * sizeof *coins);
    const size_t worth = payout_largest(take, n, PAYOUT_MAX_COINS, 3 * btc_fee(BTC_P2PKH_IN_SIZE, t->feerate));
    uint64_t total = 0;
    btc_tx_init(tx);
    for (size_t i = 0; i < worth; i++) {
        if (total + take[i].value > BTC_MAX_MONEY || !btc_tx_add_in(tx, take[i].txid, take[i].vout, BTC_SEQUENCE_FINAL)) return false;
        total += take[i].value;
    }
    *used = tx->n_in;
    btc_out out[2] = {{0}};                                     /* the outputs as they will be, for the size */
    for (int k = 0; k < 2; k++) {
        out[k].script_len = BTC_P2PKH_LEN;
        memcpy(out[k].script, k ? t->coin_spk[t->first] : t->puzzle_spk, BTC_P2PKH_LEN);
    }
    const uint64_t fee = btc_fee(btc_vsize_p2pkh(tx->n_in, out, 2), t->feerate);
    uint64_t v[3];                                              /* the prize must stay larger than a tiny coin, and pay the second */
    if (!tx->n_in || total < fee + 2 * tiny || !second_values(t, total - fee, v)) return false;
    return btc_tx_add_out(tx, total - fee - tiny, t->puzzle_spk, BTC_P2PKH_LEN) &&
           btc_tx_add_out(tx, tiny, t->coin_spk[t->first], BTC_P2PKH_LEN);
}

/* the outputs of the first transaction: the prize at 0, the tiny coin of F at 1 */
static bool first_ok(const payout_terms *t, const btc_tx *first)
{
    return (t->first == 0 || t->first == 1) && first->n_out == 2 && first->out[0].script_len == BTC_P2PKH_LEN &&
           !memcmp(first->out[0].script, t->puzzle_spk, BTC_P2PKH_LEN) && first->out[1].value == payout_tiny(t) &&
           first->out[1].script_len == BTC_P2PKH_LEN && !memcmp(first->out[1].script, t->coin_spk[t->first], BTC_P2PKH_LEN);
}

bool payout_second(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    if (!first_ok(t, first) || !btc_tx_txid(first, txid)) return false;
    btc_tx_init(tx);
    uint64_t v[3];                                              /* the prize and F's tiny coin, which came from the prize too */
    return second_values(t, first->out[0].value + first->out[1].value, v) && btc_tx_add_in(tx, txid, 0, BTC_SEQUENCE_FINAL) &&
           btc_tx_add_in(tx, txid, 1, BTC_SEQUENCE_FINAL) && btc_tx_add_out(tx, v[0], t->pay_spk[0], t->pay_len[0]) &&
           btc_tx_add_out(tx, v[1], t->pay_spk[1], t->pay_len[1]) && btc_tx_add_out(tx, v[2], t->author_spk, t->author_len);
}

bool payout_cancel(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    btc_out out;
    if (!first_ok(t, first) || t->pay_len[t->first] > BTC_SPK_MAX || !btc_tx_txid(first, txid)) return false;
    const uint64_t fee = cancel_out(t, &out);
    btc_tx_init(tx);
    return btc_tx_add_in(tx, txid, 1, BTC_SEQUENCE_FINAL) && btc_tx_add_out(tx, payout_tiny(t) - fee, out.script, out.script_len);
}

bool payout_digests(const btc_tx *tx, const uint8_t puzzle_spk[BTC_P2PKH_LEN], size_t n, uint8_t (*digests)[32])
{
    if (n > tx->n_in) return false;
    for (size_t i = 0; i < n; i++)
        if (!btc_sighash_legacy(tx, i, puzzle_spk, BTC_P2PKH_LEN, digests[i])) return false;
    return true;
}
