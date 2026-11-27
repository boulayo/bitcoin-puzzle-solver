/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_store.c - the records (docs/en/NETWORK.md 7.3, 7.4): what this node takes as a custodian, and what it publishes (own.bin, the
 * republish). */
#include "dht_priv.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- custody ---- */

/* Whether this node takes a record of another node, at the address from, as STORED tells it: one it may be a custodian of (else FAR),
 * within the rate of the sender's block (else FULL). An owner the store does not know counts as STORE_OWNER_RECORDS records when the
 * store takes it (owner_back): a store holds an owner for ever and has room for one in that many records, so a block fills the owners
 * no sooner than the store. */
static int admit(dht *d, const record *r, const net_addr *from)
{
    uint8_t k[32];
    store_key(r->x, k);
    if (!responsible(d, k)) return DHT_ST_FAR;
    return lim_take(d, from, LIM_REC, store_owner(d->custody, r->owner) ? 1 : STORE_OWNER_RECORDS) ? DHT_ST_STORED : DHT_ST_FULL;
}

/* After the store had r: an owner it did not take (it is full, or it refused r) costs one token, as any record, and the block gets the
 * others back. A full custodian still tells the coincidences of a new home at the rate of every home (before, at one record in 256). */
static void owner_back(dht *d, const record *r, const net_addr *from)
{
    if (!store_owner(d->custody, r->owner)) (void)lim_take(d, from, LIM_REC, 1 - STORE_OWNER_RECORDS);
}

/* A record for the custody of this node: its status for STORED. from: the address of the owner that sent it, NULL for this node's own.
 * A custodian is passive: when other owners have the x of the record it says COINCIDENCE (also for a duplicate, and for a record it
 * does not keep because it is full) and alerts nobody. The owner proves its record (PROVE) and tells the others itself, with its proof.
 * A record of this node that meets others is an alert of this node at once; a record of another that meets one of this node is not
 * (it may be a copy): this node hears of it when that owner proves it (custody_proved), or at its own republish to every custodian.
 * A node of a linear search has no store, and takes and publishes no record (dht.c, answer). */
int custody_put(dht *d, const record *r, const net_addr *from)
{
    static const net_addr self;                                /* the alerts of its own custody count as those of one address block */
    record others[STORE_X_OWNERS];
    int no;
    const int st = from ? admit(d, r, from) : DHT_ST_STORED;
    if (st != DHT_ST_STORED) return st;
    const store_status s = store_put(d->custody, r, false, others, STORE_X_OWNERS, &no);
    if (from) owner_back(d, r, from);
    for (int i = 0; !from && i < no; i++) (void)alert_in(d, &others[i], r, NULL, &self);
    if (no && from) return DHT_ST_COINCIDENCE;
    switch (s) {
    case STORE_OK: return DHT_ST_STORED;
    case STORE_DUP: return DHT_ST_DUP;
    case STORE_INVALID: return DHT_ST_INVALID;
    case STORE_FULL: return DHT_ST_FULL;
    default: return DHT_ST_FAILED;
    }
}

/* A record of PROVE: sig, the proof of the owner of r (the sender, at from) for this custodian. Nothing is remembered between STORE and
 * PROVE: a record that its owner proves is taken as a STORE takes it, and kept also past the owners of its x that a store takes (copies
 * planted under other identities before it cannot keep it out). The other owners of its x go into pairs (at most STORE_X_OWNERS, the
 * newest first: the owner's proofs and POSSESS tell the copies apart), for the answer: the owner hears of them from a custodian of its
 * point, and not only from its mailboxes, which nodes placed near their key could keep from it. A pair with a record of this node is an
 * alert of this node. The number of pairs; -1 for a record this node does not take, or a wrong proof. */
