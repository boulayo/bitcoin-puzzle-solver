/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* session.h - the session of a coincidence between the two homes (docs/en/NETWORK.md 7.5), from the hello to the payout, in the thread of its
 * caller. The channel and the chain are given as interfaces: the encrypted channel of pzpeer and bitcoind in the program, memory in the tests. */
#ifndef PZPEER_SESSION_H
#define PZPEER_SESSION_H

#include "payout.h"
#include "pieces.h"
#include <pthread.h>

#define SESSION_MSG_MAX 65535               /* a Noise frame */

/* The messages of a session (the first byte of a frame; docs/en/NETWORK.md 7.4 keeps 16 to 31 for them). S_ALIVE keeps the channel busy
 * while a session waits for blocks, and is skipped by the reader. */
enum { S_HELLO = 16, S_PROOF, S_PLAN, S_SIGNER, S_SIGNED, S_TINYSIG, S_ABORT, S_ALIVE };

typedef struct {
    void *ctx;
    bool (*send)(void *ctx, const uint8_t *msg, size_t len);
    /* the next message, at most cap bytes; false at the end of the channel or after timeout_ms */
    bool (*recv)(void *ctx, uint8_t *msg, size_t cap, size_t *len, int timeout_ms);
} session_chan;

typedef struct {
    void *ctx;
    /* the confirmed coins of a P2PKH script, their values verified from the transactions that made them; -1 when the chain cannot be asked */
    int (*coins)(void *ctx, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out, int max);
    bool (*broadcast)(void *ctx, const btc_tx *tx);
    /* the confirmations of an output: -1 spent or unknown, 0 in the mempool, n in the n-th block from the top; -2 when the chain did not
     * answer (never taken for spent: F would not cancel) */
    int (*confirmations)(void *ctx, const uint8_t txid[32], uint32_t vout);
    uint64_t (*height)(void *ctx);
    uint64_t (*feerate)(void *ctx);         /* satoshis per 1000 virtual bytes for a confirmation within a few blocks */
    /* one output of a P2PKH script (a prize of the network file): 1 when it is confirmed, unspent and spendable, with its value in out;
     * 0 when it is not; -1 when the chain cannot be asked. NULL: not offered */
    int (*coin)(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out);
} session_chain;

#define SESSION_PRIZES_MAX 16

/* The limit of failed signings of a home (docs/en/NETWORK.md 7.5, project 4.6). A bug of the two-party signer could leak a bit of a piece
 * through each signing that fails, so every run of pzsign counts as failed, in a file, before it starts, and is taken off once it gave valid
 * signatures; at max failures on the network, or pair_max for one pair, there are no more signings. The file survives a restart and a crash
 * in the middle of a run. Runs are one at a time (busy): a session that does not get its turn within two minutes gives up without one. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool busy;                              /* a run of pzsign is under way */
    const char *path;                       /* lines "<hex of F or of a session id> <failures>" */
    unsigned max, pair_max;
} session_limit;

typedef struct {
    const secp256k1_context *ctx;
    const record_net *net;
    uint8_t base[32];                       /* the start of the range, big-endian */
    uint8_t pubkey33[33];                   /* the key of the puzzle */
    const secp256k1_keypair *identity;      /* of this home */
    uint8_t partner[32];                    /* the x-only identity at the other end of the channel (its Noise static key) */
    record mine, theirs;                    /* the pair, as the alert gave it */
    uint8_t d[32];                          /* the distance of mine (d mod n, big-endian) */
    uint8_t coin_sec[32];                   /* the key of the tiny coin of this home for this session */
    uint8_t pay_spk[BTC_SPK_MAX];           /* the payout address of this home */
    size_t pay_len;
    uint64_t max_feerate;                   /* the highest fee rate this home accepts (satoshis per 1000 virtual bytes) */
    payout_coin prizes[SESSION_PRIZES_MAX]; /* the prize= outpoints of the network file (their values are the chain's) */
    int nprizes;
    const char *pzsign, *primes;           /* the signer and the primes file of this session (made by the session when it is missing) */
    session_limit *limit;                   /* NULL: no limit (tests) */
    bool *proved;                           /* set when the partner has proved its distance (NULL: not wanted) */
    const session_chan *chan;
    const session_chain *chain;
    int timeout_ms;                         /* the longest wait for one message of the partner */
    int poll_ms;                            /* how often the chain is asked while waiting for a block */
    int alive_ms;                           /* how often S_ALIVE goes to the partner while waiting for a block (0: every minute) */
    int block_ms;                           /* a wait for n blocks gives up after (n + 6) times this, if the chain stops (0: 20 minutes) */
    int cancel_blocks;                      /* the blocks F waits for the second transaction before it cancels (3) */
    bool withhold;                          /* tests only: as S, do not send the second transaction (the test channel also drops S's last message) */
} session_cfg;

typedef enum { SESSION_PAID, SESSION_CANCELLED, SESSION_FAILED } session_result;

/* Runs the session. why gets the reason of a failure, or what happened. */
session_result session_run(const session_cfg *cfg, char *why, size_t whylen);

#endif
