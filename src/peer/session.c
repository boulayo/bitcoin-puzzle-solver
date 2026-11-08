/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* session.c - the session of a coincidence (see session.h). The steps, each a message both sides send and then read:
 *   hello   the pair, a nonce, the fee rate proposed, the tiny-coin and payout scripts;
 *   proof   the proof of possession of the own record, over the partner's nonce (it shows the distance, hence the piece);
 *   plan    the hash of the first transaction each side built on its own; then the two-party signing, the broadcast, one confirmation;
 *   plan    the hash of the second; F gives the signature of its tiny coin, then the two-party signing with F sending first;
 *   then S, who has the whole signature first, broadcasts; F waits cancel_blocks blocks for the prize to move and else cancels.
 * Party 0 owns the smaller identity; F (the party whose tiny coin goes into the second transaction) is the last bit of the session id. */
#include "session.h"
#include "json.h"
#include "signer.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <pthread.h>
#include <secp256k1_extrakeys.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HELLO_FIXED (2 * RECORD_LEN + 32 + 8 + BTC_P2PKH_LEN + 1)
#define CHUNK 65000                         /* a piece of a pzfirma message in one S_SIGNER */

typedef struct {
    const session_cfg *c;
    record pair[2];
    int me, F;
    uint8_t eid[32], my_x[32];
    payout_terms t;
    char *why;
    size_t whylen;
} sess;

static bool fail(sess *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->why, s->whylen, fmt, ap);
    va_end(ap);
    uint8_t m[1 + 200] = {S_ABORT};
    const size_t n = strlen(s->why) < 200 ? strlen(s->why) : 200;
    memcpy(m + 1, s->why, n);
    (void)s->c->chan->send(s->c->chan->ctx, m, 1 + n);         /* best effort: the partner stops at once instead of waiting */
    return false;
}

static bool send_msg(sess *s, uint8_t type, const uint8_t *p, size_t len)
{
    uint8_t m[SESSION_MSG_MAX];
    if (len + 1 > sizeof m) return false;
    m[0] = type;
    memcpy(m + 1, p, len);
    return s->c->chan->send(s->c->chan->ctx, m, len + 1);
}

/* the next message, which must be of the given type; an abort of the partner gives its reason */
static bool recv_msg(sess *s, uint8_t type, uint8_t *p, size_t cap, size_t *len)
{
    uint8_t m[SESSION_MSG_MAX];
    size_t n;
    if (!s->c->chan->recv(s->c->chan->ctx, m, sizeof m, &n, s->c->timeout_ms) || n == 0) return fail(s, "the partner did not answer");
    if (m[0] == S_ABORT) {
        snprintf(s->why, s->whylen, "the partner stopped: %.*s", (int)(n - 1 < 200 ? n - 1 : 200), (const char *)m + 1);
        return false;
    }
    if (m[0] != type || n - 1 > cap) return fail(s, "an unexpected message (type %d)", m[0]);
    memcpy(p, m + 1, n - 1);
    *len = n - 1;
    return true;
}

/* ---- the hello and the proofs ---- */

static void my_nonce(const sess *s, uint8_t out[32])        /* unpredictable to the partner, new for every pair, no randomness needed */
{
    uint8_t m[12 + 32 + 32];
    memcpy(m, "pznet1/nonce", 12);
    memcpy(m + 12, s->c->coin_sec, 32);
    memcpy(m + 44, s->eid, 32);
    pz_sha256(m, sizeof m, out);
}

static bool p2pkh_of(const secp256k1_context *ctx, const uint8_t sec[32], uint8_t spk[BTC_P2PKH_LEN])
{
    secp256k1_pubkey p;
    uint8_t pub[33], h[20];
    size_t n = 33;
    if (!secp256k1_ec_pubkey_create(ctx, &p, sec) || !secp256k1_ec_pubkey_serialize(ctx, pub, &n, &p, SECP256K1_EC_COMPRESSED)) return false;
    pz_hash160(pub, 33, h);
    btc_script_p2pkh(h, spk);
    return true;
}

