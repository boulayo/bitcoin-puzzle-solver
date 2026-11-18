/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_store.c - the records (docs/en/NETWORK.md 7.3, 7.4): what this node takes as a custodian, and what it publishes (own.bin, the
 * republish). */
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

/* an alert that found the queue full: the next record of the pair tries again, within the hour */
static void sent_forget(dht *d, const uint8_t h[32])
{
    pthread_mutex_lock(&d->a_mu);
    for (int i = 0; i < d->nsent; i++)
        if (!memcmp(d->sent[i].h, h, 32)) d->sent[i].at = sys_now_ms() - RESEND_MS;
    pthread_mutex_unlock(&d->a_mu);
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
        if ((!from || lim_take(d, from, LIM_COIN, 1)) && sent_check(d, h)) {
            if (job_push(d, JOB_ALERT_OUT, &held, &others[i], NULL)) dlog(d, "coincidence (%s): alerting the owners", held.t == others[i].t ? "merge" : "useful");
            else sent_forget(d, h);
        }
    }
    switch (s) {
    case STORE_OK: return no ? DHT_ST_COINCIDENCE : DHT_ST_STORED;
    case STORE_DUP: return DHT_ST_DUP;
    case STORE_INVALID: return DHT_ST_INVALID;
    case STORE_FULL: return DHT_ST_FULL;
    default: return DHT_ST_FAILED;
    }
}

/* ---- the custodians of a key ---- */

#define CC_MAX 8192                /* lookups kept */
#define CC_TTL_MS (3600 * 1000LL)  /* for an hour, as the buckets are refreshed */

