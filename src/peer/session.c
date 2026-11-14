/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* session.c - the session of a coincidence (see session.h). The steps, each a message both sides send and then read:
 *   hello   the pair, a nonce, the fee rate proposed, the tiny-coin and payout scripts;
 *   proof   the proof of possession of the own record, over the partner's nonce (it shows the distance, hence the piece);
 *   plan    the hash of the first transaction each side built on its own; then the two-party signing, the broadcast, one confirmation;
 *   plan    the hash of the second; F gives the signature of its tiny coin, then the two-party signing in which S gets the whole signature
 *           first (it plays the party of pzsign that gets the signatures and gives them to the other); then S broadcasts, and F waits
 *           cancel_blocks blocks for the prize to move and else cancels.
 * Party 0 owns the smaller identity; F (the party whose tiny coin goes into the second transaction) is the last bit of the session id. */
#include "dbg.h"
#include "session.h"
#include "net.h"
#include "signer.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <errno.h>
#include <secp256k1_extrakeys.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELLO_FIXED (2 * RECORD_LEN + 32 + 8 + BTC_P2PKH_LEN + 1)
#define CHUNK 65000                         /* a piece of a pzsign message in one S_SIGNER */
_Static_assert(1 + HELLO_FIXED + BTC_SPK_MAX <= SESSION_HELLO_MAX, "a hello fits in SESSION_HELLO_MAX");
#define HELLO_MS 30000                      /* the longest wait for the hello and the proof of the partner, which it makes at once */
#define LIMIT_LINES 1024                    /* lines of the file of the limit: one per network, one per pair that failed */
#define LIMIT_WAIT_S 120                    /* the longest wait for the turn of a signing (a partner waits 300 s for a message) */
#define ALIVE_MS 60000                      /* S_ALIVE while waiting for blocks: an idle channel is cut by NATs and relays */
#define BLOCK_MS (20 * 60000)               /* twice the mean time of a block: the measure of a wait when the chain stops */
#define SIGN_MS (15 * 60000)                /* the longest relay of a signing (one takes seconds) */
#define REVEAL_CONF 6                       /* the confirmations of the payout before the pieces are exchanged */
#define REVEAL_DUST 100000                  /* satoshis at the puzzle address that the exchange of the pieces leaves to whoever is first */

typedef struct {
    const session_cfg *c;
    record pair[2];
    int me, F;
    uint8_t eid[32], my_x[32];
    uint8_t nonce[2][32];                   /* of party 0 and party 1: fresh in every session */
    int wait_ms;                            /* the wait for a message of the partner now */
    bool proved;                            /* the partner proved its distance */
    payout_terms t;
    char *why;
    size_t whylen;
    char tag[17];
} sess;

/* the measure of a wait for blocks when the chain stops */
static int64_t block_ms(const sess *s) { return s->c->block_ms > 0 ? s->c->block_ms : BLOCK_MS; }

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

/* a step that matters to the owner, for the terminal */
static void note(const sess *s, const char *fmt, ...)
{
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    dbg("session %s: %s", s->tag, line);
    if (s->c->say) s->c->say(s->c->user, line);
}

