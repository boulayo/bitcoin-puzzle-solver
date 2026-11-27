/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* chain_rpc.c - session_chain on bitcoind (see chain_rpc.h). */
#include "chain_rpc.h"
#include "sys.h"
#include <pzcore/address.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char addr[64];
    const char *addrs[1] = {addr};
    btc_utxo *u = max > 0 ? malloc((size_t)max * sizeof *u) : NULL;   /* a sweep asks for hundreds */
    size_t n = 0;
    uint64_t tip;
    bool ok = u && p2pkh_address(c->chain, spk, addr, sizeof addr);
    /* the node runs one scan at a time and answers another one (the partner on the same node, a wallet) with an error, not in a queue:
     * on the main chain a scan takes minutes, so the wait is as long as a scan may be */
    const int64_t until = sys_now_ms() + c->rpc->scan_timeout_ms;
    while (ok && !btc_rpc_scan(c->rpc, addrs, 1, u, (size_t)max, &n, &tip)) {
        ok = c->rpc->err.kind == BTC_RPC_ERPC && strstr(c->rpc->err.msg, "in progress") && sys_now_ms() <= until;
        if (ok) sys_sleep_ms(1000);
    }
    if (n > (size_t)max) n = (size_t)max;                       /* the scan reports every coin, and fills at most max */
    int k = 0;
    for (size_t i = 0; ok && i < n; i++) {
        if (!u[i].height || (u[i].coinbase && tip + 1 < u[i].height + BTC_COINBASE_MATURITY)) continue;   /* not spendable yet */
        if (u[i].spk_len != BTC_P2PKH_LEN || memcmp(u[i].spk, spk, BTC_P2PKH_LEN)) continue;           /* not the script asked */
        memcpy(out[k].txid, u[i].txid, 32), out[k].vout = u[i].vout, out[k].value = u[i].value, k++;
    }
    free(u);
    return ok ? k : -1;
}

static bool broadcast(void *ctx, const uint8_t *raw, size_t n)
{
    chain_rpc *c = ctx;
    uint8_t txid[32];
    if (btc_rpc_sendrawtransaction(c->rpc, raw, n, txid)) return true;
    /* sent already, by the partner or by an earlier try: -27 (in the chain) or a mempool that knows it */
    return c->rpc->err.kind == BTC_RPC_ERPC && (c->rpc->err.code == -27 || strstr(c->rpc->err.msg, "already") != NULL);
}

static int confirmations(void *ctx, const uint8_t txid[32], uint32_t vout)
{
    chain_rpc *c = ctx;
    bool found;
    btc_utxo u;
    bool pooled;
    if (!btc_rpc_gettxout(c->rpc, txid, vout, &found, &u)) return -2;
    if (found) return (int)u.confirmations;                     /* in the set of the chain: a spend that only waits in the mempool is none yet */
    /* in no block's set: spent in a block, unless the transaction itself waits in the mempool (it is new, or a reorganisation took its
     * block away, and its spend may wait there too): that is no spend in a block */
    return !btc_rpc_inmempool(c->rpc, txid, &pooled) ? -2 : pooled ? 0 : -1;
}

/* The height has no value for "unknown", and a session counts blocks from it: a 0 after a call that failed would end a wait of a
 * few blocks at once (and F would cancel). So the last height the node gave. */
static uint64_t height(void *ctx)
{
    chain_rpc *c = ctx;
    uint64_t h;
    if (btc_rpc_getblockcount(c->rpc, &h)) c->tip = h;
    return c->tip;
}

static uint64_t feerate(void *ctx)                              /* within 2 blocks (project 4.5) */
{
    chain_rpc *c = ctx;
    uint64_t f = 0;
    return btc_rpc_estimatesmartfee(c->rpc, 2, &f) ? f : 0;
}

/* a prize of the network file: an output of the puzzle in the node's set, confirmed (the mempool does not count) and mature */
static int coin(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out)
{
    chain_rpc *c = ctx;
    bool found;
    btc_utxo u;
    if (!btc_rpc_gettxout(c->rpc, txid, vout, &found, &u)) return -1;
    if (!found || !u.confirmations || (u.coinbase && u.confirmations < BTC_COINBASE_MATURITY) || u.spk_len != BTC_P2PKH_LEN ||
        memcmp(u.spk, spk, BTC_P2PKH_LEN))
        return 0;
    memcpy(out->txid, txid, 32), out->vout = vout, out->value = u.value;
    return 1;
}

static int ready(void *ctx, char *why, size_t cap) { return chain_rpc_ready(ctx, why, cap); }

session_chain chain_rpc_iface(chain_rpc *c) { return (session_chain){c, coins, broadcast, confirmations, height, feerate, coin, ready, false}; }