int custody_proved(dht *d, const record *r, const uint8_t sig[64], const net_addr *from, mbox_pair *pairs)
{
    record others[STORE_X_OWNERS];
    int no;
    if (admit(d, r, from) != DHT_ST_STORED || !record_proof_ok(d->cfg.ctx, d->cfg.net, r, d->me.xonly, CUSTODY_NONCE, sig)) return -1;
    (void)store_put(d->custody, r, true, others, STORE_X_OWNERS, &no);
    owner_back(d, r, from);
    for (int i = 0; i < no; i++) {
        memset(&pairs[i], 0, sizeof *pairs);
        pair_order(r, &others[i], pairs[i].r);
        if (mine(d, &others[i])) (void)alert_in(d, r, &others[i], NULL, from);
    }
    return no;
}

/* ---- the custodians of a key ---- */

#define CC_MAX 8192                /* lookups kept */
#define CC_TTL_MS (3600 * 1000LL)  /* for an hour, as the buckets are refreshed */
#define REPUBLISH_FULL_S (7 * 86400) /* a republish to every custodian, once a week */

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

enum { CC_FRESH, CC_ANY, CC_CLEAN };   /* which entries cc_find takes: of the last hour, all, the clean ones */

/* The custodians of k from one of the two entries around k in the order of the targets (the nearest: they share the longest prefix
 * with k) that shows them, among those cc_find takes (an entry it does not take is skipped, also as a neighbour); 0 when neither does. */
static int cc_find(const cc_list *l, const uint8_t k[32], int how, dht_contact out[DHT_KSTORE], cc_entry **e)
{
    const int at = cc_at(l, k);
    int side[2] = {at - 1, at};
    for (int s = 0; s < 2; s++)
        for (; side[s] >= 0 && side[s] < l->n; side[s] += s ? 1 : -1) {
            const cc_entry *x = &l->e[side[s]];
            if (how == CC_ANY || (how == CC_FRESH ? sys_now_ms() - x->at < CC_TTL_MS : x->clean)) break;
        }
    for (int s = 0; s < 2; s++) {
        const int n = side[s] >= 0 && side[s] < l->n ? cc_cover(&l->e[side[s]], k, out) : 0;
        if (n) return *e = &l->e[side[s]], n;
    }
    return 0;
}

/* The custodians of key k, from a lookup of the last hour that shows them, else from a new one, kept when it was whole (one cut short
 * shows only its own key). *t: the target of that lookup; used, when not NULL, gets it too (when it was whole). */
static int custodians(dht *d, const uint8_t k[32], dht_contact out[DHT_KSTORE], uint8_t t[32], cc_list *used)
{
    cc_entry *e, f;
    bool whole = true;
    pthread_mutex_lock(&d->p_mu);
    int n = cc_find(&d->cc, k, CC_FRESH, out, &e);
    if (n) f = *e;
    pthread_mutex_unlock(&d->p_mu);
    if (!n) {
        memcpy(f.t, k, 32), f.at = sys_now_ms(), f.clean = true;
        if ((f.n = lookup_whole(d, k, f.c, DHT_K, &whole)) <= 0) return 0;
        pthread_mutex_lock(&d->p_mu);
        if (whole) cc_put(&d->cc, &f);
        pthread_mutex_unlock(&d->p_mu);
        n = cc_cover(&f, k, out);
    }
    memcpy(t, f.t, 32);
    const int at = used && whole ? cc_at(used, f.t) : 0;
    if (used && whole && (at == used->n || memcmp(used->e[at].t, f.t, 32))) cc_put(used, &f);
    return n;
}

/* the lookup with target t is out of date (a custodian it gave is not one, or does not answer): l keeps it no more */
static void cc_stale(cc_list *l, const uint8_t t[32], bool drop)
{
    const int at = cc_at(l, t);
    if (at == l->n || memcmp(l->e[at].t, t, 32)) return;
    if (drop) l->e[at].at = INT64_MIN / 2;
    l->e[at].clean = false;
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
    uint8_t e[RECORD_WIRE];
    return sys_pread(d->ownfd, e, RECORD_WIRE, STORE_HDR + i * RECORD_WIRE) == RECORD_WIRE && record_wire_decode(e, d->me.xonly, r);
}

