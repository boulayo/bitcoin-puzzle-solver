/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* spv.c - the chain of block headers and the Merkle proofs (see spv.h). The rules are those of Bitcoin Core for headers: on the main chain
 * the target changes every 2016 blocks by the time the last 2016 took, within a factor of 4; on regtest it never changes; the test chains
 * have rules of their own for the target (blocks of minimum difficulty, signed blocks), so there only the proof of work of a header against
 * its own target is checked: the light verification protects a payment on the main chain, the test chains serve to try it. */
#include "spv.h"
#include "sys.h"
#include <pzcore/hash.h>
#include <pzcore/u256.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMESPAN (14 * 24 * 3600)  /* the two weeks a period should take */
#define FUTURE (2 * 3600)          /* a header may be this far ahead of our clock */
#define LOOKBACK (SPV_PERIOD + 11) /* headers before a fork that the checks of its branch read */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t hdr_time(const uint8_t *h) { return le32(h + 68); }
static uint32_t hdr_bits(const uint8_t *h) { return le32(h + 72); }

static uint32_t pow_limit(btc_chain chain) { return chain == BTC_REGTEST ? 0x207fffffu : chain == BTC_SIGNET ? 0x1e0377aeu : 0x1d00ffffu; }

void spv_genesis(btc_chain chain, uint8_t hash[32])
{
    static const char *const HEX[] = {
        [BTC_MAIN] = "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
        [BTC_TEST] = "000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943",
        [BTC_SIGNET] = "00000008819873e925422c1ff0f99f7cc9bbb232af63a077a480a3633bee1ef6",
        [BTC_REGTEST] = "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206",
    };
    (void)btc_hash_from_hex(HEX[chain], hash);
}

bool spv_target(uint32_t bits, uint8_t target[32])
{
    const unsigned e = bits >> 24;
    const uint32_t m = bits & 0x007fffffu;
    memset(target, 0, 32);
    if ((bits & 0x00800000u) || (m && (e > 34 || (m > 0xff && e > 33) || (m > 0xffff && e > 32)))) return false;   /* negative, or above 2^256 */
    for (int b = 0; b < 3; b++) {                              /* the mantissa byte of weight 256^(e - 3 + b) */
        const int pos = 31 - ((int)e - 3 + b);
        if (pos >= 0 && pos < 32) target[pos] = (uint8_t)(m >> (8 * b));
    }
    return true;
}

static uint32_t compact_of(const pz_u256 *t)
{
    unsigned n = (pz_u256_bits(t) + 7) / 8;
    uint32_t m;
    if (n <= 3) m = (uint32_t)(t->w[0] << (8 * (3 - n)));
    else {
        const pz_u256 s = pz_u256_shr(t, 8 * (n - 3));
        m = (uint32_t)s.w[0];
    }
    if (m & 0x00800000u) m >>= 8, n++;                         /* the top bit of the mantissa is a sign */
    return m | (uint32_t)n << 24;
}

uint32_t spv_retarget(uint32_t bits, int64_t timespan)
{
    uint8_t be[32], lim[32];
    pz_u256 t, limit;
    uint32_t rem;
    if (timespan < TIMESPAN / 4) timespan = TIMESPAN / 4;
    if (timespan > TIMESPAN * 4) timespan = TIMESPAN * 4;
    if (!spv_target(bits, be) || !spv_target(0x1d00ffffu, lim)) return 0;
    pz_u256_from_be(&t, be);
    pz_u256_from_be(&limit, lim);
    t = pz_u256_mul_u32(&t, (uint32_t)timespan);               /* a target of the main chain is below 2^224: no overflow */
    t = pz_u256_divmod_u32(&t, TIMESPAN, &rem);
    if (pz_u256_cmp(&t, &limit) > 0) t = limit;
    return compact_of(&t);
}

static void hdr_hash(const uint8_t *h, uint8_t out[32]) { pz_sha256d(h, SPV_HEADER, out); }