/* a payout script of a standard address: P2PKH, P2SH, P2WPKH, P2WSH or P2TR (anything else would make the transaction non-standard) */
static bool standard_spk(const uint8_t *p, size_t n)
{
    return (n == 25 && p[0] == 0x76 && p[1] == 0xa9 && p[2] == 20 && p[23] == 0x88 && p[24] == 0xac) ||
           (n == 23 && p[0] == 0xa9 && p[1] == 20 && p[22] == 0x87) || (n == 22 && p[0] == 0 && p[1] == 20) ||
           (n == 34 && (p[0] == 0 || p[0] == 0x51) && p[1] == 32);
}

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

static bool hello(sess *s)
{
    const session_cfg *c = s->c;
    secp256k1_xonly_pubkey xo;
    if (!secp256k1_keypair_xonly_pub(c->ctx, &xo, NULL, c->identity) || !secp256k1_xonly_pubkey_serialize(c->ctx, s->my_x, &xo))
        return fail(s, "no identity");
    if (memcmp(c->mine.owner, s->my_x, 32) || memcmp(c->theirs.owner, c->partner, 32)) return fail(s, "the records are not of the two homes");
    if (!record_check(c->ctx, c->net, &c->mine) || !record_check(c->ctx, c->net, &c->theirs) || !pieces_order(&c->mine, &c->theirs, s->pair))
        return fail(s, "not a useful coincidence");
    s->me = memcmp(s->pair[0].owner, s->my_x, 32) == 0 ? 0 : 1;
    pieces_eid(c->net, s->pair, s->eid);
    s->F = s->eid[31] & 1;
    uint8_t m[HELLO_FIXED + BTC_SPK_MAX], in[sizeof m], nonce[32], coin_spk[BTC_P2PKH_LEN];
    size_t n;
    my_nonce(s, nonce);
    if (!p2pkh_of(c->ctx, c->coin_sec, coin_spk)) return fail(s, "invalid coin key");
    const uint64_t fr = c->chain->feerate(c->chain->ctx);
    record_encode(&c->mine, m);
    record_encode(&c->theirs, m + RECORD_LEN);
    memcpy(m + 2 * RECORD_LEN, nonce, 32);
    for (int b = 0; b < 8; b++) m[2 * RECORD_LEN + 32 + b] = (uint8_t)(fr >> (56 - 8 * b));
    memcpy(m + 2 * RECORD_LEN + 40, coin_spk, BTC_P2PKH_LEN);
    m[HELLO_FIXED - 1] = (uint8_t)c->pay_len;
    memcpy(m + HELLO_FIXED, c->pay_spk, c->pay_len);
    if (!send_msg(s, S_HELLO, m, HELLO_FIXED + c->pay_len) || !recv_msg(s, S_HELLO, in, sizeof in, &n)) return false;
    if (n < HELLO_FIXED || n != (size_t)HELLO_FIXED + in[HELLO_FIXED - 1] || memcmp(in, m + RECORD_LEN, RECORD_LEN) || memcmp(in + RECORD_LEN, m, RECORD_LEN))
        return fail(s, "the hello of the partner is not about the same pair");
    /* the terms: both sides fill them the same way from the two hellos */
    payout_terms *t = &s->t;
    const int you = 1 - s->me;
    uint8_t h160[20];
    pz_hash160(c->pubkey33, 33, h160);
    btc_script_p2pkh(h160, t->puzzle_spk);
    memcpy(t->coin_spk[s->me], coin_spk, BTC_P2PKH_LEN);
    memcpy(t->coin_spk[you], in + 2 * RECORD_LEN + 40, BTC_P2PKH_LEN);
    if (!btc_script_is_p2pkh(t->coin_spk[you], BTC_P2PKH_LEN, h160)) return fail(s, "the tiny coin of the partner is not P2PKH");
    memcpy(t->pay_spk[s->me], c->pay_spk, c->pay_len), t->pay_len[s->me] = c->pay_len;
    memcpy(t->pay_spk[you], in + HELLO_FIXED, in[HELLO_FIXED - 1]), t->pay_len[you] = in[HELLO_FIXED - 1];
    if (!standard_spk(t->pay_spk[you], t->pay_len[you]) || !standard_spk(c->pay_spk, c->pay_len)) return fail(s, "a payout address is not standard");
    const uint64_t theirs = be64(in + 2 * RECORD_LEN + 32);
    t->feerate = fr > theirs ? fr : theirs;
    if (t->feerate > c->max_feerate) return fail(s, "fee rate %llu above the limit %llu", (unsigned long long)t->feerate, (unsigned long long)c->max_feerate);
    t->first = s->F;
    /* the proofs of possession: each over the nonce of the other */
    uint8_t proof[64], their_proof[64];
    if (!record_prove(c->ctx, c->net, &c->mine, c->d, c->partner, in + 2 * RECORD_LEN, proof)) return fail(s, "the distance does not fit the record");
    if (!send_msg(s, S_PROOF, proof, 64) || !recv_msg(s, S_PROOF, their_proof, 64, &n)) return false;
    if (n != 64 || !record_proof_ok(c->ctx, c->net, &c->theirs, s->my_x, nonce, their_proof)) return fail(s, "the partner did not prove its distance");
    return true;
}

