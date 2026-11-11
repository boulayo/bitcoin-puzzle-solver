/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_alert.c - the alerts of a coincidence (docs/en/NETWORK.md 7.3): those a custodian sends, those of this node and the proofs of
 * possession they wait for, the mailboxes. */
#include "dht_priv.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <stdlib.h>

#define ALERTS_MAX 1024            /* alerts of this node remembered */
#define POST_MS (3600 * 1000LL)    /* this node's proof of an alert, sent again to the other owner while the alert waits */
#define BACKOFF_MAX_MS (3600 * 1000LL)
#define ALERT_TTL_MS (24 * 3600 * 1000LL)

enum { AL_WAIT, AL_DONE, AL_REFUSED };

struct own_alert {
    uint8_t h[32];
    record mine, theirs;
    int state;
    bool mine_ok, theirs_ok;       /* the proofs checked: this node's own (it knows the distance), the other owner's */
    bool busy;                     /* a job works on it */
    int tries;
    int64_t until, next, posted;   /* retried at next (later each time) until until; this node's proof last sent to the other owner */
};

void dht_mbox_key(const uint8_t owner[32], uint8_t k[32])
{
    uint8_t m[11 + 32] = "pznet1/mbox";
    memcpy(m + 11, owner, 32);
    pz_sha256(m, sizeof m, k);
}

/* the owner of a record is a node: its node ID has the zero bits (one hash; the custodian alerts it by its ID) */
bool owner_ok(const dht *d, const record *r)
{
    uint8_t id[32];
    keys_node_id(r->owner, id);
    return keys_id_ok(id, d->cfg.id_bits);
}

bool pair_ok(const dht *d, const record *a, const record *b)
{
    return owner_ok(d, a) && owner_ok(d, b) && store_pair_ok(d->cfg.ctx, d->cfg.net, a, b);
}

/* SHA256("pznet1/pair" || the two records in the order of their owners): the name of a pair, whichever order it came in; also the nonce
 * of the proof an owner sends for a pair (docs/en/NETWORK.md 7.3) */
void pair_hash(const record *a, const record *b, uint8_t h[32])
{
    uint8_t m[11 + PAIR_LEN] = "pznet1/pair";
    const bool swap = memcmp(a->owner, b->owner, 32) > 0;
    record_encode(swap ? b : a, m + 11);
    record_encode(swap ? a : b, m + 11 + RECORD_LEN);
    pz_sha256(m, sizeof m, h);
}

/* ---- alerts ---- */

static own_alert *alert_find(dht *d, const uint8_t h[32])   /* a_mu held */
{
    for (int i = 0; i < d->nalerts; i++)
        if (!memcmp(d->alerts[i].h, h, 32)) return &d->alerts[i];
    return NULL;
}

/* the state of an alert of this node (-1: unknown), and whether the other owner's proof checked out */
static int alert_state(dht *d, const uint8_t h[32], bool *theirs_ok)
{
    pthread_mutex_lock(&d->a_mu);
    const own_alert *e = alert_find(d, h);
    const int st = e ? e->state : -1;
    if (theirs_ok) *theirs_ok = e && e->theirs_ok;
    pthread_mutex_unlock(&d->a_mu);
    return st;
}

/* The alert of a pair with a record m of this node (a_mu held): found, or added (*fresh). When the table is full the oldest one that is
 * not waiting goes, else the oldest waiting one no job holds: a flood of alerts that will never be proved (copies of this node's points
 * by owners that do not answer) cannot shut out the next real one, which is tried at once; an alert pushed out comes back with the
 * next republish or mailbox poll. NULL only without memory. */
