/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* perm.c - keyed bijection on [0, M). See perm.h. */
#include "perm.h"
#include "hash.h"

static u128 mask_bits(unsigned w) { return w >= 128 ? ~(u128)0 : (((u128)1 << w) - 1); }

static u128 round_fn(const perm_t *p, unsigned round, u128 v, unsigned out_bits)
{
    u8 buf[7 + 32 + 2 + 16];
    memcpy(buf, "PZPERM1", 7);
    memcpy(buf + 7, p->seed, 32);
    buf[39] = (u8)p->bits;
    buf[40] = (u8)round;
    for (int i = 0; i < 16; i++) buf[41 + i] = (u8)(v >> (8 * i));
    u8 h[32];
    sha256(buf, sizeof buf, h);
    u128 o = 0;
    for (int i = 0; i < 16; i++) o |= (u128)h[i] << (8 * i);
    return o & mask_bits(out_bits);
}

void perm_init(perm_t *p, const u8 seed[32], const u256 *m)
{
    memcpy(p->seed, seed, 32);
    p->m = *m;
    u256 mm1;
    u256 one = u256_from_u64(1);
    if (u256_is_zero(m)) pz_die("perm_init: empty domain");
    u256_sub(&mm1, m, &one);
    p->bits = u256_bits(&mm1);
    if (p->bits > 192) pz_die("perm_init: domain too large (%u bits)", p->bits);
    p->wl = p->bits / 2;
    p->wr = p->bits - p->wl;
    u8 h[32];
    u8 buf[39];
    memcpy(buf, "PZPERMSWAP1", 11);
    memcpy(buf + 11, seed, 28);
    sha256(buf, sizeof buf, h);
    p->swap1 = h[0] & 1;
}

static u256 pack(u128 a, u128 b, unsigned wb)
{
    /* value = a << wb | b, with a,b up to 96 bits */
    u256 r = u256_from_u128(b);
    u256 ah = u256_from_u128(a);
    ah = u256_shl(&ah, wb);
    u256_add(&r, &r, &ah);
    return r;
}

static u256 enc_once(const perm_t *p, const u256 *x)
{
    if (p->bits == 0) return *x;
    if (p->bits == 1) {
        u256 r = *x;
        if (p->swap1) r.w[0] ^= 1;
        return r;
    }
    u256 hi = u256_shr(x, p->wr);
    u128 a = (u128)hi.w[0] | (u128)hi.w[1] << 64;
    u128 b = ((u128)x->w[1] << 64 | x->w[0]) & mask_bits(p->wr);
    unsigned wa = p->wl, wb = p->wr;
    for (unsigned i = 0; i < PERM_ROUNDS; i++) {
        u128 na = b;
        u128 nb = (a ^ round_fn(p, i, b, wa)) & mask_bits(wa);
        a = na; b = nb;
        unsigned t = wa; wa = wb; wb = t;
    }
    return pack(a, b, p->wr);
}

static u256 dec_once(const perm_t *p, const u256 *y)
{
    if (p->bits <= 1) return enc_once(p, y);   /* identity / involution */
    u256 hi = u256_shr(y, p->wr);
    u128 a = (u128)hi.w[0] | (u128)hi.w[1] << 64;
    u128 b = ((u128)y->w[1] << 64 | y->w[0]) & mask_bits(p->wr);
    /* widths after PERM_ROUNDS (even) rounds equal the initial ones */
    for (int i = PERM_ROUNDS - 1; i >= 0; i--) {
        /* widths of (a,b) after round i: round i started with (wa_i, wb_i) = i even ? (wl,wr) : (wr,wl) */
        unsigned wa_i = (i % 2 == 0) ? p->wl : p->wr;
        u128 pb = a;                                          /* b_before = a_after */
        u128 pa = (b ^ round_fn(p, (unsigned)i, pb, wa_i)) & mask_bits(wa_i);
        a = pa; b = pb;
    }
    return pack(a, b, p->wr);
}

u256 perm_apply(const perm_t *p, const u256 *x)
{
    u256 y = enc_once(p, x);
    while (u256_cmp(&y, &p->m) >= 0) y = enc_once(p, &y);
    return y;
}

u256 perm_invert(const perm_t *p, const u256 *y)
{
    u256 x = dec_once(p, y);
    while (u256_cmp(&x, &p->m) >= 0) x = dec_once(p, &x);
    return x;
}