/* satoshis as BTC, for the notes */
static const char *btc_amount(uint64_t sat, char out[32])
{
    snprintf(out, 32, "%llu.%08llu", (unsigned long long)(sat / 100000000), (unsigned long long)(sat % 100000000));
    return out;
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
    /* S_ALIVE, sent while the partner waits for blocks (its chain may be behind ours), gives another wait only once the partner proved
     * its distance, and for 2 hours at most: before, anyone could hold a session with it. Each recv waits wait_ms, the same as for the
     * relay of a signing (the timeout of the connection is not changed while two threads use it). */
    const int64_t last = sys_now_ms() + (s->proved ? 6 * block_ms(s) : s->wait_ms);
    int64_t until = sys_now_ms() + s->wait_ms;
    do {
        if (sys_now_ms() > until || !s->c->chan->recv(s->c->chan->ctx, m, sizeof m, &n, s->wait_ms) || n == 0)
            return fail(s, "the partner did not answer");
        const int64_t next = sys_now_ms() + s->wait_ms, more = next < last ? next : last;
        if (m[0] == S_ALIVE && more > until) until = more;       /* never shorter */
    } while (m[0] == S_ALIVE);
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
     * must never repeat (a signing id names the parties of pzsign, and a pair can have more than one session) */
    if (!sys_random(s->nonce[s->me], 32)) return fail(s, "no randomness from the system");
    if (!p2pkh_of(c->ctx, c->coin_sec, coin_spk)) return fail(s, "invalid coin key");
    /* twice what the chain estimates, and never little: these transactions move a prize, and no fee is large beside it */
    uint64_t fr = 2 * (c->feerate ? c->feerate : c->chain->feerate(c->chain->ctx));
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
    const bool only_pieces = c->paid && c->paid->paid_at;       /* the fee rates do not matter */
    /* the higher of the two, but not one far above this home's: every try of the payout pays it from the prize, and so does a tiny coin */
    if (theirs > 4 * fr && !only_pieces) return fail(s, "the partner's fee rate %llu is more than 4 times this home's %llu", (unsigned long long)theirs, (unsigned long long)fr);
    t->feerate = fr > theirs ? fr : theirs;
    t->first = s->F;
    dbg("session %s terms: party %d, F is party %d, fee rate mine %llu theirs %llu sat/kvB, tiny coin %llu sat", s->tag, s->me, s->F,
        (unsigned long long)fr, (unsigned long long)theirs, (unsigned long long)payout_tiny(t));
    if (t->feerate > c->max_feerate && !only_pieces) return fail(s, "fee rate %llu above the limit %llu", (unsigned long long)t->feerate, (unsigned long long)c->max_feerate);
    /* the proofs of possession: each over the nonce of the other */
    uint8_t proof[64], their_proof[64];
    if (!record_prove(c->ctx, c->net, &c->mine, c->d, c->partner, s->nonce[you], proof)) return fail(s, "the distance does not fit the record");
    if (!send_msg(s, S_PROOF, proof, 64) || !recv_msg(s, S_PROOF, their_proof, 64, &n)) return false;
    if (n != 64 || !record_proof_ok(c->ctx, c->net, &c->theirs, s->my_x, s->nonce[s->me], their_proof)) return fail(s, "the partner did not prove its distance");
    s->proved = true;
    if (c->proved) *c->proved = true;
    note(s, "the partner proved it holds the other piece of the key");
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

/* through a new file, synced, that replaces the old one: a crash leaves either. Lines at 0 are left out */
static bool limit_write(const char *path, uint8_t (*key)[32], const unsigned *count, int n)
{
    char tmp[1100], hex[65];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "w");
    bool ok = f != NULL;
    for (int i = 0; ok && i < n; i++) btc_hex(key[i], 32, hex), ok = !count[i] || fprintf(f, "%s %u\n", hex, count[i]) > 0;
    ok = ok && fflush(f) == 0 && sys_sync(fileno(f));
    if (f) ok = fclose(f) == 0 && ok;
    return ok && sys_replace(tmp, path);
}

/* d (+1 or -1) to the failed signings of the network and of the pair, or (payout) to the payouts of the pair that did not pay; +1 is
 * refused at a limit, and 0 only checks. False with the reason in why (mu held) */