static void own_append(dht *d, const record *recs, int n)  /* p_mu held */
{
    uint8_t b[DHT_STORE_MAX * RECORD_WIRE];
    for (int i = 0; i < n; i++) record_wire_encode(&recs[i], b + (size_t)i * RECORD_WIRE);
    const size_t len = (size_t)n * RECORD_WIRE;
    if (sys_pwrite(d->ownfd, b, len, STORE_HDR + d->nown * RECORD_WIRE) == (long)len) d->nown += (uint64_t)n;
}

/* ---- publishing ---- */

/* what a republish keeps for own.cust: the lookups it used, and the keys of the records that not all their custodians took */
typedef struct {
    cc_list used;
    uint8_t (*miss)[32];
    size_t nmiss, cap;
} repub;

#define REPUB_MISS_MAX 65536       /* keys of records not taken by all their custodians a republish remembers; beyond, own.cust is dropped */
#define ST_NONE 0xff               /* no status: the custodian did not answer */

/* A batch on its way: its custodians, a group each, and what each record got from each of its custodians. */
typedef struct {
    dht *d;
    const record *recs;
    int n, ngroups;
    atomic_int next;               /* the group the next worker takes */
    int (*group)[DHT_KSTORE];      /* of record i at its custodian j: the group that sends it there, -1 for none */
    uint8_t (*st)[DHT_KSTORE];     /* ... and its status there: what STORED said, ST_NONE */
    dht_contact *cust;             /* of each group: the custodian */
    bool *failed;                  /* ... and whether an exchange with it failed (STORE or PROVE) */
} batch_run;

/* A worker of a batch: one custodian after the other, the STORE of all its records, then the PROVEs of those it told COINCIDENCE,
 * DHT_PROVE_MAX at a time, until one fails: every such record is proved, so copies planted on some records cannot keep the owner from the
 * other owners of another. A slow or dead custodian holds one worker for one timeout, or two. */
static void *store_main(void *arg)
{
    batch_run *b = *(batch_run **)arg;
    dht *d = b->d;
    uint8_t req[2 + DHT_STORE_MAX * RECORD_WIRE], ans[2 + DHT_STORE_MAX];
    int at[DHT_STORE_MAX][2];
    for (int g; !stopping(d) && (g = atomic_fetch_add(&b->next, 1)) < b->ngroups;) {
        const dht_contact *t = &b->cust[g];
        record coin[DHT_STORE_MAX];
        int m = 0, nc = 0;
        size_t len;
        for (int i = 0; i < b->n; i++)
            for (int j = 0; j < DHT_KSTORE; j++)
                if (b->group[i][j] == g) at[m][0] = i, at[m++][1] = j;
        req[0] = DHT_STORE, req[1] = (uint8_t)m;
        for (int q = 0; q < m; q++) record_wire_encode(&b->recs[at[q][0]], req + 2 + (size_t)q * RECORD_WIRE);   /* the sender is the owner */
        const bool me = is_me(d, t->id);
        const bool got = me || (exchange(d, t, req, 2 + (size_t)m * RECORD_WIRE, ans, sizeof ans, &len) && len == 2 + (size_t)m && ans[1] == m);
        if (!me) heard(d, t, got);
        for (int q = 0; q < m; q++) {
            const record *r = &b->recs[at[q][0]];
            const uint8_t st = me ? (uint8_t)custody_put(d, r, NULL) : got ? ans[2 + q] : ST_NONE;
            b->st[at[q][0]][at[q][1]] = st;
            if (!me && st == DHT_ST_COINCIDENCE) coin[nc++] = *r;
        }
        bool ok = got;
        for (int o = 0; ok && o < nc; o += DHT_PROVE_MAX) ok = prove_to(d, t, coin + o, nc - o < DHT_PROVE_MAX ? nc - o : DHT_PROVE_MAX);
        b->failed[g] = !ok;
    }
    return NULL;
}

/* At most DHT_STORE_MAX records: the custodians of each, then one STORE per custodian with all its records, PARALLEL_MAX custodians at a
 * time. A record with old set goes only to custodians that are not those prev shows (the last republish). A lookup that gave a custodian
 * that did not take a record is no clean one in used: the next republish sends the records it covers to all their custodians. The
 * records that at least one custodian holds. */
