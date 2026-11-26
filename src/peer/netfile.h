/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* netfile.h - the network file of a shared network (docs/en/NETWORK.md 5): a kangaroo network (a puzzle whose public key is known) or a
 * linear one (only its address is known, docs/en/NETWORK.md 11). The lines that define the network (the puzzle, the range, the jumps and
 * the distinguished points, or the address and the blocks, the chain) have a fingerprint F, and F is the network: every key, record and message is bound
 * to it, so two nodes are on the same network exactly when their files have the same F. Nobody signs a file: whoever changes one of those
 * lines is on another network. The other lines (first contacts, Electrum servers, the checkpoint of the headers, the prize coins)
 * are settings of the node that reads them, checked where they are used. */
#ifndef PZ_NETFILE_H
#define PZ_NETFILE_H

#include "block.h"
#include <pzcore/u256.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETFILE_OPTS_MAX 4096    /* the canonical optional lines */
#define NETFILE_LINE_MAX 256     /* a line of the file */

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
    bool linear;                 /* address= and blockbits= in place of pubkey=, jumpseed=, schema= and dpbits= */
    char address[64];            /* linear: the P2PKH address of the puzzle on its chain */
    uint8_t h160[20];            /*   its hash160 */
    unsigned blockbits;          /*   a block of the search is 2^blockbits keys (block.h) */
    uint8_t fp[32];              /* F = SHA256("pznet1/file" || canonical defining lines) */
    /* the optional lines: prize, entry, electrum, checkpoint */
    char opts[NETFILE_OPTS_MAX]; /* canonical: "key=value", sorted, joined with '\n' */
    size_t opts_len;
} netfile;

/* Parses the text of a network file. False, with the reason in err, for an unknown key, a repeated or missing defining key or a value out
 * of range. */
bool netfile_parse(const char *text, netfile *nf, char *err, size_t errlen);
bool netfile_load(const char *path, netfile *nf, char *err, size_t errlen);
/* The network of puzzle n, written in the program (docs/en/NETWORK.md 5): the public key and the range from the table of the puzzles,
 * the jump seed SHA256("pznet1/jumps" || public key), the schema net-06, at least max(8, (n - 1) / 2 - 44) zero bits, the main chain.
 * A puzzle without a known public key has a linear network: its address, blocks of 2^44 keys, the main chain. Either has the coins of
 * the address of the puzzle as prize= lines (data/prizes.csv). n = 0: the first puzzle whose public key is known and whose key is not. False, with the reason, for a puzzle that is not in the table. */
bool netfile_puzzle(int n, netfile *nf, char *err, size_t errlen);
/* What the checks of the claims need of a linear network. */
void netfile_blocks(const netfile *nf, block_net *out);
/* The value of the i-th optional line "key=value" (0 the first, in the canonical order), into out; NULL when there is no such line, or
 * when it does not fit (NETFILE_LINE_MAX always fits). */
const char *netfile_opt(const netfile *nf, const char *key, int i, char *out, size_t cap);
/* The network of the file at path, or of puzzle n when path is NULL. */
bool netfile_get(const char *path, int n, netfile *nf, char *err, size_t errlen);

#endif
