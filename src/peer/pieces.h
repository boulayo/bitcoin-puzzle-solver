/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* pieces.h - the two pieces of the key that a coincidence gives (docs/en/NETWORK.md 7.5, step 2). Two records on the same x, of different
 * owners and types: t0*x + d0 = t1*x + d1, so the key base + x is x0 + x1 with
 *     x0 = base - d0/D,  x1 = d1/D,  D = t0 - t1 (1 or 2 in absolute value), mod n,
 * party 0 being the owner with the smaller identity. Each home computes its piece from its own distance only; the public piece of either
 * home follows from public data (d_i*G = R - t_i*Q'), so each side knows in advance the point the other must show. */
#ifndef PZPEER_PIECES_H
#define PZPEER_PIECES_H

#include "record.h"

/* The pair in the order of the parties; false when the records are not a useful coincidence (another x, the same owner or the same type). */
bool pieces_order(const record *a, const record *b, record pair[2]);
/* The public piece X_who of a pair (ordered). base32: the start of the range, big-endian. */
bool pieces_public(const secp256k1_context *ctx, const record_net *net, const uint8_t base32[32], const record pair[2], int who,
                   secp256k1_pubkey *out);
/* The secret piece of party `me` from its distance d (d mod n, big-endian, of its record of the pair), checked against its public piece. */
bool pieces_secret(const secp256k1_context *ctx, const record_net *net, const uint8_t base32[32], const record pair[2], int me,
                   const uint8_t d[32], uint8_t piece[32]);
/* The id of the session of a pair: SHA256("pznet1/eid" || F || record 0 || record 1). */
void pieces_eid(const record_net *net, const record pair[2], uint8_t eid[32]);

#endif