static bool limit_add(const sess *s, int d, bool payout, char *why, size_t whylen)
{
    const session_limit *l = s->c->limit;
    uint8_t key[LIMIT_LINES][32];
    unsigned count[LIMIT_LINES];
    int n;
    if (!limit_read(l->path, key, count, &n)) return snprintf(why, whylen, "the count of failed signings cannot be read"), false;   /* no path: the partner is told */
    /* the pair by the own point, on this network: the piece of this home is that of its point, and the partner can change any byte of
     * its own record (a new session id each time) */
    uint8_t m[11 + 64], pair[32];
    memcpy(m, payout ? "pznet1/paid" : "pznet1/fail", 11), memcpy(m + 11, s->c->net->fp, 32), memcpy(m + 43, s->c->mine.x, 32);
    pz_sha256(m, sizeof m, pair);
    const uint8_t *which[2] = {payout ? pair : s->c->net->fp, pair};
    const unsigned max[2] = {payout ? l->pay_max : l->max, payout ? l->pay_max : l->pair_max};
    const int lines = payout ? 1 : 2;
    int at[2];
    for (int w = 0; w < lines; w++) {
        for (at[w] = 0; at[w] < n && memcmp(key[at[w]], which[w], 32); at[w]++) {}
        if (at[w] == n) {
            if (n == LIMIT_LINES) return snprintf(why, whylen, "the count of failed signings is full"), false;
            memcpy(key[n], which[w], 32), count[n++] = 0;
        }
        if (d >= 0 && count[at[w]] >= max[w])
            return snprintf(why, whylen, "%u %s %s: no more (the limit of a home, docs/en/NETWORK.md 7.5)", count[at[w]],
                            payout ? "payouts that did not pay" : "failed signings", w || payout ? "of this pair" : "on this network"), false;
    }
    if (!d) return true;
    for (int w = 0; w < lines; w++) count[at[w]] = d > 0 ? count[at[w]] + 1 : count[at[w]] ? count[at[w]] - 1 : 0;
    return limit_write(l->path, key, count, n) || (snprintf(why, whylen, "cannot write the count of failed signings"), false);
}

/* whether this home may still sign for the pair: asked before the plan, so that a home at its limit stops before the partner's pzsign
 * starts, which would then count a failure of its own */
static bool limit_ok(sess *s)
{
    session_limit *l = s->c->limit;
    char why[300];
    if (!l) return true;
    pthread_mutex_lock(&l->mu);
    const bool ok = limit_add(s, 0, false, why, sizeof why) && limit_add(s, 0, true, why, sizeof why);
    pthread_mutex_unlock(&l->mu);
    return ok || fail(s, "%s", why);
}

/* A payout counts as one that did not pay from the signing of its first transaction until the prize is paid: each first transaction
 * spends a fee and a tiny coin of the prize, and a partner could otherwise make it try for ever (F spending its tiny coin before the
 * second transaction, S keeping either signature back, an old copy of the first sent before the new one). Counted before the signing:
 * the side that does not get the signatures cannot tell whether the other sent the transaction */
static bool limit_payout(sess *s, int d)
{
    session_limit *l = s->c->limit;
    char why[300];
    if (!l) return true;
    pthread_mutex_lock(&l->mu);
    const bool ok = limit_add(s, d, true, why, sizeof why);
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
    const bool ok = !l->busy && limit_add(s, +1, false, why, sizeof why);
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
    if (ok) (void)limit_add(s, -1, false, why, sizeof why);
    l->busy = false;
    pthread_cond_signal(&l->cv);
    pthread_mutex_unlock(&l->mu);
}

/* ---- the two-party signing: pzsign here, its messages relayed to the partner's ---- */

typedef struct {
    sess *s;
    signer sg;
    bool heard;                             /* a whole message of the partner's pzsign came */
    uint8_t *result;                        /* the answer of pzsign (0x82), when it gave one */
    bool unpaid;                            /* it ended as P2 that gave all its messages, without the signatures of P1 (0x84) */
    atomic_bool in_done;                    /* the partner's messages are all given to pzsign, whose input is closed */
    size_t result_len;
} relay;

/* pzsign to the partner: its messages, in pieces of at most CHUNK; then the end of this side's messages, which says whether pzsign gave
 * its answer. An error of pzsign is not relayed: its reason would tell the partner which check failed, and a failure must say nothing
 * of where it happened (project 4.6). */
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