/* the work a header stands for, 2^256 / (target + 1), near enough to compare two branches */
static long double hdr_work(const uint8_t *h)
{
    uint8_t t[32];
    long double v = 0;
    if (!spv_target(hdr_bits(h), t)) return 0;
    for (int i = 0; i < 32; i++) v = v * 256 + t[i];
    return ldexpl(1.0L, 256) / (v + 1);
}

/* Whether header i of arr (whose first header has height h0) follows from the ones before it. */
static bool hdr_valid(btc_chain chain, const uint8_t *arr, uint64_t h0, size_t i, uint64_t now)
{
    const uint8_t *h = arr + i * SPV_HEADER, *prev = h - SPV_HEADER;
    uint8_t hash[32], target[32], limit[32], rev[32];
    hdr_hash(prev, hash);
    if (memcmp(h + 4, hash, 32) != 0) return false;            /* the block before */
    const uint32_t bits = hdr_bits(h);
    if (!spv_target(bits, target) || !spv_target(pow_limit(chain), limit) || memcmp(target, limit, 32) > 0) return false;
    hdr_hash(h, hash);
    for (int b = 0; b < 32; b++) rev[b] = hash[31 - b];        /* the hash as a number, most significant byte first */
    if (memcmp(rev, target, 32) > 0) return false;             /* the proof of work */
    if (chain == BTC_REGTEST && bits != hdr_bits(prev)) return false;
    if (chain == BTC_MAIN) {
        uint32_t want = hdr_bits(prev);
        if ((h0 + i) % SPV_PERIOD == 0) {
            if (i < SPV_PERIOD) return false;                  /* the first header of the period that ended is not kept */
            want = spv_retarget(want, (int64_t)hdr_time(prev) - (int64_t)hdr_time(h - (size_t)SPV_PERIOD * SPV_HEADER));
        }
        if (bits != want) return false;
    }
    uint32_t t[11];                                            /* above the median time of the 11 blocks before */
    size_t n = 0;
    for (; n < 11 && n < i; n++) {
        uint32_t v = hdr_time(h - (n + 1) * SPV_HEADER);
        size_t k = n;
        for (; k > 0 && t[k - 1] > v; k--) t[k] = t[k - 1];
        t[k] = v;
    }
    return hdr_time(h) > t[n / 2] && hdr_time(h) <= now + FUTURE;
}

bool spv_init(spv_chain *c, btc_chain chain, uint64_t base, const uint8_t hash[32])
{
    memset(c, 0, sizeof *c);
    if (chain == BTC_MAIN && base % SPV_PERIOD) return false;
    c->chain = chain, c->base = base;
    memcpy(c->cp, hash, 32);
    return true;
}

void spv_free(spv_chain *c)
{
    free(c->hdr);
    c->hdr = NULL, c->n = c->cap = 0;
}

static bool reserve(spv_chain *c, size_t n)
{
    if (n <= c->cap) return true;
    size_t cap = c->cap ? c->cap : 4096;
    while (cap < n) cap *= 2;
    uint8_t *p = realloc(c->hdr, cap * SPV_HEADER);
    if (!p) return false;
    c->hdr = p, c->cap = cap;
    return true;
}

