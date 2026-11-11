/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_rpc.c - session_chain on bitcoind (see chain_rpc.h). */
#include "chain_rpc.h"
#include "sys.h"
#include <pzcore/address.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define COINBASE_MATURITY 100
#define SCAN_MAX 64

/* the address of a P2PKH script on the chain, for the descriptor of scantxoutset */
static bool p2pkh_address(btc_chain chain, const uint8_t spk[BTC_P2PKH_LEN], char *out, size_t cap)
{
    uint8_t payload[21];
    if (!btc_script_is_p2pkh(spk, BTC_P2PKH_LEN, payload + 1)) return false;
    payload[0] = chain == BTC_MAIN ? 0x00 : 0x6f;
    return pz_base58check_encode(payload, sizeof payload, out, cap) > 0;
}

int chain_rpc_ready(chain_rpc *c, char *why, size_t cap)
{
    btc_chaininfo ci;
    btc_chain on;
    if (!btc_rpc_getblockchaininfo(c->rpc, &ci)) return snprintf(why, cap, "the node does not answer: %s", c->rpc->err.msg), 0;
    /* the names of getblockchaininfo are those of the network file, but for the fourth test network, which shares the forms of test */
    if (!btc_chain_parse(strcmp(ci.chain, "testnet4") ? ci.chain : "test", &on) || on != c->chain)
        return snprintf(why, cap, "the node is on the chain %s, not on the chain of the network", ci.chain), -1;
    if (ci.ibd || ci.headers > ci.blocks + 1)
        return snprintf(why, cap, "the node is still syncing: block %llu of %llu", (unsigned long long)ci.blocks,
                        (unsigned long long)ci.headers), 0;
    snprintf(why, cap, "the node is at block %llu%s", (unsigned long long)ci.blocks, ci.pruned ? ", pruned" : "");
    return 1;
}

static int coins(void *ctx, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max)
{
    chain_rpc *c = ctx;
    char addr[64], why[300];
    const char *addrs[1] = {addr};
    btc_utxo u[SCAN_MAX];
    size_t n;
    uint64_t tip;
    if (max <= 0 || !p2pkh_address(c->chain, spk, addr, sizeof addr) || chain_rpc_ready(c, why, sizeof why) != 1) return -1;
    /* the node runs one scan at a time and answers another one (the partner on the same node, a wallet) with an error, not in a queue:
     * on the main chain a scan takes minutes, so the wait is as long as a scan may be */
    const int64_t until = sys_now_ms() + c->rpc->scan_timeout_ms;
    while (!btc_rpc_scan(c->rpc, addrs, 1, u, max < SCAN_MAX ? (size_t)max : SCAN_MAX, &n, &tip)) {
        if (c->rpc->err.kind != BTC_RPC_ERPC || !strstr(c->rpc->err.msg, "in progress") || sys_now_ms() > until) return -1;
        sys_sleep_ms(1000);
    }
    if (n > SCAN_MAX) n = SCAN_MAX;
    int k = 0;
    for (size_t i = 0; i < n && k < max; i++) {
        if (!u[i].height || (u[i].coinbase && tip + 1 < u[i].height + COINBASE_MATURITY)) continue;   /* not spendable yet */
        if (u[i].spk_len != BTC_P2PKH_LEN || memcmp(u[i].spk, spk, BTC_P2PKH_LEN)) continue;           /* not the script asked */
        memcpy(out[k].txid, u[i].txid, 32), out[k].vout = u[i].vout, out[k].value = u[i].value, k++;
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

/* The height has no value for "unknown", and a session counts blocks from it: a 0 after a call that failed would end a wait of a
 * few blocks at once (and F would cancel). So a few tries, then the last height the node gave. */
static uint64_t height(void *ctx)
{
    chain_rpc *c = ctx;
    uint64_t h;
    for (int tries = 0; tries < 10; tries++) {
        if (btc_rpc_getblockcount(c->rpc, &h)) return c->tip = h;
        sys_sleep_ms(1000);
    }
    return c->tip;
}

static uint64_t feerate(void *ctx)
{
    chain_rpc *c = ctx;
    uint64_t f = 0;
    if (!btc_rpc_estimatesmartfee(c->rpc, c->conf_target ? c->conf_target : 6, &f) || f == 0) f = c->fallback_feerate;
    return f;
}

/* a prize of the network file: an output of the puzzle in the node's set, confirmed (the mempool does not count) and mature */
static int coin(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    chain_rpc *c = ctx;
    bool found;
    btc_utxo u;
    if (!btc_rpc_gettxout(c->rpc, txid, vout, false, &found, &u)) return -1;
    if (!found || !u.confirmations || (u.coinbase && u.confirmations < COINBASE_MATURITY) || u.spk_len != BTC_P2PKH_LEN ||
        memcmp(u.spk, spk, BTC_P2PKH_LEN))
        return 0;
    memcpy(out->txid, txid, 32), out->vout = vout, out->value = u.value;
    return 1;
}

session_chain chain_rpc_iface(chain_rpc *c) { return (session_chain){c, coins, broadcast, confirmations, height, feerate, coin}; }
