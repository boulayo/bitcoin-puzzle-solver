/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_alert.c - the alerts of a coincidence (docs/en/NETWORK.md 7.3): those of this node and the proofs of possession they wait for, the
 * proof it sends to the other owner, the mailboxes. */
#include "dht_priv.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <stdlib.h>

#define ALERTS_MAX 1024            /* alerts of this node remembered */
#define POSTS 24                   /* this node's proof of an alert goes to the other owner this many times in the life of the alert (every
                                    * hour of its day), also once it is done: a mailbox that lost it gets it again */
#define BACKOFF_MAX_MS (3600 * 1000LL)

enum { AL_WAIT, AL_DONE, AL_REFUSED };

struct own_alert {
    uint8_t h[32];
    record mine, theirs;
    int state;
    bool mine_ok, theirs_ok;       /* the proofs checked: this node's own (it knows the distance), the other owner's */
    bool queued, busy;             /* a job for it waits in the queue; a job works on it (one at a time) */
    bool blocked;                  /* it came from another node: its jobs count against the address block it came from, also when retried */
    uint8_t block[16];
    int tries;
    int64_t until, next, posted;   /* its life; its next try (once done, its next post); this node's proof last taken by the other owner
                                    * or a mailbox of it */
};

void dht_mbox_key(const uint8_t owner[32], uint8_t k[32])
{
    uint8_t m[11 + 32] = "pznet1/mbox";
    memcpy(m + 11, owner, 32);
    pz_sha256(m, sizeof m, k);
}

/* the owner of a record is a node: its node ID has the zero bits (one hash; the other owner finds it by its ID) */
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

/* The alert of a pair with a record m of this node, from the address block `block` (NULL: none) (a_mu held): found, or added (*fresh).
 * When the table is full the oldest one that is not waiting goes, else the oldest waiting one with no job queued, else the oldest no job
 * works on: a flood of alerts that will never be proved (pairs of a point this node does not hold, the copies of its points that a
 * custodian answers to PROVE) cannot shut out the next real one, whose job is queued at once and keeps it while it waits behind the jobs
 * of the flood (the alerts of one address block hold ALERT_JOBS_BLOCK places in the queue, also when tried again). An alert pushed out
 * comes back with a mailbox poll, or the next republish to every custodian. NULL only without memory. */
static own_alert *alert_add_locked(dht *d, const record *m, const record *t, const uint8_t h[32], const uint8_t *block, bool *fresh)
{
    *fresh = false;
    own_alert *e = alert_find(d, h);
    if (e) return e;
    if (d->nalerts == ALERTS_MAX) {
        int at = -1, rank = 3;                                 /* 0 done or refused, 1 waiting, 2 its job queued, 3 worked on */
        for (int i = 0; i < d->nalerts && rank; i++) {
            const own_alert *a = &d->alerts[i];
            const int r = a->busy ? 3 : a->state != AL_WAIT ? 0 : a->queued ? 2 : 1;
            if (r < rank) at = i, rank = r;
        }
        if (at < 0) return NULL;
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
    if ((e->blocked = block != NULL)) memcpy(e->block, block, 16);
    e->state = AL_WAIT;
    e->until = sys_now_ms() + d->alert_ms;
    e->next = sys_now_ms() + d->retry_ms;
    *fresh = true;
    return e;
}

/* the job of the alert h, which the caller marked queued: with no room in the queue the mark goes, and the maintenance thread retries it */
static void alert_job(dht *d, const uint8_t h[32], const record *m, const record *t, const uint8_t *block)
{
    if (job_push(d, JOB_ALERT, m, t, NULL, block)) return;
    pthread_mutex_lock(&d->a_mu);
    own_alert *e = alert_find(d, h);
    if (e) e->queued = false;
    pthread_mutex_unlock(&d->a_mu);
}

/* A checked pair that came to this node, from the address from (NULL: this node). With the proof of the owner of a for the owner of b
 * (checked): an ALERT, or a pair of a mailbox. Without one (NULL): the answer of a custodian to PROVE, or this node's own custody. A pair
 * with a proof is kept as a mailbox when this node may be a mailbox of one of the owners; a pair with a record of this node is an alert
 * of this node, whose job counts against the address block of from. True for a new alert of this node. */
bool alert_in(dht *d, const record *a, const record *b, const uint8_t *proof, const net_addr *from)
{
    uint8_t ka[32], kb[32], h[32], blk[16];
    dht_mbox_key(a->owner, ka), dht_mbox_key(b->owner, kb);
    if (proof && (responsible(d, ka) || responsible(d, kb)) && mbox_put(d->mb, a, b, proof)) dlog(d, "an alert pair in the mailbox");
    if (!mine(d, a) && !mine(d, b)) return false;
    const record *m = mine(d, a) ? a : b, *t = m == a ? b : a;
    const bool blocked = from && limited(d, from);
    bool fresh, go = false;
    if (blocked) net_addr_block(from, blk);
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *e = alert_add_locked(d, m, t, h, blocked ? blk : NULL, &fresh);
    if (e && e->state == AL_WAIT && proof && t == a && !e->theirs_ok) e->theirs_ok = go = true;   /* the other owner proved its record */
    const bool push = (fresh || go) && !e->queued && !e->busy; /* a job queued or under way sees the proof that came */
    if (push) e->queued = true;
    pthread_mutex_unlock(&d->a_mu);
    if (fresh) dlog(d, "an alert for this node: checking the proofs");
    if (push) alert_job(d, h, m, t, blocked ? blk : NULL);
    return fresh;
}

/* ALERT(a, b, the proof of the owner of a for the owner of b) to a contact; to this node itself, what it would do with it. Whether it was
 * taken. */
static bool send_alert(dht *d, const dht_contact *c, const record *a, const record *b, const uint8_t proof[64])
{
    if (is_me(d, c->id)) {
        (void)alert_in(d, a, b, proof, NULL);
        return true;
    }
    uint8_t req[1 + PAIR_LEN + 64], ans[1];
    size_t n;
    req[0] = DHT_ALERT;
    record_encode(a, req + 1);
    record_encode(b, req + 1 + RECORD_LEN);
    memcpy(req + 1 + PAIR_LEN, proof, 64);
    const bool ok = exchange(d, c, req, sizeof req, ans, sizeof ans, &n) && n == 1;
    heard(d, c, ok);
    return ok;
}

/* whether this node can prove its own record: it knows the distance, and record_prove finds that it fits (the proof is not sent, so
 * any nonce does) */
static bool own_proof(dht *d, const record *r)
{
    uint8_t dd[32], sig[64];
    const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, r->x, r->t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, r, dd, d->me.xonly, CUSTODY_NONCE, sig);
    pz_secure_zero(dd, sizeof dd);
    return ok;
}