static int send_batch(dht *d, const record *recs, const bool *old, int n, cc_list *prev, repub *rp, uint64_t *copies)
{
    cc_list *used = rp ? &rp->used : NULL;
    batch_run b = {.d = d, .recs = recs, .n = n};
    dht_contact (*cs)[DHT_KSTORE] = malloc((size_t)n * sizeof *cs);   /* the custodians of each record */
    uint8_t (*ts)[32] = calloc((size_t)n, sizeof *ts);         /* the lookup they come from */
    int *ncs = calloc((size_t)n, sizeof *ncs), done = 0;
    b.group = malloc((size_t)n * sizeof *b.group), b.st = malloc((size_t)n * sizeof *b.st);
    b.cust = malloc((size_t)n * DHT_KSTORE * sizeof *b.cust), b.failed = calloc((size_t)n * DHT_KSTORE, sizeof *b.failed);
    const bool ok = cs && ts && ncs && b.group && b.st && b.cust && b.failed;
    if (ok) memset(b.group, 0xff, (size_t)n * sizeof *b.group), memset(b.st, ST_NONE, (size_t)n * sizeof *b.st);
    for (int i = 0; ok && i < n && !stopping(d); i++) {
        uint8_t k[32];
        dht_contact had[DHT_KSTORE];
        cc_entry *e;
        store_key(recs[i].x, k);
        ncs[i] = custodians(d, k, cs[i], ts[i], used);
        const int nh = old && old[i] ? cc_find(prev, k, CC_ANY, had, &e) : 0;
        for (int j = 0; j < ncs[i]; j++) {
            int h = 0, g = 0;
            while (h < nh && memcmp(cs[i][j].id, had[h].id, 32)) h++;
            while (g < b.ngroups && memcmp(b.cust[g].id, cs[i][j].id, 32)) g++;
            if (h < nh) {
                b.st[i][j] = DHT_ST_DUP;                       /* it had it at the last republish: it has it */
                continue;
            }
            if (g == b.ngroups) b.cust[b.ngroups++] = cs[i][j];
            b.group[i][j] = g;
        }
    }
    atomic_init(&b.next, 0);
    batch_run *w[PARALLEL_MAX];
    for (int i = 0; i < PARALLEL_MAX; i++) w[i] = &b;
    if (ok) parallel(store_main, w, sizeof *w, b.ngroups < PARALLEL_MAX ? b.ngroups : PARALLEL_MAX);
    pthread_mutex_lock(&d->p_mu);
    for (int i = 0; ok && i < n; i++) {
        int held = 0;                                          /* 1: on this node, 2: on another */
        bool all = !stopping(d) && ncs[i] > 0;
        for (int j = 0; j < ncs[i]; j++) {
            const int g = b.group[i][j], st = b.st[i][j];
            const bool me = is_me(d, cs[i][j].id), has = st == DHT_ST_STORED || st == DHT_ST_DUP || st == DHT_ST_COINCIDENCE;
            held |= has ? (me ? 1 : 3) : 0;
            all &= has && !(st == DHT_ST_COINCIDENCE && b.failed[g]);   /* its PROVE failed: it goes there again */
            if (copies && g >= 0 && !me && st != ST_NONE) (*copies)++;
            if (g >= 0 && (b.failed[g] || st == DHT_ST_FAR)) cc_stale(&d->cc, ts[i], true);   /* dead, or not a custodian: a new lookup */
        }
        done += held != 0;
        if (!(held & 2)) atomic_store(&d->unplaced, true);     /* on this node alone, or nowhere */
        if (rp && !all) {                                      /* the next republish sends it to all its custodians */
            cc_stale(used, ts[i], false);
            if (rp->nmiss == rp->cap && rp->cap < REPUB_MISS_MAX) {
                const size_t c = rp->cap ? rp->cap * 2 : 256;
                uint8_t (*m)[32] = realloc(rp->miss, c * 32);
                if (m) rp->miss = m, rp->cap = c;
            }
            if (rp->nmiss < rp->cap) store_key(recs[i].x, rp->miss[rp->nmiss]);
            rp->nmiss++;
        }
    }
    pthread_mutex_unlock(&d->p_mu);
    free(cs), free(ts), free(ncs), free(b.group), free(b.st), free(b.cust), free(b.failed);
    return done;
}