static own_alert *alert_add_locked(dht *d, const record *m, const record *t, const uint8_t h[32], bool *fresh)
{
    *fresh = false;
    own_alert *e = alert_find(d, h);
    if (e) return e;
    if (d->nalerts == ALERTS_MAX) {
        int at = 0;
        while (at < d->nalerts && d->alerts[at].state == AL_WAIT) at++;
        if (at == d->nalerts)
            for (at = 0; at < d->nalerts && d->alerts[at].busy; at++) {}
        if (at == d->nalerts) return NULL;
        memmove(&d->alerts[at], &d->alerts[at + 1], (size_t)(d->nalerts - at - 1) * sizeof *d->alerts);
        d->nalerts--;
    }
    if (d->nalerts == d->calerts) {
        const int nc = d->calerts ? 2 * d->calerts : 16;
        own_alert *g = realloc(d->alerts, (size_t)nc * sizeof *g);
        if (!g) return NULL;
        d->alerts = g, d->calerts = nc;
    }
    e = &d->alerts[d->nalerts++];
    memset(e, 0, sizeof *e);
    memcpy(e->h, h, 32);
    e->mine = *m, e->theirs = *t;
    e->state = AL_WAIT;
    e->until = sys_now_ms() + ALERT_TTL_MS;
    e->next = sys_now_ms() + d->retry_ms;
    *fresh = true;
    return e;
}

/* A checked pair that came to this node (an ALERT, a mailbox, its own custody), with the proof of the owner of a for the owner of b when
 * one came (checked): kept as a mailbox when this node may be a mailbox of one of the owners, and an alert of this node when one record
 * is its own. True for a new alert of this node. */
bool alert_in(dht *d, const record *a, const record *b, const uint8_t *proof)
{
    uint8_t ka[32], kb[32], h[32];
    dht_mbox_key(a->owner, ka), dht_mbox_key(b->owner, kb);
    if ((responsible(d, ka) || responsible(d, kb)) && mbox_put(d->mb, a, b, proof)) dlog(d, "an alert pair in the mailbox");
    if (!mine(d, a) && !mine(d, b)) return false;
    const record *m = mine(d, a) ? a : b, *t = m == a ? b : a;
    bool fresh, go = false;
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *e = alert_add_locked(d, m, t, h, &fresh);
    if (e && e->state == AL_WAIT && proof && t == a && !e->theirs_ok) e->theirs_ok = go = true;   /* the other owner proved its record */
    pthread_mutex_unlock(&d->a_mu);
    if (fresh) dlog(d, "an alert for this node: checking the proofs");
    if (fresh || go) (void)job_push(d, JOB_ALERT_OWN, m, t, NULL);   /* if the queue is full, the maintenance thread retries it */
    return fresh;
}

/* an ALERT (with or without a proof) to a contact; to this node itself, what it would do with it */
static void send_alert(dht *d, const dht_contact *c, const record *a, const record *b, const uint8_t *proof)
{
    if (is_me(d, c->id)) {
        (void)alert_in(d, a, b, proof);
        return;
    }
    uint8_t req[1 + PAIR_LEN + 64], ans[1];
    size_t n;
    req[0] = DHT_ALERT;
    record_encode(a, req + 1);
    record_encode(b, req + 1 + RECORD_LEN);
    if (proof) memcpy(req + 1 + PAIR_LEN, proof, 64);
    heard(d, c, exchange(d, c, req, 1 + PAIR_LEN + (proof ? 64 : 0), ans, sizeof ans, &n) && n == 1);
}

/* the alert to an owner (found by its node ID) and to its mailboxes */
static void alert_owner(dht *d, const uint8_t owner[32], const record *a, const record *b, const uint8_t *proof)
{
    dht_contact c, mb[DHT_KSTORE];
    uint8_t k[32];
    if (!memcmp(owner, d->me.xonly, 32)) (void)alert_in(d, a, b, proof);
    else if (dht_find(d, owner, &c)) send_alert(d, &c, a, b, proof);
    dht_mbox_key(owner, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    for (int i = 0; i < m && !stopping(d); i++) send_alert(d, &mb[i], a, b, proof);
}

/* the job of a custodian: ALERT to both owners and to the mailboxes of each */
void job_alert_out(dht *d, const record *a, const record *b)
{
    for (int w = 0; w < 2 && !stopping(d); w++) alert_owner(d, (w ? b : a)->owner, a, b, NULL);
}

/* this node knows the distance of its own record: the proof it would give, made and checked */
static bool own_proof(dht *d, const record *r)
{
    uint8_t dd[32], nonce[32], sig[64];
    const bool ok = d->cfg.own_distance && sys_random(nonce, sizeof nonce) && d->cfg.own_distance(d->cfg.user, r->x, r->t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, r, dd, d->me.xonly, nonce, sig) &&
                    record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, nonce, sig);
    pz_secure_zero(dd, sizeof dd);
    return ok;
}

