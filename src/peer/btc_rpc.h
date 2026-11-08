/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* btc_rpc.h - the JSON-RPC client of bitcoind: one HTTP/1.1 POST per connection, basic authentication from a user and a password or
 * from the cookie file of the node, a time limit on every call.
 *
 * Each call returns false on failure and leaves the reason in c->err: the transport (no connection, a timeout, a reply cut short),
 * an HTTP status without a JSON-RPC error (401: wrong credentials), a reply that is not what the call returns, or the error bitcoind
 * gave, with its code and message. The node may be someone else's: what it says about amounts is checked by the callers against the
 * raw previous transactions (btc_prevout), and every reply is read as hostile input. POSIX sockets; a client is used by one thread at
 * a time. */
#ifndef PZPEER_BTC_RPC_H
#define PZPEER_BTC_RPC_H

#include "btc.h"
#include "json.h"

typedef enum {
    BTC_RPC_OK,
    BTC_RPC_ENET,     /* no connection, a timeout, the reply cut short */
    BTC_RPC_EHTTP,    /* an HTTP status other than 200 and no JSON-RPC error with it */
    BTC_RPC_EPROTO,   /* a reply that is not JSON-RPC, or not what the call returns */
    BTC_RPC_ERPC,     /* the error of bitcoind */
} btc_rpc_kind;

typedef struct {
    btc_rpc_kind kind;
    int code;         /* BTC_RPC_ERPC: the code of bitcoind (-5 no such transaction, -8 bad parameter, -25 and -26 transaction refused,
                       * -32601 no such method); BTC_RPC_EHTTP: the HTTP status */
    char msg[256];
} btc_rpc_err;

#define BTC_RPC_AUTH_MAX 512   /* user:password, in bytes */

typedef struct {
    char host[256], port[8];
    char auth[BTC_RPC_AUTH_MAX / 3 * 4 + 8];   /* the base64 of user:password */
    char wallet[80];        /* the path of the wallet calls, /wallet/<name>, or empty */
    int timeout_ms;         /* of every call but scantxoutset; 30 s from btc_rpc_init */
    int scan_timeout_ms;    /* scantxoutset reads the whole set of unspent outputs: minutes on the main chain; 30 min */
    uint64_t id;
    btc_rpc_err err;
} btc_rpc;

/* A client of the node at host:port (a name, an IPv4 or an IPv6 address). False when an argument is too long or empty, or the host
 * has a character no name or address has (it goes into the Host header). */
bool btc_rpc_init(btc_rpc *c, const char *host, unsigned port, const char *user, const char *pass);
/* The same with the credentials of the cookie file the node writes (.cookie in its data directory: "__cookie__:<password>"). */
bool btc_rpc_init_cookie(btc_rpc *c, const char *host, unsigned port, const char *cookie_path);
/* The wallet the wallet calls (getnewaddress, sendtoaddress) use, when the node has more than one loaded; NULL or "" for none. The
 * name is limited to letters, digits, '-', '_' and '.', which need no escaping in the path. */
bool btc_rpc_set_wallet(btc_rpc *c, const char *name);
/* wipes the client, credentials included */
void btc_rpc_done(btc_rpc *c);

/* A call: method with the parameters in params (an array written with json_w, or NULL for none), to the wallet path when wallet is
 * true. On success *result is the "result" member of the reply, which lives in doc until json_free(doc); on failure doc is empty. */
bool btc_rpc_call(btc_rpc *c, bool wallet, const char *method, json_w *params, json_doc *doc, const json_val **result);

bool btc_rpc_getblockcount(btc_rpc *c, uint64_t *height);
bool btc_rpc_getblockhash(btc_rpc *c, uint64_t height, uint8_t hash[32]);
/* The bytes of a transaction, in *raw (malloc'd: the caller frees it) and *len: from the mempool or the transaction index, or from
 * the block blockhash when it is not NULL, which needs no index. As the server sent them: btc_prevout is the check. */
bool btc_rpc_getrawtransaction(btc_rpc *c, const uint8_t txid[32], const uint8_t *blockhash, uint8_t **raw, size_t *len);

/* an unspent output as scantxoutset or gettxout report it; the value is the server's word until btc_prevout confirms it */
typedef struct {
    uint8_t  txid[32];
    uint32_t vout;
    uint64_t value;
    uint32_t height;          /* scantxoutset: the block that holds it */
    uint32_t confirmations;   /* gettxout: 0 while in the mempool */
    bool     coinbase;        /* false when the node does not say (before 25.0) */
    uint8_t  spk_len;
    uint8_t  spk[BTC_SPK_MAX];
} btc_utxo;

/* scantxoutset with an addr() descriptor for each address: the unspent outputs of the addresses as of the tip the scan saw (its
 * height in *height). The first cap go to out and their total number to *n, which can be more than cap. */
bool btc_rpc_scan(btc_rpc *c, const char *const *addrs, size_t n_addr, btc_utxo *out, size_t cap, size_t *n, uint64_t *height);
/* gettxout: *found false when the output is spent or never was; with mempool, a spend in the mempool counts as spent */
bool btc_rpc_gettxout(btc_rpc *c, const uint8_t txid[32], uint32_t vout, bool mempool, bool *found, btc_utxo *out);
/* estimatesmartfee: satoshis per 1000 virtual bytes to confirm within target blocks, or 0 when the node has no estimate (a node that
 * has just started, regtest) */
bool btc_rpc_estimatesmartfee(btc_rpc *c, unsigned target, uint64_t *sat_per_kvb);
/* testmempoolaccept of one transaction: *ok, and the reason of a refusal in reason (cap bytes, "" when accepted) */
bool btc_rpc_testmempoolaccept(btc_rpc *c, const uint8_t *raw, size_t len, bool *ok, char *reason, size_t cap);
/* sendrawtransaction: the txid the node gives back */
bool btc_rpc_sendrawtransaction(btc_rpc *c, const uint8_t *raw, size_t len, uint8_t txid[32]);

/* The wallet calls of the tests on regtest. type of getnewaddress: "legacy", "p2sh-segwit", "bech32", "bech32m", or NULL for the
 * node's default. generatetoaddress gives the hash of the last block in last when it is not NULL. */
bool btc_rpc_createwallet(btc_rpc *c, const char *name);
bool btc_rpc_getnewaddress(btc_rpc *c, const char *type, char *addr, size_t cap);
bool btc_rpc_sendtoaddress(btc_rpc *c, const char *addr, uint64_t sats, uint8_t txid[32]);
bool btc_rpc_generatetoaddress(btc_rpc *c, unsigned blocks, const char *addr, uint8_t last[32]);

#endif
