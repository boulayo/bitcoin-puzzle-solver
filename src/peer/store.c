/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.c - the custodian and the mailbox (see store.h). Both tables are open addressing with linear probing on uniform keys (a hash, an
 * x-only key), grown at 70% load. A record is read back from the file only when its 8-byte key matches: that is, for a duplicate or a
 * coincidence, or one time in 2^64 otherwise. */
#include "store.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char MAGIC[8] = {'P', 'Z', 'S', 'T', 'O', 'R', 'E', '1'};
#define EMPTY UINT32_MAX           /* a free slot of the index; a store holds fewer records than this */
#define LOAD_CHUNK 1024            /* records read at a time when the index is rebuilt */

typedef struct {
    uint8_t id[32];
    uint64_t seq, n;               /* the last sequence number, the records held */
    bool used;
} owner_slot;

struct store {
    pthread_mutex_t mu;
    int fd;
    const secp256k1_context *ctx;
    record_net net;
    uint64_t count;                /* records in the file */
    uint64_t *keys;                /* the index: the first 8 bytes of K and the number of the record */
    uint32_t *idx;
    size_t cap, used;
    owner_slot *own;               /* the last sequence number of each owner */
    size_t ocap, oused;
    uint64_t max, max_owner;       /* the limits of a custodian; 0: none */
};

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

void store_key(const uint8_t x[32], uint8_t k[32])
{
    uint8_t m[10 + 32] = "pznet1/key";
    memcpy(m + 10, x, 32);
    pz_sha256(m, sizeof m, k);
}

static uint64_t key8(const uint8_t x[32])
{
    uint8_t k[32];
    store_key(x, k);
    return be64(k);
}

/* ---- the index ---- */

static void index_add(store *st, uint64_t k, uint32_t i)
{
    size_t s = (size_t)k & (st->cap - 1);
    while (st->idx[s] != EMPTY) s = (s + 1) & (st->cap - 1);
    st->keys[s] = k, st->idx[s] = i;
    st->used++;
}

static bool index_grow(store *st)
{
    if (st->cap && st->used * 10 < st->cap * 7) return true;
    const size_t oc = st->cap, nc = oc ? oc * 2 : 1024;
    uint64_t *ok = st->keys, *nk = malloc(nc * sizeof *nk);
    uint32_t *oi = st->idx, *ni = malloc(nc * sizeof *ni);
    if (!nk || !ni) {
        free(nk), free(ni);
        return false;
    }
    memset(ni, 0xff, nc * sizeof *ni);
    st->keys = nk, st->idx = ni, st->cap = nc, st->used = 0;
    for (size_t s = 0; s < oc; s++)
        if (oi[s] != EMPTY) index_add(st, ok[s], oi[s]);
    free(ok), free(oi);
    return true;
}

static owner_slot *owner_find(store *st, const uint8_t id[32], bool add)
{
    if (add && (!st->ocap || st->oused * 10 >= st->ocap * 7)) {
        const size_t oc = st->ocap, nc = oc ? oc * 2 : 64;
        owner_slot *old = st->own, *n = calloc(nc, sizeof *n);
        if (!n) return NULL;
        st->own = n, st->ocap = nc, st->oused = 0;
        for (size_t i = 0; i < oc; i++)
            if (old[i].used) *owner_find(st, old[i].id, true) = old[i];
        free(old);
    }
    if (!st->ocap) return NULL;
    size_t s = (size_t)be64(id) & (st->ocap - 1);
    for (; st->own[s].used; s = (s + 1) & (st->ocap - 1))
        if (!memcmp(st->own[s].id, id, 32)) return &st->own[s];
    if (!add) return NULL;
    memcpy(st->own[s].id, id, 32);
    st->own[s].used = true;
    st->own[s].seq = st->own[s].n = 0;
    st->oused++;
    return &st->own[s];
}

static void owner_saw(store *st, const record *r)
{
    owner_slot *o = owner_find(st, r->owner, true);   /* a new owner starts at 0, below or at any sequence number */
    if (o && r->seq > o->seq) o->seq = r->seq;
    if (o) o->n++;
}

