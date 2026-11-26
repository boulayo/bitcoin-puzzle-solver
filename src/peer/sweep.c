/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* sweep.c - see sweep.h. */
#include "sweep.h"
#include "chain_rpc.h"
#include "chain_spv.h"
#include "sys.h"
#include "../puzzles.h"
#include <pzcore/address.h>
#include <pzcore/seal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sweep_tx(const session_chain *ch, const uint8_t sec[32], const payout_coin *named, int nnamed, const uint8_t *to, size_t to_len,
                     const payout_terms *author, sweep_info *info, btc_tx *tx)
{
    uint8_t spk[BTC_P2PKH_LEN];
    payout_coin coins[SESSION_COINS_MAX];
    btc_out out[2] = {{.script_len = (uint8_t)to_len}};       /* the outputs as they will be, for the size */
    const bool light = ch->pending != NULL;
    int unseen;
    memcpy(out[0].script, to, to_len);
    if (author) out[1].script_len = (uint8_t)author->author_len, memcpy(out[1].script, author->author_spk, author->author_len);
    btc_tx_init(tx);
    info->total = 0, info->listed = 0;
    if (!btc_key_script(sec, spk)) return "not a key";
    if (light && !nnamed)
        return "servers cannot show the coins of an address that are older than their checkpoint, and no coin of this one is named: a"
               " sweep of a part of them would leave the rest to whoever is first. Use a node of your own (--rpc)";
    int n = info->listed = session_coins(ch, spk, named, nnamed, coins, &unseen);
    if (light && unseen)
        return "a server hid a coin of the address, or none answered about it: a sweep of a part of the coins would leave the rest to"
               " whoever is first. Try again, or use a node of your own (--rpc)";
    if (!n) return unseen ? "the chain did not answer" : "the address has no confirmed coin";
    /* Twice the estimate, 50 to 1000 sat/vB: no server sets the fee of a prize. The floor is above the 20 sat/vB of a pair (home.c): a
     * pair can pay again at a higher rate, while this transaction goes once, and whoever sees its key in it races it */
    if (!info->rate) {
        const uint64_t est = 2 * ch->feerate(ch->ctx);
        info->rate = est < 50000 ? 50000 : est > 1000000 ? 1000000 : est;
    }
    n = (int)payout_largest(coins, (size_t)n, BTC_MAX_IN, btc_fee(BTC_P2PKH_IN_SIZE, info->rate) + 1);
    bool ok = n > 0;
    for (int i = 0; ok && i < n; i++) info->total += coins[i].value, ok = btc_tx_add_in(tx, coins[i].txid, coins[i].vout, BTC_SEQUENCE_FINAL);
    const uint64_t fee = ok ? btc_fee(btc_vsize_p2pkh(tx->n_in, out, author ? 2 : 1), info->rate) : 0;
    const uint64_t left = info->total > fee ? info->total - fee : 0, cut = author ? left * author->author_pct / 100 : 0;
    ok = ok && left - cut > 546 && (!author || cut >= 546) && btc_tx_add_out(tx, left - cut, to, to_len) &&
         (!author || btc_tx_add_out(tx, cut, out[1].script, out[1].script_len));
    for (size_t i = 0; ok && i < tx->n_in; i++) ok = btc_sign_p2pkh(tx, i, sec) && btc_verify_p2pkh(tx, i, spk, sizeof spk);
    return ok ? NULL : "its coins do not pay the fee";
}

/* the named coins of the address of a key, from the table of the program (data/prizes.csv): those of the open puzzle it is the key of */
static int named_coins(const uint8_t spk[BTC_P2PKH_LEN], payout_coin out[SESSION_PRIZES_MAX])
{
    uint8_t h160[20], a[20];
    int n = 0;
    (void)btc_script_is_p2pkh(spk, BTC_P2PKH_LEN, h160);
    for (int i = 0; i < PZ_NUM_PUZZLES; i++)
        if (pz_address_to_hash160(PZ_PUZZLES[i].address, a) && !memcmp(a, h160, 20))
            for (const puzzle_prize *z = PZ_PRIZES; z->n && n < SESSION_PRIZES_MAX; z++)
                if (z->n == PZ_PUZZLES[i].n && btc_hash_from_hex(z->txid, out[n].txid)) out[n++].vout = z->vout;
    return n;
}

/* The transaction to a mining pool that does not show it in the public mempool (--private): POST {"tx_hex": ...} to url through the curl
 * of the system (Linux, macOS, and Windows since 10 have it), so that pzpeer needs no TLS of its own. What the pool answers is printed. */
