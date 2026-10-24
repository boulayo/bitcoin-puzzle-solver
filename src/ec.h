/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.h - secp256k1 group operations (y^2 = x^3 + 7).
 *
 * Only fixed-base multiplication (k*G) is needed by the search engines; the
 * hot loops use affine additions with batched inversions implemented
 * directly on top of fe.h inside each engine. */
#ifndef PZ_EC_H
#define PZ_EC_H

#include "fe.h"

typedef struct { fe x, y; } ge;                  /* affine point (never infinity) */
typedef struct { fe x, y, z; bool inf; } gej;    /* Jacobian point */

void ec_init(void);                              /* builds the comb table; idempotent, call before threads start */

extern const ge EC_G;

void gej_set_ge(gej *r, const ge *a);
void gej_double(gej *r, const gej *a);
void gej_add_ge(gej *r, const gej *a, const ge *b);
void gej_add(gej *r, const gej *a, const gej *b);
bool gej_to_ge(ge *r, const gej *a);             /* false for infinity */
void ge_neg(ge *r, const ge *a);
bool ge_on_curve(const ge *p);
bool ge_equal(const ge *a, const ge *b);
/* Converts n Jacobian points to affine with one inversion; infinity points are not allowed. */
void gej_to_ge_batch(ge *out, const gej *in, size_t n);

/* r = k*G. Returns false when k == 0 mod n (point at infinity). */
bool ec_mul_g(ge *r, const u256 *k);
void ec_mul_g_j(gej *r, const u256 *k);

/* a + b in affine form (one inversion). Returns false for infinity. */
bool ge_add(ge *r, const ge *a, const ge *b);
/* a - b */
bool ge_sub(ge *r, const ge *a, const ge *b);

/* Serialization. */
void ge_to_pub33(u8 out[33], const ge *p);
/* Accepts 33-byte compressed or 65-byte uncompressed encodings. */
bool ge_from_pubkey(ge *r, const u8 *bytes, size_t len);

#endif
