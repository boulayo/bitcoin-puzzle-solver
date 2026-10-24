/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* u256.h - 256-bit unsigned integers: four 64-bit limbs, least significant first, plus the arithmetic modulo the order of
 * the secp256k1 group.
 *
 * Everything small is static inline. Nothing here allocates, reads global state or fails: an operation either has a result for
 * every input or documents the one input it cannot take. All functions are thread-safe and none is constant-time. */
#ifndef PZCORE_U256_H
#define PZCORE_U256_H

#include <pzcore/defs.h>

PZ_BEGIN_DECLS

typedef struct { uint64_t w[4]; } pz_u256;

/* minimum sizes of the buffers of pz_u256_to_hex and pz_u256_to_dec (the longest text plus the terminating NUL) */
#define PZ_U256_HEX_BUF 65
#define PZ_U256_DEC_BUF 80

static inline pz_u256 pz_u256_zero(void) { pz_u256 r = {{0, 0, 0, 0}}; return r; }
static inline pz_u256 pz_u256_from_u64(uint64_t v) { pz_u256 r = {{v, 0, 0, 0}}; return r; }
static inline pz_u256 pz_u256_from_u128(pz_u128 v) { pz_u256 r = {{(uint64_t)v, (uint64_t)(v >> 64), 0, 0}}; return r; }
static inline bool pz_u256_is_zero(const pz_u256 *a) { return (a->w[0] | a->w[1] | a->w[2] | a->w[3]) == 0; }

/* -1, 0 or 1 as a is below, equal to or above b */
static inline int pz_u256_cmp(const pz_u256 *a, const pz_u256 *b)
{
    for (int i = 3; i >= 0; i--) {
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    }
    return 0;
}

/* r = a + b mod 2^256; returns the carry out (0 or 1). r may alias a or b. */
static inline uint64_t pz_u256_add(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    pz_u128 c = 0;
    for (int i = 0; i < 4; i++) {
        c += (pz_u128)a->w[i] + b->w[i];
        r->w[i] = (uint64_t)c;
        c >>= 64;
    }
    return (uint64_t)c;
}

/* r = a - b mod 2^256; returns the borrow out (0 or 1). r may alias a or b. */
static inline uint64_t pz_u256_sub(pz_u256 *r, const pz_u256 *a, const pz_u256 *b)
{
    uint64_t br = 0;
    for (int i = 0; i < 4; i++) {
        pz_u128 d = (pz_u128)a->w[i] - b->w[i] - br;
        r->w[i] = (uint64_t)d;
        br = (uint64_t)(d >> 64) & 1;
    }
    return br;
}

/* r = a + v mod 2^256; returns the carry out. r may alias a. */
static inline uint64_t pz_u256_add_u64(pz_u256 *r, const pz_u256 *a, uint64_t v)
{
    pz_u256 t = pz_u256_from_u64(v);
    return pz_u256_add(r, a, &t);
}

/* a << s (zero when s >= 256); the bits shifted out are lost */
static inline pz_u256 pz_u256_shl(const pz_u256 *a, unsigned s)
{
    pz_u256 r = pz_u256_zero();
    if (s >= 256) return r;
    unsigned ws = s / 64, bs = s % 64;
    for (int i = 3; i >= (int)ws; i--) {
        uint64_t v = a->w[i - (int)ws] << bs;
        if (bs && i - (int)ws - 1 >= 0) v |= a->w[i - (int)ws - 1] >> (64 - bs);
        r.w[i] = v;
    }
    return r;
}

/* a >> s (zero when s >= 256) */
static inline pz_u256 pz_u256_shr(const pz_u256 *a, unsigned s)
{
    pz_u256 r = pz_u256_zero();
    if (s >= 256) return r;
    unsigned ws = s / 64, bs = s % 64;
    for (int i = 0; i + (int)ws < 4; i++) {
        uint64_t v = a->w[i + (int)ws] >> bs;
        if (bs && i + (int)ws + 1 < 4) v |= a->w[i + (int)ws + 1] << (64 - bs);
        r.w[i] = v;
    }
    return r;
}

/* number of significant bits (0 for zero) */
static inline unsigned pz_u256_bits(const pz_u256 *a)
{
    for (int i = 3; i >= 0; i--)
        if (a->w[i]) return (unsigned)(i * 64 + 64 - __builtin_clzll(a->w[i]));
    return 0;
}

/* bit i of a (0 = least significant); false for i >= 256 */
static inline bool pz_u256_bit(const pz_u256 *a, unsigned i) { return i < 256 && ((a->w[i >> 6] >> (i & 63)) & 1); }

/* a * m mod 2^256: the carry out of the top limb is dropped, so the caller keeps the result small */
static inline pz_u256 pz_u256_mul_u32(const pz_u256 *a, uint32_t m)
{
    pz_u256 r;
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        const pz_u128 t = (pz_u128)a->w[i] * m + carry;
        r.w[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    return r;
}

/* a / d; *rem (if not NULL) gets a mod d. d must not be zero. */
static inline pz_u256 pz_u256_divmod_u32(const pz_u256 *a, uint32_t d, uint32_t *rem)
{
    pz_u256 q;
    uint64_t r = 0;
    for (int i = 3; i >= 0; i--) {
        const pz_u128 cur = ((pz_u128)r << 64) | a->w[i];
        q.w[i] = (uint64_t)(cur / d);
        r = (uint64_t)(cur % d);
    }
    if (rem) *rem = (uint32_t)r;
    return q;
}

/* 2^s (zero when s >= 256) */
static inline pz_u256 pz_u256_pow2(unsigned s) { pz_u256 one = pz_u256_from_u64(1); return pz_u256_shl(&one, s); }

/* 32 bytes, most significant first */
static inline void pz_u256_to_be(const pz_u256 *a, uint8_t out[32])
{
    for (int i = 0; i < 4; i++) {
        const uint64_t v = a->w[3 - i];
        for (int j = 0; j < 8; j++) out[i * 8 + j] = (uint8_t)(v >> (56 - 8 * j));
    }
}

static inline void pz_u256_from_be(pz_u256 *r, const uint8_t in[32])
{
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = v << 8 | in[i * 8 + j];
        r->w[3 - i] = v;
    }
}

/* Parses 1 to 64 hexadecimal digits (either case), optionally behind "0x". Returns false, and leaves *r untouched, for anything
 * else: NULL, an empty string, a longer one (more than 256 bits, leading zeros included), a character that is not a digit. */
bool pz_u256_from_hex(pz_u256 *r, const char *s);
/* Writes the shortest lower-case hexadecimal text (at least one digit) into buf, which holds at least PZ_U256_HEX_BUF bytes,
 * and returns buf. */
char *pz_u256_to_hex(const pz_u256 *a, char *buf);
/* Writes the decimal text into buf, which holds at least PZ_U256_DEC_BUF bytes, and returns buf. */
char *pz_u256_to_dec(const pz_u256 *a, char *buf);
/* Nearest long double (64-bit mantissa on x86-64): for statistics, not for exact work. */
long double pz_u256_to_ld(const pz_u256 *a);

/* The order n of the secp256k1 group. */
extern const pz_u256 pz_secp256k1_n;

/* r = a + b mod n and r = a - b mod n. Both operands must already be below n. r may alias a or b. */
void pz_u256_addmod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b);
void pz_u256_submod_n(pz_u256 *r, const pz_u256 *a, const pz_u256 *b);

PZ_END_DECLS

#endif
