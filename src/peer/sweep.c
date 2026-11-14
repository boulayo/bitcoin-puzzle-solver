/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sweep.c - see sweep.h. */
#include "sweep.h"
#include "chain_rpc.h"
#include "chain_spv.h"
#include <pzcore/seal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool sweep_tx(const session_chain *ch, const uint8_t sec[32], const uint8_t *to, size_t to_len, const payout_terms *author, sweep_info *info,
              btc_tx *tx)
{
    uint8_t spk[BTC_P2PKH_LEN];
    payout_coin coins[64];
    btc_out out[2] = {{.script_len = (uint8_t)to_len}};       /* the outputs as they will be, for the size */
    memcpy(out[0].script, to, to_len);
    if (author) out[1].script_len = (uint8_t)author->author_len, memcpy(out[1].script, author->author_spk, author->author_len);
    int n = info->listed = btc_key_script(sec, spk) ? ch->coins(ch->ctx, spk, coins, 64) : -1;
    if (!info->rate) {                                          /* twice the estimate, 50 to 1000 sat/vB: no server sets the fee of a prize */
        const uint64_t est = 2 * ch->feerate(ch->ctx);
        info->rate = est < 50000 ? 50000 : est > 1000000 ? 1000000 : est;
    }
    btc_tx_init(tx);
    info->total = 0;
    if (n > 0) n = (int)payout_largest(coins, (size_t)n, BTC_MAX_IN, btc_fee(BTC_P2PKH_IN_SIZE, info->rate) + 1);
    bool ok = n > 0;
    for (int i = 0; ok && i < n; i++) info->total += coins[i].value, ok = btc_tx_add_in(tx, coins[i].txid, coins[i].vout, BTC_SEQUENCE_FINAL);
    const uint64_t fee = ok ? btc_fee(btc_vsize_p2pkh(tx->n_in, out, author ? 2 : 1), info->rate) : 0;
    const uint64_t left = info->total > fee ? info->total - fee : 0, cut = author ? left * author->author_pct / 100 : 0;
    ok = ok && left - cut > 546 && (!author || cut >= 546) && btc_tx_add_out(tx, left - cut, to, to_len) &&
         (!author || btc_tx_add_out(tx, cut, out[1].script, out[1].script_len));
    for (size_t i = 0; ok && i < tx->n_in; i++) ok = btc_sign_p2pkh(tx, i, sec) && btc_verify_p2pkh(tx, i, spk, sizeof spk);
    return ok;
}

int sweep_main(int argc, char **argv)
{
    const char *to = NULL, *chain_name = "main", *rpc_arg = NULL, *cookie = NULL, *servers[CHAIN_SPV_SERVERS];
    int nservers = 0;
    uint64_t rate = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        const char *a = argv[i], *v = argv[i + 1];
        if (!strcmp(a, "--to")) to = v;
        else if (!strcmp(a, "--chain")) chain_name = v;
        else if (!strcmp(a, "--rpc")) rpc_arg = v;
        else if (!strcmp(a, "--rpc-cookie")) cookie = v;
        else if (!strcmp(a, "--electrum") && nservers < CHAIN_SPV_SERVERS) servers[nservers++] = v;
        else if (!strcmp(a, "--feerate")) rate = 1000 * strtoull(v, NULL, 10);
        else to = NULL, i = argc;
    }
    btc_chain chain;
    uint8_t to_spk[BTC_SPK_MAX], spk[BTC_P2PKH_LEN], sec[32], cp[32];
    const size_t to_len = to && btc_chain_parse(chain_name, &chain) ? btc_address_script(chain, to, to_spk) : 0;
    if (!to_len || argc % 2) return fprintf(stderr, "usage: pzpeer sweep --to ADDRESS [--chain main|test|signet|regtest]\n"
                                                    "                    [--rpc H:P:USER:PASSWORD | --electrum HOST:PORT ...] [--feerate SAT_PER_VBYTE]\n"
                                                    "       the private key, 64 hex digits, on the standard input\n"), 2;
    char hex[80] = "";
    if (!fgets(hex, sizeof hex, stdin)) hex[0] = 0;
    hex[strcspn(hex, " \t\r\n")] = 0;
    const bool key_ok = strlen(hex) == 64 && btc_unhex(hex, 64, sec) && btc_key_script(sec, spk);
    pz_secure_zero(hex, sizeof hex);
    if (!key_ok) return fprintf(stderr, "the key must be 64 hex digits on the standard input\n"), 1;

    /* the chain: the node of the user, or Electrum servers whose answers are proven */
    btc_rpc rpc;
    chain_rpc crpc = {.rpc = &rpc, .chain = chain, .conf_target = 2, .fallback_feerate = 2000};
    chain_spv spv;
    uint64_t cph = 0;
    bool has_cp = false;
    char why[300];
    if (rpc_arg && !btc_rpc_init_arg(&rpc, rpc_arg, cookie)) return fprintf(stderr, "--rpc: %s\n", rpc.err.msg), 1;
    if (!rpc_arg) {
        if (chain == BTC_MAIN) has_cp = true, nservers = nservers ? (chain_spv_defaults(NULL, &cph, cp), nservers) : chain_spv_defaults(servers, &cph, cp);
        if (!nservers || !chain_spv_init(&spv, chain, cph, has_cp ? cp : NULL, servers, nservers, NULL)) return fprintf(stderr, "no chain: give --rpc or --electrum\n"), 1;
        spv.fallback_feerate = 2000;
    }
    if ((rpc_arg ? chain_rpc_ready(&crpc, why, sizeof why) : chain_spv_ready(&spv, why, sizeof why)) != 1) return fprintf(stderr, "the chain is not ready: %s\n", why), 1;
    const session_chain ch = rpc_arg ? chain_rpc_iface(&crpc) : chain_spv_iface(&spv);
    btc_tx tx;
    sweep_info si = {.rate = rate};
    const bool ok = sweep_tx(&ch, sec, to_spk, to_len, NULL, &si, &tx);
    pz_secure_zero(sec, sizeof sec);
    if (!ok) return fprintf(stderr, si.listed > 0 ? "the coins of this key do not pay the fee\n" : si.listed == 0 ? "this key has no confirmed coin\n" : "the chain did not answer\n"), 1;
    const int n = (int)tx.n_in, listed = si.listed;
    const uint64_t total = si.total, fee = total - tx.out[0].value;
    rate = si.rate;

    static uint8_t raw[BTC_TX_MAX_SIZE];
    static char text[2 * BTC_TX_MAX_SIZE + 1];
    char id[65];
    uint8_t txid[32];
    const size_t len = btc_tx_serialize(&tx, raw, sizeof raw);
    btc_tx_txid(&tx, txid), btc_hash_to_hex(txid, id), btc_hex(raw, len, text);
    printf("coins: %d, %llu satoshis; to %s: %llu satoshis; fee %llu satoshis (%llu sat/vB)\ntxid: %s\n\n%s\n\n", n, (unsigned long long)total, to,
           (unsigned long long)(total - fee), (unsigned long long)fee, (unsigned long long)(rate / 1000), id, text);
    if (n < listed) printf("%d smaller coins are left where they are: those worth their fee can go in a second transaction, once this one is in a block.\n\n", listed - n);
    printf("NOT SENT. Do not paste this transaction into a wallet, a block explorer or a node: in the public mempool it shows the public key,\n"
           "and with a key this short a program finds the private key from it in seconds and replaces your transaction with its own.\n"
           "Give it to a mining pool that takes private transactions (for example the Slipstream service of MARA), and wait for the block.\n");
    return 0;
}
