/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* perm.c - keyed bijection on [0, M). See perm.h. */
#include <pzcore/hash.h>
#include <pzcore/perm.h>

static pz_u128 mask_bits(unsigned w) { return w >= 128 ? ~(pz_u128)0 : (((pz_u128)1 << w) - 1); }

static pz_u128 round_fn(const pz_perm *p, unsigned round, pz_u128 v, unsigned out_bits)
{
    uint8_t buf[7 + 32 + 2 + 16];
    memcpy(buf, "PZPERM1", 7);
    memcpy(buf + 7, p->seed, 32);
    buf[39] = (uint8_t)p->bits;
    buf[40] = (uint8_t)round;
    for (int i = 0; i < 16; i++) buf[41 + i] = (uint8_t)(v >> (8 * i));
    uint8_t h[32];
    pz_sha256(buf, sizeof buf, h);
    pz_u128 o = 0;
    for (int i = 0; i < 16; i++) o |= (pz_u128)h[i] << (8 * i);
    return o & mask_bits(out_bits);
}

bool pz_perm_init(pz_perm *p, const uint8_t seed[32], const pz_u256 *m)
{
    if (pz_u256_is_zero(m)) return false;
    const pz_u256 one = pz_u256_from_u64(1);
    pz_u256 mm1;
    pz_u256_sub(&mm1, m, &one);
    const unsigned bits = pz_u256_bits(&mm1);
    if (bits > PZ_PERM_MAX_BITS) return false;
    memcpy(p->seed, seed, 32);
    p->m = *m;
    p->bits = bits;
    p->wl = bits / 2;
    p->wr = bits - p->wl;
    /* Part of the definition, kept as it was in the first release: only the first 28 bytes of the seed select the swap of the
     * two-element domain. */
    uint8_t h[32], buf[39];
    memcpy(buf, "PZPERMSWAP1", 11);
    memcpy(buf + 11, seed, 28);
    pz_sha256(buf, sizeof buf, h);
    p->swap1 = h[0] & 1;
    return true;
}

static pz_u256 pack(pz_u128 a, pz_u128 b, unsigned wb)
{
    /* value = a << wb | b, with a,b up to 96 bits */
    pz_u256 r = pz_u256_from_u128(b);
    pz_u256 ah = pz_u256_from_u128(a);
    ah = pz_u256_shl(&ah, wb);
    pz_u256_add(&r, &r, &ah);
    return r;
}

static pz_u256 enc_once(const pz_perm *p, const pz_u256 *x)
{
    if (p->bits == 0) return *x;
    if (p->bits == 1) {
        pz_u256 r = *x;
        if (p->swap1) r.w[0] ^= 1;
        return r;
    }
    pz_u256 hi = pz_u256_shr(x, p->wr);
    pz_u128 a = (pz_u128)hi.w[0] | (pz_u128)hi.w[1] << 64;
    pz_u128 b = ((pz_u128)x->w[1] << 64 | x->w[0]) & mask_bits(p->wr);
    unsigned wa = p->wl, wb = p->wr;
    for (unsigned i = 0; i < PZ_PERM_ROUNDS; i++) {
        pz_u128 na = b;
        pz_u128 nb = (a ^ round_fn(p, i, b, wa)) & mask_bits(wa);
        a = na; b = nb;
        unsigned t = wa; wa = wb; wb = t;
    }
    return pack(a, b, p->wr);
}

static pz_u256 dec_once(const pz_perm *p, const pz_u256 *y)
{
    if (p->bits <= 1) return enc_once(p, y);   /* identity / involution */
    pz_u256 hi = pz_u256_shr(y, p->wr);
    pz_u128 a = (pz_u128)hi.w[0] | (pz_u128)hi.w[1] << 64;
    pz_u128 b = ((pz_u128)y->w[1] << 64 | y->w[0]) & mask_bits(p->wr);
    /* widths after PZ_PERM_ROUNDS (even) rounds equal the initial ones */
    for (int i = PZ_PERM_ROUNDS - 1; i >= 0; i--) {
        /* widths of (a,b) after round i: round i started with (wa_i, wb_i) = i even ? (wl,wr) : (wr,wl) */
        unsigned wa_i = (i % 2 == 0) ? p->wl : p->wr;
        pz_u128 pb = a;                                          /* b_before = a_after */
        pz_u128 pa = (b ^ round_fn(p, (unsigned)i, pb, wa_i)) & mask_bits(wa_i);
        a = pa; b = pb;
    }
    return pack(a, b, p->wr);
}

bool pz_perm_apply(const pz_perm *p, const pz_u256 *x, pz_u256 *y)
{
    if (pz_u256_cmp(x, &p->m) >= 0) return false;
    pz_u256 v = enc_once(p, x);
    while (pz_u256_cmp(&v, &p->m) >= 0) v = enc_once(p, &v);
    *y = v;
    return true;
}

bool pz_perm_invert(const pz_perm *p, const pz_u256 *y, pz_u256 *x)
{
    if (pz_u256_cmp(y, &p->m) >= 0) return false;
    pz_u256 v = dec_once(p, y);
    while (pz_u256_cmp(&v, &p->m) >= 0) v = dec_once(p, &v);
    *x = v;
    return true;
}