/* Only a dial that fails counts against the contact. An owner that closes without a proof (it has no such point) is neither seen nor
 * failed: a pair that names an honest node, at a lying custodian, cannot make this node drop it from the table. */
int ask_proof(dht *d, const dht_contact *c, const record *r)
{
    uint8_t req[1 + RECORD_LEN + 32], ans[1 + 64];
    size_t n;
    int st = -1;
    net_conn *k = dht_dial(d, c);                              /* checked to be the owner: the identity of its contact */
    if (!k) return rt_failed(d, c), -1;
    req[0] = DHT_POSSESS;
    record_encode(r, req + 1);
    if (sys_random(req + 1 + RECORD_LEN, 32) && net_send(k, req, sizeof req) && net_recv(k, ans, sizeof ans, &n))
        st = n == sizeof ans && ans[0] == DHT_PROOF && record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, req + 1 + RECORD_LEN, ans + 1);
    net_close(k);
    if (st >= 0) rt_seen(d, c);
    return st;
}

/* This node's proof of possession for the other owner of an alert, over the pair: the pair's name is the nonce, so the proof can wait in
 * a mailbox, and it binds the other owner as the asker, so it proves nothing to anyone else (docs/en/NETWORK.md 7.3). In an ALERT to
 * the other owner at its contact c when it was found, and to its mailboxes, where it finds it when it does not listen: this is how
 * the other owner hears of the pair. Whether the other owner or one of its mailboxes took it: a post that reached nobody is no post. */
static bool post_proof(dht *d, const own_alert *e, const dht_contact *c)
{
    uint8_t dd[32], proof[64], k[32];
    dht_contact mb[DHT_KSTORE];
    const bool ok = d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, e->mine.x, e->mine.t, dd) &&
                    record_prove(d->cfg.ctx, d->cfg.net, &e->mine, dd, e->theirs.owner, e->h, proof);
    pz_secure_zero(dd, sizeof dd);
    if (!ok) return false;
    bool took = c && send_alert(d, c, &e->mine, &e->theirs, proof);
    dht_mbox_key(e->theirs.owner, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    for (int i = 0; i < m && !stopping(d); i++) took |= send_alert(d, &mb[i], &e->mine, &e->theirs, proof);
    return took;
}

static int64_t backoff_ms(const dht *d, int tries)
{
    int64_t w = d->retry_ms;
    for (int i = 1; i < tries && w < BACKOFF_MAX_MS; i++) w *= 2;
    return w < BACKOFF_MAX_MS ? w : BACKOFF_MAX_MS;
}

