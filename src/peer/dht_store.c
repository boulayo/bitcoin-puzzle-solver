/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_store.c - the records (docs/en/NETWORK.md 7.3, 7.4): what this node takes as a custodian, and what it publishes (own.bin, own.seq,
 * the republish). */
#include "dht_priv.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define RESEND_MS (3600 * 1000LL)  /* a custodian alerts the same pair at most once an hour (a republished record alerts again) */

/* ---- custody ---- */

/* whether a custodian may alert this pair now: not within RESEND_MS of the last time */
static bool sent_check(dht *d, const uint8_t h[32])
{
    const int64_t now = sys_now_ms();
    bool ok = true;
    int slot = -1, oldest = 0;
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nsent && slot < 0; i++) {
        if (!memcmp(d->sent[i].h, h, 32)) slot = i, ok = now - d->sent[i].at >= RESEND_MS;
        else if (d->sent[i].at < d->sent[oldest].at) oldest = i;
    }
    if (slot < 0) slot = d->nsent < SENT_MAX ? d->nsent++ : oldest;
    if (ok) {
        memcpy(d->sent[slot].h, h, 32);
        d->sent[slot].at = now;
    }
    pthread_mutex_unlock(&d->a_mu);
    return ok;
}

/* A record for the custody of this node: its status for STORED. from: the address of the owner that sent it, NULL for this node's own.
 * A record of another node is taken only when this node may be one of its custodians, and within the rate of the sender's block. Each
 * coincidence becomes a job that alerts the owners and their mailboxes, also for a duplicate (a republished record alerts again, at most
 * once an hour: an alert lost on the way comes back). Each alert costs this node lookups and a dozen connections, so the coincidences of
 * one block are limited too, and each record alerts at most two pairs: more owners of one x are a flood, not walks. */
int custody_put(dht *d, const record *r, const net_addr *from)
{
    uint8_t k[32];
    record held, others[2];
    int no;
    if (!owner_ok(d, r)) return DHT_ST_INVALID;
    store_key(r->x, k);
    if (from && !responsible(d, k)) return DHT_ST_FAR;
    if (from && !lim_take(d, from, LIM_REC, 1)) return DHT_ST_FULL;
    const store_status s = store_put(d->custody, r, &held, others, 2, &no);
    if (s == STORE_FULL) held = *r;                            /* not kept, but its coincidences are alerted all the same */
    for (int i = 0; i < no; i++) {
        uint8_t h[32];
        pair_hash(&held, &others[i], h);
        if ((!from || lim_take(d, from, LIM_COIN, 1)) && sent_check(d, h) && job_push(d, JOB_ALERT_OUT, &held, &others[i], NULL))
            dlog(d, "coincidence (%s): alerting the owners", held.t == others[i].t ? "merge" : "useful");
    }
    switch (s) {
    case STORE_OK: return no ? DHT_ST_COINCIDENCE : DHT_ST_STORED;
    case STORE_DUP: return DHT_ST_DUP;
    case STORE_INVALID: return DHT_ST_INVALID;
    case STORE_OLD: return DHT_ST_OLD;
    case STORE_FULL: return DHT_ST_FULL;
    default: return DHT_ST_FAILED;
    }
}

/* ---- publishing ---- */

/* own.seq: the last sequence number this node signed. own.bin keeps the records as they were first published, and a republish signs
 * them again with higher numbers: after a restart with a clock that went back, the next records would be refused by every custodian
 * until the clock caught up with the last republish. */
