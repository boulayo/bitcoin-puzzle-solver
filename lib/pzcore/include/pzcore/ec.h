/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.h - secp256k1 group operations (y^2 = x^3 + 7).
 *
 * Only fixed-base multiplication (k*G) is needed by the search engines; the
 * hot loops use affine additions with batched inversions implemented
 * directly on top of pz_fe.h inside each engine. */
#ifndef PZ_EC_H
#define PZ_EC_H

#include <pzcore/fe.h>

typedef struct { pz_fe x, y; } pz_ge;                  /* affine point (never infinity) */
typedef struct { pz_fe x, y, z; bool inf; } pz_gej;    /* Jacobian point */

void pz_ec_init(void);                              /* builds the comb table; idempotent, call before threads start */

extern const pz_ge pz_ec_g;

void pz_gej_set_ge(pz_gej *r, const pz_ge *a);
void pz_gej_double(pz_gej *r, const pz_gej *a);
void pz_gej_add_ge(pz_gej *r, const pz_gej *a, const pz_ge *b);
void pz_gej_add(pz_gej *r, const pz_gej *a, const pz_gej *b);
bool pz_gej_to_ge(pz_ge *r, const pz_gej *a);             /* false for infinity */
void pz_ge_neg(pz_ge *r, const pz_ge *a);
bool pz_ge_on_curve(const pz_ge *p);
bool pz_ge_equal(const pz_ge *a, const pz_ge *b);
/* Converts n Jacobian points to affine with one inversion; infinity points are not allowed. */
void pz_gej_to_ge_batch(pz_ge *out, const pz_gej *in, size_t n);

/* r = k*G. Returns false when k == 0 mod n (point at infinity). */
bool pz_ec_mul_g(pz_ge *r, const pz_u256 *k);
void pz_ec_mul_g_j(pz_gej *r, const pz_u256 *k);

/* a + b in affine form (one inversion). Returns false for infinity. */
bool pz_ge_add(pz_ge *r, const pz_ge *a, const pz_ge *b);
/* a - b */
bool pz_ge_sub(pz_ge *r, const pz_ge *a, const pz_ge *b);

/* Serialization. */
void pz_ge_to_pub33(u8 out[33], const pz_ge *p);
/* Accepts 33-byte compressed or 65-byte uncompressed encodings. */
bool pz_ge_from_pubkey(pz_ge *r, const u8 *bytes, size_t len);

#endif
