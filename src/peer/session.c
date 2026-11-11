/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* session.c - the session of a coincidence (see session.h). The steps, each a message both sides send and then read:
 *   hello   the pair, a nonce, the fee rate proposed, the tiny-coin and payout scripts;
 *   proof   the proof of possession of the own record, over the partner's nonce (it shows the distance, hence the piece);
 *   plan    the hash of the first transaction each side built on its own; then the primes of pzsign (only now: they cost a minute of
 *           CPU, and a partner has proved its distance), the two-party signing, the broadcast, one confirmation;
 *   plan    the hash of the second; F gives the signature of its tiny coin, then the two-party signing in which S gets the whole signature
 *           first; then S broadcasts, and F waits cancel_blocks blocks for the prize to move and else cancels.
 * Party 0 owns the smaller identity; F (the party whose tiny coin goes into the second transaction) is the last bit of the session id. */
#include "dbg.h"
#include "session.h"
#include "json.h"
#include "net.h"
#include "signer.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <errno.h>
#include <secp256k1_extrakeys.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELLO_FIXED (2 * RECORD_LEN + 32 + 8 + BTC_P2PKH_LEN + 1)
#define CHUNK 65000                         /* a piece of a pzsign message in one S_SIGNER */
#define HELLO_MS 30000                      /* the longest wait for the hello and the proof of the partner, which it makes at once */
#define LIMIT_LINES 1024                    /* lines of the file of the limit: one per network, one per pair that failed */
#define LIMIT_WAIT_S 120                    /* the longest wait for the turn of a signing (a partner waits 300 s for a message) */
#define ALIVE_MS 60000                      /* S_ALIVE while waiting for blocks: an idle channel is cut by NATs and relays */
#define BLOCK_MS (20 * 60000)               /* twice the mean time of a block: the measure of a wait when the chain stops */

typedef struct {
    const session_cfg *c;
    record pair[2];
    int me, F;
    uint8_t eid[32], my_x[32];
    uint8_t nonce[2][32];                   /* of party 0 and party 1: fresh in every session */
    int wait_ms;                            /* the wait for a message of the partner now */
    payout_terms t;
    char *why;
    size_t whylen;
    char tag[17];
} sess;

static bool fail(sess *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->why, s->whylen, fmt, ap);
    va_end(ap);
    dbg("session %s stops: %s", s->tag, s->why);
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
    if (len) memcpy(m + 1, p, len);
    if (type != S_ALIVE) dbg("session %s > type %u len %zu", s->tag, type, len);
    return s->c->chan->send(s->c->chan->ctx, m, len + 1);
}

/* the next message, which must be of the given type; an abort of the partner gives its reason */
static bool recv_msg(sess *s, uint8_t type, uint8_t *p, size_t cap, size_t *len)
{
    uint8_t m[SESSION_MSG_MAX];
    size_t n;
    do
        if (!s->c->chan->recv(s->c->chan->ctx, m, sizeof m, &n, s->wait_ms) || n == 0) return fail(s, "the partner did not answer");
    while (m[0] == S_ALIVE);                                    /* sent while the partner waited for blocks */
    if (m[0] == S_ABORT) {                                      /* its words go into our log: printable characters only */
        char r[201];
        size_t k = 0;
        for (size_t i = 1; i < n && k < 200; i++) r[k++] = m[i] >= 0x20 && m[i] < 0x7f ? (char)m[i] : '?';
        r[k] = 0;
        snprintf(s->why, s->whylen, "the partner stopped: %s", r);
        dbg("session %s < abort: %s", s->tag, r);
        return false;
    }
    if (m[0] != type || n - 1 > cap) return fail(s, "an unexpected message (type %d)", m[0]);
    memcpy(p, m + 1, n - 1);
    *len = n - 1;
    dbg("session %s < type %u len %zu", s->tag, type, n - 1);
    return true;
}