/* The job of an alert of this node: its own proof (when the distance is missing the daemon may not have read that point yet, as after a
 * restart: the alert waits), then this node's proof sent to the other owner, then the other owner's proof, asked with POSSESS unless it
 * came already (an ALERT, a mailbox); the other owner is looked up once for both. on_alert when both check out and the other owner, or
 * a mailbox of it, took this node's proof (else that owner may never hear of the pair); only a wrong proof of the other owner refuses
 * it. An alert that waits is tried again later each time (backoff_ms) for alert_ms, then forgotten (retry_alerts); one done posts this
 * node's proof again every alert_ms / POSTS for the rest of that life, and on_alert is not called again. */
void job_alert(dht *d, const record *a, const record *b)
{
    uint8_t h[32];
    own_alert e;
    pair_hash(a, b, h);
    pthread_mutex_lock(&d->a_mu);
    own_alert *p = alert_find(d, h);
    if (p) p->queued = false;
    const bool go = p && p->state != AL_REFUSED && !p->busy;
    if (go) p->busy = true, e = *p;
    pthread_mutex_unlock(&d->a_mu);
    if (!go) return;                                           /* refused, or a job on it now */
    const int was = e.state;
    if (!e.mine_ok) e.mine_ok = own_proof(d, &e.mine);
    dht_contact c;
    const bool post = e.mine_ok && (!e.posted || sys_now_ms() - e.posted >= d->alert_ms / POSTS), ask = e.mine_ok && !e.theirs_ok;
    const bool found = (post || ask) && dht_find(d, e.theirs.owner, &c);
    if (post && post_proof(d, &e, found ? &c : NULL)) e.posted = sys_now_ms();
    const int asked = !ask ? -2 : found ? ask_proof(d, &c, &e.theirs) : -1;
    int st = AL_WAIT;
    pthread_mutex_lock(&d->a_mu);
    if ((p = alert_find(d, h))) {
        p->busy = false;
        p->mine_ok = e.mine_ok, p->posted = e.posted;
        p->theirs_ok = p->theirs_ok || asked == 1;             /* or its proof came while this job asked */
        if (asked == 0) p->state = AL_REFUSED;
        else if (p->mine_ok && p->theirs_ok && p->posted) p->state = AL_DONE, p->next = sys_now_ms() + d->alert_ms / POSTS;
        else p->tries++, p->next = sys_now_ms() + backoff_ms(d, p->tries);
        st = p->state;
        e = *p;
    }
    pthread_mutex_unlock(&d->a_mu);
    dlog(d, was == AL_DONE     ? "alert: done, this node's proof sent again (a mailbox may have lost it)"
            : st == AL_DONE    ? "alert: both proofs check out"
            : st == AL_REFUSED ? "alert refused: the other owner's proof does not check out"
            : !e.mine_ok       ? "alert: this node does not know the distance of its point (yet), retried later"
            : !e.posted        ? "alert: this node's proof reached neither the other owner nor a mailbox of it, retried later"
                               : "alert: the other owner's proof has not come, retried later");
    if (st == AL_DONE && was != AL_DONE && d->cfg.on_alert) d->cfg.on_alert(d->cfg.user, &e.mine, &e.theirs, e.mine.t == e.theirs.t);
}

/* The alerts that wait, and those done within their life: those that are due queued again (one done posts this node's proof again, for
 * a mailbox that may have lost it). Those that wait past their life forgotten, so that the pair is a new alert when it comes again (the
 * other owner's ALERT, a mailbox, the next republish to every custodian); before, they were refused as for a wrong proof, and nothing
 * of the pair counted any more. One done stays, so the daemon is told once. */
void retry_alerts(dht *d)
{
    own_alert due[16];
    int n = 0;
    const int64_t now = sys_now_ms();
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nalerts; i++) {
        own_alert *e = &d->alerts[i];
        const bool done = e->state == AL_DONE;
        if (e->state == AL_REFUSED || e->queued || e->busy || (done && now >= e->until)) continue;
        if (now >= e->until) {
            memmove(e, e + 1, (size_t)(d->nalerts - i - 1) * sizeof *e);
            d->nalerts--, i--;
        } else if (now >= e->next && n < 16) {
            e->next = now + (done ? d->alert_ms / POSTS : backoff_ms(d, e->tries + 1)), e->queued = true, due[n++] = *e;
        }
    }
    pthread_mutex_unlock(&d->a_mu);
    for (int i = 0; i < n; i++) alert_job(d, due[i].h, &due[i].mine, &due[i].theirs, due[i].blocked ? due[i].block : NULL);
}

/* The pairs of an MBOX answer from c that concern this node. From a mailbox, each that brings the other owner's proof becomes an alert:
 * a mailbox cannot make one of a pair nobody proved. From a custodian after PROVE (the n records proved), each new pair of those
 * records does, at most as many for each as a custodian keeps owners of one x: the custodian cannot bring alerts of the other records
 * of this node, nor more of them than it may hold. False when the answer is malformed (its type exchange checked). */