static void *relay_out(void *arg)
{
    relay *r = arg;
    uint8_t *p = NULL, type = 0;
    size_t len;
    bool up = true;                                             /* pzsign is read to its end even when the partner is gone: its answer is ours */
    while (signer_recv(&r->sg, &type, &p, &len, r->s->wait_ms) && type == SIGNER_OUT) up = up && send_chunks(r, p, len), free(p), p = NULL;
    /* no answer yet: pzsign waits for messages of the partner, whose relay may last one wait more (keep-alives); once its input is
     * closed it answers at once */
    for (const int64_t until = sys_now_ms() + 2LL * r->s->wait_ms; !p && !atomic_load(&r->in_done) && sys_now_ms() < until;) sys_sleep_ms(50);
    if (!p && atomic_load(&r->in_done) && signer_recv(&r->sg, &type, &p, &len, 5000) && type == SIGNER_OUT) free(p), p = NULL;
    if (p && type == SIGNER_RESULT) r->result = p, r->result_len = len, p = NULL;
    r->unpaid = p && type == SIGNER_UNPAID;
    free(p);
    const uint8_t st = r->result != NULL;
    (void)send_msg(r->s, S_SIGNED, &st, 1);
    return NULL;
}

/* the partner to pzsign: pieces joined into whole messages. Only a whole message is progress: keep-alives or pieces that never end a
 * message cannot hold this side here (the partner would keep the other side from its cancel, or from its signatures), and a signing
 * ends within SIGN_MS whatever comes (each bound checked when a message comes, or at the end of a wait: one more wait at most). */
static void *relay_in(void *arg)
{
    relay *r = arg;
    uint8_t *msg = NULL, m[SESSION_MSG_MAX];
    size_t len = 0;
    const int64_t start = sys_now_ms();
    int64_t progress = start;
    for (;;) {
        size_t n;
        const bool got = r->s->c->chan->recv(r->s->c->chan->ctx, m, sizeof m, &n, r->s->wait_ms);
        const int64_t now = sys_now_ms();
        if (!got || n < 1 || now - progress > r->s->wait_ms || now - start > SIGN_MS) break;
        if (m[0] == S_ALIVE) continue;
        if (m[0] != S_SIGNER || n < 2 || len + n - 2 > SIGNER_FRAME_MAX) break;   /* S_SIGNED, S_ABORT, or anything else */
        uint8_t *grown = realloc(msg, len + n - 2 + 1);
        if (!grown) break;
        msg = grown;
        memcpy(msg + len, m + 2, n - 2);
        len += n - 2;
        if (m[1]) continue;                                     /* more pieces follow */
        const bool ok = signer_send(&r->sg, SIGNER_IN, msg, len);
        len = 0, progress = now, r->heard = true;
        if (!ok) break;
    }
    free(msg);
    signer_end_input(&r->sg);                                   /* nothing more for pzsign: at the end of its input it gives its answer */
    atomic_store(&r->in_done, true);
    return NULL;
}

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
    /* S gets the signatures first: in the second transaction this is what matters, F (whose tiny coin is in it) holding the cancel; in the
     * first, which only gives the prize back to the puzzle, it does not, and both signings run alike */
    uint8_t job[SIGNER_JOB_MAX];
    const size_t jl = signer_job(job, s->me, 1 - s->F, eid, piece, (const uint8_t(*)[33])pub, c->pubkey33, (const uint8_t(*)[32])digests, (int)n);
    pz_secure_zero(piece, sizeof piece);
    relay r = {.s = s};
    const bool started = signer_start(&r.sg, c->pzsign);
    bool ok = started && signer_send(&r.sg, SIGNER_JOB, job, jl);
    pz_secure_zero(job, sizeof job);
    if (ok) {
        pthread_t a, b;
        const bool ta = pthread_create(&a, NULL, relay_out, &r) == 0, tb = pthread_create(&b, NULL, relay_in, &r) == 0;
        if (ta) pthread_join(a, NULL);
        signer_kill(&r.sg);                                     /* it has nothing more to say: a write to it that waits (it stopped reading) ends */
        if (tb) pthread_join(b, NULL);
        ok = ta && tb;
    }
    signer_stop(&r.sg);
    /* signatures that verify are kept whatever the partner sent after them */
    ok = ok && r.result && r.result_len == 64 * n;
    for (size_t i = 0; ok && i < n; i++)
        ok = btc_put_sig_p2pkh(tx, i, r.result + 64 * i, r.result + 64 * i + 32, c->pubkey33) && btc_verify_p2pkh(tx, i, s->t.puzzle_spk, BTC_P2PKH_LEN);
    free(r.result);
    /* a pzsign that did not start told nothing, and one that got no message of the partner answered none: a partner that is late, or
     * gone, costs no count (the leak the limit bounds is in the answers to its messages). Nor does a P2 that gave all its answers and
     * only lacks the signatures of P1: a partner that keeps them learns nothing from it, and could otherwise use up this home's count */
    limit_end(s, ok || !started || !r.heard || r.unpaid);
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
    const int64_t end = sys_now_ms() + (int64_t)(blocks + 6) * block_ms(s);
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
typedef struct {
    sess *s;
    atomic_bool stop;
} alive;

