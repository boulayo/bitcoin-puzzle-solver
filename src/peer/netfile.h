/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* netfile.h - the network file of the shared kangaroo network (docs/en/NETWORK.md 5). The lines that define the network (the puzzle, the
 * range, the jumps, the distinguished points, the chain) have a fingerprint F, and F is the network: every key, record and message is bound
 * to it, so two nodes are on the same network exactly when their files have the same F. Nobody signs a file: whoever changes one of those
 * lines is on another network. The other lines (first contacts, Electrum servers, the checkpoint of the headers, the prize coins)
 * are settings of the node that reads them, checked where they are used. */
#ifndef PZ_NETFILE_H
#define PZ_NETFILE_H

#include <pzcore/u256.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETFILE_OPTS_MAX 4096    /* the canonical optional lines */

typedef struct {
    /* the lines that define the network */
    char net[32];                /* name: lower-case letters, digits and '-' (the campaign is net-<net>) */
    int puzzle;
    uint8_t pubkey33[33];        /* compressed public key of the puzzle */
    pz_u256 start, end;          /* the range, inclusive */
    uint8_t jumpseed[32];        /* public seed of the jump table */
    char schema[16];             /* the table of species: "net-06" */
    unsigned dpbits;
    char chain[8];               /* main, test, signet or regtest */
    uint8_t fp[32];              /* F = SHA256("pznet1/file" || canonical defining lines) */
    /* the optional lines: prize, entry, electrum, checkpoint */
    char opts[NETFILE_OPTS_MAX]; /* canonical: "key=value", sorted, joined with '\n' */
    size_t opts_len;
} netfile;

/* Parses the text of a network file. False, with the reason in err, for an unknown key, a repeated or missing defining key or a value out
 * of range. */
bool netfile_parse(const char *text, netfile *nf, char *err, size_t errlen);
bool netfile_load(const char *path, netfile *nf, char *err, size_t errlen);

#endif