/* ---- the hello and the proofs ---- */

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
    const int you = 1 - s->me;
    uint8_t m[HELLO_FIXED + BTC_SPK_MAX], in[sizeof m], coin_spk[BTC_P2PKH_LEN];
    size_t n;
    /* the nonce: the partner's proof is made over it, and with the partner's it makes the ids of the signings of this session, which
     * must never repeat (cggmp21: an execution id is assumed never to repeat, and a pair can have more than one session) */
    if (!sys_random(s->nonce[s->me], 32)) return fail(s, "no randomness from the system");
    if (!p2pkh_of(c->ctx, c->coin_sec, coin_spk)) return fail(s, "invalid coin key");
    /* twice what the chain estimates, and never little: these transactions move a prize, and no fee is large beside it */
    uint64_t fr = 2 * c->chain->feerate(c->chain->ctx);
    if (fr < c->min_feerate) fr = c->min_feerate;
    if (fr > c->max_feerate) fr = c->max_feerate;
    record_encode(&c->mine, m);
    record_encode(&c->theirs, m + RECORD_LEN);
    memcpy(m + 2 * RECORD_LEN, s->nonce[s->me], 32);
    for (int b = 0; b < 8; b++) m[2 * RECORD_LEN + 32 + b] = (uint8_t)(fr >> (56 - 8 * b));
    memcpy(m + 2 * RECORD_LEN + 40, coin_spk, BTC_P2PKH_LEN);
    m[HELLO_FIXED - 1] = (uint8_t)c->pay_len;
    memcpy(m + HELLO_FIXED, c->pay_spk, c->pay_len);
    if (!send_msg(s, S_HELLO, m, HELLO_FIXED + c->pay_len) || !recv_msg(s, S_HELLO, in, sizeof in, &n)) return false;
    if (n < HELLO_FIXED || n != (size_t)HELLO_FIXED + in[HELLO_FIXED - 1] || memcmp(in, m + RECORD_LEN, RECORD_LEN) || memcmp(in + RECORD_LEN, m, RECORD_LEN))
        return fail(s, "the hello of the partner is not about the same pair");
    memcpy(s->nonce[you], in + 2 * RECORD_LEN, 32);
    /* the terms: both sides fill them the same way from the two hellos */
    payout_terms *t = &s->t;
    uint8_t h160[20];
    pz_hash160(c->pubkey33, 33, h160);
    btc_script_p2pkh(h160, t->puzzle_spk);
    memcpy(t->coin_spk[s->me], coin_spk, BTC_P2PKH_LEN);
    memcpy(t->coin_spk[you], in + 2 * RECORD_LEN + 40, BTC_P2PKH_LEN);
    if (!btc_script_is_p2pkh(t->coin_spk[you], BTC_P2PKH_LEN, NULL)) return fail(s, "the tiny coin of the partner is not P2PKH");
    memcpy(t->pay_spk[s->me], c->pay_spk, c->pay_len), t->pay_len[s->me] = c->pay_len;
    memcpy(t->pay_spk[you], in + HELLO_FIXED, in[HELLO_FIXED - 1]), t->pay_len[you] = in[HELLO_FIXED - 1];
    if (!standard_spk(t->pay_spk[you], t->pay_len[you]) || !standard_spk(c->pay_spk, c->pay_len)) return fail(s, "a payout address is not standard");
    const uint64_t theirs = be64(in + 2 * RECORD_LEN + 32);
    t->feerate = fr > theirs ? fr : theirs;
    t->first = s->F;
    dbg("session %s terms: party %d, F is party %d, fee rate mine %llu theirs %llu sat/kvB, tiny coin %llu sat", s->tag, s->me, s->F,
        (unsigned long long)fr, (unsigned long long)theirs, (unsigned long long)payout_tiny(t));
    if (t->feerate > c->max_feerate) return fail(s, "fee rate %llu above the limit %llu", (unsigned long long)t->feerate, (unsigned long long)c->max_feerate);
    /* the proofs of possession: each over the nonce of the other */
    uint8_t proof[64], their_proof[64];
    if (!record_prove(c->ctx, c->net, &c->mine, c->d, c->partner, s->nonce[you], proof)) return fail(s, "the distance does not fit the record");
    if (!send_msg(s, S_PROOF, proof, 64) || !recv_msg(s, S_PROOF, their_proof, 64, &n)) return false;
    if (n != 64 || !record_proof_ok(c->ctx, c->net, &c->theirs, s->my_x, s->nonce[s->me], their_proof)) return fail(s, "the partner did not prove its distance");
    if (c->proved) *c->proved = true;
    return true;
}

/* ---- the plan of a transaction: both build it, both compare its hash ---- */

/* a transaction in the debug log, whole: it is public as soon as it is sent */
static void dbg_tx(const sess *s, const char *what, const btc_tx *tx)
{
    uint8_t raw[BTC_TX_MAX_SIZE];
    static _Thread_local char hex[2 * BTC_TX_MAX_SIZE + 1];
    const size_t n = btc_tx_serialize(tx, raw, sizeof raw);
    btc_hex(raw, n, hex);
    dbg("session %s tx %s: %s", s->tag, what, hex);
}