/* ---- the plan of a transaction: both build it, both compare its hash ---- */

static bool plan(sess *s, const btc_tx *tx)
{
    uint8_t raw[BTC_TX_MAX_SIZE], m[11 + BTC_TX_MAX_SIZE], h[32], theirs[32];
    const size_t n = btc_tx_serialize(tx, raw, sizeof raw);
    size_t got;
    memcpy(m, "pznet1/plan", 11);
    memcpy(m + 11, raw, n);
    pz_sha256(m, 11 + n, h);
    if (!n || !send_msg(s, S_PLAN, h, 32) || !recv_msg(s, S_PLAN, theirs, 32, &got)) return false;
    return got == 32 && !memcmp(h, theirs, 32) ? true : fail(s, "the two homes built different transactions");
}

/* ---- the two-party signing: pzfirma here, its messages relayed to the partner's ---- */

typedef struct {
    sess *s;
    signer sg;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int in_count;                           /* whole messages of the partner given to pzfirma */
    bool in_done, failed;
    uint8_t *result;                        /* the last frame of pzfirma, 0x82 or 0x83 */
    uint8_t result_type;
} relay;

static bool send_chunks(relay *r, const uint8_t *p, size_t len)
{
    uint8_t m[1 + CHUNK];
    do {
        const size_t n = len < CHUNK ? len : CHUNK;
        m[0] = len > CHUNK;
        memcpy(m + 1, p, n);
        if (!send_msg(r->s, S_SIGNER, m, 1 + n)) return false;
        p += n, len -= n;
    } while (len);
    return true;
}

/* pzfirma to the partner. S sends its k-th message only after the k-th of F has arrived: F's last message reaches S before S's reaches F,
 * so S has the whole signature first and F, whose tiny coin is in the transaction, holds the cancel. */
static void *relay_out(void *arg)
{
    relay *r = arg;
    const bool S = r->s->me != r->s->F;
    int out_count = 0;
    for (;;) {
        uint8_t type, *p;
        size_t len;
        if (!signer_recv(&r->sg, &type, &p, &len, r->s->c->timeout_ms)) break;
        if (type != SIGNER_OUT) {
            r->result = p, r->result_type = type;
            break;
        }
        if (S) {
            pthread_mutex_lock(&r->mu);
            while (r->in_count <= out_count && !r->in_done) pthread_cond_wait(&r->cv, &r->mu);
            pthread_mutex_unlock(&r->mu);
        }
        out_count++;
        const bool ok = send_chunks(r, p, len);
        free(p);
        if (!ok) break;
    }
    const uint8_t st = r->result_type == SIGNER_RESULT;
    (void)send_msg(r->s, S_SIGNED, &st, 1);                     /* the end of this side's messages */
    return NULL;
}

