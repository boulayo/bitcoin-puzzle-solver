/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* claims.c - see claims.h. The file: the header of store.h ("PZCLAIM1", F), then slots of CLAIM_SLOT bytes, each a claim on the wire
 * (block.c) after its length (2 bytes, big-endian). In memory, the block and the owner of each slot, sorted by block, then owner: the file
 * is this program's own, so its claims are not checked again when it is opened. */
#include "claims.h"
#include "store.h"
#include "sys.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLAIM_SLOT (2 + BLOCK_CLAIM_MAX)

typedef struct {
    uint64_t index;
    uint8_t owner[32];
    uint32_t slot;
} centry;

struct claims {
    pthread_mutex_t mu;
    int fd;
    centry *e;                     /* by block, then owner */
    size_t n, cap;
};

static int by_block(const void *a, const void *b)
{
    const centry *x = a, *y = b;
    return x->index != y->index ? (x->index > y->index) - (x->index < y->index) : memcmp(x->owner, y->owner, 32);
}

/* the first entry not below (index, owner) */
static size_t lower(const claims *c, uint64_t index, const uint8_t *owner)
{
    size_t lo = 0, hi = c->n;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const centry *m = &c->e[mid];
        if (m->index < index || (m->index == index && owner && memcmp(m->owner, owner, 32) < 0)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* the entry of slot at the end, or at its place in the order */
static bool insert(claims *c, const block_claim *cl, uint32_t slot, bool in_order)
{
    if (c->n == c->cap) {
        const size_t cap = c->cap ? 2 * c->cap : 1024;
        centry *g = realloc(c->e, cap * sizeof *g);
        if (!g) return false;
        c->e = g, c->cap = cap;
    }
    const size_t at = in_order ? lower(c, cl->index, cl->owner) : c->n;
    memmove(c->e + at + 1, c->e + at, (c->n - at) * sizeof *c->e);
    c->e[at] = (centry){.index = cl->index, .slot = slot};
    memcpy(c->e[at].owner, cl->owner, 32);
    c->n++;
    return true;
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
        if (!insert(c, &cl, (uint32_t)i, false)) n = -1;
    }
    if (c && c->e) qsort(c->e, c->n, sizeof *c->e, by_block);
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
    free(c->e);
    free(c);
}

/* under mu: what claims_put does with cl */
static int put(claims *c, const block_claim *cl)
{
    block_claim held[CLAIMS_PER_BLOCK + 1];
    uint8_t b[CLAIM_SLOT] = {0};
    const size_t at = lower(c, cl->index, NULL);
    int same = 0;
    for (; at + same < c->n && c->e[at + same].index == cl->index; same++)
        if (!memcmp(c->e[at + same].owner, cl->owner, 32)) return CLAIM_DUP;
    if (same >= CLAIMS_PER_BLOCK || c->n >= CLAIMS_MAX) return CLAIM_FULL;
    for (int k = 0; k < same; k++)
        if (!slot_read(c, c->e[at + k].slot, &held[k])) return CLAIM_FAILED;
    held[same] = *cl;
    if (same && block_claims_keys(held, same + 1) == block_claims_keys(held, same)) return CLAIM_COPY;
    const size_t len = block_claim_encode(cl, b + 2);
    b[0] = (uint8_t)(len >> 8), b[1] = (uint8_t)len;
    return sys_pwrite(c->fd, b, CLAIM_SLOT, STORE_HDR + (uint64_t)c->n * CLAIM_SLOT) == CLAIM_SLOT && insert(c, cl, (uint32_t)c->n, true)
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
    int k = 0;
    pthread_mutex_lock(&c->mu);
    for (size_t at = lower(c, index, NULL); at < c->n && c->e[at].index == index && k < max; at++) k += slot_read(c, c->e[at].slot, &out[k]);
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