static bool plan(sess *s, const btc_tx *tx)
{
    uint8_t raw[BTC_TX_MAX_SIZE], m[11 + BTC_TX_MAX_SIZE], h[32], theirs[32];
    const size_t n = btc_tx_serialize(tx, raw, sizeof raw);
    size_t got;
    memcpy(m, "pznet1/plan", 11);
    memcpy(m + 11, raw, n);
    pz_sha256(m, 11 + n, h);
    dbg_tx(s, "plan", tx);
    if (!n || !send_msg(s, S_PLAN, h, 32) || !recv_msg(s, S_PLAN, theirs, 32, &got)) return false;
    return got == 32 && !memcmp(h, theirs, 32) ? true : fail(s, "the two homes built different transactions");
}

/* ---- the limit of failed signings (session_limit) ---- */

/* the lines of the file; false when it is there but cannot be read whole (no signing then: the count is not known) */
static bool limit_read(const char *path, uint8_t (*key)[32], unsigned *count, int *n)
{
    *n = 0;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT;
    char hex[65];
    unsigned v;
    int got;
    bool ok = true;
    while (ok && (got = fscanf(f, "%64s %u", hex, &v)) == 2) {
        ok = *n < LIMIT_LINES && strlen(hex) == 64 && btc_unhex(hex, 64, key[*n]);
        if (ok) count[(*n)++] = v;
    }
    ok = ok && got == EOF && !ferror(f);
    fclose(f);
    return ok;
}

/* through a new file, synced, that replaces the old one: a crash leaves either */
static bool limit_write(const char *path, uint8_t (*key)[32], const unsigned *count, int n)
{
    char tmp[1100], hex[65];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    for (int i = 0; ok && i < n; i++) btc_hex(key[i], 32, hex), ok = fprintf(f, "%s %u\n", hex, count[i]) > 0;
    ok = ok && fflush(f) == 0 && sys_sync(fileno(f));
    if (f) ok = fclose(f) == 0 && ok;
    return ok && sys_replace(tmp, path);
}

/* d (+1 or -1) to the failures of the network and of the pair; +1 is refused at either limit, and 0 only checks them. False with the
 * reason in why (mu held) */
static bool limit_add(const sess *s, int d, char *why, size_t whylen)
{
    const session_limit *l = s->c->limit;
    uint8_t key[LIMIT_LINES][32];
    unsigned count[LIMIT_LINES];
    int n;
    if (!limit_read(l->path, key, count, &n)) return snprintf(why, whylen, "the count of failed signings in %.200s cannot be read", l->path), false;
    const uint8_t *which[2] = {s->c->net->fp, s->eid};
    const unsigned max[2] = {l->max, l->pair_max};
    int at[2];
    for (int w = 0; w < 2; w++) {
        for (at[w] = 0; at[w] < n && memcmp(key[at[w]], which[w], 32); at[w]++) {}
        if (at[w] == n) {
            if (n == LIMIT_LINES) return snprintf(why, whylen, "the count of failed signings in %.200s is full", l->path), false;
            memcpy(key[n], which[w], 32), count[n++] = 0;
        }
        if (d >= 0 && count[at[w]] >= max[w])
            return snprintf(why, whylen, "%u failed signings %s: no more (the limit of a home, docs/en/NETWORK.md 7.5)", count[at[w]],
                            w ? "of this pair" : "on this network"), false;
    }
    if (!d) return true;
    for (int w = 0; w < 2; w++) count[at[w]] = d > 0 ? count[at[w]] + 1 : count[at[w]] ? count[at[w]] - 1 : 0;
    return limit_write(l->path, key, count, n) || (snprintf(why, whylen, "cannot write the count of failed signings to %.200s", l->path), false);
}

/* whether this home may still sign for the pair: asked before the plan, so that a home at its limit stops before the partner's pzsign
 * starts, which would then count a failure of its own */
static bool limit_ok(sess *s)
{
    session_limit *l = s->c->limit;
    char why[300];
    if (!l) return true;
    pthread_mutex_lock(&l->mu);
    const bool ok = limit_add(s, 0, why, sizeof why);
    pthread_mutex_unlock(&l->mu);
    return ok || fail(s, "%s", why);
}

/* before a run of pzsign: its turn (one at a time), and counted as failed until it gives valid signatures. The wait is bounded: two homes
 * with two pairs could each wait for the other's turn, and the partner of a session that waits too long gives up anyway. */
