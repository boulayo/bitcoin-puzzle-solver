/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* payout.c - the transactions of a pair (see payout.h). */
#include "payout.h"
#include <stdlib.h>
#include <string.h>

static int by_value(const void *a, const void *b)
{
    const payout_coin *x = a, *y = b;
    if (x->value != y->value) return x->value > y->value ? -1 : 1;
    const int c = memcmp(x->txid, y->txid, 32);
    return c ? c : (x->vout > y->vout) - (x->vout < y->vout);
}

bool payout_first(const payout_terms *t, const payout_coin *coins, size_t n, btc_tx *tx, size_t *used)
{
    payout_coin take[64];
    if (n > 64) n = 64;
    memcpy(take, coins, n * sizeof *coins);
    qsort(take, n, sizeof *take, by_value);
    const uint64_t in_fee = btc_fee(BTC_P2PKH_IN_SIZE, t->feerate);
    uint64_t total = 0;
    btc_tx_init(tx);
    for (size_t i = 0; i < n && tx->n_in < PAYOUT_MAX_COINS; i++) {
        if (take[i].value < 3 * in_fee) break;                 /* sorted: the rest are worth even less */
        if (total + take[i].value > BTC_MAX_MONEY || !btc_tx_add_in(tx, take[i].txid, take[i].vout, BTC_SEQUENCE_FINAL)) return false;
        total += take[i].value;
    }
    *used = tx->n_in;
    btc_out out[3] = {{0}};                                     /* the outputs as they will be, for the size */
    for (int k = 0; k < 3; k++) {
        out[k].script_len = BTC_P2PKH_LEN;
        memcpy(out[k].script, k ? t->coin_spk[k - 1] : t->puzzle_spk, BTC_P2PKH_LEN);
    }
    const uint64_t fee = btc_fee(btc_vsize_p2pkh(tx->n_in, out, 3), t->feerate);
    if (!tx->n_in || total < fee + 2 * PAYOUT_TINY + PAYOUT_TINY) return false;   /* the prize must stay larger than a tiny coin */
    return btc_tx_add_out(tx, total - fee - 2 * PAYOUT_TINY, t->puzzle_spk, BTC_P2PKH_LEN) &&
           btc_tx_add_out(tx, PAYOUT_TINY, t->coin_spk[0], BTC_P2PKH_LEN) && btc_tx_add_out(tx, PAYOUT_TINY, t->coin_spk[1], BTC_P2PKH_LEN);
}

/* the outputs of the first transaction: the prize at 0, the tiny coin of party p at 1 + p */
static bool first_ok(const payout_terms *t, const btc_tx *first)
{
    return first->n_out == 3 && first->out[0].script_len == BTC_P2PKH_LEN && !memcmp(first->out[0].script, t->puzzle_spk, BTC_P2PKH_LEN) &&
           first->out[1].value == PAYOUT_TINY && first->out[2].value == PAYOUT_TINY && (t->first == 0 || t->first == 1);
}

bool payout_second(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    if (!first_ok(t, first) || !btc_tx_txid(first, txid)) return false;
    btc_tx_init(tx);
    btc_out out[2] = {{0}};
    for (int p = 0; p < 2; p++) {
        out[p].script_len = (uint8_t)t->pay_len[p];
        memcpy(out[p].script, t->pay_spk[p], t->pay_len[p]);
    }
    const uint64_t prize = first->out[0].value, fee = btc_fee(btc_vsize_p2pkh(2, out, 2), t->feerate);
    if (prize < fee + 2 * 546) return false;                    /* each half above the dust limit */
    const uint64_t half = (prize - fee) / 2, other = prize - fee - half;
    const uint64_t v[2] = {t->first == 0 ? half + PAYOUT_TINY : other, t->first == 1 ? half + PAYOUT_TINY : other};
    return btc_tx_add_in(tx, txid, 0, BTC_SEQUENCE_FINAL) && btc_tx_add_in(tx, txid, (uint32_t)(1 + t->first), BTC_SEQUENCE_FINAL) &&
           btc_tx_add_out(tx, v[0], t->pay_spk[0], t->pay_len[0]) && btc_tx_add_out(tx, v[1], t->pay_spk[1], t->pay_len[1]);
}

bool payout_cancel(const payout_terms *t, const btc_tx *first, btc_tx *tx)
{
    uint8_t txid[32];
    if (!first_ok(t, first) || !btc_tx_txid(first, txid)) return false;
    btc_tx_init(tx);
    btc_out out = {.script_len = (uint8_t)t->pay_len[t->first]};
    memcpy(out.script, t->pay_spk[t->first], t->pay_len[t->first]);
    const uint64_t fee = btc_fee(btc_vsize_p2pkh(1, &out, 1), t->feerate);
    if (PAYOUT_TINY < fee + 546) return false;                  /* a fee rate this high would leave dust: wait for a lower one */
    return btc_tx_add_in(tx, txid, (uint32_t)(1 + t->first), BTC_SEQUENCE_FINAL) &&
           btc_tx_add_out(tx, PAYOUT_TINY - fee, out.script, out.script_len);
}

bool payout_digests(const btc_tx *tx, const uint8_t puzzle_spk[BTC_P2PKH_LEN], size_t n, uint8_t (*digests)[32])
{
    if (n > tx->n_in) return false;
    for (size_t i = 0; i < n; i++)
        if (!btc_sighash_legacy(tx, i, puzzle_spk, BTC_P2PKH_LEN, digests[i])) return false;
    return true;
}