static bool send_private(const char *url, const char *hex)
{
    const char *argv[] = {"curl", "-sS", "--max-time", "60", "-H", "Content-Type: application/json", "--data-binary", "@-", "-w", "\nHTTP %{http_code}\n", url, NULL};
    int to = -1, from = -1;
    const sys_proc p = sys_spawn(argv, NULL, &to, &from);
    if (!p) return fprintf(stderr, "cannot start curl: install it, or give the transaction to the pool by hand\n"), false;
    const bool sent = sys_pipe_write(to, "{\"tx_hex\":\"", 11) && sys_pipe_write(to, hex, strlen(hex)) && sys_pipe_write(to, "\"}", 2);
    sys_pipe_close(to);
    char ans[2048];
    size_t n = 0;
    while (n + 1 < sizeof ans && sys_pipe_read(from, ans + n, 1, 70000)) n++;
    ans[n] = 0;
    sys_pipe_close(from);
    int st;
    (void)sys_proc_ended(p, true, &st);
    const bool ok = sent && st == 0 && strstr(ans, "\nHTTP 2") != NULL;
    printf("the pool at %s answered%s:\n%s\n", url, ok ? "" : " (not taken)", ans);
    return ok;
}

int sweep_main(int argc, char **argv)
{
    const char *to = NULL, *chain_name = "main", *rpc_arg = NULL, *cookie = NULL, *servers[CHAIN_SPV_SERVERS], *pool = NULL;
    int nservers = 0;
    uint64_t rate = 0;
    for (int i = 2; i < argc; i += 2) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--private")) {
            pool = pool ? pool : SWEEP_POOL;
            i--;
            continue;
        }
        if (!v) to = NULL, i = argc;
        else if (!strcmp(a, "--pool")) pool = v;
        else if (!strcmp(a, "--to")) to = v;
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
    if (!to_len) return fprintf(stderr, "usage: pzpeer sweep --to ADDRESS [--chain main|test|signet|regtest]\n"
                                        "                    [--rpc H:P:USER:PASSWORD | --electrum HOST:PORT ...] [--feerate SAT_PER_VBYTE]\n"
                                        "                    [--private | --pool URL]\n"
                                        "       the private key, 64 hex digits, on the standard input\n"
                                        "       without --rpc: the key of an open puzzle of the program (its coins are named there), every coin or none\n"), 2;
    char hex[80] = "";
    if (!fgets(hex, sizeof hex, stdin)) hex[0] = 0;
    hex[strcspn(hex, " \t\r\n")] = 0;
    const bool key_ok = strlen(hex) == 64 && pz_unhex(hex, 64, sec) && btc_key_script(sec, spk);
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
    payout_coin named[SESSION_PRIZES_MAX];
    const char *no = sweep_tx(&ch, sec, named, chain == BTC_MAIN ? named_coins(spk, named) : 0, to_spk, to_len, NULL, &si, &tx);
    pz_secure_zero(sec, sizeof sec);
    if (no) return fprintf(stderr, "no transaction: %s\n", no), 1;
    const int n = (int)tx.n_in, listed = si.listed;
    const uint64_t total = si.total, fee = total - tx.out[0].value;
    rate = si.rate;

    static uint8_t raw[BTC_TX_MAX_SIZE];
    static char text[2 * BTC_TX_MAX_SIZE + 1];
    char id[65];
    uint8_t txid[32];
    const size_t len = btc_tx_serialize(&tx, raw, sizeof raw);
    btc_tx_txid(&tx, txid), btc_hash_to_hex(txid, id), pz_hex_encode(raw, len, text);
    printf("coins: %d, %llu satoshis; to %s: %llu satoshis; fee %llu satoshis (%llu sat/vB)\ntxid: %s\n\n%s\n\n", n, (unsigned long long)total, to,
           (unsigned long long)(total - fee), (unsigned long long)fee, (unsigned long long)(rate / 1000), id, text);
    if (n < listed) printf("%d smaller coins are left where they are: those worth their fee can go in a second transaction, once this one is in a block.\n\n", listed - n);
    if (pool) return send_private(pool, text) ? 0 : 1;
    printf("NOT SENT. Do not paste this transaction into a wallet, a block explorer or a node: in the public mempool it shows the public key,\n"
           "and with a key this short a program finds the private key from it in seconds and replaces your transaction with its own.\n"
           "Give it to a mining pool that takes private transactions (for example the Slipstream service of MARA), and wait for the block:\n"
           "pzpeer sweep --private does it, with the same options.\n");
    return 0;
}