int dht_proof_on(dht *d, net_conn *c, const record *r)
{
    if (memcmp(c->remote, r->owner, 32)) return 0;
    uint8_t req[1 + RECORD_LEN + 32], ans[1 + 64];
    size_t n;
    req[0] = DHT_POSSESS;
    record_encode(r, req + 1);
    if (!sys_random(req + 1 + RECORD_LEN, 32) || !net_send(c, req, sizeof req) || !net_recv(c, ans, sizeof ans, &n)) return -1;
    return n == sizeof ans && ans[0] == DHT_PROOF && record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, req + 1 + RECORD_LEN, ans + 1);
}

int dht_ask_proof(dht *d, const record *r)
{
    dht_contact c;
    if (mine(d, r)) return own_proof(d, r) ? 1 : 0;
    if (!dht_find(d, r->owner, &c)) return -1;
    net_conn *k = dht_dial(d, &c);
    if (!k) {
        rt_failed(d, c.id);
        return -1;
    }
    const int st = dht_proof_on(d, k, r);
    net_close(k);
    if (st >= 0) rt_seen(d, &c, true);
    return st;
}

/* This node's proof of possession for the other owner of an alert, over the pair: the pair's name is the nonce, so the proof can wait in
 * a mailbox, and it binds the other owner as the asker, so it proves nothing to anyone else (docs/en/NETWORK.md 7.3). To the other owner
 * when it can be reached, and to its mailboxes, where it finds it when it does not listen. False when this node cannot make it. */
static bool post_proof(dht *d, const own_alert *e)
{
    uint8_t dd[32], proof[64];
    const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, e->mine.x, e->mine.t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, &e->mine, dd, e->theirs.owner, e->h, proof);
    pz_secure_zero(dd, sizeof dd);
    if (ok) alert_owner(d, e->theirs.owner, &e->mine, &e->theirs, proof);
    return ok;
}

static int64_t backoff_ms(const dht *d, int tries)
{
    int64_t w = d->retry_ms;
    for (int i = 1; i < tries && w < BACKOFF_MAX_MS; i++) w *= 2;
    return w < BACKOFF_MAX_MS ? w : BACKOFF_MAX_MS;
}

/* The job of an alert of this node: its own proof (the record is signed by this node, so when its distance is missing the daemon has not
 * read that point yet: the alert waits), then this node's proof sent to the other owner, then the other owner's proof, asked with POSSESS
 * unless it came already (an ALERT with its proof, a mailbox). on_alert when both check out; only a wrong proof of the other owner
 * refuses it. An alert that waits is tried again later each time (backoff_ms) for a day. */
void job_alert_own(dht *d, const record *a, const record *b)
{
    uint8_t h[32];
    own_alert e;
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *p = alert_find(d, h);
    const bool go = p && p->state == AL_WAIT && !p->busy;
    if (go) p->busy = true, e = *p;
    pthread_mutex_unlock(&d->a_mu);
    if (!go) return;                                           /* done already, or a job on it now */
    if (!e.mine_ok) e.mine_ok = own_proof(d, &e.mine);
    if (e.mine_ok && (!e.posted || sys_now_ms() - e.posted >= POST_MS) && post_proof(d, &e)) e.posted = sys_now_ms();
    const int asked = e.mine_ok && !e.theirs_ok ? dht_ask_proof(d, &e.theirs) : -2;
    int st = AL_WAIT;
    pthread_mutex_lock(&d->a_mu);
    if ((p = alert_find(d, h))) {
        p->busy = false;
        p->mine_ok = e.mine_ok, p->posted = e.posted;
        p->theirs_ok = p->theirs_ok || asked == 1;             /* or its proof came while this job asked */
        if (asked == 0) p->state = AL_REFUSED;
        else if (p->mine_ok && p->theirs_ok) p->state = AL_DONE;
        else p->tries++, p->next = sys_now_ms() + backoff_ms(d, p->tries);
        st = p->state;
        e = *p;
    }
    pthread_mutex_unlock(&d->a_mu);
    dlog(d, st == AL_DONE      ? "alert: both proofs check out"
            : st == AL_REFUSED ? "alert refused: the other owner's proof does not check out"
            : !e.mine_ok       ? "alert: this node does not know the distance of its point (yet), retried later"
                               : "alert: the other owner's proof has not come, retried later");
    if (st == AL_DONE && d->cfg.on_alert) d->cfg.on_alert(d->cfg.user, &e.mine, &e.theirs, e.mine.t == e.theirs.t);
}

