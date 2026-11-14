/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* btc.h - Bitcoin transactions as pzpeer builds and checks them (docs/en/NETWORK.md 8): serialization, txid, the legacy signature hash,
 * the scripts of the standard addresses, DER signatures, signing and verification of P2PKH inputs, and the check of a previous output
 * against the raw bytes of its transaction.
 *
 * Our transactions spend P2PKH outputs only (the puzzle address and the tiny coins of the home), with the legacy signature hash,
 * SIGHASH_ALL and low s, and are written in the legacy serialization. The transactions they spend may be anything, segwit included:
 * btc_txid and btc_prevout read both serializations.
 *
 * Byte order: hashes (txids, block hashes) are kept in the internal order, the one in which they are computed and serialized. The hex
 * that bitcoind and block explorers show is the reverse (btc_hash_to_hex, btc_hash_from_hex).
 *
 * Every function that reads bytes or text takes hostile input and says so by its return value. The secret keys are given by the caller
 * and stay the caller's to wipe; the operations on them are those of libsecp256k1 (constant time). Thread-safe. */
#ifndef PZPEER_BTC_H
#define PZPEER_BTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BTC_MAX_IN        16
#define BTC_MAX_OUT       16
#define BTC_SCRIPTSIG_MAX 140    /* a P2PKH spend: a push of a 73-byte signature with its hashtype, a push of a 65-byte key */
#define BTC_SPK_MAX       83     /* an output script: of an address (34 at most: P2WSH, P2TR), P2PK (67), 80 bytes of data (83) */
#define BTC_SCRIPTCODE_MAX 252   /* the scriptCode the signature hash takes: P2PKH is 25 bytes, P2PK 35 or 67 */
#define BTC_P2PKH_LEN     25
#define BTC_DER_MAX       72
#define BTC_MAX_MONEY     2100000000000000ull   /* 21 million BTC in satoshis */
#define BTC_SEQUENCE_FINAL 0xffffffffu

typedef struct {
    uint8_t  prev_txid[32];                 /* internal order */
    uint32_t prev_vout;
    uint32_t sequence;
    uint8_t  script_len;
    uint8_t  script[BTC_SCRIPTSIG_MAX];     /* scriptSig */
} btc_in;

typedef struct {
    uint64_t value;                         /* satoshis */
    uint8_t  script_len;
    uint8_t  script[BTC_SPK_MAX];           /* scriptPubKey */
} btc_out;

typedef struct {
    uint32_t version, locktime;
    size_t   n_in, n_out;
    btc_in   in[BTC_MAX_IN];
    btc_out  out[BTC_MAX_OUT];
} btc_tx;

/* the longest legacy serialization of a btc_tx */
#define BTC_TX_MAX_SIZE (4 + 1 + BTC_MAX_IN * (36 + 1 + BTC_SCRIPTSIG_MAX + 4) + 1 + BTC_MAX_OUT * (8 + 1 + BTC_SPK_MAX) + 4)

/* ---- hex ---- */

/* n bytes as 2n lower-case hex digits and a NUL */
void btc_hex(const uint8_t *p, size_t n, char *out);
/* len hex digits (either case) to len / 2 bytes; false when len is odd or a character is not a hex digit */
bool btc_unhex(const char *s, size_t len, uint8_t *out);
/* a hash in internal order to and from the reversed hex of txids and block hashes (exactly 64 digits) */
void btc_hash_to_hex(const uint8_t h[32], char out[65]);
bool btc_hash_from_hex(const char *s, uint8_t h[32]);

/* ---- addresses and scripts ---- */

/* the chains of the network file: chain=main, test, signet, regtest */
typedef enum { BTC_MAIN, BTC_TEST, BTC_SIGNET, BTC_REGTEST } btc_chain;
bool btc_chain_parse(const char *name, btc_chain *chain);

/* The scriptPubKey of a P2PKH, P2SH, P2WPKH, P2WSH or P2TR address of the chain: its length (22 to 34), or 0 for anything else, an
 * address of another chain included (testnet and signet share their forms; regtest has its own bech32 prefix, bcrt). */
size_t btc_address_script(btc_chain chain, const char *addr, uint8_t spk[BTC_SPK_MAX]);
/* satoshis as BTC with 8 decimals, for the terminal */
const char *btc_amount(uint64_t sat, char out[32]);
/* OP_DUP OP_HASH160 <h160> OP_EQUALVERIFY OP_CHECKSIG */
void btc_script_p2pkh(const uint8_t h160[20], uint8_t spk[BTC_P2PKH_LEN]);
/* whether spk is that script; its hash160 goes to h160 when it is not NULL */
bool btc_script_is_p2pkh(const uint8_t *spk, size_t len, uint8_t h160[20]);
/* The P2PKH script of the compressed public key of a secret key; false for a key that is not one. */
bool btc_key_script(const uint8_t seckey[32], uint8_t spk[BTC_P2PKH_LEN]);

/* ---- transactions ---- */

/* version 2, lock time 0, no inputs or outputs */
void btc_tx_init(btc_tx *tx);
/* append an input with an empty scriptSig, an output; false when the transaction is full (or the script too long, the value above
 * BTC_MAX_MONEY) */