static void *alive_main(void *arg)
{
    alive *a = arg;
    const int every = a->s->c->alive_ms > 0 ? a->s->c->alive_ms : ALIVE_MS;
    for (int64_t next = sys_now_ms() + every; !atomic_load(&a->stop); sys_sleep_ms(200))
        if (sys_now_ms() >= next) (void)send_msg(a->s, S_ALIVE, NULL, 0), next = sys_now_ms() + every;
    return NULL;
}

/* The coins of the puzzle address (-1: the chain cannot be asked), while S_ALIVE goes to the partner, which waits for a message: a scan of
 * the node's whole set can take half an hour, and the partner would give up after one wait (and this side, reading its next message
 * later, start a signing nobody answers). With prizes, also the prizes of the network file that the scan did not list. */
static int puzzle_coins(sess *s, payout_coin coins[64], bool prizes)
{
    const session_chain *ch = s->c->chain;
    alive al = {.s = s};
    pthread_t t;
    const bool beat = pthread_create(&t, NULL, alive_main, &al) == 0;
    int n = ch->coins(ch->ctx, s->t.puzzle_spk, coins, 64 - SESSION_PRIZES_MAX);
    if (n < 0 && prizes) n = 0;
    for (int i = 0; prizes && i < s->c->nprizes && i < SESSION_PRIZES_MAX && ch->coin; i++) {
        const payout_coin *p = &s->c->prizes[i];
        bool have = false;
        for (int j = 0; j < n && !have; j++) have = !memcmp(coins[j].txid, p->txid, 32) && coins[j].vout == p->vout;
        if (!have && ch->coin(ch->ctx, p->txid, p->vout, s->t.puzzle_spk, &coins[n]) == 1) n++;
    }
    atomic_store(&al.stop, true);
    if (beat) pthread_join(t, NULL);
    return n;
}

/* ---- after the payout: the pieces ---- */

/* Whether the payout is settled at height h: the block of the spend (at or below paid_at) has conf confirmations, the prize output is still
 * spent in a block (a reorganization that took the payout back shows it unspent, or the coins of the puzzle again), and less than
 * REVEAL_DUST is left at the puzzle address. Then the key protects nothing more, and it can be given to the partner. */
static bool settled(sess *s, uint64_t h, int conf)
{
    const session_chain *ch = s->c->chain;
    const session_paid *p = s->c->paid;
    payout_coin coins[64];
    if (h + 1 < p->paid_at + (uint64_t)conf || ch->confirmations(ch->ctx, p->first, 0) != -1) return false;
    const int n = puzzle_coins(s, coins, false);
    uint64_t left = 0;
    for (int i = 0; i < n; i++) left += coins[i].value;
    return n >= 0 && left < REVEAL_DUST;
}

/* Once the payout is settled each side gives its piece and checks the other's against the public piece of the record: their sum is the
 * key of the puzzle. Someone must give first, and a partner can keep its own: all it gains is what the key still opens (docs/en/NETWORK.md
 * 7.5). Waits at most conf + 6 blocks of block_ms; true when the key is in paid->key. */
