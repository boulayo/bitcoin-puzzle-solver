/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ec.h - the secp256k1 curve y^2 = x^3 + 7: points in affine (pz_ge) and Jacobian (pz_gej) form, their sums, and the
 * multiplication of the generator G by a scalar.
 *
 * Affine points are never the point at infinity; a Jacobian point carries an explicit flag for it. Coordinates are lazy field
 * elements (see fe.h), so compare points with pz_ge_equal, not with memcmp. The group has prime order n and cofactor 1: every
 * point on the curve is in the group.
 *
 * Nothing here allocates or can fail except by the documented return values. pz_ec_mul_g and pz_ec_init build a table of 264 KB on
 * first use; both may be called from any number of threads at any time. The code is not constant-time (table lookups and
 * branches depend on the scalar): do not use it with secret scalars where a side channel matters. */
#ifndef PZCORE_EC_H
#define PZCORE_EC_H

#include <pzcore/fe.h>

PZ_BEGIN_DECLS

typedef struct { pz_fe x, y; } pz_ge;                  /* affine point (never infinity) */
typedef struct { pz_fe x, y, z; bool inf; } pz_gej;    /* Jacobian point: (x/z^2, y/z^3), or infinity when inf is set */

/* Builds the table of pz_ec_mul_g now (a few milliseconds). Optional: the multiplication does it on first use. Idempotent and
 * thread-safe; threads that arrive while another one is building wait for it. */
void pz_ec_init(void);

/* the generator G */
extern const pz_ge pz_ec_g;

void pz_gej_set_ge(pz_gej *r, const pz_ge *a);
void pz_gej_double(pz_gej *r, const pz_gej *a);
void pz_gej_add_ge(pz_gej *r, const pz_gej *a, const pz_ge *b);
void pz_gej_add(pz_gej *r, const pz_gej *a, const pz_gej *b);
/* Converts to affine (one inversion). Returns false for the point at infinity. */
bool pz_gej_to_ge(pz_ge *r, const pz_gej *a);
/* Converts n points to affine with a single inversion (Montgomery's trick); out holds n points and is used as scratch space.
 * Returns false if some input is the point at infinity, and then the contents of out are unspecified. n == 0 is fine. */
bool pz_gej_to_ge_batch(pz_ge *out, const pz_gej *in, size_t n);

void pz_ge_neg(pz_ge *r, const pz_ge *a);
bool pz_ge_on_curve(const pz_ge *p);
bool pz_ge_equal(const pz_ge *a, const pz_ge *b);
/* r = a + b and r = a - b in affine form (one inversion each). Return false if the result is the point at infinity. r may alias
 * a or b. */
bool pz_ge_add(pz_ge *r, const pz_ge *a, const pz_ge *b);
bool pz_ge_sub(pz_ge *r, const pz_ge *a, const pz_ge *b);

/* r = k*G for any 256-bit k (taken modulo n). Returns false when k is a multiple of n, i.e. the result is the point at infinity. */
bool pz_ec_mul_g(pz_ge *r, const pz_u256 *k);
void pz_ec_mul_g_j(pz_gej *r, const pz_u256 *k);

/* Serialization (SEC1). The compressed form is 02/03 (parity of y) and x; the uncompressed form is 04, x and y. */
void pz_ge_to_pub33(uint8_t out[33], const pz_ge *p);
void pz_ge_to_pub65(uint8_t out[65], const pz_ge *p);
/* Parses a 33-byte compressed or a 65-byte uncompressed public key. Returns false, and leaves *r untouched, for any other length
 * or prefix (hybrid 06/07 included), for coordinates that are not below the field prime, and for a point that is not on the curve. */
bool pz_ge_from_pubkey(pz_ge *r, const uint8_t *bytes, size_t len);

PZ_END_DECLS

#endif