static bool mbox_take(dht *d, const dht_contact *c, const uint8_t *ans, size_t len, const record *proved, int n, int *fresh)
{
    int took[DHT_PROVE_MAX] = {0};                             /* after PROVE: the pairs of each record proved */
    if (len < 2 || ans[1] > DHT_MBOX_MAX || len != 2 + (size_t)ans[1] * DHT_MBOX_ENTRY) return false;
    for (int p = 0; p < ans[1]; p++) {
        const uint8_t *q = ans + 2 + (size_t)p * DHT_MBOX_ENTRY, flags = q[PAIR_LEN];
        record r[2];
        uint8_t h[32];
        bool theirs_ok;
        if (!record_decode(q, &r[0]) || !record_decode(q + RECORD_LEN, &r[1]) || flags > 3) continue;
        const int me = mine(d, &r[0]) ? 0 : mine(d, &r[1]) ? 1 : -1;
        int of = 0;                                            /* after PROVE: which record proved it is of */
        while (proved && me >= 0 && of < n && !same_record(&r[me], &proved[of])) of++;
        if (me < 0 || (proved && (of == n || took[of]++ >= STORE_X_OWNERS))) continue;
        pair_hash(&r[0], &r[1], h);
        const int st = alert_state(d, h, &theirs_ok);
        if (st > AL_WAIT || (st < 0 && !pair_ok(d, &r[0], &r[1]))) continue;   /* the checks only of a new pair */
        /* the other owner's proof for this node, when the mailbox has it */
        const uint8_t *pf = flags & (1 << (1 - me)) ? q + PAIR_LEN + 1 + (size_t)(1 - me) * 64 : NULL;
        if (pf && (theirs_ok || !record_proof_ok(d->cfg.ctx, d->cfg.net, &r[1 - me], d->me.xonly, h, pf))) pf = NULL;
        if (pf || (proved && st < 0)) *fresh += alert_in(d, &r[1 - me], &r[me], pf, &c->addr);
    }
    return true;
}

int dht_mbox_poll(dht *d)
{
    uint8_t k[32], req[33];
    dht_contact mb[DHT_KSTORE];
    if (d->cfg.blocks) return 0;                               /* a linear search has no records, so no alerts */
    dht_mbox_key(d->me.xonly, k);
    const int m = dht_lookup(d, k, mb, DHT_KSTORE);
    uint8_t *ans = malloc(MBOX_LEN);
    int fresh = 0;
    req[0] = DHT_MBOX_GET;
    memcpy(req + 1, d->me.xonly, 32);
    for (int i = 0; i < m && ans && !stopping(d); i++) {
        size_t len;
        if (is_me(d, mb[i].id)) continue;                      /* what came here was handled when it came */
        heard(d, &mb[i], exchange(d, &mb[i], req, sizeof req, ans, MBOX_LEN, &len) && mbox_take(d, &mb[i], ans, len, NULL, 0, &fresh));
    }
    free(ans);
    if (fresh) dlog(d, "%d alerts from the mailboxes", fresh);
    return fresh;
}

/* The nonce of PROVE: fixed, so the owner makes the proof without a round trip. The proof binds the custodian as the asker, so a copy of
 * it convinces that custodian alone, of what is true. */
const uint8_t CUSTODY_NONCE[32];

/* A custodian c told COINCIDENCE for the n records r (at most DHT_PROVE_MAX): one PROVE with this node's proof of each (of those whose
 * distance it knows), and the pairs it answers become alerts of this node. False when the custodian did not answer it as it should. */
bool prove_to(dht *d, const dht_contact *c, const record *r, int n)
{
    uint8_t req[2 + DHT_PROVE_MAX * PROVE_ENTRY], *ans = malloc(MBOX_LEN);
    record proved[DHT_PROVE_MAX];
    size_t len;
    int k = 0, fresh = 0;
    for (int i = 0; i < n && i < DHT_PROVE_MAX; i++) {
        uint8_t dd[32], *q = req + 2 + (size_t)k * PROVE_ENTRY;
        if (d->cfg.own_distance && d->cfg.own_distance(d->cfg.user, r[i].x, r[i].t, dd) &&
            record_prove(d->cfg.ctx, d->cfg.net, &r[i], dd, c->xonly, CUSTODY_NONCE, q + RECORD_WIRE))
            record_wire_encode(&r[i], q), proved[k++] = r[i];
        pz_secure_zero(dd, sizeof dd);
    }
    req[0] = DHT_PROVE, req[1] = (uint8_t)k;
    const bool ok = !k || (ans && exchange(d, c, req, 2 + (size_t)k * PROVE_ENTRY, ans, MBOX_LEN, &len) &&
                           mbox_take(d, c, ans, len, proved, k, &fresh));
    if (k) heard(d, c, ok);
    free(ans);
    if (fresh) dlog(d, "%d alerts from a custodian of the point", fresh);
    return ok;
}