static bool limit_begin(sess *s)
{
    session_limit *l = s->c->limit;
    char why[300] = "another signing took too long";
    if (!l) return true;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += LIMIT_WAIT_S;
    pthread_mutex_lock(&l->mu);
    while (l->busy && pthread_cond_timedwait(&l->cv, &l->mu, &until) == 0) {}
    const bool ok = !l->busy && limit_add(s, +1, why, sizeof why);
    l->busy |= ok;
    pthread_mutex_unlock(&l->mu);
    return ok || fail(s, "%s", why);
}

static void limit_end(sess *s, bool ok)
{
    session_limit *l = s->c->limit;
    char why[300];
    if (!l) return;
    pthread_mutex_lock(&l->mu);
    if (ok) (void)limit_add(s, -1, why, sizeof why);
    l->busy = false;
    pthread_cond_signal(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

/* ---- the two-party signing: pzsign here, its messages relayed to the partner's ---- */

typedef struct {
    sess *s;
    signer sg;
    bool hold;                              /* S in the signing of the second transaction: see relay_out */
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int in_count;                           /* whole messages of the partner given to pzsign */
    bool in_done, failed;
    uint8_t *result;                        /* the JSON result of pzsign (0x82), when it gave one */
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

/* The envelope with which pzsign tells its partner why it gave up (signer/src/envelope.rs: version 1, the sender, phase 2): never relayed.
 * Its reason would tell the partner which check failed, and a failure must say nothing of where it happened (project 4.6); the S_SIGNED
 * that follows stops the partner as well. */
static bool is_abort(const uint8_t *p, size_t n) { return n >= 3 && p[0] == 1 && p[2] == 2; }

/* whether the k-th message of the partner was given to pzsign; false when the partner's messages ended before it */
static bool partner_gave(relay *r, int k)
{
    pthread_mutex_lock(&r->mu);
    while (r->in_count < k && !r->in_done) pthread_cond_wait(&r->cv, &r->mu);
    const bool ok = r->in_count >= k;
    pthread_mutex_unlock(&r->mu);
    return ok;
}

/* pzsign to the partner. In the signing of the second transaction S holds each of its messages until F's message of the same rank has
 * reached its pzsign and its pzsign went on past it, with its next message or with the signatures: F's last message reaches S before
 * S's last one leaves, and S's leaves only once S has the whole signature. A last message of F that is not one (garbage, an abort, the end
 * of F's stream) gets nothing in exchange. So S has the whole signature first, and F, whose tiny coin is in the transaction, holds the
 * cancel. In the first transaction, which only gives the prize back to the puzzle, both send at once. */
static void *relay_out(void *arg)
{
    relay *r = arg;
    uint8_t *held = NULL, *p = NULL, type;
    size_t held_len = 0, len;
    int out_count = 0;
    while (signer_recv(&r->sg, &type, &p, &len, r->s->wait_ms)) {
        const bool more = type == SIGNER_OUT && !is_abort(p, len);
        if (held && !((more || type == SIGNER_RESULT) && partner_gave(r, out_count) && send_chunks(r, held, held_len))) break;
        free(held), held = NULL;
        if (!more) {
            if (type == SIGNER_RESULT) r->result = p, p = NULL;
            break;
        }
        out_count++;
        if (r->hold) held = p, held_len = len;
        else if (!send_chunks(r, p, len)) break;
        else free(p);
        p = NULL;
    }
    free(held), free(p);
    const uint8_t st = r->result != NULL;
    (void)send_msg(r->s, S_SIGNED, &st, 1);                     /* the end of this side's messages */
    return NULL;
}

/* the partner to pzsign: pieces joined into whole messages */
static void *relay_in(void *arg)
{
    relay *r = arg;
    uint8_t *msg = NULL, m[SESSION_MSG_MAX];
    size_t len = 0;
    for (;;) {
        size_t n;
        if (!r->s->c->chan->recv(r->s->c->chan->ctx, m, sizeof m, &n, r->s->wait_ms) || n < 1) break;
        if (m[0] == S_ALIVE) continue;
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
    signer_end_input(&r->sg);                                   /* nothing more for pzsign: at the end of its input it gives its answer */
    return NULL;
}

static bool hex32(const char *h, uint8_t out[32]) { return h && strlen(h) == 64 && btc_unhex(h, 64, out); }

/* signs the first n inputs of tx (which spend the puzzle address) with the partner; true when every one of them is a valid spend */
static bool sign_jointly(sess *s, btc_tx *tx, size_t n, char tag)
{
    const session_cfg *c = s->c;
    uint8_t digests[PAYOUT_MAX_COINS][32], piece[32], pub[2][33], run[12 + 32 + 64];
    if (n > PAYOUT_MAX_COINS || !payout_digests(tx, s->t.puzzle_spk, n, digests)) return fail(s, "no digests");
    for (int i = 0; i < 2; i++) {
        secp256k1_pubkey X;
        size_t l = 33;
        if (!pieces_public(c->ctx, c->net, c->base, s->pair, i, &X) || !secp256k1_ec_pubkey_serialize(c->ctx, pub[i], &l, &X, SECP256K1_EC_COMPRESSED))
            return fail(s, "no public piece");
    }
    /* each signing has an id of its own, new in every session: the pair, the transaction, the nonces of both */
    memcpy(run, "pznet1/run/", 11), run[11] = (uint8_t)tag;
    memcpy(run + 12, s->eid, 32);
    memcpy(run + 44, s->nonce, 64);
    uint8_t eid[32];
    pz_sha256(run, sizeof run, eid);
    if (!pieces_secret(c->ctx, c->net, c->base, s->pair, s->me, c->d, piece)) return fail(s, "no piece");
    if (!limit_begin(s)) return pz_secure_zero(piece, sizeof piece), false;
    char *job = signer_job(s->me, eid, piece, (const uint8_t(*)[33])pub, c->pubkey33, c->primes, (const uint8_t(*)[32])digests, (int)n);
    pz_secure_zero(piece, sizeof piece);
    relay r = {.s = s, .hold = tag == 'B' && s->me != s->F};
    pthread_mutex_init(&r.mu, NULL);
    pthread_cond_init(&r.cv, NULL);
    bool ok = job && signer_start(&r.sg, c->pzsign) && signer_send(&r.sg, SIGNER_JOB, (const uint8_t *)job, strlen(job));
    if (job) pz_secure_zero(job, strlen(job)), free(job);
    if (ok) {
        pthread_t a, b;
        pthread_create(&a, NULL, relay_out, &r);
        pthread_create(&b, NULL, relay_in, &r);
        pthread_join(a, NULL);
        signer_kill(&r.sg);                                     /* it has nothing more to say: a write to it that waits (it stopped reading) ends */
        pthread_join(b, NULL);
    }
    signer_stop(&r.sg);
    pthread_mutex_destroy(&r.mu);
    pthread_cond_destroy(&r.cv);
    ok = ok && r.result && !r.failed;
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
    limit_end(s, ok);
    return ok;
}

/* ---- the chain ---- */

/* Waits until output vout of txid has at least `conf` confirmations (or, conf < 0, until it is spent), for at most `blocks` new blocks
 * counted from the first height the chain gives (0 is no answer: a wait counted from it would end at once), and at most (blocks + 6)
 * times block_ms: a node that stops would keep the session for ever. Meanwhile an S_ALIVE now and then keeps the channel from being cut
 * as idle (the partner may have closed it: that is no error). */
static bool wait_for(sess *s, const uint8_t txid[32], uint32_t vout, int conf, uint64_t blocks)
{
    const session_chain *ch = s->c->chain;
    const int every = s->c->alive_ms > 0 ? s->c->alive_ms : ALIVE_MS;
    const int64_t end = sys_now_ms() + (int64_t)(blocks + 6) * (s->c->block_ms > 0 ? s->c->block_ms : BLOCK_MS);
    int64_t alive = sys_now_ms() + every;
    uint64_t until = 0, last_h = 0;
    int last_k = -3;
    for (;;) {
        const uint64_t h = ch->height(ch->ctx);
        if (h && !until) until = h + blocks;
        const int k = ch->confirmations(ch->ctx, txid, vout);   /* -2: the chain did not answer, which says nothing */
        if (k != last_k || h != last_h) dbg("session %s wait: height %llu, output %s...:%u is %d (wanted %d)", s->tag, (unsigned long long)h,
                                            dbg_hex(txid, 32), vout, k, conf), last_k = k, last_h = h;
        if (k != -2 && (conf < 0 ? k == -1 : k >= conf)) return true;
        if ((until && h >= until) || sys_now_ms() >= end) return false;
        if (sys_now_ms() >= alive) (void)send_msg(s, S_ALIVE, NULL, 0), alive = sys_now_ms() + every;
        sys_sleep_ms(s->c->poll_ms);
    }
}

/* The coins of the puzzle: the largest of a scan of its address, and the prizes of the network file (project 4.4). A scan can fail:
 * thousands of coins of dust sent to the address make the node's answer too long to read, and the prizes are still found one by one. */
static int puzzle_coins(sess *s, payout_coin coins[64])
{
    const session_chain *ch = s->c->chain;
    int n = ch->coins(ch->ctx, s->t.puzzle_spk, coins, 64 - SESSION_PRIZES_MAX);
    if (n < 0) n = 0;
    for (int i = 0; i < s->c->nprizes && i < SESSION_PRIZES_MAX && ch->coin; i++) {
        const payout_coin *p = &s->c->prizes[i];
        bool have = false;
        for (int j = 0; j < n && !have; j++) have = !memcmp(coins[j].txid, p->txid, 32) && coins[j].vout == p->vout;
        if (!have && ch->coin(ch->ctx, p->txid, p->vout, s->t.puzzle_spk, &coins[n]) == 1) n++;
    }
    return n;
}

session_result session_run(const session_cfg *c, char *why, size_t whylen)
{
    sess s = {.c = c, .why = why, .whylen = whylen, .wait_ms = c->timeout_ms < HELLO_MS ? c->timeout_ms : HELLO_MS};
    why[0] = 0;
    snprintf(s.tag, sizeof s.tag, "%s", dbg_hex(c->mine.x, 32));   /* the x of the own point names the session in the debug log */
    if (!hello(&s)) return SESSION_FAILED;
    s.wait_ms = c->timeout_ms;
    const session_chain *ch = c->chain;
    /* the first transaction: the coins of the puzzle back to its address, and the tiny coins */
    payout_coin coins[64];
    btc_tx a, b, x;
    size_t used;
    const int nc = puzzle_coins(&s, coins);
    if (nc <= 0 || !payout_first(&s.t, coins, (size_t)nc, &a, &used)) return fail(&s, "no coin of the puzzle to spend"), SESSION_FAILED;
    if (!limit_ok(&s) || !plan(&s, &a)) return SESSION_FAILED;
    FILE *made = fopen(c->primes, "rb");                        /* the primes made ahead, or made now */
    if (made) fclose(made);
    else if (!signer_primes(c->pzsign, c->primes)) return fail(&s, "pzsign could not make its primes"), SESSION_FAILED;
    if (!sign_jointly(&s, &a, a.n_in, 'A')) return SESSION_FAILED;
    uint8_t aid[32];
    btc_tx_txid(&a, aid);
    dbg_tx(&s, "first, signed", &a);
    dbg("session %s broadcast of the first: %s", s.tag, ch->broadcast(ch->ctx, &a) ? "taken" : "not taken");   /* both send it: the second to arrive is a duplicate */
    if (!wait_for(&s, aid, 0, 1, 144)) return fail(&s, "the first transaction was not confirmed"), SESSION_FAILED;
    /* the second: F gives the signature of its tiny coin, then the joint signature, which S gets first */
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
    if (signed_b && c->withhold && s.me != s.F) return snprintf(why, whylen, "withheld the second transaction"), SESSION_FAILED;
    if (signed_b) {                                             /* S, and F when S sent its last message: either may send it */
        dbg_tx(&s, "second, signed", &b);
        dbg("session %s broadcast of the second: %s", s.tag, ch->broadcast(ch->ctx, &b) ? "taken" : "not taken");
        return wait_for(&s, aid, 0, -1, 144) ? (snprintf(why, whylen, "paid"), SESSION_PAID)
                                            : (snprintf(why, whylen, "the second transaction did not go through"), SESSION_FAILED);
    }
    if (s.me != s.F) return snprintf(why, whylen, "the joint signature of the second transaction failed"), SESSION_FAILED;
    /* F without the whole signature: S may hold it. F waits for the prize to move, and else spends its tiny coin, which makes it worthless */
    if (wait_for(&s, aid, 0, -1, (uint64_t)c->cancel_blocks)) return snprintf(why, whylen, "paid"), SESSION_PAID;
    if (!payout_cancel(&s.t, &a, &x) || !btc_sign_p2pkh(&x, 0, c->coin_sec) || !ch->broadcast(ch->ctx, &x))
        return wait_for(&s, aid, 0, -1, 0) ? (snprintf(why, whylen, "paid"), SESSION_PAID)   /* it moved in the meantime */
                                           : (snprintf(why, whylen, "the cancel could not be sent"), SESSION_FAILED);
    snprintf(why, whylen, "the partner held the second transaction back: cancelled with the tiny coin");
    return SESSION_CANCELLED;
}