static void own_cut(dht *d, uint64_t n);

int dht_publish(dht *d, record *recs, int n)
{
    int done = 0, skip = 0;
    record r;
    for (int i = 0; i < n; i++) memcpy(recs[i].owner, d->me.xonly, 32);
    /* after a restart the home gives its records again from the first: those that own.bin has, in the same order, went out then. The
     * first that differs (another dp.bin) ends that, and own.bin is cut there: the records after it are not the home's any more */
    pthread_mutex_lock(&d->p_mu);
    for (; skip < n && d->replay < d->replay_end; skip++, d->replay++)
        if (!own_read(d, d->replay, &r) || !same_record(&r, &recs[skip])) {
            own_cut(d, d->replay);
            break;
        }
    pthread_mutex_unlock(&d->p_mu);
    for (int o = skip; o < n && !stopping(d); o += DHT_STORE_MAX) {
        const int m = n - o < DHT_STORE_MAX ? n - o : DHT_STORE_MAX;
        done += send_batch(d, recs + o, NULL, m, NULL, NULL, NULL);
        pthread_mutex_lock(&d->p_mu);
        if (!stopping(d)) own_append(d, recs + o, m);     /* cut by a stop: given again after the restart */
        pthread_mutex_unlock(&d->p_mu);
    }
    return done;
}

/* own.cust: what the last republish saw. "PZCUST02", F, the records own.bin had then and the time of the last republish to every
 * custodian (8 bytes each, little-endian; the time in seconds since 1970), then its clean lookups: the target, the number of nodes (1
 * byte), their IDs. */
static void cust_path(const dht *d, char *out, size_t cap) { path_of(d, "own.cust", out, cap); }

