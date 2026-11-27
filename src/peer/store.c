/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* store.c - the custodian and the mailbox (see store.h). The tables are open addressing with linear probing, grown at 70% load, on keys
 * that SipHash-2-4 makes of x and of the identity of an owner, under a key drawn at each opening: a peer chooses x (within the zero bits
 * and the rule of the custodian) and its identities, and with keys it could compute it would put all its records in one run of the
 * table, which every record of that run would then walk. A record is read back from the file only when its 4-byte key matches: that
 * is, for a duplicate or a coincidence, or about one time in 2^32 otherwise (8 bytes a slot keep the index small). The opening reads the
 * entries in chunks, finds their owners by number, and fetches the slots of a chunk together into a table made at its size at once:
 * about 50 s for a full store of 2^27 records, where a SHA-256 for every entry, an owner looked up by its identity and a table that grew
 * took two minutes. */
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
#define X_MAX (2 * STORE_X_OWNERS) /* records of one x read back: the owners a store takes, and as many more that their owners proved */

typedef struct {
    uint32_t key, num;             /* 4 bytes of the SipHash of x, the number of the entry (EMPTY: a free slot) */
} islot;

struct store {
    pthread_mutex_t mu;
    int fd, ofd;                   /* the records, the owners */
    const secp256k1_context *ctx;
    record_net net;
    uint64_t count;                /* entries in the file */
    long double weight;            /* the sum of 2^dp of the records indexed */
    uint64_t hkey[2];              /* of SipHash, drawn at the opening */
    islot *idx;                    /* the index, one place of the memory a slot */
    size_t cap, used;
    uint32_t *own;                 /* the owners by identity: their numbers (EMPTY: a free place) */
    size_t ocap, oused;
    uint8_t (*ids)[32];            /* the owners by number: the entries name them so, and at the opening nothing is looked up (zeros: a
                                    * number whose line a crash lost) */
    uint32_t nids, capids;
    uint64_t max;                  /* the limit of a custodian; 0: none */
};

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 8; i-- > 0;) v = v << 8 | p[i];
    return v;
}

void store_key(const uint8_t x[32], uint8_t k[32])
{
    uint8_t m[10 + 32] = "pznet1/key";
    memcpy(m + 10, x, 32);
    pz_sha256(m, sizeof m, k);
}

#define ROTL(x, b) ((x) << (b) | (x) >> (64 - (b)))

static void sipround(uint64_t v[4])
{
    v[0] += v[1], v[1] = ROTL(v[1], 13), v[1] ^= v[0], v[0] = ROTL(v[0], 32);
    v[2] += v[3], v[3] = ROTL(v[3], 16), v[3] ^= v[2];
    v[0] += v[3], v[3] = ROTL(v[3], 21), v[3] ^= v[0];
    v[2] += v[1], v[1] = ROTL(v[1], 17), v[1] ^= v[2], v[2] = ROTL(v[2], 32);
}

