/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* trust.h - the signatures of the maintainers on a network file (docs/en/NETWORK.md 5): BIP340 signatures over
 * SHA256("pznet1/sig" || F || canonical updatable lines). A file is accepted with valid signatures of a threshold of distinct trusted keys.
 * The trusted keys are those compiled into pzpeer, or, for a test network, those of PZNET_TRUST (x-only keys in hex, separated by commas)
 * with the threshold of PZNET_THRESHOLD (1 when it is not set). */
#ifndef PZPEER_TRUST_H
#define PZPEER_TRUST_H

#include "../netfile.h"
#include <secp256k1.h>

#define TRUST_MAX 16

typedef struct {
    uint8_t key[TRUST_MAX][32];
    int n, threshold;
} trust_set;

/* False, with the reason, when there are no keys or the variables are malformed. */
bool trust_load(trust_set *t, char *err, size_t errlen);
/* The distinct trusted keys with a valid signature in *valid; true when they reach the threshold. */
bool trust_check(const secp256k1_context *ctx, const trust_set *t, const netfile *nf, int *valid);
/* The line "sig=<x-only key>:<signature>" of the key sec over the file, to add to it. */
bool trust_sign(const secp256k1_context *ctx, const uint8_t sec[32], const netfile *nf, char line[200]);

#endif