void retry_alerts(dht *d)
{
    record pairs[16][2];
    int n = 0;
    const int64_t now = sys_now_ms();
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts; i++) {
        own_alert *e = &d->alerts[i];
        if (e->state != AL_WAIT || e->busy || now < e->next) continue;
        if (now >= e->until) e->state = AL_REFUSED;
        else if (n < 16) e->next = now + backoff_ms(d, e->tries + 1), pairs[n][0] = e->mine, pairs[n][1] = e->theirs, n++;
    }
    pthread_mutex_unlock(&d->a_mu);
    for (int i = 0; i < n; i++) (void)job_push(d, JOB_ALERT_OWN, &pairs[i][0], &pairs[i][1], NULL);
}

int dht_mbox_poll(dht *d)
{
    uint8_t k[32], req[33];
    dht_contact mb[DHT_KSTORE];
    dht_mbox_key(d->me.xonly, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    uint8_t *ans = malloc(MBOX_LEN);
    int fresh = 0;
    req[0] = DHT_MBOX_GET;
    memcpy(req + 1, d->me.xonly, 32);
    for (int i = 0; i < m && ans && !stopping(d); i++) {
        size_t len;
        if (is_me(d, mb[i].id)) continue;                      /* what came here was handled when it came */
        const bool ok = exchange(d, &mb[i], req, sizeof req, ans, MBOX_LEN, &len) && len >= 2 && ans[1] <= DHT_MBOX_MAX &&
                        len == 2 + (size_t)ans[1] * DHT_MBOX_ENTRY;
        heard(d, &mb[i], ok);
        for (int p = 0; ok && p < ans[1]; p++) {
            const uint8_t *q = ans + 2 + (size_t)p * DHT_MBOX_ENTRY, flags = q[PAIR_LEN];
            record r[2];
            uint8_t h[32];
            bool theirs_ok;
            if (!record_decode(q, &r[0]) || !record_decode(q + RECORD_LEN, &r[1]) || flags > 3) continue;
            const int me = mine(d, &r[0]) ? 0 : mine(d, &r[1]) ? 1 : -1;
            if (me < 0) continue;
            pair_hash(&r[0], &r[1], h);
            const int st = alert_state(d, h, &theirs_ok);
            if (st > AL_WAIT || (st < 0 && !pair_ok(d, &r[0], &r[1]))) continue;   /* the signatures only of a new pair */
            /* the other owner's proof for this node, when the mailbox has it */
            const uint8_t *pf = flags & (1 << (1 - me)) ? q + PAIR_LEN + 1 + (size_t)(1 - me) * 64 : NULL;
            if (pf && (theirs_ok || !record_proof_ok(d->cfg.ctx, d->cfg.net, &r[1 - me], d->me.xonly, h, pf))) pf = NULL;
            if (st < 0 || pf) fresh += alert_in(d, &r[1 - me], &r[me], pf);
        }
    }
    free(ans);
    if (fresh) dlog(d, "%d alerts from the mailboxes", fresh);
    return fresh;
}

int dht_local_mbox(dht *d, const uint8_t owner[32], mbox_pair *pairs, int max) { return mbox_get(d->mb, owner, pairs, max); }
