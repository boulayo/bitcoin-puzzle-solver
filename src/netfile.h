/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* netfile.h - the network file of the shared kangaroo network (docs/en/NETWORK.md 5): its lines, its fingerprint F and the values of its
 * immutable part. pzsolve reads the immutable part (hunt --network); pzpeer also checks the signatures over the updatable part. libc and
 * pzcore only, so both programs link the same object. */
#ifndef PZ_NETFILE_H
#define PZ_NETFILE_H

#include <pzcore/u256.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETFILE_MAX_SIGS 8
#define NETFILE_UPD_MAX 4096     /* the canonical updatable lines: prizes, entry nodes, fee cap, minimum version */

typedef struct {
    /* the immutable part */
    char net[32];                /* name: lower-case letters, digits and '-' (the campaign is net-<net>) */
    int puzzle;
    uint8_t pubkey33[33];        /* compressed public key of the puzzle */
    pz_u256 start, end;          /* the range, inclusive */
    uint8_t jumpseed[32];        /* public seed of the jump table */
    char schema[16];             /* the table of species: "rete-06" */
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

#endif
