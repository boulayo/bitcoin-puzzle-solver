/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dpfeed.h - the distinguished points of the home, from the dp.bin of its pzsolve campaign as it grows (docs/en/NETWORK.md 6). A record of
 * dp.bin is the kind and the exact distance d of a point; the point is recomputed with libsecp256k1 (d is a secret of the home) and checked
 * against the tag pzsolve wrote. */
#ifndef PZPEER_DPFEED_H
#define PZPEER_DPFEED_H

#include "record.h"
#include <stdio.h>

#define DPBIN_HDR 42               /* "PZDP5\n", the fingerprint of the campaign, the tag of the jump table (src/kang_log.c) */
#define DPBIN_REC 32               /* u64 tagk, then d as three u64 (two's complement); little-endian, as pzsolve writes them */

typedef struct {
    int t;                         /* the type of the record, for the even-y point */
    uint8_t x[32];
    uint8_t d[32];                 /* d mod n, big-endian: R = t*Q' + d*G */
    uint64_t index;                /* the position of the record in dp.bin */
} own_point;

typedef struct {
    FILE *f;
    uint64_t next;                 /* records read so far */
    uint64_t bad;                  /* records that did not give a point with their tag (a damaged file) or of a kind the network has not */
} dpfeed;

/* Opens dp.bin and checks its header; false when it is missing or of another format. */
bool dpfeed_open(dpfeed *df, const char *path);
void dpfeed_close(dpfeed *df);
/* The points of the records written since the last call, at most max; 0 at the current end of the file (call again later: pzsolve appends).
 * A record cut at the end of the file is left for the next call. */
int dpfeed_read(const secp256k1_context *ctx, const record_net *net, dpfeed *df, own_point *out, int max);
/* The point of one record of dp.bin (the 32 bytes as in the file). False for a kind the network has not (wild on 3Q), d = 0, or a point
 * that does not match the tag or the zero bits. */
bool dpfeed_point(const secp256k1_context *ctx, const record_net *net, const uint8_t rec[DPBIN_REC], own_point *out);

#endif
