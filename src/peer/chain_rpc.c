/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_rpc.c - session_chain on bitcoind (see chain_rpc.h). The node is trusted for what it can only report (which outputs exist and are
 * unspent, the height), not for what can be checked: every value comes from the bytes of the transaction that made it, whose hash is its id. */
#include "chain_rpc.h"
#include <pzcore/address.h>
#include <stdlib.h>
#include <string.h>

#define COINBASE_MATURITY 100

/* the address of a P2PKH script on the chain, for the descriptor of scantxoutset */
static bool p2pkh_address(btc_chain chain, const uint8_t spk[BTC_P2PKH_LEN], char *out, size_t cap)
{
    uint8_t payload[21];
    if (!btc_script_is_p2pkh(spk, BTC_P2PKH_LEN, payload + 1)) return false;
    payload[0] = chain == BTC_MAIN ? 0x00 : 0x6f;
    return pz_base58check_encode(payload, sizeof payload, out, cap) > 0;
}

static int coins(void *ctx, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    chain_rpc *c = ctx;
    char addr[64];
    const char *addrs[1] = {addr};
    btc_utxo u[64];
    size_t n;
    uint64_t tip;
    if (!p2pkh_address(c->chain, spk, addr, sizeof addr) || !btc_rpc_scan(c->rpc, addrs, 1, u, 64, &n, &tip)) return -1;
    if (n > 64) n = 64;
    int k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        uint8_t bh[32], *raw = NULL;
        size_t len;
        uint64_t value;
        const uint8_t *s;
        size_t sl;
        if (!u[i].height || (u[i].coinbase && tip + 1 < u[i].height + COINBASE_MATURITY)) continue;   /* not spendable yet */
        if (!btc_rpc_getblockhash(c->rpc, u[i].height, bh) || !btc_rpc_getrawtransaction(c->rpc, u[i].txid, bh, &raw, &len)) {
            free(raw);
            return -1;
        }
        const bool ok = btc_prevout(raw, len, u[i].txid, u[i].vout, &value, &s, &sl) && sl == BTC_P2PKH_LEN && !memcmp(s, spk, sl);
        free(raw);
        if (!ok) continue;                                     /* the node said something its own bytes do not: not taken */
        memcpy(out[k].txid, u[i].txid, 32), out[k].vout = u[i].vout, out[k].value = value, k++;
    }
    return k;
}

static bool broadcast(void *ctx, const btc_tx *tx)
{
    chain_rpc *c = ctx;
    uint8_t raw[BTC_TX_MAX_SIZE], txid[32];
    const size_t n = btc_tx_serialize(tx, raw, sizeof raw);
    if (!n) return false;
    if (btc_rpc_sendrawtransaction(c->rpc, raw, n, txid)) return true;
    /* sent already, by the partner or by an earlier try: -27 (in the chain) or a mempool that knows it */
    return c->rpc->err.kind == BTC_RPC_ERPC && (c->rpc->err.code == -27 || strstr(c->rpc->err.msg, "already") != NULL);
}

static int confirmations(void *ctx, const uint8_t txid[32], uint32_t vout)
{
    chain_rpc *c = ctx;
    bool found;
    btc_utxo u;
    if (!btc_rpc_gettxout(c->rpc, txid, vout, true, &found, &u)) return -2;
    return found ? (int)u.confirmations : -1;
}

static uint64_t height(void *ctx)
{
    uint64_t h = 0;
    (void)btc_rpc_getblockcount(((chain_rpc *)ctx)->rpc, &h);
    return h;
}

static uint64_t feerate(void *ctx)
{
    chain_rpc *c = ctx;
    uint64_t f = 0;
    if (!btc_rpc_estimatesmartfee(c->rpc, c->conf_target ? c->conf_target : 6, &f) || f == 0) f = c->fallback_feerate;
    return f;
}

session_chain chain_rpc_iface(chain_rpc *c) { return (session_chain){c, coins, broadcast, confirmations, height, feerate}; }
