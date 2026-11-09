/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* dpfeed.c - the points of dp.bin (see dpfeed.h). */
#include "dpfeed.h"
#include <pzcore/seal.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>

static const uint8_t ORDER[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                                  0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};

static uint64_t le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}

/* out = n - mag (mag < 2^192 < n), big-endian */
static void order_minus(const uint8_t mag[32], uint8_t out[32])
{
    int borrow = 0;
    for (int i = 31; i >= 0; i--) {
        const int v = ORDER[i] - mag[i] - borrow;
        out[i] = (uint8_t)v;
        borrow = v < 0;
    }
}

bool dpfeed_point(const secp256k1_context *ctx, const record_net *net, const uint8_t rec[DPBIN_REC], own_point *out)
{
    static const int COEF[3] = {0, 1, -1};                    /* the kinds of src/kangaroo_priv.h: tame, wild +, wild - */
    const uint64_t tagk = le64(rec);
    const unsigned kind = (unsigned)(tagk & 7);
    if (kind > 2) return false;
    uint64_t w[3] = {le64(rec + 8), le64(rec + 16), le64(rec + 24)};
    const bool neg = w[2] >> 63;
    if (neg) {                                                 /* |d| of the two's complement */
        unsigned carry = 1;
        for (int i = 0; i < 3; i++) {
            w[i] = ~w[i] + carry;
            carry = carry && w[i] == 0;
        }
    }
    uint8_t mag[32] = {0};
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 8; b++) mag[31 - 8 * i - b] = (uint8_t)(w[i] >> (8 * b));
    if (neg) order_minus(mag, out->d);
    else memcpy(out->d, mag, 32);
    out->t = COEF[kind];
    secp256k1_pubkey r, q = net->qp;
    uint8_t c[33];
    size_t cl = 33;
    bool ok = secp256k1_ec_pubkey_create(ctx, &r, out->d);    /* d*G, constant time: d is a secret */
    if (ok && out->t) {
        const secp256k1_pubkey dg = r;
        const secp256k1_pubkey *both[2] = {&dg, &q};
        ok = (out->t > 0 || secp256k1_ec_pubkey_negate(ctx, &q)) && secp256k1_ec_pubkey_combine(ctx, &r, both, 2);
    }
    ok = ok && secp256k1_ec_pubkey_serialize(ctx, c, &cl, &r, SECP256K1_EC_COMPRESSED);
    if (ok && c[0] == 3) {                                     /* the record names the even-y point: -R = (-t)Q' + (-d)G */
        out->t = -out->t;
        ok = secp256k1_ec_seckey_negate(ctx, out->d);
    }
    if (ok) memcpy(out->x, c + 1, 32);
    uint64_t limb2 = 0;                                        /* x.n[2] of pzsolve: bits 128 to 191 */
    for (int i = 8; i < 16; i++) limb2 = limb2 << 8 | out->x[i];
    ok = ok && (limb2 & ~7ULL) == (tagk & ~7ULL) && record_dp_ok(out->x, net->dpbits);
    pz_secure_zero(mag, sizeof mag);
    pz_secure_zero(w, sizeof w);
    if (!ok) pz_secure_zero(out, sizeof *out);
    return ok;
}

bool dpfeed_open(dpfeed *df, const char *path)
{
    memset(df, 0, sizeof *df);
    uint8_t hdr[DPBIN_HDR];
    df->f = fopen(path, "rb");
    if (!df->f) return false;
    (void)fcntl(fileno(df->f), F_SETFD, FD_CLOEXEC);           /* the distances of the home: not for the programs pzpeer starts */
    if (fread(hdr, 1, sizeof hdr, df->f) != sizeof hdr || memcmp(hdr, "PZDP5\n", 6) != 0) {
        dpfeed_close(df);
        return false;
    }
    return true;
}

void dpfeed_close(dpfeed *df)
{
    if (df->f) fclose(df->f);
    df->f = NULL;
}

int dpfeed_read(const secp256k1_context *ctx, const record_net *net, dpfeed *df, own_point *out, int max)
{
    int n = 0;
    uint8_t rec[DPBIN_REC];
    if (fseeko(df->f, (off_t)(DPBIN_HDR + df->next * DPBIN_REC), SEEK_SET) != 0) return 0;
    while (n < max && fread(rec, 1, sizeof rec, df->f) == sizeof rec) {   /* up to the end, or to a record being written */
        if (dpfeed_point(ctx, net, rec, &out[n])) out[n++].index = df->next;
        else df->bad++;
        df->next++;
    }
    clearerr(df->f);                                           /* the next call sees what pzsolve appended meanwhile */
    pz_secure_zero(rec, sizeof rec);
    return n;
}
