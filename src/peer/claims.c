/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* claims.c - see claims.h. The file: the header of store.h ("PZCLAIM1", F), then slots of CLAIM_SLOT bytes, each a claim on the wire
 * (block.c) after its length (2 bytes, big-endian). In memory, the block of each slot in a table by the index of the block (open
 * addressing, linear probing, at most half full): the claims of a block are found, and one added, in a few steps however many there are
 * (before, an array sorted by block moved half of itself for each claim added, 4 ms near CLAIMS_MAX). A claim costs the scan of its
 * block, so nobody makes many that meet in one place of the table. The file is this program's own, so its claims are not checked again
 * when it is opened. */
#include "claims.h"
#include "store.h"
#include "sys.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLAIM_SLOT (2 + BLOCK_CLAIM_MAX)
#define NONE UINT32_MAX            /* a free entry of the table */

typedef struct {
    uint64_t index;
    uint32_t slot;
} centry;

struct claims {
    pthread_mutex_t mu;
    int fd;
    centry *t;                     /* cap entries, a power of two */
    size_t n, cap;                 /* the slots of the file, each in the table */
};

/* the first place of block index in a table of cap entries: its index mixed (splitmix64), so that blocks near each other spread */
static size_t home_of(uint64_t x, size_t cap)
{
    x ^= x >> 30, x *= 0xbf58476d1ce4e5b9ull, x ^= x >> 27, x *= 0x94d049bb133111ebull;
    return (size_t)(x ^ x >> 31) & (cap - 1);
}

static void place(centry *t, size_t cap, centry e)
{
    size_t s = home_of(e.index, cap);
    while (t[s].slot != NONE) s = (s + 1) & (cap - 1);
    t[s] = e;
}

/* slot, a claim of block index, into the table, grown to stay at most half full */
static bool add(claims *c, uint64_t index, uint32_t slot)
{
    if (2 * (c->n + 1) > c->cap) {
        const size_t cap = c->cap ? 2 * c->cap : 1024;
        centry *t = malloc(cap * sizeof *t);
        if (!t) return false;
        for (size_t i = 0; i < cap; i++) t[i].slot = NONE;
        for (size_t i = 0; i < c->cap; i++)
            if (c->t[i].slot != NONE) place(t, cap, c->t[i]);
        free(c->t), c->t = t, c->cap = cap;
    }
    place(c->t, c->cap, (centry){index, slot});
    c->n++;
    return true;
}

/* the slots of the claims of block index, at most max; their number */
static int slots_of(const claims *c, uint64_t index, uint32_t *out, int max)
{
    int k = 0;
    for (size_t s = c->cap ? home_of(index, c->cap) : 0; c->cap && c->t[s].slot != NONE && k < max; s = (s + 1) & (c->cap - 1))
        if (c->t[s].index == index) out[k++] = c->t[s].slot;
    return k;
}

static bool slot_read(const claims *c, uint32_t slot, block_claim *out)
{
    uint8_t b[CLAIM_SLOT];
    if (sys_pread(c->fd, b, CLAIM_SLOT, STORE_HDR + (uint64_t)slot * CLAIM_SLOT) != CLAIM_SLOT) return false;
    const size_t len = (size_t)b[0] << 8 | b[1];
    return len <= BLOCK_CLAIM_MAX && block_claim_decode(b + 2, len, out);
}

claims *claims_open(const char *path, const uint8_t fp[32], char *err, size_t errlen)
{
    claims *c = calloc(1, sizeof *c);
    const char *why = "out of memory";
    int64_t n = -1;
    if (c) pthread_mutex_init(&c->mu, NULL), n = store_file(path, "PZCLAIM1", fp, CLAIM_SLOT, &c->fd, &why);
    for (int64_t i = 0; i >= 0 && i < n && i <= CLAIMS_MAX; i++) {
        block_claim cl;
        if (!slot_read(c, (uint32_t)i, &cl)) {
            if (!sys_truncate(c->fd, STORE_HDR + (uint64_t)i * CLAIM_SLOT)) why = "cannot cut a claim that cannot be read", n = -1;
            break;
        }
        if (!add(c, cl.index, (uint32_t)i)) n = -1;
    }
    if (n < 0 || n > CLAIMS_MAX) {
        snprintf(err, errlen, "%s: %s", path, n > CLAIMS_MAX ? "too many claims" : why);
        claims_close(c);
        return NULL;
    }
    return c;
}

void claims_close(claims *c)
{
    if (!c) return;
    if (c->fd >= 0) (void)sys_sync(c->fd), close(c->fd);
    pthread_mutex_destroy(&c->mu);
    free(c->t);
    free(c);
}

/* under mu: what claims_put does with cl */
static int put(claims *c, const block_claim *cl)
{
    block_claim held[CLAIMS_PER_BLOCK + 1];
    uint32_t slots[CLAIMS_PER_BLOCK];
    uint8_t b[CLAIM_SLOT] = {0};
    const int same = slots_of(c, cl->index, slots, CLAIMS_PER_BLOCK);
    for (int k = 0; k < same; k++) {
        if (!slot_read(c, slots[k], &held[k])) return CLAIM_FAILED;
        if (!memcmp(held[k].owner, cl->owner, 32)) return CLAIM_DUP;
    }
    if (same >= CLAIMS_PER_BLOCK || c->n >= CLAIMS_MAX) return CLAIM_FULL;
    held[same] = *cl;
    if (same && block_claims_keys(held, same + 1) == block_claims_keys(held, same)) return CLAIM_COPY;
    const size_t len = block_claim_encode(cl, b + 2);
    b[0] = (uint8_t)(len >> 8), b[1] = (uint8_t)len;
    return sys_pwrite(c->fd, b, CLAIM_SLOT, STORE_HDR + (uint64_t)c->n * CLAIM_SLOT) == CLAIM_SLOT && add(c, cl->index, (uint32_t)c->n)
               ? CLAIM_STORED : CLAIM_FAILED;
}

int claims_put(claims *c, const block_claim *cl)
{
    pthread_mutex_lock(&c->mu);
    const int st = put(c, cl);
    pthread_mutex_unlock(&c->mu);
    return st;
}

int claims_get(claims *c, uint64_t index, block_claim *out, int max)
{
    uint32_t slots[CLAIMS_PER_BLOCK];
    int k = 0;
    pthread_mutex_lock(&c->mu);
    const int n = slots_of(c, index, slots, max < CLAIMS_PER_BLOCK ? max : CLAIMS_PER_BLOCK);
    for (int i = 0; i < n; i++) k += slot_read(c, slots[i], &out[k]);
    pthread_mutex_unlock(&c->mu);
    return k;
}

bool claims_at(claims *c, uint64_t k, block_claim *out)
{
    pthread_mutex_lock(&c->mu);
    const bool ok = k < c->n && slot_read(c, (uint32_t)k, out);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

uint64_t claims_count(claims *c)
{
    pthread_mutex_lock(&c->mu);
    const uint64_t n = c->n;
    pthread_mutex_unlock(&c->mu);
    return n;
}