static void cust_load(dht *d, cc_list *l, uint64_t *n, uint64_t *full)
{
    char path[4200];
    uint8_t h[56];
    cc_entry e = {0};
    int c;
    cust_path(d, path, sizeof path);
    FILE *f = sys_fopen(path, "rb");
    *n = *full = 0;
    if (!f || fread(h, 1, 56, f) != 56 || memcmp(h, "PZCUST02", 8) || memcmp(h + 8, d->cfg.net->fp, 32)) {
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
    for (int i = 0; i < 8; i++) *n |= (uint64_t)h[40 + i] << (8 * i), *full |= (uint64_t)h[48 + i] << (8 * i);
    fclose(f);
}

static void cust_save(dht *d, const cc_list *l, uint64_t n, uint64_t full)
{
    char path[4200];
    uint8_t h[56] = "PZCUST02";
    cust_path(d, path, sizeof path);
    memcpy(h + 8, d->cfg.net->fp, 32);
    for (int i = 0; i < 8; i++) h[40 + i] = (uint8_t)(n >> (8 * i)), h[48 + i] = (uint8_t)(full >> (8 * i));
    FILE *f = sys_new_file(path, false);
    bool ok = f && fwrite(h, 1, 56, f) == 56;
    for (int i = 0; ok && i < l->n; i++) {
        if (!l->e[i].clean) continue;
        ok = fwrite(l->e[i].t, 1, 32, f) == 32 && fputc(l->e[i].n, f) != EOF;
        for (int j = 0; ok && j < l->e[i].n; j++) ok = fwrite(l->e[i].c[j].id, 1, 32, f) == 32;
    }
    (void)sys_new_file_done(f, path, ok);
}

/* own.bin cut to its first n records (p_mu held), and own.cust forgotten: its records are not those any more */
static void own_cut(dht *d, uint64_t n)
{
    char path[4200];
    if (n < d->nown && sys_truncate(d->ownfd, STORE_HDR + n * RECORD_WIRE)) d->nown = n;
    d->ncut++;
    d->replay = d->replay_end = n;
    cust_path(d, path, sizeof path);
    remove(path);
}

#define REPUB_WINDOW 65536         /* records of own.bin a republish sorts by their keys at a time */

typedef struct {
    uint8_t k[32];
    record r;
    bool old;                      /* own.bin had it at the last republish */
} keyed;

static int by_key(const void *a, const void *b) { return memcmp(((const keyed *)a)->k, ((const keyed *)b)->k, 32); }

/* The records of own.bin again, to the custodians each did not have at the last republish: the nodes that joined since, or that moved
 * up among the closest of a key as others left. The records published since, and those of a lookup whose custodians did not all take
 * them, go to all their custodians; and every record does when all is set (the first contacts after none) or once a week (a custodian
 * that lost its store gets it back, and a coincidence lost on the way comes back: a record that comes again gets its COINCIDENCE
 * again). The records go by REPUB_WINDOW, in the order of their keys, so a batch has keys that follow each other and few custodians:
 * in the order of own.bin (of discovery) its keys were random, and at 1000 nodes a batch went to about 700 custodians. The lookups
 * are new: those kept are dropped. The copies sent. */
uint64_t dht_republish(dht *d, bool all)
{
    cc_list prev = {0};
    repub rp = {0};
    uint64_t before = 0, full = 0, copies = 0;
    const uint64_t now = wall_us() / 1000000;
    if (!dht_count(d)) return atomic_store(&d->unplaced, true), 0;   /* nobody to send them to: at the first contacts */
    record *batch = malloc(DHT_STORE_MAX * sizeof *batch);
    bool old[DHT_STORE_MAX];
    cust_load(d, &prev, &before, &full);
    pthread_mutex_lock(&d->p_mu);
    const uint64_t total = d->nown, cuts = d->ncut;
    d->cc.n = 0;
    pthread_mutex_unlock(&d->p_mu);
    const int wn = total < REPUB_WINDOW ? (int)total + 1 : REPUB_WINDOW;
    keyed *win = malloc((size_t)wn * sizeof *win);
    if (all || before > total || now - full >= REPUBLISH_FULL_S) before = 0, full = now;   /* to every custodian */
    for (uint64_t i = 0; win && batch && i < total && !stopping(d);) {
        int m = 0;
        for (; i < total && m < wn; i++)                       /* past a cut of own.bin meanwhile, nothing is read */
            if (own_read(d, i, &win[m].r)) store_key(win[m].r.x, win[m].k), win[m++].old = i < before;
        qsort(win, (size_t)m, sizeof *win, by_key);
        for (int o = 0; o < m && !stopping(d); o += DHT_STORE_MAX) {
            const int k = m - o < DHT_STORE_MAX ? m - o : DHT_STORE_MAX;
            for (int j = 0; j < k; j++) batch[j] = win[o + j].r, old[j] = win[o + j].old;
            (void)send_batch(d, batch, old, k, &prev, &rp, &copies);
        }
    }
    /* a record that not all its custodians took goes to all of them next time: no lookup kept may show it held, whichever covers it */
    cc_entry *e;
    dht_contact tmp[DHT_KSTORE];
    for (size_t k = 0; k < rp.nmiss && k < rp.cap; k++)
        while (cc_find(&rp.used, rp.miss[k], CC_CLEAN, tmp, &e)) e->clean = false;
    pthread_mutex_lock(&d->p_mu);
    const bool same = d->ncut == cuts;                          /* own.bin was not cut meanwhile (dht_publish after a restart) */
    pthread_mutex_unlock(&d->p_mu);
    if (win && batch && !stopping(d) && same && rp.nmiss <= rp.cap) cust_save(d, &rp.used, total, full);
    else if (!same || rp.nmiss > rp.cap) {
        char path[4200];
        cust_path(d, path, sizeof path);
        remove(path);
    }
    dlog(d, "republished %llu records: %llu copies sent", (unsigned long long)total, (unsigned long long)copies);
    free(win), free(batch), free(prev.e), free(rp.used.e), free(rp.miss);
    return copies;
}

/* a node of a linear search has no store */
uint64_t dht_local_count(dht *d) { return d->custody ? store_count(d->custody) : 0; }

double dht_local_weight(dht *d) { return d->custody ? (double)store_weight(d->custody) : 0; }