/* the partner to pzfirma: pieces joined into whole messages */
static void *relay_in(void *arg)
{
    relay *r = arg;
    uint8_t *msg = NULL, m[SESSION_MSG_MAX];
    size_t len = 0;
    for (;;) {
        size_t n;
        if (!r->s->c->chan->recv(r->s->c->chan->ctx, m, sizeof m, &n, r->s->c->timeout_ms) || n < 1) break;
        if (m[0] == S_SIGNED || m[0] == S_ABORT) break;
        if (m[0] != S_SIGNER || n < 2 || len + n - 2 > SIGNER_FRAME_MAX) {
            r->failed = true;
            break;
        }
        uint8_t *grown = realloc(msg, len + n - 2 + 1);
        if (!grown) break;
        msg = grown;
        memcpy(msg + len, m + 2, n - 2);
        len += n - 2;
        if (m[1]) continue;                                     /* more pieces follow */
        const bool ok = signer_send(&r->sg, SIGNER_IN, msg, len);
        len = 0;
        if (!ok) break;
        pthread_mutex_lock(&r->mu);
        r->in_count++;
        pthread_cond_signal(&r->cv);
        pthread_mutex_unlock(&r->mu);
    }
    free(msg);
    pthread_mutex_lock(&r->mu);
    r->in_done = true;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mu);
    close(r->sg.to), r->sg.to = -1;                             /* nothing more for pzfirma: at the end of its input it gives its answer */
    return NULL;
}

static bool hex32(const char *h, uint8_t out[32]) { return h && strlen(h) == 64 && btc_unhex(h, 64, out); }

/* signs the first n inputs of tx (which spend the puzzle address) with the partner; true when every one of them is a valid spend */
static bool sign_jointly(sess *s, btc_tx *tx, size_t n, char tag)
{
    const session_cfg *c = s->c;
    uint8_t digests[PAYOUT_MAX_COINS][32], piece[32], pub[2][33], run[12 + 32 + 1];
    if (n > PAYOUT_MAX_COINS || !payout_digests(tx, s->t.puzzle_spk, n, digests)) return fail(s, "no digests");
    for (int i = 0; i < 2; i++) {
        secp256k1_pubkey X;
        size_t l = 33;
        if (!pieces_public(c->ctx, c->net, c->base, s->pair, i, &X) || !secp256k1_ec_pubkey_serialize(c->ctx, pub[i], &l, &X, SECP256K1_EC_COMPRESSED))
            return fail(s, "no public piece");
    }
    if (!pieces_secret(c->ctx, c->net, c->base, s->pair, s->me, c->d, piece)) return fail(s, "no piece");
    memcpy(run, "pznet1/run/", 11), run[11] = (uint8_t)tag;     /* each signing of the session has an id of its own */
    memcpy(run + 12, s->eid, 32);
    uint8_t eid[32];
    pz_sha256(run, 44, eid);
    char *job = signer_job(s->me, eid, piece, (const uint8_t(*)[33])pub, c->pubkey33, c->primes, (const uint8_t(*)[32])digests, (int)n);
    pz_secure_zero(piece, sizeof piece);
    relay r = {.s = s};
    pthread_mutex_init(&r.mu, NULL);
    pthread_cond_init(&r.cv, NULL);
    bool ok = job && signer_start(&r.sg, c->pzfirma) && signer_send(&r.sg, SIGNER_JOB, (const uint8_t *)job, strlen(job));
    if (job) pz_secure_zero(job, strlen(job)), free(job);
    if (ok) {
        pthread_t a, b;
        pthread_create(&a, NULL, relay_out, &r);
        pthread_create(&b, NULL, relay_in, &r);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
    }
    signer_stop(&r.sg);
    pthread_mutex_destroy(&r.mu);
    pthread_cond_destroy(&r.cv);
    ok = ok && r.result && r.result_type == SIGNER_RESULT && !r.failed;
    json_doc doc = {0};
    const json_val *sigs = ok && json_parse(&doc, (const char *)r.result, strlen((const char *)r.result)) ? json_get(doc.v, "sigs") : NULL;
    for (size_t i = 0; ok && i < n; i++) {
        const json_val *e = json_at(sigs, i);
        uint8_t rr[32], ss[32];
        ok = e && hex32(json_str(json_get(e, "r")), rr) && hex32(json_str(json_get(e, "s")), ss) && btc_put_sig_p2pkh(tx, i, rr, ss, c->pubkey33) &&
             btc_verify_p2pkh(tx, i, s->t.puzzle_spk, BTC_P2PKH_LEN);
    }
    if (sigs || doc.v) json_free(&doc);
    free(r.result);
    return ok;
}

/* ---- the chain ---- */