bool spv_add(spv_chain *c, uint64_t at, const uint8_t *hdrs, size_t count, uint64_t now)
{
    if (!count) return true;
    if (c->n == 0) {                                           /* the checkpoint itself: trusted by its hash */
        uint8_t hash[32];
        hdr_hash(hdrs, hash);
        if (at != c->base || memcmp(hash, c->cp, 32) != 0 || !reserve(c, 1)) return false;
        memcpy(c->hdr, hdrs, SPV_HEADER);
        c->n = 1;
    }
    if (at < c->base || at > c->base + c->n) return false;
    size_t j = (size_t)(at - c->base);                         /* the headers we have already */
    while (count && j < c->n && !memcmp(c->hdr + j * SPV_HEADER, hdrs, SPV_HEADER)) j++, hdrs += SPV_HEADER, count--;
    if (!count) return true;
    if (j == 0) return false;                                  /* another header at the checkpoint */
    if (j == c->n) {                                           /* they extend the chain */
        if (!reserve(c, c->n + count)) return false;
        memcpy(c->hdr + c->n * SPV_HEADER, hdrs, count * SPV_HEADER);
        for (size_t i = 0; i < count; i++)
            if (!hdr_valid(c->chain, c->hdr, c->base, c->n + i, now)) return false;
        c->n += count;
        return true;
    }
    /* a branch that leaves the chain at j: checked on a copy of what its checks read, taken only with more work than what it replaces */
    const size_t back = j < LOOKBACK ? j : LOOKBACK;
    uint8_t *tmp = malloc((back + count) * SPV_HEADER);
    if (!tmp) return false;
    memcpy(tmp, c->hdr + (j - back) * SPV_HEADER, back * SPV_HEADER);
    memcpy(tmp + back * SPV_HEADER, hdrs, count * SPV_HEADER);
    bool ok = true;
    long double theirs = 0, ours = 0;
    for (size_t i = 0; ok && i < count; i++) {
        ok = hdr_valid(c->chain, tmp, c->base + j - back, back + i, now);
        theirs += hdr_work(hdrs + i * SPV_HEADER);
    }
    free(tmp);
    for (size_t i = j; i < c->n; i++) ours += hdr_work(c->hdr + i * SPV_HEADER);
    if (!ok) return false;
    if (theirs <= ours) return true;                           /* valid, but ours is the better chain */
    if (!reserve(c, j + count)) return false;
    memcpy(c->hdr + j * SPV_HEADER, hdrs, count * SPV_HEADER);
    c->n = j + count;
    return true;
}

bool spv_tip(const spv_chain *c, uint64_t *height)
{
    if (!c->n) return false;
    *height = c->base + c->n - 1;
    return true;
}

const uint8_t *spv_header(const spv_chain *c, uint64_t height)
{
    return height >= c->base && height - c->base < c->n ? c->hdr + (size_t)(height - c->base) * SPV_HEADER : NULL;
}

bool spv_proof(const spv_chain *c, uint64_t height, const uint8_t txid[32], const uint8_t (*branch)[32], size_t n, uint32_t pos)
{
    const uint8_t *h = spv_header(c, height);
    uint8_t cat[64], node[32];
    if (!h || n > SPV_BRANCH_MAX || (n < 32 && pos >> n)) return false;
    memcpy(node, txid, 32);
    for (size_t i = 0; i < n; i++, pos >>= 1) {
        memcpy(cat + (pos & 1 ? 32 : 0), node, 32);
        memcpy(cat + (pos & 1 ? 0 : 32), branch[i], 32);
        pz_sha256d(cat, 64, node);
    }
    return !memcmp(node, h + 36, 32);
}

static const char MAGIC[8] = "PZHDR1\n";

bool spv_save(const spv_chain *c, const char *path)
{
    char tmp[1100];
    uint8_t head[8 + 1 + 8 + 32];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    memcpy(head, MAGIC, 8);
    head[8] = (uint8_t)c->chain;
    for (int b = 0; b < 8; b++) head[9 + b] = (uint8_t)(c->base >> (8 * b));
    memcpy(head + 17, c->cp, 32);
    const bool ok = fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(c->hdr, SPV_HEADER, c->n, f) == c->n;
    return (fclose(f) == 0) && ok && sys_replace(tmp, path);
}

bool spv_load(spv_chain *c, const char *path, uint64_t now)
{
    uint8_t head[8 + 1 + 8 + 32], buf[SPV_HEADER];
    uint64_t base = 0, at = c->base;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head, MAGIC, 8) && head[8] == (uint8_t)c->chain;
    for (int b = 0; b < 8; b++) base |= (uint64_t)head[9 + b] << (8 * b);
    ok = ok && base == c->base && !memcmp(head + 17, c->cp, 32);
    for (; ok && fread(buf, SPV_HEADER, 1, f) == 1; at++) ok = spv_add(c, at, buf, 1, now);   /* every header checked again: what is valid stays */
    fclose(f);
    return ok;
}