static bool read_at(store *st, uint64_t i, record *r)
{
    uint8_t b[RECORD_LEN];
    return sys_pread(st->fd, b, RECORD_LEN, STORE_HDR + i * RECORD_LEN) == RECORD_LEN && record_decode(b, r);
}

/* the records with x: the slots of its 8-byte key, read back and compared */
static int find_locked(store *st, const uint8_t x[32], record *out, int max)
{
    if (!st->cap) return 0;
    const uint64_t k = key8(x);
    int n = 0;
    for (size_t s = (size_t)k & (st->cap - 1); st->idx[s] != EMPTY && n < max; s = (s + 1) & (st->cap - 1)) {
        record r;
        if (st->keys[s] == k && read_at(st, st->idx[s], &r) && !memcmp(r.x, x, 32)) out[n++] = r;
    }
    return n;
}

/* ---- the file ---- */

static store *fail(store *st, char *err, size_t errlen, const char *msg, const char *path)
{
    snprintf(err, errlen, "%s: %s", path, msg);
    store_close(st);
    return NULL;
}

store *store_open(const char *path, const secp256k1_context *ctx, const record_net *net, char *err, size_t errlen)
{
    store *st = calloc(1, sizeof *st);
    if (!st) {
        snprintf(err, errlen, "%s: out of memory", path);
        return NULL;
    }
    pthread_mutex_init(&st->mu, NULL);
    st->ctx = ctx;
    st->net = *net;
    st->fd = sys_open_rw(path);
    if (st->fd < 0) return fail(st, err, errlen, strerror(errno), path);
    const int64_t size = sys_size(st->fd);
    uint8_t hdr[STORE_HDR];
    memcpy(hdr, MAGIC, sizeof MAGIC);
    memcpy(hdr + sizeof MAGIC, net->fp, 32);
    if (size < 0) return fail(st, err, errlen, strerror(errno), path);
    if (size == 0) {
        if (sys_pwrite(st->fd, hdr, STORE_HDR, 0) != STORE_HDR) return fail(st, err, errlen, "cannot write the header", path);
    } else {
        uint8_t got[STORE_HDR];
        if (size < STORE_HDR || sys_pread(st->fd, got, STORE_HDR, 0) != STORE_HDR || memcmp(got, MAGIC, sizeof MAGIC))
            return fail(st, err, errlen, "not a store of records", path);
        if (memcmp(got + sizeof MAGIC, net->fp, 32)) return fail(st, err, errlen, "a store of another network", path);
    }
    st->count = size > STORE_HDR ? (uint64_t)(size - STORE_HDR) / RECORD_LEN : 0;
    if (size > STORE_HDR && (uint64_t)(size - STORE_HDR) % RECORD_LEN != 0 && !sys_truncate(st->fd, STORE_HDR + st->count * RECORD_LEN))
        return fail(st, err, errlen, "cannot cut the torn record at the end", path);   /* a write cut by a crash */
    if (st->count >= EMPTY) return fail(st, err, errlen, "too many records", path);
    uint8_t *buf = malloc((size_t)LOAD_CHUNK * RECORD_LEN);
    if (!buf) return fail(st, err, errlen, "out of memory", path);
    for (uint64_t i = 0; i < st->count; i += LOAD_CHUNK) {
        const size_t n = st->count - i < LOAD_CHUNK ? (size_t)(st->count - i) : LOAD_CHUNK;
        if (sys_pread(st->fd, buf, n * RECORD_LEN, STORE_HDR + i * RECORD_LEN) != (long)(n * RECORD_LEN)) {
            free(buf);
            return fail(st, err, errlen, "cannot read the records", path);
        }
        for (size_t j = 0; j < n; j++) {
            record r;
            if (!record_decode(buf + j * RECORD_LEN, &r)) continue;   /* damaged on disk: not indexed (store_read refuses it too) */
            /* the tail, where a crash can leave a record of the right length and the wrong content, is checked again: a damaged record
             * there could otherwise give its owner a sequence number that refuses all its next ones */
            if (i + j + STORE_CHECK_TAIL >= st->count && !record_check(ctx, net, &r)) continue;
            if (!index_grow(st)) {
                free(buf);
                return fail(st, err, errlen, "out of memory", path);
            }
            index_add(st, key8(r.x), (uint32_t)(i + j));
            owner_saw(st, &r);
        }
    }
    free(buf);
    return st;
}