static bool exchange(sess *s)
{
    const session_cfg *c = s->c;
    const session_chain *ch = c->chain;
    session_paid *p = c->paid;
    const int conf = c->reveal_conf > 0 ? c->reveal_conf : REVEAL_CONF, every = c->alive_ms > 0 ? c->alive_ms : ALIVE_MS;
    const int64_t end = sys_now_ms() + (int64_t)(conf + 6) * block_ms(s);
    int64_t beat = sys_now_ms() + every;
    note(s, "the pieces of the key are exchanged after %d confirmations of the payout", conf);
    for (uint64_t last = 0;;) {
        const uint64_t h = ch->height(ch->ctx);
        if (h && h != last && (last = h, settled(s, h, conf))) break;   /* the chain asked once a block */
        if (sys_now_ms() >= end) return snprintf(s->why, s->whylen, "paid; the payout is not settled yet, the pieces wait"), false;
        if (sys_now_ms() >= beat) (void)send_msg(s, S_ALIVE, NULL, 0), beat = sys_now_ms() + every;
        sys_sleep_ms(c->poll_ms);
    }
    uint8_t mine[32], theirs[32], key[32], k33[33];
    secp256k1_pubkey X, Y, K;
    size_t n = 0, kl = 33;
    bool ok = pieces_secret(c->ctx, c->net, c->base, s->pair, s->me, c->d, mine);
    if (ok && (!send_msg(s, S_PIECE, mine, 32) || !recv_msg(s, S_PIECE, theirs, 32, &n))) {
        char r[200];
        snprintf(r, sizeof r, "%s", s->why);
        snprintf(s->why, s->whylen, "paid; no piece from the partner (%s)", r);
        return pz_secure_zero(mine, sizeof mine), false;
    }
    memcpy(key, mine, 32);
    ok = ok && n == 32 && pieces_public(c->ctx, c->net, c->base, s->pair, 1 - s->me, &X) && secp256k1_ec_pubkey_create(c->ctx, &Y, theirs) &&
         !secp256k1_ec_pubkey_cmp(c->ctx, &X, &Y) && secp256k1_ec_seckey_tweak_add(c->ctx, key, theirs) &&
         secp256k1_ec_pubkey_create(c->ctx, &K, key) && secp256k1_ec_pubkey_serialize(c->ctx, k33, &kl, &K, SECP256K1_EC_COMPRESSED) &&
         !memcmp(k33, c->pubkey33, 33);
    if (ok) memcpy(p->key, key, 32), p->have_key = true;
    pz_secure_zero(mine, sizeof mine), pz_secure_zero(theirs, sizeof theirs), pz_secure_zero(key, sizeof key);
    snprintf(s->why, s->whylen, ok ? "paid; the key of the puzzle is known" : "paid; the piece of the partner is not the right one");
    return ok;
}

/* The prize moved: the payout counts no more against the limit, and the pieces are exchanged once it is settled */
static session_result paid(sess *s, const uint8_t aid[32], const btc_tx *b)
{
    char v[32];
    (void)limit_payout(s, -1);
    snprintf(s->why, s->whylen, "paid");
    note(s, "paid: %s BTC to the payout address of this home", btc_amount(b->out[s->me].value, v));
    if (!s->c->paid) return SESSION_PAID;
    s->c->paid->paid_at = s->c->chain->height(s->c->chain->ctx);   /* the spend is in a block at or below it */
    memcpy(s->c->paid->first, aid, 32);
    (void)exchange(s);
    return SESSION_PAID;
}

