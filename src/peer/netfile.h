/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* netfile.h - the network file of the shared kangaroo network (docs/en/NETWORK.md 5): its lines, its fingerprint F, the values of its
 * immutable part, and the signatures of the maintainers: BIP340 signatures over SHA256("pznet1/sig" || F || canonical updatable lines). A
 * file is accepted with valid signatures of a threshold of distinct trusted keys: those compiled into pzpeer, or, for a test network, those
 * of PZNET_TRUST (x-only keys in hex, separated by commas) with the threshold of PZNET_THRESHOLD (1 when it is not set). */
#ifndef PZ_NETFILE_H
#define PZ_NETFILE_H

#include <pzcore/u256.h>
#include <secp256k1.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETFILE_MAX_SIGS 8
#define NETFILE_UPD_MAX 4096     /* the canonical updatable lines: prizes, entry nodes, fee cap, minimum version, Electrum servers, checkpoint */

typedef struct {
    /* the immutable part */
    char net[32];                /* name: lower-case letters, digits and '-' (the campaign is net-<net>) */
    int puzzle;
    uint8_t pubkey33[33];        /* compressed public key of the puzzle */
    pz_u256 start, end;          /* the range, inclusive */
    uint8_t jumpseed[32];        /* public seed of the jump table */
    char schema[16];             /* the table of species: "net-06" */
    unsigned dpbits;
    char chain[8];               /* main, test, signet or regtest */
    uint8_t fp[32];              /* F = SHA256("pznet1/file" || canonical immutable lines) */
    /* the updatable part and its signatures (checked by pzpeer, with libsecp256k1) */
    char upd[NETFILE_UPD_MAX];   /* canonical updatable lines: sorted, joined with '\n' */
    size_t upd_len;
    int nsig;
    uint8_t sig_key[NETFILE_MAX_SIGS][32], sig[NETFILE_MAX_SIGS][64];
} netfile;

/* Parses the text of a network file. False, with the reason in err, for an unknown or repeated key, a missing key of the immutable part or
 * a value out of range. */
bool netfile_parse(const char *text, netfile *nf, char *err, size_t errlen);
bool netfile_load(const char *path, netfile *nf, char *err, size_t errlen);
/* SHA256("pznet1/sig" || F || the canonical updatable lines): the message the maintainers sign. */
void netfile_sig_msg(const netfile *nf, uint8_t out[32]);

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