/* the place of k in the order of the targets: the first entry whose target is not below k */
static int cc_at(const cc_list *l, const uint8_t k[32])
{
    int lo = 0, hi = l->n;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (memcmp(l->e[mid].t, k, 32) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* e, in place of an entry with its target, or added (the oldest goes when the list is full) */
static void cc_put(cc_list *l, const cc_entry *e)
{
    int at = cc_at(l, e->t);
    if (at < l->n && !memcmp(l->e[at].t, e->t, 32)) {
        l->e[at] = *e;
        return;
    }
    if (l->n == l->cap) {
        const int cap = l->cap ? l->cap * 2 : 64;
        cc_entry *m = cap <= CC_MAX ? realloc(l->e, (size_t)cap * sizeof *m) : NULL;
        if (m) l->e = m, l->cap = cap;
        else if (l->n) {                                       /* full: the oldest goes */
            int old = 0;
            for (int i = 1; i < l->n; i++) old = l->e[i].at < l->e[old].at ? i : old;
            memmove(l->e + old, l->e + old + 1, (size_t)(l->n - old - 1) * sizeof *l->e), l->n--;
            if (old < at) at--;
        }
        if (l->n == l->cap) return;
    }
    memmove(l->e + at + 1, l->e + at, (size_t)(l->n - at) * sizeof *e), l->n++;
    l->e[at] = *e;
}

/* The DHT_KSTORE nodes of e closest to k, when e shows they are the closest of all (else 0). A node not in e is at least D from its
 * target t, D the distance of the farthest in e; a XOR with d(t, k) < 2^b changes only the bits of a distance below b, so that node is at
 * least D with those bits cleared from k. A lookup that found fewer than DHT_K knows every node. */
static int cc_cover(const cc_entry *e, const uint8_t k[32], dht_contact out[DHT_KSTORE])
{
    int idx[DHT_K], last = 0;
    const int m = e->n < DHT_KSTORE ? e->n : DHT_KSTORE;
    for (int i = 0; i < e->n; i++) idx[i] = i, last = closer(e->t, e->c[last].id, e->c[i].id) ? i : last;
    for (int i = 0; i < m; i++)                                /* the m closest to k, in order */
        for (int j = i + 1; j < e->n; j++)
            if (closer(k, e->c[idx[j]].id, e->c[idx[i]].id)) {
                const int x = idx[i];
                idx[i] = idx[j], idx[j] = x;
            }
    if (e->n == DHT_K) {
        uint8_t bound[32], dk[32];
        int b = 0;                                             /* the first bit where t and k differ: the bits from b on are below 2^b */
        while (b < 256 && !((e->t[b / 8] ^ k[b / 8]) & (0x80 >> (b % 8)))) b++;
        for (int i = 0; i < 32; i++) bound[i] = (uint8_t)(e->t[i] ^ e->c[last].id[i]), dk[i] = (uint8_t)(k[i] ^ e->c[idx[m - 1]].id[i]);
        for (int i = b; i < 256; i++) bound[i / 8] &= (uint8_t)~(0x80 >> (i % 8));
        if (memcmp(dk, bound, 32) >= 0) return 0;
    }
    for (int i = 0; i < m; i++) out[i] = e->c[idx[i]];
    return m;
}

/* The custodians of k from one of the two entries around k in the order of the targets (the nearest: they share the longest prefix
 * with k) that shows them; with fresh, of the last hour only. 0 when neither does. */
static int cc_find(const cc_list *l, const uint8_t k[32], bool fresh, dht_contact out[DHT_KSTORE], const cc_entry **e)
{
    const int at = cc_at(l, k);
    for (int i = at - 1; i <= at; i++) {
        const int n = i >= 0 && i < l->n && (!fresh || sys_now_ms() - l->e[i].at < CC_TTL_MS) ? cc_cover(&l->e[i], k, out) : 0;
        if (n) return *e = &l->e[i], n;
    }
    return 0;
}

/* The custodians of key k, from a lookup of the last hour that shows them, else from a new one, kept. *t: the target of that lookup;
 * used, when not NULL, gets it too. */
static int custodians(dht *d, const uint8_t k[32], dht_contact out[DHT_KSTORE], uint8_t t[32], cc_list *used)
{
    const cc_entry *e = NULL;
    cc_entry f;
    int n = cc_find(&d->cc, k, true, out, &e);
    if (!n) {
        memcpy(f.t, k, 32), f.at = sys_now_ms();
        if ((f.n = dht_lookup(d, k, f.c, DHT_K)) <= 0) return 0;
        cc_put(&d->cc, &f);
        e = &f, n = cc_cover(&f, k, out);
    }
    memcpy(t, e->t, 32);
    if (used) cc_put(used, e);
    return n;
}

/* ---- own.bin: the records this node published, in order ("PZOWN002", F, then RECORD_WIRE bytes a record: the owner is this node) ---- */

bool own_open(dht *d, char *err, size_t errlen)
{
    char path[4200];
    const char *why = "";
    path_of(d, "own.bin", path, sizeof path);
    const int64_t n = store_file(path, "PZOWN002", d->cfg.net->fp, RECORD_WIRE, &d->ownfd, &why);
    if (n < 0) return snprintf(err, errlen, "%s: %s", path, why), false;
    d->nown = d->replay_end = (uint64_t)n;
    return true;
}

static bool own_read(dht *d, uint64_t i, record *r)
{
    uint8_t e[RECORD_LEN];
    memcpy(e + RECORD_WIRE, d->me.xonly, 32);
    return sys_pread(d->ownfd, e, RECORD_WIRE, STORE_HDR + i * RECORD_WIRE) == RECORD_WIRE && record_decode(e, r);
}

static void own_append(dht *d, const record *recs, int n)
{
    uint8_t b[DHT_STORE_MAX * RECORD_WIRE];
    for (int i = 0; i < n; i++) {
        uint8_t e[RECORD_LEN];
        record_encode(&recs[i], e);
        memcpy(b + (size_t)i * RECORD_WIRE, e, RECORD_WIRE);
    }
    const size_t len = (size_t)n * RECORD_WIRE;
    if (sys_pwrite(d->ownfd, b, len, STORE_HDR + d->nown * RECORD_WIRE) == (long)len) d->nown += (uint64_t)n;
}

/* ---- publishing ---- */

/* At most DHT_STORE_MAX records: the custodians of each, then one STORE per custodian with all its records. The first nold of them go
 * only to custodians that are not those old shows (the last republish). The records that at least one custodian holds. */
static int send_batch(dht *d, const record *recs, int n, cc_list *old, int nold, cc_list *used, uint64_t *copies)
{
    dht_contact (*cs)[DHT_KSTORE] = malloc((size_t)n * sizeof *cs);   /* the custodians of each record */
    uint8_t (*ts)[32] = malloc((size_t)n * sizeof *ts);        /* the lookup they come from */
    int *ncs = calloc((size_t)n, sizeof *ncs), done = 0;
    bool (*sent)[DHT_KSTORE] = calloc((size_t)n, sizeof *sent);
    uint8_t *held = calloc((size_t)n, 1), *req = malloc(2 + DHT_STORE_MAX * RECORD_WIRE);
    int *idx = malloc((size_t)n * sizeof *idx);
    const bool ok = cs && ts && ncs && sent && held && req && idx;
    for (int i = 0; ok && i < n && !stopping(d); i++) {
        uint8_t k[32];
        dht_contact had[DHT_KSTORE];
        store_key(recs[i].x, k);
        ncs[i] = custodians(d, k, cs[i], ts[i], used);
        const cc_entry *e;
        const int nh = i < nold ? cc_find(old, k, false, had, &e) : 0;
        for (int j = 0; j < ncs[i]; j++)                       /* those that had it at the last republish have it */
            for (int h = 0; h < nh && !sent[i][j]; h++)
                if (!memcmp(cs[i][j].id, had[h].id, 32)) sent[i][j] = true, held[i] |= 2;
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
            const bool me = is_me(d, t.id);
            if (me) {
                for (int q = 0; q < m; q++) st[q] = (uint8_t)custody_put(d, &recs[idx[q]], NULL);
            } else {
                size_t len;
                req[0] = DHT_STORE, req[1] = (uint8_t)m;
                for (int q = 0; q < m; q++) {                  /* the record without the owner, which the custodian knows */
                    uint8_t e[RECORD_LEN];
                    record_encode(&recs[idx[q]], e);
                    memcpy(req + 2 + (size_t)q * RECORD_WIRE, e, RECORD_WIRE);
                }
                const bool got = exchange(d, &t, req, 2 + (size_t)m * RECORD_WIRE, ans, sizeof ans, &len) && len == 2 + (size_t)m && ans[1] == m;
                heard(d, &t, got);
                if (!got) continue;
                memcpy(st, ans + 2, (size_t)m);
                if (copies) *copies += (uint64_t)m;
            }
            for (int q = 0; q < m; q++) {
                if (st[q] == DHT_ST_STORED || st[q] == DHT_ST_DUP || st[q] == DHT_ST_COINCIDENCE) held[idx[q]] |= me ? 1 : 3;
                const int at = st[q] == DHT_ST_FAR ? cc_at(&d->cc, ts[idx[q]]) : d->cc.n;
                if (at < d->cc.n && !memcmp(d->cc.e[at].t, ts[idx[q]], 32)) d->cc.e[at].at = 0;   /* its lookup is out of date */
            }
        }
    for (int i = 0; ok && i < n; i++) {
        done += held[i] != 0;
        if (!(held[i] & 2)) atomic_store(&d->unplaced, true);  /* on this node alone, or nowhere */
    }
    free(cs), free(ts), free(ncs), free(sent), free(held), free(req), free(idx);
    return done;
}

static bool same_record(const record *a, const record *b)
{
    uint8_t ea[RECORD_LEN], eb[RECORD_LEN];
    record_encode(a, ea), record_encode(b, eb);
    return !memcmp(ea, eb, RECORD_WIRE);
}

int dht_publish(dht *d, record *recs, int n)
{
    int done = 0, skip = 0;
    record r;
    pthread_mutex_lock(&d->p_mu);
    for (int i = 0; i < n; i++) memcpy(recs[i].owner, d->me.xonly, 32);
    /* after a restart the home gives its records again from the first: those that own.bin has, in the same order, went out then */
    for (; skip < n && d->replay < d->replay_end; skip++, d->replay++)
        if (!own_read(d, d->replay, &r) || !same_record(&r, &recs[skip])) {
            d->replay = d->replay_end;
            break;
        }
    for (int o = skip; o < n && !stopping(d); o += DHT_STORE_MAX) {
        const int m = n - o < DHT_STORE_MAX ? n - o : DHT_STORE_MAX;
        done += send_batch(d, recs + o, m, NULL, 0, NULL, NULL);
        if (!stopping(d)) own_append(d, recs + o, m);     /* cut by a stop: given again after the restart */
    }
    pthread_mutex_unlock(&d->p_mu);
    return done + skip;
}

/* own.cust: what the last republish saw. "PZCUST01", F, the records own.bin had then (8 bytes, little-endian), then its lookups: the
 * target, the number of nodes (1 byte), their IDs. The records it had: *n. */
static void cust_load(dht *d, cc_list *l, uint64_t *n)
{
    char path[4200];
    uint8_t h[48];
    cc_entry e = {0};
    int c;
    path_of(d, "own.cust", path, sizeof path);
    FILE *f = sys_fopen(path, "rb");
    *n = 0;
    if (!f || fread(h, 1, 48, f) != 48 || memcmp(h, "PZCUST01", 8) || memcmp(h + 8, d->cfg.net->fp, 32)) {
        if (f) fclose(f);
        return;
    }
    while (fread(e.t, 1, 32, f) == 32 && (c = fgetc(f)) >= 1 && c <= DHT_K) {
        e.n = c;
        for (int j = 0; j < c; j++)
            if (fread(e.c[j].id, 1, 32, f) != 32) e.n = 0;
        if (!e.n) break;
        cc_put(l, &e);
    }
    for (int i = 0; i < 8; i++) *n |= (uint64_t)h[40 + i] << (8 * i);
    fclose(f);
}

static void cust_save(dht *d, const cc_list *l, uint64_t n)
{
    char path[4200], tmp[4300];
    uint8_t h[48] = "PZCUST01";
    path_of(d, "own.cust", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    memcpy(h + 8, d->cfg.net->fp, 32);
    for (int i = 0; i < 8; i++) h[40 + i] = (uint8_t)(n >> (8 * i));
    FILE *f = sys_fopen(tmp, "wb");
    bool ok = f && fwrite(h, 1, 48, f) == 48;
    for (int i = 0; ok && i < l->n; i++) {
        ok = fwrite(l->e[i].t, 1, 32, f) == 32 && fputc(l->e[i].n, f) != EOF;
        for (int j = 0; ok && j < l->e[i].n; j++) ok = fwrite(l->e[i].c[j].id, 1, 32, f) == 32;
    }
    if (f) ok = fclose(f) == 0 && ok;
    if (!ok || !sys_replace(tmp, path)) remove(tmp);
}

/* The records of own.bin again, to the custodians each did not have at the last republish: the nodes that joined since, or that moved
 * up among the closest of a key as others left. The records published since go to all their custodians, and with all every record does
 * (the first contacts after none). The lookups are new: those kept are dropped. The copies sent. */
uint64_t dht_republish(dht *d, bool all)
{
    cc_list old = {0}, used = {0};
    uint64_t before = 0, copies = 0, i = 0;
    record *batch = malloc(DHT_STORE_MAX * sizeof *batch);
    if (!all) cust_load(d, &old, &before);
    pthread_mutex_lock(&d->p_mu);
    const uint64_t total = d->nown;
    d->cc.n = 0;
    pthread_mutex_unlock(&d->p_mu);
    while (batch && i < total && !stopping(d)) {
        int m = 0;
        const uint64_t first = i;
        pthread_mutex_lock(&d->p_mu);
        for (; i < total && m < DHT_STORE_MAX; i++)
            if (own_read(d, i, &batch[m])) m++;
        const uint64_t nold = first < before ? before - first : 0;   /* those the last republish had */
        (void)send_batch(d, batch, m, &old, nold < (uint64_t)m ? (int)nold : m, &used, &copies);
        pthread_mutex_unlock(&d->p_mu);
    }
    if (batch && !stopping(d)) cust_save(d, &used, total);
    dlog(d, "republished %llu records: %llu copies sent", (unsigned long long)total, (unsigned long long)copies);
    free(batch), free(old.e), free(used.e);
    return copies;
}

int dht_local_records(dht *d, const uint8_t x[32], record *out, int max) { return store_find(d->custody, x, out, max); }

uint64_t dht_local_count(dht *d) { return store_count(d->custody); }

double dht_local_weight(dht *d) { return (double)store_weight(d->custody); }

