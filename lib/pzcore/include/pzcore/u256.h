/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* pz_u256.h - minimal 256-bit unsigned integer helpers (little-endian limbs). */
#ifndef PZ_U256_H
#define PZ_U256_H

#include "common.h"

typedef struct { u64 w[4]; } pz_u256;

static inline pz_u256 pz_u256_zero(void) { pz_u256 r = {{0, 0, 0, 0}}; return r; }
static inline pz_u256 pz_u256_from_u64(u64 v) { pz_u256 r = {{v, 0, 0, 0}}; return r; }
static inline pz_u256 pz_u256_from_u128(u128 v) { pz_u256 r = {{(u64)v, (u64)(v >> 64), 0, 0}}; return r; }
static inline bool pz_u256_is_zero(const pz_u256 *a) { return (a->w[0] | a->w[1] | a->w[2] | a->w[3]) == 0; }

static inline int pz_u256_cmp(const pz_u256 *a, const pz_u256 *b)
{
    for (int i = 3; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

/* r = a + b, returns carry out. r may alias a or b. */
static inline u64 pz_u256_add(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += (u128)a->w[i] + b->w[i];
        r->w[i] = (u64)c;
        c >>= 64;
    }
    return (u64)c;
}

/* r = a - b, returns borrow out. */
static inline u64 pz_u256_sub(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    u64 br = 0;
    for (int i = 0; i < 4; i++) {
        u128 d = (u128)a->w[i] - b->w[i] - br;
        r->w[i] = (u64)d;
        br = (u64)(d >> 64) & 1;
    }
    return br;
}

static inline u64 pz_u256_add_u64(pz_u256 *r, const pz_u256 *a, u64 v)
{
    pz_u256 t = pz_u256_from_u64(v);
    return pz_u256_add(r, a, &t);
}

static inline pz_u256 pz_u256_shl(const pz_u256 *a, unsigned s)
{
    pz_u256 r = pz_u256_zero();
    if (s >= 256) return r;
    unsigned ws = s / 64, bs = s % 64;
    for (int i = 3; i >= (int)ws; i--) {
        u64 v = a->w[i - ws] << bs;
        if (bs && i - (int)ws - 1 >= 0) v |= a->w[i - ws - 1] >> (64 - bs);
        r.w[i] = v;
    }
    return r;
}

static inline pz_u256 pz_u256_shr(const pz_u256 *a, unsigned s)
{
    pz_u256 r = pz_u256_zero();
    if (s >= 256) return r;
    unsigned ws = s / 64, bs = s % 64;
    for (int i = 0; i + (int)ws < 4; i++) {
        u64 v = a->w[i + ws] >> bs;
        if (bs && i + (int)ws + 1 < 4) v |= a->w[i + ws + 1] << (64 - bs);
        r.w[i] = v;
    }
    return r;
}

/* Number of significant bits (0 for zero). */
static inline unsigned pz_u256_bits(const pz_u256 *a)
{
    for (int i = 3; i >= 0; i--)
        if (a->w[i]) return (unsigned)(i * 64 + 64 - __builtin_clzll(a->w[i]));
    return 0;
}

static inline bool pz_u256_bit(const pz_u256 *a, unsigned i) { return (a->w[i >> 6] >> (i & 63)) & 1; }

/* a * m: the carry out of the top limb is dropped (callers keep results small) */
static inline pz_u256 pz_u256_mul_u32(const pz_u256 *a, u32 m)
{
    pz_u256 r;
    u64 carry = 0;
    for (int i = 0; i < 4; i++) {
        const u128 t = (u128)a->w[i] * m + carry;
        r.w[i] = (u64)t;
        carry = (u64)(t >> 64);
    }
    return r;
}

/* a / d (d != 0); *rem gets a mod d */
static inline pz_u256 pz_u256_divmod_u32(const pz_u256 *a, u32 d, u32 *rem)
{
    pz_u256 q;
    u64 r = 0;
    for (int i = 3; i >= 0; i--) {
        const u128 cur = ((u128)r << 64) | a->w[i];
        q.w[i] = (u64)(cur / d);
        r = (u64)(cur % d);
    }
    if (rem) *rem = (u32)r;
    return q;
}

static inline pz_u256 pz_u256_pow2(unsigned s) { pz_u256 one = pz_u256_from_u64(1); return pz_u256_shl(&one, s); }

/* Parse hex (optional 0x). Returns false on overflow / bad chars. */
bool pz_u256_from_hex(pz_u256 *r, const char *s);
/* Writes minimal lowercase hex (at least 1 digit). buf >= 65 bytes. */
char *pz_u256_to_hex(const pz_u256 *a, char *buf);
/* 32 bytes big-endian. */
void pz_u256_to_be(const pz_u256 *a, u8 out[32]);
void pz_u256_from_be(pz_u256 *r, const u8 in[32]);
/* Decimal (buf >= 80 bytes). */
char *pz_u256_to_dec(const pz_u256 *a, char *buf);
/* Approximate conversion to long double (for statistics). */
long double pz_u256_to_ld(const pz_u256 *a);

/* The secp256k1 group order n. */
extern const pz_u256 pz_secp256k1_n;

/* Modular helpers mod n (inputs < n). */
void pz_u256_addmod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b);
void pz_u256_submod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b);

#endif