void store_limits(store *st, uint64_t max_records, uint64_t max_per_owner)
{
    pthread_mutex_lock(&st->mu);
    st->max = max_records, st->max_owner = max_per_owner;
    pthread_mutex_unlock(&st->mu);
}

void store_close(store *st)
{
    if (!st) return;
    if (st->fd >= 0) {
        (void)sys_sync(st->fd);
        close(st->fd);
    }
    free(st->keys);
    free(st->idx);
    free(st->own);
    pthread_mutex_destroy(&st->mu);
    free(st);
}

store_status store_put(store *st, const record *r, record *mine, record *others, int max, int *nothers)
{
    *nothers = 0;
    if (!record_check(st->ctx, &st->net, r)) return STORE_INVALID;   /* outside the lock: the costly part */
    record same[2 * STORE_X_OWNERS];                           /* room for a file written before the limit, or by hand */
    pthread_mutex_lock(&st->mu);
    const int ns = find_locked(st, r->x, same, (int)(sizeof same / sizeof *same));
    int held = -1;
    for (int i = 0; i < ns && held < 0; i++)
        if (!memcmp(same[i].owner, r->owner, 32)) held = i;
    store_status s;
    const owner_slot *o = owner_find(st, r->owner, false);
    if (held >= 0) {
        s = STORE_DUP;
        if (mine) *mine = same[held];
    } else if (o && r->seq <= o->seq) {
        s = STORE_OLD;
    } else if ((st->max && st->count >= st->max) || (st->max_owner && o && o->n >= st->max_owner) || ns >= STORE_X_OWNERS) {
        s = STORE_FULL;
    } else {
        uint8_t enc[RECORD_LEN];
        record_encode(r, enc);
        s = STORE_FAIL;
        if (st->count + 1 < EMPTY && index_grow(st) && sys_pwrite(st->fd, enc, RECORD_LEN, STORE_HDR + st->count * RECORD_LEN) == RECORD_LEN) {
            index_add(st, key8(r->x), (uint32_t)st->count++);
            owner_saw(st, r);
            s = STORE_OK;
            if (mine) *mine = *r;
        }
    }
    pthread_mutex_unlock(&st->mu);
    /* the records of other owners, checked again outside the lock: one damaged on disk is no coincidence. A record that a full store
     * does not keep still shows its coincidences: the alert matters more than the copy */
    int no = 0;
    for (int i = 0; (s == STORE_OK || s == STORE_DUP || s == STORE_FULL) && i < ns && no < max; i++)
        if (i != held && record_check(st->ctx, &st->net, &same[i])) others[no++] = same[i];
    *nothers = no;
    return s;
}

int store_find(store *st, const uint8_t x[32], record *out, int max)
{
    pthread_mutex_lock(&st->mu);
    const int n = find_locked(st, x, out, max);
    pthread_mutex_unlock(&st->mu);
    return n;
}

uint64_t store_count(store *st)
{
    pthread_mutex_lock(&st->mu);
    const uint64_t n = st->count;
    pthread_mutex_unlock(&st->mu);
    return n;
}

bool store_read(store *st, uint64_t i, record *out)
{
    pthread_mutex_lock(&st->mu);
    const bool ok = i < st->count && read_at(st, i, out);
    pthread_mutex_unlock(&st->mu);
    return ok;
}

bool store_last_seq(store *st, const uint8_t owner[32], uint64_t *seq)
{
    pthread_mutex_lock(&st->mu);
    const owner_slot *o = owner_find(st, owner, false);
    if (o) *seq = o->seq;
    pthread_mutex_unlock(&st->mu);
    return o != NULL;
}

bool store_pair_ok(const secp256k1_context *ctx, const record_net *net, const record *a, const record *b)
{
    return !memcmp(a->x, b->x, 32) && memcmp(a->owner, b->owner, 32) != 0 && record_check(ctx, net, a) && record_check(ctx, net, b);
}

/* ---- the mailbox ---- */

typedef struct {
    mbox_pair p;
    uint8_t h[32];                 /* SHA256("pznet1/pair" || both records), to find a pair again */
} mentry;

struct mbox {
    pthread_mutex_t mu;
    mentry *e;                     /* the oldest first */
    size_t cap, per_owner, n;
};