static void save_seq(dht *d)
{
    char path[4096], tmp[4100];
    snprintf(path, sizeof path, "%s/own.seq", d->dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    const bool ok = fprintf(f, "%" PRIu64 "\n", d->seq) > 0;
    if (fclose(f) == 0 && ok) (void)sys_replace(tmp, path);
    else (void)remove(tmp);
}

uint64_t load_seq(const dht *d)
{
    char path[4096];
    uint64_t v = 0;
    snprintf(path, sizeof path, "%s/own.seq", d->dir);
    FILE *f = fopen(path, "r");
    if (f && fscanf(f, "%" SCNu64, &v) != 1) v = 0;
    if (f) fclose(f);
    return v;
}

/* At most DHT_STORE_MAX records, in the order of their sequence numbers: a lookup for each, then one STORE per custodian with all its
 * records in that order. The records that at least one custodian holds. */
static int send_batch(dht *d, const record *recs, int n)
{
    dht_contact (*cs)[DHT_KSTORE] = malloc((size_t)n * sizeof *cs);   /* the custodians of each record */
    int *ncs = calloc((size_t)n, sizeof *ncs), done = 0;
    bool (*sent)[DHT_KSTORE] = calloc((size_t)n, sizeof *sent);
    uint8_t *held = calloc((size_t)n, 1), *req = malloc(2 + DHT_STORE_MAX * RECORD_LEN);
    int *idx = malloc((size_t)n * sizeof *idx);
    const bool ok = cs && ncs && sent && held && req && idx;
    for (int i = 0; ok && i < n && !stopping(d); i++) {
        uint8_t k[32];
        store_key(recs[i].x, k);
        ncs[i] = dht_lookup(d, k, cs[i], DHT_KSTORE);
    }
    for (int i = 0; ok && i < n && !stopping(d); i++)
        for (int j = 0; j < ncs[i]; j++) {
            if (sent[i][j]) continue;
            const dht_contact t = cs[i][j];                    /* a custodian not served yet: all its records, in order */
            int m = 0;
            for (int i2 = i; i2 < n; i2++)
                for (int j2 = 0; j2 < ncs[i2]; j2++)
                    if (!sent[i2][j2] && !memcmp(cs[i2][j2].id, t.id, 32)) sent[i2][j2] = true, idx[m++] = i2;
            uint8_t st[DHT_STORE_MAX], ans[2 + DHT_STORE_MAX];
            if (is_me(d, t.id)) {
                for (int q = 0; q < m; q++) st[q] = (uint8_t)custody_put(d, &recs[idx[q]], NULL);
            } else {
                size_t len;
                req[0] = DHT_STORE, req[1] = (uint8_t)m;
                for (int q = 0; q < m; q++) record_encode(&recs[idx[q]], req + 2 + (size_t)q * RECORD_LEN);
                const bool got = exchange(d, &t, req, 2 + (size_t)m * RECORD_LEN, ans, sizeof ans, &len) && len == 2 + (size_t)m && ans[1] == m;
                heard(d, &t, got);
                if (!got) continue;
                memcpy(st, ans + 2, (size_t)m);
            }
            for (int q = 0; q < m; q++) held[idx[q]] |= st[q] == DHT_ST_STORED || st[q] == DHT_ST_DUP || st[q] == DHT_ST_COINCIDENCE;
        }
    for (int i = 0; ok && i < n; i++) done += held[i];
    free(cs), free(ncs), free(sent), free(held), free(req), free(idx);
    return done;
}

/* the next sequence number of this owner: microseconds of the clock, and always above the last (p_mu held) */
static bool sign_locked(dht *d, record *r)
{
    const uint64_t now = wall_us();
    d->seq = now > d->seq ? now : d->seq + 1;
    r->seq = d->seq;
    memcpy(r->owner, d->me.xonly, 32);
    return record_sign(d->cfg.ctx, d->cfg.net, &d->cfg.self->kp, r);
}

int dht_publish(dht *d, record *recs, int n)
{
    int done = 0;
    pthread_mutex_lock(&d->p_mu);
    for (int i = 0; i < n; i++) {
        record held;
        int no;
        if (!sign_locked(d, &recs[i])) continue;              /* never in practice; the custodians would refuse it */
        if (store_put(d->own, &recs[i], &held, NULL, 0, &no) == STORE_DUP && held.t != recs[i].t)
            dlog(d, "this node has the same x with two types");   /* a coincidence of the home with itself: its pzsolve sees it */
    }
    save_seq(d);
    for (int o = 0; o < n && !stopping(d); o += DHT_STORE_MAX) done += send_batch(d, recs + o, n - o < DHT_STORE_MAX ? n - o : DHT_STORE_MAX);
    pthread_mutex_unlock(&d->p_mu);
    return done;
}

/* The records of own.bin again, signed with new sequence numbers: a custodian that joined since the last time takes them (it would refuse
 * the old numbers, which are below the last it saw of this owner); the ones that hold them already answer DUP. */
void republish(dht *d)
{
    record *batch = malloc(DHT_STORE_MAX * sizeof *batch);
    const uint64_t total = store_count(d->own);
    uint64_t i = 0;
    while (batch && i < total && !stopping(d)) {
        int m = 0;
        pthread_mutex_lock(&d->p_mu);
        for (; i < total && m < DHT_STORE_MAX; i++)
            if (store_read(d->own, i, &batch[m]) && sign_locked(d, &batch[m])) m++;
        save_seq(d);
        (void)send_batch(d, batch, m);
        pthread_mutex_unlock(&d->p_mu);
    }
    free(batch);
    dlog(d, "republished %llu records", (unsigned long long)total);
}

int dht_local_records(dht *d, const uint8_t x[32], record *out, int max) { return store_find(d->custody, x, out, max); }

uint64_t dht_local_count(dht *d) { return store_count(d->custody); }