static void pause_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* waits until output vout of txid has at least `conf` confirmations (or, conf < 0, until it is spent), for at most `blocks` new blocks */
static bool wait_for(sess *s, const uint8_t txid[32], uint32_t vout, int conf, uint64_t blocks)
{
    const session_chain *ch = s->c->chain;
    const uint64_t until = ch->height(ch->ctx) + blocks;
    for (;;) {
        const int k = ch->confirmations(ch->ctx, txid, vout);   /* -2: the chain did not answer, which says nothing */
        if (k != -2 && (conf < 0 ? k == -1 : k >= conf)) return true;
        if (ch->height(ch->ctx) >= until) return false;
        pause_ms(s->c->poll_ms);
    }
}

session_result session_run(const session_cfg *c, char *why, size_t whylen)
{
    sess s = {.c = c, .why = why, .whylen = whylen};
    why[0] = 0;
    if (!hello(&s)) return SESSION_FAILED;
    const session_chain *ch = c->chain;
    /* the first transaction: the coins of the puzzle back to its address, and the tiny coins */
    payout_coin coins[64];
    const int nc = ch->coins(ch->ctx, s.t.puzzle_spk, coins, 64);
    btc_tx a, b, x;
    size_t used;
    if (nc <= 0 || !payout_first(&s.t, coins, (size_t)nc, &a, &used)) return fail(&s, "no coin of the puzzle to spend"), SESSION_FAILED;
    if (!plan(&s, &a) || !sign_jointly(&s, &a, a.n_in, 'A')) return SESSION_FAILED;
    uint8_t aid[32];
    btc_tx_txid(&a, aid);
    (void)ch->broadcast(ch->ctx, &a);                           /* both send it: the second to arrive is a duplicate */
    if (!wait_for(&s, aid, 0, 1, 144)) return fail(&s, "the first transaction was not confirmed"), SESSION_FAILED;
    /* the second: F gives the signature of its tiny coin, then the joint signature, F sending first */
    if (!payout_second(&s.t, &a, &b) || !plan(&s, &b)) return SESSION_FAILED;
    uint8_t m[1 + BTC_SCRIPTSIG_MAX];
    size_t n;
    if (s.me == s.F) {
        if (!btc_sign_p2pkh(&b, 1, c->coin_sec) || !send_msg(&s, S_TINYSIG, b.in[1].script, b.in[1].script_len)) return SESSION_FAILED;
    } else {
        if (!recv_msg(&s, S_TINYSIG, m, BTC_SCRIPTSIG_MAX, &n)) return SESSION_FAILED;
        memcpy(b.in[1].script, m, n), b.in[1].script_len = (uint8_t)n;
        if (!btc_verify_p2pkh(&b, 1, s.t.coin_spk[s.F], BTC_P2PKH_LEN)) return fail(&s, "the tiny coin of the partner is not signed"), SESSION_FAILED;
    }
    const bool signed_b = sign_jointly(&s, &b, 1, 'B');
    if (s.me != s.F) {                                          /* S has the whole signature first: it sends the transaction */
        if (!signed_b) return snprintf(why, whylen, "the joint signature of the second transaction failed"), SESSION_FAILED;
        if (c->withhold) return snprintf(why, whylen, "withheld the second transaction"), SESSION_FAILED;
        (void)ch->broadcast(ch->ctx, &b);
        return wait_for(&s, aid, 0, -1, 144) ? (snprintf(why, whylen, "paid"), SESSION_PAID)
                                            : (snprintf(why, whylen, "the second transaction did not go through"), SESSION_FAILED);
    }
    if (signed_b) (void)ch->broadcast(ch->ctx, &b);             /* F has it too when S sent its last message: either may send it */
    if (wait_for(&s, aid, 0, -1, (uint64_t)c->cancel_blocks)) return snprintf(why, whylen, "paid"), SESSION_PAID;
    /* F: the second transaction did not appear: the tiny coin goes home and the held transaction is worth nothing */
    if (!payout_cancel(&s.t, &a, &x) || !btc_sign_p2pkh(&x, 0, c->coin_sec) || !ch->broadcast(ch->ctx, &x))
        return snprintf(why, whylen, "the cancel could not be sent"), SESSION_FAILED;
    snprintf(why, whylen, "the partner held the second transaction back: cancelled with the tiny coin");
    return SESSION_CANCELLED;
}