bool btc_tx_add_in(btc_tx *tx, const uint8_t prev_txid[32], uint32_t prev_vout, uint32_t sequence);
bool btc_tx_add_out(btc_tx *tx, uint64_t value, const uint8_t *spk, size_t len);

/* The legacy serialization: its size, and the bytes written to out (cap bytes) with their number as the return value, or 0 when cap is
 * too small or tx is not a valid btc_tx (counts or script lengths beyond the capacities). */
size_t btc_tx_size(const btc_tx *tx);
size_t btc_tx_serialize(const btc_tx *tx, uint8_t *out, size_t cap);
/* Parses a transaction in either serialization; the witness, when there is one, is checked and left out. False for anything that is
 * not exactly one transaction (bytes left over included), with no inputs or outputs, a value above BTC_MAX_MONEY, or more inputs,
 * outputs or script bytes than a btc_tx holds. */
bool btc_tx_parse(btc_tx *tx, const uint8_t *raw, size_t len);
/* the txid: double SHA-256 of the legacy serialization */
bool btc_tx_txid(const btc_tx *tx, uint8_t txid[32]);
/* the txid of any transaction in either serialization, with no limit on its size; false when raw is not a transaction */
bool btc_txid(const uint8_t *raw, size_t len, uint8_t txid[32]);

/* The amount check (docs/en/NETWORK.md 8). raw is the transaction that created the output txid:vout, as some server sent it: it is
 * accepted only when its hash is txid, which makes the value and the script it holds those of the chain. The value goes to *value
 * and the scriptPubKey to *spk (a pointer into raw) and *spk_len. */
bool btc_prevout(const uint8_t *raw, size_t len, const uint8_t txid[32], uint32_t vout, uint64_t *value, const uint8_t **spk,
                 size_t *spk_len);

/* ---- signatures ---- */

/* The legacy signature hash (SignatureHash before BIP143) of input i with SIGHASH_ALL: the transaction with the scriptSig of input i
 * replaced by script_code and the others emptied, followed by the hashtype as 4 bytes, hashed twice with SHA-256. script_code is the
 * scriptPubKey of the output being spent, taken as it is: OP_CODESEPARATOR, which the original algorithm removes, is in no script we
 * sign. The digest is in the order libsecp256k1 takes it. False when i or the lengths are out of range. */
bool btc_sighash_legacy(const btc_tx *tx, size_t i, const uint8_t *script_code, size_t code_len, uint8_t digest[32]);

/* The DER form of the signature (r, s), 32 bytes each, big-endian, with s replaced by n - s when it is above n / 2 (low s, the
 * standardness rule). Returns its length (8 to 71) or 0 when r or s is zero or not below the group order n. */
size_t btc_der_encode(const uint8_t r[32], const uint8_t s[32], uint8_t der[BTC_DER_MAX]);

/* Signs input i, which spends the P2PKH output of the key seckey (the scriptCode is the P2PKH script of its compressed public key):
 * ECDSA of libsecp256k1 with its default nonces (RFC 6979, deterministic), low s, SIGHASH_ALL; the scriptSig becomes the signature
 * with its hashtype byte and the compressed public key. False when i is out of range or seckey is not a valid key. */
bool btc_sign_p2pkh(btc_tx *tx, size_t i, const uint8_t seckey[32]);
/* Puts in place a signature made elsewhere (the two-party signer) as (r, s): the same scriptSig as btc_sign_p2pkh, with the
 * compressed key pub. It is not checked here: btc_verify_p2pkh does that. False when i is out of range, pub is not a compressed key
 * or (r, s) cannot be encoded. */
bool btc_put_sig_p2pkh(btc_tx *tx, size_t i, const uint8_t r[32], const uint8_t s[32], const uint8_t pub[33]);
/* Whether input i is a valid, standard spend of the P2PKH output prev_spk: a scriptSig of exactly two pushes, a strict-DER signature
 * with low s and SIGHASH_ALL, a compressed or uncompressed key whose hash160 is the one of prev_spk, and the signature valid for the
 * legacy signature hash. */
bool btc_verify_p2pkh(const btc_tx *tx, size_t i, const uint8_t *prev_spk, size_t spk_len);
/* ECDSA verification of a strict-DER signature (without hashtype) with low s against a 33- or 65-byte public key */
bool btc_ecdsa_verify(const uint8_t digest[32], const uint8_t *der, size_t der_len, const uint8_t *pub, size_t pub_len);

/* ---- size and fee ---- */

/* the most a signed P2PKH input takes: 36 bytes of outpoint, a 1-byte length, a scriptSig of 107 (a push of a signature of at most
 * 71 bytes, r of 33 with its sign byte and a low s of 32, and its hashtype, a push of a compressed key), 4 of sequence */
#define BTC_P2PKH_IN_SIZE 148
/* The virtual size of a transaction of n_in signed P2PKH inputs and the given outputs, an upper bound: a legacy transaction has no
 * witness, so its virtual size is its size, and the signatures are counted at their longest. */
size_t btc_vsize_p2pkh(size_t n_in, const btc_out *out, size_t n_out);
/* the fee of vsize virtual bytes at sat_per_kvb satoshis per 1000, rounded up */
uint64_t btc_fee(size_t vsize, uint64_t sat_per_kvb);

#endif
