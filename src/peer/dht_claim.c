/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dht_claim.c - the claims of the blocks of a shared linear search (docs/en/NETWORK.md 11): a claim goes to the DHT_KSTORE custodians of
 * the key of its block, from its owner only (as a record), and anyone asks them for the claims of a block before it scans it. */
#include "dht_priv.h"
#include <stdlib.h>

/* CLAIM(claim) and CLAIM_GET(index 8): the answer into ans, its length; 0 for a request to refuse (the connection is closed) */
size_t claim_answer(dht *d, const net_conn *c, const uint8_t *m, size_t len, uint8_t *ans)
{
    block_claim cl[CLAIMS_PER_BLOCK];
    uint8_t k[32];
    if (!d->claims) return 0;
    if (m[0] == DHT_CLAIM) {
        /* the owner's own, for a block whose key this node may be a custodian of, within the rate of the records of its address block;
         * the checks that cost come last (a signature and a few tens of hashes) */
        if (len > 1 + BLOCK_CLAIM_MAX || !block_claim_decode(m + 1, len - 1, &cl[0]) || memcmp(cl[0].owner, c->remote, 32)) return 0;
        block_key(d->cfg.blocks, cl[0].index, k);
        ans[0] = DHT_CLAIMED;
        ans[1] = !responsible(d, k) ? CLAIM_FULL : !lim_take(d, &c->addr, LIM_REC, 1) ? CLAIM_FULL
               : !block_claim_check(d->cfg.ctx, d->cfg.blocks, &cl[0]) ? CLAIM_FAILED : (uint8_t)claims_put(d->claims, &cl[0]);
        return 2;
    }
    if (m[0] != DHT_CLAIM_GET || len != 9) return 0;
    uint64_t index = 0;
    for (int i = 0; i < 8; i++) index = index << 8 | m[1 + i];
    const int n = claims_get(d->claims, index, cl, CLAIMS_PER_BLOCK);
    size_t at = 2;
    ans[0] = DHT_CLAIMS, ans[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        const size_t l = block_claim_encode(&cl[i], ans + at + 2);
        ans[at] = (uint8_t)(l >> 8), ans[at + 1] = (uint8_t)l, at += 2 + l;
    }
    return at;
}

int dht_claim_publish(dht *d, const block_claim *c)
{
    dht_contact cs[DHT_KSTORE];
    uint8_t k[32], req[1 + BLOCK_CLAIM_MAX], ans[2];
    block_key(d->cfg.blocks, c->index, k);
    const int m = d->claims ? dht_lookup(d, k, cs, DHT_KSTORE) : 0;
    const size_t len = 1 + block_claim_encode(c, req + 1);
    int took = 0;
    req[0] = DHT_CLAIM;
    for (int i = 0; i < m && !stopping(d); i++) {
        size_t n;
        if (is_me(d, cs[i].id)) {
            took += claims_put(d->claims, c) <= CLAIM_DUP;
            continue;
        }
        const bool ok = exchange(d, &cs[i], req, len, ans, sizeof ans, &n) && n == 2 && ans[0] == DHT_CLAIMED;
        heard(d, &cs[i], ok);
        took += ok && ans[1] <= CLAIM_DUP;
    }
    return took;
}

int dht_claims_of(dht *d, uint64_t index, block_claim *out, int max)
{
    dht_contact cs[DHT_KSTORE];
    uint8_t k[32], req[9] = {DHT_CLAIM_GET}, *ans = malloc(2 + CLAIMS_PER_BLOCK * (2 + BLOCK_CLAIM_MAX));
    block_key(d->cfg.blocks, index, k);
    for (int i = 0; i < 8; i++) req[1 + i] = (uint8_t)(index >> (56 - 8 * i));
    const int m = d->claims && ans ? dht_lookup(d, k, cs, DHT_KSTORE) : 0;
    int got = 0, answered = 0;
    for (int i = 0; i < m && got < max && !stopping(d); i++) {
        block_claim cl[CLAIMS_PER_BLOCK];
        int n = 0;
        size_t len;
        if (is_me(d, cs[i].id)) n = claims_get(d->claims, index, cl, CLAIMS_PER_BLOCK), answered++;
        else {
            const bool ok = exchange(d, &cs[i], req, sizeof req, ans, 2 + CLAIMS_PER_BLOCK * (2 + BLOCK_CLAIM_MAX), &len) && len >= 2 &&
                            ans[0] == DHT_CLAIMS && ans[1] <= CLAIMS_PER_BLOCK;
            heard(d, &cs[i], ok);
            answered += ok;
            for (size_t at = 2, j = 0; ok && j < ans[1] && at + 2 <= len; j++) {
                const size_t l = (size_t)ans[at] << 8 | ans[at + 1];
                if (at + 2 + l > len || !block_claim_decode(ans + at + 2, l, &cl[n])) break;
                n++, at += 2 + l;
            }
        }
        for (int j = 0; j < n && got < max; j++) {             /* a custodian is not trusted: each claim checked, one an owner */
            bool seen = cl[j].index != index || !block_claim_check(d->cfg.ctx, d->cfg.blocks, &cl[j]);
            for (int h = 0; h < got && !seen; h++) seen = !memcmp(out[h].owner, cl[j].owner, 32);
            if (!seen) out[got++] = cl[j];
        }
    }
    free(ans);
    return answered || got ? got : -1;
}

uint64_t dht_claims_held(dht *d) { return d->claims ? claims_count(d->claims) : 0; }