session_result session_run(const session_cfg *c, char *why, size_t whylen)
{
    sess s = {.c = c, .why = why, .whylen = whylen, .wait_ms = c->timeout_ms < HELLO_MS ? c->timeout_ms : HELLO_MS};
    why[0] = 0;
    snprintf(s.tag, sizeof s.tag, "%s", dbg_hex(c->mine.x, 32));   /* the x of the own point names the session in the debug log */
    if (!hello(&s)) return SESSION_FAILED;
    s.wait_ms = c->timeout_ms;
    if (c->paid && c->paid->paid_at) return (void)exchange(&s), SESSION_PAID;   /* an earlier session paid: only the pieces */
    const session_chain *ch = c->chain;
    /* the first transaction: the coins of the puzzle back to its address, and the tiny coins */
    payout_coin coins[64];
    btc_tx a, b, x;
    size_t used;
    const int nc = puzzle_coins(&s, coins, true);
    if (nc <= 0 || !payout_first(&s.t, coins, (size_t)nc, &a, &used)) return fail(&s, "no coin of the puzzle to spend"), SESSION_FAILED;
    if (!limit_ok(&s) || !plan(&s, &a)) return SESSION_FAILED;
    if (!limit_payout(&s, +1) || !sign_jointly(&s, &a, a.n_in, 'A')) return SESSION_FAILED;
    uint8_t aid[32];
    char hex[65], v[32];
    btc_tx_txid(&a, aid);
    dbg_tx(&s, "first, signed", &a);
    dbg("session %s broadcast of the first: %s", s.tag, ch->broadcast(ch->ctx, &a) ? "taken" : "not taken");   /* both send it: the second to arrive is a duplicate */
    btc_hash_to_hex(aid, hex);
    note(&s, "first transaction sent, the prize back to the puzzle address: %s", hex);
    if (!wait_for(&s, aid, 0, 1, 144)) return fail(&s, "the first transaction was not confirmed"), SESSION_FAILED;
    note(&s, "first transaction confirmed");
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
        uint8_t bid[32];
        dbg_tx(&s, "second, signed", &b);
        dbg("session %s broadcast of the second: %s", s.tag, ch->broadcast(ch->ctx, &b) ? "taken" : "not taken");
        btc_tx_txid(&b, bid), btc_hash_to_hex(bid, hex);
        note(&s, "second transaction sent, %s BTC to each payout address: %s", btc_amount(b.out[s.me].value, v), hex);
        return wait_for(&s, aid, 0, -1, 144) ? paid(&s, aid, &b)
                                            : (snprintf(why, whylen, "the second transaction did not go through"), SESSION_FAILED);
    }
    if (s.me != s.F) return snprintf(why, whylen, "the joint signature of the second transaction failed"), SESSION_FAILED;
    /* F without the whole signature: S may hold it. F waits for the prize to move, and else spends its tiny coin, which makes it worthless */
    note(&s, "the partner has the signature of the second transaction: %d blocks for it to send it, else the cancel", c->cancel_blocks);
    if (wait_for(&s, aid, 0, -1, (uint64_t)c->cancel_blocks)) return paid(&s, aid, &b);
    if (!payout_cancel(&s.t, &a, &x) || !btc_sign_p2pkh(&x, 0, c->coin_sec)) return fail(&s, "no cancel"), SESSION_FAILED;
    uint8_t xid[32];
    btc_tx_txid(&x, xid);
    /* sent again at every block until it confirms, or the prize moves: a node or a server may refuse it for a while, or drop it. For 144
     * blocks, and as long as a wait for them may last when the chain stops */
    const int64_t end = sys_now_ms() + (int64_t)(144 + 6) * block_ms(&s);
    note(&s, "the cancel: the tiny coin of this home to its payout address, which voids the second transaction");
    for (int k = 0; k < 144 && sys_now_ms() < end; k++) {
        dbg("session %s broadcast of the cancel: %s", s.tag, ch->broadcast(ch->ctx, &x) ? "taken" : "not taken");
        if (wait_for(&s, xid, 0, 1, 1)) {
            snprintf(why, whylen, "the partner held the second transaction back: cancelled with the tiny coin");
            return SESSION_CANCELLED;
        }
        if (wait_for(&s, aid, 0, -1, 0)) return paid(&s, aid, &b);   /* it moved in the meantime */
    }
    snprintf(why, whylen, "the cancel did not confirm in 144 blocks");
    return SESSION_FAILED;
}
