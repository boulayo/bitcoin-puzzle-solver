/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.c - the custodian and the mailbox (see store.h). The tables are open addressing with linear probing on uniform keys (a hash, an
 * x-only key), grown at 70% load. A record is read back from the file only when its 4-byte key matches: that is, for a duplicate or a
 * coincidence, or about one time in 2^32 otherwise (8 bytes a slot keep the index small). */
#include "store.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char MAGIC[8] = {'P', 'Z', 'S', 'T', 'O', 'R', 'E', '2'}, OMAGIC[8] = {'P', 'Z', 'O', 'W', 'N', 'E', 'R', '2'};
#define EMPTY UINT32_MAX           /* a free slot of the index; a store holds fewer records than this */
#define LOAD_CHUNK 1024            /* entries read at a time when the index is rebuilt */

typedef struct {
    uint8_t id[32];
    uint32_t num;                  /* its number in PATH.owners */
    uint64_t n;                    /* the records held */
    bool used;
} owner_slot;

struct store {
    pthread_mutex_t mu;
    int fd, ofd;                   /* the records, the owners */
    const secp256k1_context *ctx;
    record_net net;
    uint64_t count;                /* entries in the file */
    long double weight;            /* the sum of 2^dp of the records indexed */
    uint32_t *keys, *idx;          /* the index: the first 4 bytes of K and the number of the entry */
    size_t cap, used;
    owner_slot *own;               /* the owners by identity */
    size_t ocap, oused;
    uint8_t (*ids)[32];            /* the owners by number */
    uint32_t nids, capids;
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

static uint32_t key4(const uint8_t x[32])
{
    uint8_t k[32];
    store_key(x, k);
    return (uint32_t)(be64(k) >> 32);
}

/* ---- the index ---- */

static void index_add(store *st, uint32_t k, uint32_t i)
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
    uint32_t *ok = st->keys, *nk = malloc(nc * sizeof *nk), *oi = st->idx, *ni = malloc(nc * sizeof *ni);
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

/* ---- the owners ---- */

static owner_slot *owner_find(store *st, const uint8_t id[32])
{
    if (!st->ocap) return NULL;
    size_t s = (size_t)be64(id) & (st->ocap - 1);
    for (; st->own[s].used; s = (s + 1) & (st->ocap - 1))
        if (!memcmp(st->own[s].id, id, 32)) return &st->own[s];
    return NULL;
}

/* an owner in the tables of memory, with its number (the file has it already) */
static owner_slot *owner_index(store *st, const uint8_t id[32], uint32_t num)
{
    if (!st->ocap || st->oused * 10 >= st->ocap * 7) {
        const size_t oc = st->ocap, nc = oc ? oc * 2 : 64;
        owner_slot *old = st->own, *n = calloc(nc, sizeof *n);
        if (!n) return NULL;
        st->own = n, st->ocap = nc, st->oused = 0;
        for (size_t i = 0; i < oc; i++)
            if (old[i].used) *owner_index(st, old[i].id, old[i].num) = old[i];
        free(old);
    }
    if (num >= st->capids) {
        const uint32_t nc = st->capids ? st->capids * 2 : 64;
        uint8_t (*n)[32] = realloc(st->ids, (size_t)nc * 32);
        if (!n) return NULL;
        st->ids = n, st->capids = nc;
    }
    size_t s = (size_t)be64(id) & (st->ocap - 1);
    while (st->own[s].used) s = (s + 1) & (st->ocap - 1);
    owner_slot *o = &st->own[s];
    memcpy(o->id, id, 32), o->num = num, o->n = 0, o->used = true;
    memcpy(st->ids[num], id, 32);
    if (num >= st->nids) st->nids = num + 1;
    st->oused++;
    return o;
}

/* the numbers up to n taken, those without an owner as zeros (no owner is: an identity is a key): an entry that names an owner whose line
 * a crash lost keeps naming nobody, and a new owner never gets its number */
static bool ids_reserve(store *st, uint32_t n)
{
    if (n > st->capids) {
        uint8_t (*m)[32] = realloc(st->ids, (size_t)n * 32);
        if (!m) return false;
        st->ids = m, st->capids = n;
    }
    for (; st->nids < n; st->nids++) memset(st->ids[st->nids], 0, 32);
    return true;
}

/* a new owner: its identity at the end of PATH.owners first, so that every entry names an owner the file has */
static owner_slot *owner_add(store *st, const uint8_t id[32])
{
    const uint32_t num = st->nids;
    if (num == EMPTY || sys_pwrite(st->ofd, id, 32, STORE_HDR + (int64_t)num * 32) != 32) return NULL;
    return owner_index(st, id, num);
}

/* ---- the entries ---- */

static void entry_encode(const record *r, uint32_t num, uint8_t e[STORE_ENTRY])
{
    uint8_t b[RECORD_LEN];
    record_encode(r, b);
    memcpy(e, b, RECORD_WIRE);                              /* the type, the zero bits and x: the record of a STORE */
    for (int i = 0; i < 4; i++) e[RECORD_WIRE + i] = (uint8_t)(num >> (8 * i));
}

static bool entry_decode(const store *st, const uint8_t e[STORE_ENTRY], record *r)
{
    uint8_t b[RECORD_LEN];
    uint32_t num = 0;
    for (int i = 0; i < 4; i++) num |= (uint32_t)e[RECORD_WIRE + i] << (8 * i);
    static const uint8_t none[32];
    if (num >= st->nids || !memcmp(st->ids[num], none, 32)) return false;   /* an owner whose line a crash lost */
    memcpy(b, e, RECORD_WIRE), memcpy(b + RECORD_WIRE, st->ids[num], 32);
    return record_decode(b, r);
}

static bool read_at(store *st, uint64_t i, record *r)
{
    uint8_t e[STORE_ENTRY];
    return sys_pread(st->fd, e, STORE_ENTRY, STORE_HDR + i * STORE_ENTRY) == STORE_ENTRY && entry_decode(st, e, r);
}

/* the records with x: the slots of its 4-byte key, read back and compared */
static int find_locked(store *st, const uint8_t x[32], record *out, int max)
{
    if (!st->cap) return 0;
    const uint32_t k = key4(x);
    int n = 0;
    for (size_t s = (size_t)k & (st->cap - 1); st->idx[s] != EMPTY && n < max; s = (s + 1) & (st->cap - 1)) {
        record r;
        if (st->keys[s] == k && read_at(st, st->idx[s], &r) && !memcmp(r.x, x, 32)) out[n++] = r;
    }
    return n;
}

/* ---- the files ---- */

static store *fail(store *st, char *err, size_t errlen, const char *msg, const char *path)
{
    snprintf(err, errlen, "%s: %s", path, msg);
    store_close(st);
    return NULL;
}

/* Opens a file of the store with its header (written to a new one); the number of whole units after it, a unit cut at the end dropped.
 * -1 with the reason in why. */
int64_t store_file(const char *path, const char magic[8], const uint8_t fp[32], size_t unit, int *fd, const char **why)
{
    uint8_t hdr[STORE_HDR], got[STORE_HDR];
    memcpy(hdr, magic, 8), memcpy(hdr + 8, fp, 32);
    *fd = sys_open_rw(path);
    const int64_t size = *fd < 0 ? -1 : sys_size(*fd);
    if (size < 0) return *why = strerror(errno), -1;
    if (size == 0) return sys_pwrite(*fd, hdr, STORE_HDR, 0) == STORE_HDR ? 0 : (*why = "cannot write the header", -1);
    if (size < STORE_HDR || sys_pread(*fd, got, STORE_HDR, 0) != STORE_HDR || memcmp(got, magic, 8)) return *why = "not a store of records", -1;
    if (memcmp(got + 8, fp, 32)) return *why = "a store of another network", -1;
    const int64_t n = (size - STORE_HDR) / (int64_t)unit;
    if ((size - STORE_HDR) % (int64_t)unit && !sys_truncate(*fd, STORE_HDR + n * (int64_t)unit)) return *why = "cannot cut the torn entry at the end", -1;
    return n;
}

store *store_open(const char *path, const secp256k1_context *ctx, const record_net *net, char *err, size_t errlen)
{
    store *st = calloc(1, sizeof *st);
    char opath[4200];
    const char *why = "";
    if (!st) {
        snprintf(err, errlen, "%s: out of memory", path);
        return NULL;
    }
    pthread_mutex_init(&st->mu, NULL);
    st->ctx = ctx;
    st->net = *net;
    st->fd = st->ofd = -1;
    snprintf(opath, sizeof opath, "%s.owners", path);
    const int64_t nown = store_file(opath, OMAGIC, net->fp, 32, &st->ofd, &why), nent = nown < 0 ? -1 : store_file(path, MAGIC, net->fp, STORE_ENTRY, &st->fd, &why);
    if (nown < 0) return fail(st, err, errlen, why, opath);
    if (nent < 0) return fail(st, err, errlen, why, path);
    if (nown >= EMPTY || nent >= EMPTY) return fail(st, err, errlen, "too many records", path);
    uint8_t *buf = malloc((size_t)LOAD_CHUNK * STORE_ENTRY);
    if (!buf) return fail(st, err, errlen, "out of memory", path);
    static const uint8_t none[32];
    uint32_t refs = 0;                                          /* the owners the entries name */
    for (int64_t i = 0; i < nown; i++) {
        if (sys_pread(st->ofd, buf, 32, STORE_HDR + i * 32) != 32 || !(memcmp(buf, none, 32) ? owner_index(st, buf, (uint32_t)i) != NULL
                                                                                              : ids_reserve(st, (uint32_t)i + 1))) {
            free(buf);
            return fail(st, err, errlen, "cannot read the owners", opath);
        }
    }
    st->count = (uint64_t)nent;
    for (uint64_t i = 0; i < st->count; i += LOAD_CHUNK) {
        const size_t n = st->count - i < LOAD_CHUNK ? (size_t)(st->count - i) : LOAD_CHUNK;
        if (sys_pread(st->fd, buf, n * STORE_ENTRY, STORE_HDR + i * STORE_ENTRY) != (long)(n * STORE_ENTRY)) {
            free(buf);
            return fail(st, err, errlen, "cannot read the records", path);
        }
        for (size_t j = 0; j < n; j++) {
            record r;
            uint32_t num = 0;
            for (int b = 0; b < 4; b++) num |= (uint32_t)buf[j * STORE_ENTRY + RECORD_WIRE + b] << (8 * b);
            if (num < EMPTY && num + 1 > refs) refs = num + 1;
            if (!entry_decode(st, buf + j * STORE_ENTRY, &r)) continue;   /* damaged on disk: not indexed (store_read refuses it too) */
            if (i + j + STORE_CHECK_TAIL >= st->count && !record_check(ctx, net, &r)) continue;   /* the tail, where a crash cuts */
            if (!index_grow(st)) {
                free(buf);
                return fail(st, err, errlen, "out of memory", path);
            }
            index_add(st, key4(r.x), (uint32_t)(i + j));
            owner_find(st, r.owner)->n++;
            st->weight += ldexpl(1, (int)r.dp);
        }
    }
    free(buf);
    if (!ids_reserve(st, refs)) return fail(st, err, errlen, "out of memory", path);
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
    for (int i = 0; i < 2; i++) {
        const int fd = i ? st->ofd : st->fd;
        if (fd >= 0) (void)sys_sync(fd), close(fd);
    }
    free(st->keys), free(st->idx), free(st->own), free(st->ids);
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
    owner_slot *o = owner_find(st, r->owner);
    if (held >= 0) {
        s = STORE_DUP;
        if (mine) *mine = same[held];
    } else if ((st->max && st->count >= st->max) || (st->max_owner && o && o->n >= st->max_owner) || ns >= STORE_X_OWNERS ||
               (st->max && !o && st->oused >= st->max / 256 + 1024)) {     /* an identity costs little: owners take memory and disk */
        s = STORE_FULL;
    } else {
        uint8_t e[STORE_ENTRY];
        s = STORE_FAIL;
        if (!o) o = owner_add(st, r->owner);
        if (o) entry_encode(r, o->num, e);
        if (o && st->count + 1 < EMPTY && index_grow(st) && sys_pwrite(st->fd, e, STORE_ENTRY, STORE_HDR + st->count * STORE_ENTRY) == STORE_ENTRY) {
            index_add(st, key4(r->x), (uint32_t)st->count++);
            o->n++;
            st->weight += ldexpl(1, (int)r->dp);
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

long double store_weight(store *st)
{
    pthread_mutex_lock(&st->mu);
    const long double w = st->weight;
    pthread_mutex_unlock(&st->mu);
    return w;
}

bool store_read(store *st, uint64_t i, record *out)
{
    pthread_mutex_lock(&st->mu);
    const bool ok = i < st->count && read_at(st, i, out);
    pthread_mutex_unlock(&st->mu);
    return ok;
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

/* the same owners at the same point with the same types: one pair, whatever the zero bits of its records */
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
    record_name(&e.p.r[0], buf + 11);
    record_name(&e.p.r[1], buf + 11 + RECORD_LEN);
    pz_sha256(buf, sizeof buf, e.h);
    pthread_mutex_lock(&m->mu);
    size_t at = m->n;
    for (size_t i = 0; i < m->n && at == m->n; i++)
        if (same_pair(&m->e[i].p, &e.p)) at = i;
    bool fresh = at == m->n;
    /* the pair with other zero bits (a proof is over the records as they are): the one kept stays, unless only the new one is proved */
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
