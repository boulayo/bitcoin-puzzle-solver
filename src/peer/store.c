/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.c - the custodian and the mailbox (see store.h). Both tables are open addressing with linear probing on uniform keys (a hash, an
 * x-only key), grown at 70% load. A record is read back from the file only when its 8-byte key matches: that is, for a duplicate or a
 * coincidence, or one time in 2^64 otherwise. */
#include "store.h"
#include <pzcore/hash.h>
#include <errno.h>
#include <fcntl.h>
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
    uint64_t seq;
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
    st->own[s].seq = 0;
    st->oused++;
    return &st->own[s];
}

static void owner_saw(store *st, const record *r)
{
    owner_slot *o = owner_find(st, r->owner, true);   /* a new owner starts at 0, below or at any sequence number */
    if (o && r->seq > o->seq) o->seq = r->seq;
}

static bool read_at(store *st, uint64_t i, record *r)
{
    uint8_t b[RECORD_LEN];
    return pread(st->fd, b, RECORD_LEN, (off_t)(STORE_HDR + i * RECORD_LEN)) == RECORD_LEN && record_decode(b, r);
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
    st->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (st->fd < 0) return fail(st, err, errlen, strerror(errno), path);
    const off_t size = lseek(st->fd, 0, SEEK_END);
    uint8_t hdr[STORE_HDR];
    memcpy(hdr, MAGIC, sizeof MAGIC);
    memcpy(hdr + sizeof MAGIC, net->fp, 32);
    if (size < 0) return fail(st, err, errlen, strerror(errno), path);
    if (size == 0) {
        if (pwrite(st->fd, hdr, STORE_HDR, 0) != STORE_HDR) return fail(st, err, errlen, "cannot write the header", path);
    } else {
        uint8_t got[STORE_HDR];
        if (size < STORE_HDR || pread(st->fd, got, STORE_HDR, 0) != STORE_HDR || memcmp(got, MAGIC, sizeof MAGIC))
            return fail(st, err, errlen, "not a store of records", path);
        if (memcmp(got + sizeof MAGIC, net->fp, 32)) return fail(st, err, errlen, "a store of another network", path);
    }
    st->count = size > STORE_HDR ? (uint64_t)(size - STORE_HDR) / RECORD_LEN : 0;
    if (size > STORE_HDR && (uint64_t)(size - STORE_HDR) % RECORD_LEN != 0 && ftruncate(st->fd, (off_t)(STORE_HDR + st->count * RECORD_LEN)) != 0)
        return fail(st, err, errlen, "cannot cut the torn record at the end", path);   /* a write cut by a crash */
    if (st->count >= EMPTY) return fail(st, err, errlen, "too many records", path);
    uint8_t *buf = malloc((size_t)LOAD_CHUNK * RECORD_LEN);
    if (!buf) return fail(st, err, errlen, "out of memory", path);
    for (uint64_t i = 0; i < st->count; i += LOAD_CHUNK) {
        const size_t n = st->count - i < LOAD_CHUNK ? (size_t)(st->count - i) : LOAD_CHUNK;
        if (pread(st->fd, buf, n * RECORD_LEN, (off_t)(STORE_HDR + i * RECORD_LEN)) != (ssize_t)(n * RECORD_LEN)) {
            free(buf);
            return fail(st, err, errlen, "cannot read the records", path);
        }
        for (size_t j = 0; j < n; j++) {
            record r;
            if (!record_decode(buf + j * RECORD_LEN, &r)) continue;   /* damaged on disk: not indexed (store_read refuses it too) */
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

void store_close(store *st)
{
    if (!st) return;
    if (st->fd >= 0) {
        (void)fsync(st->fd);
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
    record same[16];
    pthread_mutex_lock(&st->mu);
    const int ns = find_locked(st, r->x, same, (int)(sizeof same / sizeof *same));
    int held = -1, no = 0;
    for (int i = 0; i < ns; i++) {
        if (!memcmp(same[i].owner, r->owner, 32)) held = i;
        else if (no < max) others[no++] = same[i];
    }
    store_status s;
    const owner_slot *o = owner_find(st, r->owner, false);
    if (held >= 0) {
        s = STORE_DUP;
        if (mine) *mine = same[held];
    } else if (o && r->seq <= o->seq) {
        s = STORE_OLD;
    } else {
        uint8_t enc[RECORD_LEN];
        record_encode(r, enc);
        s = STORE_FAIL;
        if (st->count + 1 < EMPTY && index_grow(st) && pwrite(st->fd, enc, RECORD_LEN, (off_t)(STORE_HDR + st->count * RECORD_LEN)) == RECORD_LEN) {
            index_add(st, key8(r->x), (uint32_t)st->count++);
            owner_saw(st, r);
            s = STORE_OK;
            if (mine) *mine = *r;
        }
    }
    if (s != STORE_OK && s != STORE_DUP) no = 0;
    pthread_mutex_unlock(&st->mu);
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
    record r[2];
    uint8_t h[32];                 /* SHA256("pznet1/pair" || both records), to find a pair again */
} mpair;

struct mbox {
    pthread_mutex_t mu;
    mpair *p;                      /* a ring: the oldest at head */
    size_t cap, head, n;
};

mbox *mbox_new(size_t cap)
{
    mbox *m = calloc(1, sizeof *m);
    if (!m || !(m->p = calloc(cap ? cap : 1, sizeof *m->p))) {
        free(m);
        return NULL;
    }
    m->cap = cap ? cap : 1;
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

void mbox_free(mbox *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mu);
    free(m->p);
    free(m);
}

bool mbox_put(mbox *m, const record *a, const record *b)
{
    mpair e;
    const bool swap = memcmp(a->owner, b->owner, 32) > 0;
    e.r[0] = swap ? *b : *a;
    e.r[1] = swap ? *a : *b;
    uint8_t buf[11 + 2 * RECORD_LEN] = "pznet1/pair";
    record_encode(&e.r[0], buf + 11);
    record_encode(&e.r[1], buf + 11 + RECORD_LEN);
    pz_sha256(buf, sizeof buf, e.h);
    pthread_mutex_lock(&m->mu);
    bool fresh = true;
    for (size_t i = 0; i < m->n && fresh; i++) fresh = memcmp(m->p[(m->head + i) % m->cap].h, e.h, 32) != 0;
    if (fresh) {
        if (m->n == m->cap) m->head = (m->head + 1) % m->cap, m->n--;   /* full: the oldest pair goes */
        m->p[(m->head + m->n) % m->cap] = e;
        m->n++;
    }
    pthread_mutex_unlock(&m->mu);
    return fresh;
}

int mbox_get(mbox *m, const uint8_t owner[32], record (*pairs)[2], int max)
{
    int k = 0;
    pthread_mutex_lock(&m->mu);
    for (size_t i = m->n; i-- > 0 && k < max;) {
        const mpair *e = &m->p[(m->head + i) % m->cap];
        if (!memcmp(e->r[0].owner, owner, 32) || !memcmp(e->r[1].owner, owner, 32)) {
            pairs[k][0] = e->r[0];
            pairs[k][1] = e->r[1];
            k++;
        }
    }
    pthread_mutex_unlock(&m->mu);
    return k;
}