mbox *mbox_new(size_t cap, size_t per_owner)
{
    mbox *m = calloc(1, sizeof *m);
    if (!m || !(m->e = calloc(cap ? cap : 1, sizeof *m->e))) {
        free(m);
        return NULL;
    }
    m->cap = cap ? cap : 1;
    m->per_owner = per_owner ? per_owner : m->cap;
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

void mbox_free(mbox *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mu);
    free(m->e);
    free(m);
}

static bool has_owner(const mentry *e, const uint8_t owner[32]) { return !memcmp(e->p.r[0].owner, owner, 32) || !memcmp(e->p.r[1].owner, owner, 32); }

static void drop_at(mbox *m, size_t i)
{
    memmove(&m->e[i], &m->e[i + 1], (m->n - i - 1) * sizeof *m->e);
    m->n--;
}

/* Room for one more pair among those of owner (among all when NULL), which may be limit: the oldest that no owner proved goes, else the
 * oldest. A flood for one owner pushes out its own pairs only, and pairs made of points copied from the table, which their sender
 * cannot prove, go before the ones an owner proved. */
static void make_room(mbox *m, const uint8_t *owner, size_t limit)
{
    size_t k = 0, oldest = 0, bare = m->n;
    for (size_t i = m->n; i-- > 0;)
        if (!owner || has_owner(&m->e[i], owner)) {
            k++, oldest = i;
            if (!m->e[i].p.has[0] && !m->e[i].p.has[1]) bare = i;
        }
    if (k >= limit) drop_at(m, bare < m->n ? bare : oldest);
}

/* the same owners at the same point with the same types: one pair, whatever the sequence numbers of its records */
static bool same_pair(const mbox_pair *p, const mbox_pair *q)
{
    for (int i = 0; i < 2; i++)
        if (memcmp(p->r[i].owner, q->r[i].owner, 32) || memcmp(p->r[i].x, q->r[i].x, 32) || p->r[i].t != q->r[i].t) return false;
    return true;
}

bool mbox_put(mbox *m, const record *a, const record *b, const uint8_t proof[64])
{
    mentry e;
    memset(&e, 0, sizeof e);
    const int who = memcmp(a->owner, b->owner, 32) > 0;      /* the place of a's owner, whose proof this is */
    e.p.r[who] = *a;
    e.p.r[1 - who] = *b;
    uint8_t buf[11 + 2 * RECORD_LEN] = "pznet1/pair";
    record_encode(&e.p.r[0], buf + 11);
    record_encode(&e.p.r[1], buf + 11 + RECORD_LEN);
    pz_sha256(buf, sizeof buf, e.h);
    pthread_mutex_lock(&m->mu);
    size_t at = m->n;
    for (size_t i = 0; i < m->n && at == m->n; i++)
        if (same_pair(&m->e[i].p, &e.p)) at = i;
    bool fresh = at == m->n;
    /* the pair with other sequence numbers (a proof is over the records as they are): the one kept stays, unless only the new one is proved */
    const bool other = !fresh && memcmp(m->e[at].h, e.h, 32);
    if (other && (!proof || m->e[at].p.has[0] || m->e[at].p.has[1])) {
        pthread_mutex_unlock(&m->mu);
        return false;
    }
    if (fresh) {
        make_room(m, e.p.r[0].owner, m->per_owner);
        make_room(m, e.p.r[1].owner, m->per_owner);
        make_room(m, NULL, m->cap);
        at = m->n++;
    }
    if (fresh || other) m->e[at] = e;
    if (proof && !m->e[at].p.has[who]) {
        m->e[at].p.has[who] = true;
        memcpy(m->e[at].p.proof[who], proof, 64);
        fresh = true;
    }
    pthread_mutex_unlock(&m->mu);
    return fresh;
}

int mbox_get(mbox *m, const uint8_t owner[32], mbox_pair *out, int max)
{
    int k = 0;
    pthread_mutex_lock(&m->mu);
    for (size_t i = m->n; i-- > 0 && k < max;)
        if (has_owner(&m->e[i], owner)) out[k++] = m->e[i].p;
    pthread_mutex_unlock(&m->mu);
    return k;
}