/* SipHash-2-4 of 32 bytes (x, or an identity) under the key of the store */
static uint64_t sip32(const store *st, const uint8_t p[32])
{
    uint64_t v[4] = {st->hkey[0] ^ 0x736f6d6570736575ull, st->hkey[1] ^ 0x646f72616e646f6dull, st->hkey[0] ^ 0x6c7967656e657261ull,
                     st->hkey[1] ^ 0x7465646279746573ull};
    for (int i = 0; i <= 32; i += 8) {
        const uint64_t m = i < 32 ? le64(p + i) : 32ull << 56; /* the words, then the length */
        v[3] ^= m, sipround(v), sipround(v), v[0] ^= m;
    }
    v[2] ^= 0xff;
    for (int r = 0; r < 4; r++) sipround(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

static uint32_t key4(const store *st, const uint8_t x[32]) { return (uint32_t)(sip32(st, x) >> 32); }

/* ---- the index ---- */

static void index_add(store *st, uint32_t k, uint32_t i)
{
    size_t s = (size_t)k & (st->cap - 1);
    while (st->idx[s].num != EMPTY) s = (s + 1) & (st->cap - 1);
    st->idx[s] = (islot){k, i};
    st->used++;
}

/* room in the index for want entries in all, at most 70% of it */
static bool index_room(store *st, size_t want)
{
    if (st->cap && want * 10 <= st->cap * 7) return true;
    const size_t oc = st->cap;
    size_t nc = oc ? oc * 2 : 1024;
    while (want * 10 > nc * 7) nc *= 2;
    islot *old = st->idx, *ni = malloc(nc * sizeof *ni);
    if (!ni) return false;
    memset(ni, 0xff, nc * sizeof *ni);
    st->idx = ni, st->cap = nc, st->used = 0;
    for (size_t s = 0; s < oc; s++)
        if (old[s].num != EMPTY) index_add(st, old[s].key, old[s].num);
    free(old);
    return true;
}

/* ---- the owners ---- */

/* the number of an owner, EMPTY when the store has none of it */
static uint32_t owner_find(const store *st, const uint8_t id[32])
{
    for (size_t s = st->ocap ? (size_t)sip32(st, id) & (st->ocap - 1) : 0; st->ocap && st->own[s] != EMPTY; s = (s + 1) & (st->ocap - 1))
        if (!memcmp(st->ids[st->own[s]], id, 32)) return st->own[s];
    return EMPTY;
}

static void owner_place(store *st, uint32_t num)
{
    size_t s = (size_t)sip32(st, st->ids[num]) & (st->ocap - 1);
    while (st->own[s] != EMPTY) s = (s + 1) & (st->ocap - 1);
    st->own[s] = num;
    st->oused++;
}

/* the numbers below n taken, those without an owner as zeros (no owner is: an identity is a key): an entry that names an owner whose
 * line a crash lost keeps naming nobody, and a new owner never gets its number */
static bool ids_reserve(store *st, uint32_t n)
{
    if (n > st->capids) {
        const uint32_t nc = n > 2 * st->capids ? n : 2 * st->capids;
        uint8_t (*m)[32] = realloc(st->ids, (size_t)nc * sizeof *m);
        if (!m) return false;
        st->ids = m, st->capids = nc;
    }
    for (; st->nids < n; st->nids++) memset(st->ids[st->nids], 0, sizeof *st->ids);
    return true;
}

/* the owner id with its number in the tables of memory (the file has it already); false without memory */
static bool owner_index(store *st, const uint8_t id[32], uint32_t num)
{
    if (!st->ocap || (st->oused + 1) * 10 > st->ocap * 7) {
        const size_t oc = st->ocap, nc = oc ? oc * 2 : 64;
        uint32_t *old = st->own, *n = malloc(nc * sizeof *n);
        if (!n) return false;
        memset(n, 0xff, nc * sizeof *n);
        st->own = n, st->ocap = nc, st->oused = 0;
        for (size_t i = 0; i < oc; i++)
            if (old[i] != EMPTY) owner_place(st, old[i]);
        free(old);
    }
    if (!ids_reserve(st, num + 1)) return false;
    memcpy(st->ids[num], id, 32);
    owner_place(st, num);
    return true;
}

/* a new owner: its identity at the end of PATH.owners first, so that every entry names an owner the file has; its number, EMPTY on a
 * failure */
static uint32_t owner_add(store *st, const uint8_t id[32])
{
    const uint32_t num = st->nids;
    return num != EMPTY && sys_pwrite(st->ofd, id, 32, STORE_HDR + (int64_t)num * 32) == 32 && owner_index(st, id, num) ? num : EMPTY;
}

/* ---- the entries ---- */

static void entry_encode(const record *r, uint32_t num, uint8_t e[STORE_ENTRY])
{
    record_wire_encode(r, e);                                  /* the type, the zero bits and x: the record of a STORE */
    for (int i = 0; i < 4; i++) e[RECORD_WIRE + i] = (uint8_t)(num >> (8 * i));
}

static bool entry_decode(const store *st, const uint8_t e[STORE_ENTRY], record *r)
{
    uint32_t num = 0;
    for (int i = 0; i < 4; i++) num |= (uint32_t)e[RECORD_WIRE + i] << (8 * i);
    static const uint8_t none[32];
    if (num >= st->nids || !memcmp(st->ids[num], none, 32)) return false;   /* an owner whose line a crash lost */
    return record_wire_decode(e, st->ids[num], r);
}

static bool read_at(store *st, uint64_t i, record *r)
{
    uint8_t e[STORE_ENTRY];
    return sys_pread(st->fd, e, STORE_ENTRY, STORE_HDR + i * STORE_ENTRY) == STORE_ENTRY && entry_decode(st, e, r);
}

/* The records with x, the newest first (by their place in the file), at most X_MAX: the slots of its 4-byte key, read back and
 * compared. The newest first: past STORE_X_OWNERS of an x, a record is there because its owner proved it. */
static int find_locked(store *st, const uint8_t x[32], record out[X_MAX])
{
    if (!st->cap) return 0;
    uint32_t num[X_MAX];                                       /* their entries */
    const uint32_t k = key4(st, x);
    int n = 0;
    for (size_t s = (size_t)k & (st->cap - 1); st->idx[s].num != EMPTY; s = (s + 1) & (st->cap - 1)) {
        record r;
        const uint32_t e = st->idx[s].num;
        if (st->idx[s].key != k || !read_at(st, e, &r) || memcmp(r.x, x, 32)) continue;
        int at = n;
        while (at > 0 && num[at - 1] < e) at--;
        if (at == X_MAX) continue;
        if (n == X_MAX) n--;
        memmove(out + at + 1, out + at, (size_t)(n - at) * sizeof *out), memmove(num + at + 1, num + at, (size_t)(n - at) * sizeof *num);
        out[at] = r, num[at] = e, n++;
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
    if (!sys_random(st->hkey, sizeof st->hkey)) return fail(st, err, errlen, "no randomness from the system", path);
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
        if (sys_pread(st->ofd, buf, 32, STORE_HDR + i * 32) != 32 || !(memcmp(buf, none, 32) ? owner_index(st, buf, (uint32_t)i)
                                                                                              : ids_reserve(st, (uint32_t)i + 1))) {
            free(buf);
            return fail(st, err, errlen, "cannot read the owners", opath);
        }
    }
    st->count = (uint64_t)nent;
    if (!index_room(st, (size_t)nent)) {                       /* at once: no growing, no record placed twice */
        free(buf);
        return fail(st, err, errlen, "out of memory", path);
    }
    for (uint64_t i = 0; i < st->count; i += LOAD_CHUNK) {
        const size_t n = st->count - i < LOAD_CHUNK ? (size_t)(st->count - i) : LOAD_CHUNK;
        if (sys_pread(st->fd, buf, n * STORE_ENTRY, STORE_HDR + i * STORE_ENTRY) != (long)(n * STORE_ENTRY)) {
            free(buf);
            return fail(st, err, errlen, "cannot read the records", path);
        }
        islot add[LOAD_CHUNK];                                 /* the entries of the chunk to index: their slots are fetched at once */
        size_t na = 0;
        for (size_t j = 0; j < n; j++) {
            record r;
            uint32_t num = 0;
            for (int b = 0; b < 4; b++) num |= (uint32_t)buf[j * STORE_ENTRY + RECORD_WIRE + b] << (8 * b);
            if (num < EMPTY && num + 1 > refs) refs = num + 1;
            if (!entry_decode(st, buf + j * STORE_ENTRY, &r)) continue;   /* damaged on disk: not indexed */
            if (i + j + STORE_CHECK_TAIL >= st->count && !record_check(ctx, net, &r)) continue;   /* the tail, where a crash cuts */
            add[na] = (islot){key4(st, r.x), (uint32_t)(i + j)};
            __builtin_prefetch(&st->idx[add[na++].key & (st->cap - 1)], 1);
            st->weight += ldexpl(1, (int)r.dp);
        }
        for (size_t a = 0; a < na; a++) index_add(st, add[a].key, add[a].num);
    }
    free(buf);
    if (!ids_reserve(st, refs)) return fail(st, err, errlen, "out of memory", path);
    return st;
}

void store_limit(store *st, uint64_t max_records)
{
    pthread_mutex_lock(&st->mu);
    st->max = max_records;
    pthread_mutex_unlock(&st->mu);
}

bool store_owner(store *st, const uint8_t owner[32])
{
    pthread_mutex_lock(&st->mu);
    const bool known = owner_find(st, owner) != EMPTY;
    pthread_mutex_unlock(&st->mu);
    return known;
}

void store_close(store *st)
{
    if (!st) return;
    for (int i = 0; i < 2; i++) {
        const int fd = i ? st->ofd : st->fd;
        if (fd >= 0) (void)sys_sync(fd), close(fd);
    }
    free(st->idx), free(st->own), free(st->ids);
    pthread_mutex_destroy(&st->mu);
    free(st);
}

store_status store_put(store *st, const record *r, bool proved, record *others, int max, int *nothers)
{
    *nothers = 0;
    if (!record_check(st->ctx, &st->net, r)) return STORE_INVALID;   /* outside the lock: the costly part */
    record same[X_MAX];
    pthread_mutex_lock(&st->mu);
    const int ns = find_locked(st, r->x, same);
    int held = -1;
    for (int i = 0; i < ns && held < 0; i++)
        if (!memcmp(same[i].owner, r->owner, 32)) held = i;
    store_status s;
    uint32_t o = owner_find(st, r->owner);
    if (held >= 0) {
        s = STORE_DUP;
    } else if ((st->max && st->count >= st->max) || ns >= (proved ? X_MAX : STORE_X_OWNERS) ||
               (st->max && o == EMPTY && st->oused >= st->max / STORE_OWNER_RECORDS + 1024)) {   /* the owners it may hold */
        s = STORE_FULL;
    } else {
        uint8_t e[STORE_ENTRY];
        s = STORE_FAIL;
        if (o == EMPTY) o = owner_add(st, r->owner);
        if (o != EMPTY) entry_encode(r, o, e);
        if (o != EMPTY && st->count + 1 < EMPTY && index_room(st, st->used + 1) &&
            sys_pwrite(st->fd, e, STORE_ENTRY, STORE_HDR + st->count * STORE_ENTRY) == STORE_ENTRY) {
            index_add(st, key4(st, r->x), (uint32_t)st->count++);
            st->weight += ldexpl(1, (int)r->dp);
            s = STORE_OK;
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
    record r[X_MAX];
    pthread_mutex_lock(&st->mu);
    const int n = find_locked(st, x, r), k = n < max ? n : max;
    pthread_mutex_unlock(&st->mu);
    memcpy(out, r, (size_t)k * sizeof *r);
    return k;
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

bool store_pair_ok(const secp256k1_context *ctx, const record_net *net, const record *a, const record *b)
{
    return !memcmp(a->x, b->x, 32) && memcmp(a->owner, b->owner, 32) != 0 && record_check(ctx, net, a) && record_check(ctx, net, b);
}

int pair_order(const record *a, const record *b, record out[2])
{
    const int who = memcmp(a->owner, b->owner, 32) > 0;
    out[who] = *a, out[1 - who] = *b;
    return who;
}

void pair_hash(const record *a, const record *b, uint8_t h[32])
{
    uint8_t m[11 + 2 * RECORD_LEN] = "pznet1/pair";
    record p[2];
    pair_order(a, b, p);
    record_name(&p[0], m + 11), record_name(&p[1], m + 11 + RECORD_LEN);
    pz_sha256(m, sizeof m, h);
}

/* ---- the mailbox ---- */

struct mbox {
    pthread_mutex_t mu;
    mbox_pair *e;                  /* the oldest first */
    const mbox_pair **by_x;        /* room for make_room to sort them */
    size_t cap, per_owner, n;
};

mbox *mbox_new(size_t cap, size_t per_owner)
{
    mbox *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->cap = cap ? cap : 1;
    m->per_owner = per_owner ? per_owner : m->cap;
    m->e = calloc(m->cap, sizeof *m->e), m->by_x = malloc(m->cap * sizeof *m->by_x);
    if (!m->e || !m->by_x) {
        free(m->e), free(m->by_x), free(m);
        return NULL;
    }
    pthread_mutex_init(&m->mu, NULL);
    return m;
}

void mbox_free(mbox *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->mu);
    free(m->e), free(m->by_x);
    free(m);
}

static bool has_owner(const mbox_pair *p, const uint8_t owner[32]) { return !memcmp(p->r[0].owner, owner, 32) || !memcmp(p->r[1].owner, owner, 32); }

/* pairs by their x, and at one x the oldest first */
static int by_x(const void *a, const void *b)
{
    const mbox_pair *p = *(const mbox_pair *const *)a, *q = *(const mbox_pair *const *)b;
    const int c = memcmp(p->r[0].x, q->r[0].x, 32);
    return c ? c : (p > q) - (p < q);
}

/* Room for one more pair among those of owner (among all when NULL), which may be limit: of them, the oldest at the x that has the most
 * goes (the oldest, when every x has one). Whoever holds a point can pair it with records under any names, but only at its x: such a
 * flood pushes out its own pairs, and to push out a real one it needs about a point for each pair kept, each one 2^dpbits steps of a
 * walk. Before, the oldest went, and one point was enough to empty the mailbox of an owner. */
static void make_room(mbox *m, const uint8_t *owner, size_t limit)
{
    size_t k = 0, out = 0, most = 0;
    for (size_t i = 0; i < m->n; i++)
        if (!owner || has_owner(&m->e[i], owner)) m->by_x[k++] = &m->e[i];
    if (k < limit) return;
    qsort(m->by_x, k, sizeof *m->by_x, by_x);
    for (size_t i = 0, run = 0; i < k; i++) {                  /* the runs of one x, and the oldest of each */
        run = i && !memcmp(m->by_x[i]->r[0].x, m->by_x[i - 1]->r[0].x, 32) ? run + 1 : 1;
        const size_t first = (size_t)(m->by_x[i + 1 - run] - m->e);
        if (run > most || (run == most && first < out)) most = run, out = first;
    }
    memmove(&m->e[out], &m->e[out + 1], (m->n - out - 1) * sizeof *m->e);
    m->n--;
}

/* the same owners at the same point with the same types: one pair, whatever the zero bits of its records (a proof does not cover them) */
static bool same_pair(const mbox_pair *p, const mbox_pair *q)
{
    for (int i = 0; i < 2; i++)
        if (memcmp(p->r[i].owner, q->r[i].owner, 32) || memcmp(p->r[i].x, q->r[i].x, 32) || p->r[i].t != q->r[i].t) return false;
    return true;
}

bool mbox_put(mbox *m, const record *a, const record *b, const uint8_t proof[64])
{
    mbox_pair p;
    memset(&p, 0, sizeof p);
    const int who = pair_order(a, b, p.r);                    /* the place of a's owner, whose proof this is */
    pthread_mutex_lock(&m->mu);
    size_t at = 0;
    while (at < m->n && !same_pair(&m->e[at], &p)) at++;
    if (at == m->n) {                                          /* the pair with other zero bits: the one kept stays */
        make_room(m, p.r[0].owner, m->per_owner);
        make_room(m, p.r[1].owner, m->per_owner);
        make_room(m, NULL, m->cap);
        m->e[at = m->n++] = p;
    }
    const bool fresh = !m->e[at].has[who];
    if (fresh) m->e[at].has[who] = true, memcpy(m->e[at].proof[who], proof, 64);
    pthread_mutex_unlock(&m->mu);
    return fresh;
}

int mbox_get(mbox *m, const uint8_t owner[32], mbox_pair *out, int max)
{
    int k = 0;
    pthread_mutex_lock(&m->mu);
    for (size_t i = m->n; i-- > 0 && k < max;)
        if (has_owner(&m->e[i], owner)) out[k++] = m->e[i];
    pthread_mutex_unlock(&m->mu);
    return k;
}
