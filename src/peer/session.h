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
#define SESSION_HELLO_MAX 512               /* the first message of a session: a hello, 427 bytes at most */

/* The messages of a session (the first byte of a frame; docs/en/NETWORK.md 7.4 keeps 16 to 31 for them). S_ALIVE keeps the channel busy
 * while a session waits for blocks, and is skipped by the reader. After the payout S_SECOND carries the second transaction (empty from
 * a side that does not have it) and S_PIECE the secret piece of a side. */
enum { S_HELLO = 16, S_PROOF, S_PLAN, S_SIGNER, S_SIGNED, S_TINYSIG, S_ABORT, S_ALIVE, S_PIECE, S_SECOND };

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
    /* the confirmations of an output: -1 spent in a block or unknown, 0 in the mempool, n in the n-th block from the top; -2 when the chain
     * did not answer (never taken for spent: F would not cancel). A spend that is not in a block does not count: it can still leave the
     * mempools, and S hold the second transaction again */
    int (*confirmations)(void *ctx, const uint8_t txid[32], uint32_t vout);
    uint64_t (*height)(void *ctx);
    uint64_t (*feerate)(void *ctx);         /* satoshis per 1000 virtual bytes for a confirmation within a few blocks */
    /* one output of a P2PKH script (a prize of the network file): 1 when it is confirmed, unspent and spendable, with its value in out;
     * 0 when it is not; -1 when the chain cannot be asked. NULL: not offered */
    int (*coin)(void *ctx, const uint8_t txid[32], uint32_t vout, const uint8_t spk[BTC_P2PKH_LEN], payout_coin *out);
    /* the transactions of the mempool that pay to or spend from a P2PKH script, -1 when the chain cannot be asked: a light chain does not
     * show a coin whose spend waits in the mempool. NULL: coins shows those coins (a node's scan of its set) */
    int (*pending)(void *ctx, const uint8_t spk[BTC_P2PKH_LEN]);
} session_chain;

#define SESSION_PRIZES_MAX 16

/* The limit of failed signings of a home (docs/en/NETWORK.md 7.5, project 4.6). A bug of the two-party signer could leak a bit of a piece
 * through each signing that fails, so every run of pzsign counts as failed, in a file, before it starts, and is taken off once it gave valid
 * signatures; at max failures on the network, or pair_max for one own point (with any partner), there are no more signings. The file survives a restart and a crash
 * in the middle of a run. Runs are one at a time (busy): a session that does not get its turn within two minutes gives up without one. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool busy;                              /* a run of pzsign is under way */
    const char *path;                       /* lines "<hex of the network fp, or of a hash of it and an own point> <failures>" */
    unsigned max, pair_max;
    unsigned pay_max;                       /* payouts of one own point that did not pay (each spent a fee and a tiny coin of the prize) */
} session_limit;

/* The payout of a pair and its key, kept by the caller from one session of the pair to the next. A session that sees the prize paid sets
 * paid_at and first; one that starts with paid_at set pays nothing: it only exchanges the pieces. Once the payout has its confirmations
 * and nothing of value is left at the puzzle address, the two sides give each other their pieces, and each has the key. */
#define PAID_TX_MAX 600                     /* a second transaction: two signed P2PKH inputs and three outputs, about 430 bytes */
typedef struct {
    uint64_t paid_at;                       /* the height at which the prize was seen spent in a block; 0: not paid */
    uint8_t first[32];                      /* the txid of the first transaction, whose output 0 the payout spent */
    uint8_t second[PAID_TX_MAX];            /* the second transaction, signed (second_len 0: not known here yet) */
    size_t second_len;
    bool told;                              /* the payout was shown on the terminal */
    bool have_key;
    uint8_t key[32];                        /* the key of the puzzle, the sum of the two pieces (have_key); the caller wipes it */
    bool empty;                             /* out: the chain shows the puzzle address empty, and no prize of the network file: taken */
} session_paid;

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
    uint64_t min_feerate, max_feerate;      /* the fee rate this home proposes is twice the estimate of its chain, within these (satoshis
                                             * per 1000 virtual bytes); the higher of the two homes is taken, and one above max ends the session */
    uint64_t feerate;                       /* the estimate of the chain, asked before the channel opened (the partner waits seconds for
                                             * the hello, and a chain without a node can take longer); 0: asked at the hello */
    payout_coin prizes[SESSION_PRIZES_MAX]; /* the prize= outpoints of the network file (their values are the chain's) */
    int nprizes;
    const char *pzsign;                    /* the signer */
    const char *pzsign_log;                 /* where its messages go (NULL: where pzpeer's go) */
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
    session_paid *paid;                     /* the payout of the pair, read and written (NULL: the pieces are not exchanged) */
    int reveal_conf;                        /* the confirmations of the payout before the pieces are exchanged (0: 6) */
    void (*say)(void *user, const char *line);   /* the steps that matter to the owner, one short line each (NULL: none) */
    void (*on_paid)(void *user, const session_paid *p);   /* the payout is known (paid_at, first, second): to keep it (NULL: none) */
    void *user;
} session_cfg;

typedef enum { SESSION_PAID, SESSION_CANCELLED, SESSION_FAILED } session_result;

/* Runs the session. why gets the reason of a failure, or what happened. A session that only exchanges the pieces (paid->paid_at set) gives
 * SESSION_PAID, and paid->have_key tells whether it got the key. */
session_result session_run(const session_cfg *cfg, char *why, size_t whylen);

#endif
