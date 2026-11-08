/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* pieces.c - the pieces of the key of a coincidence (see pieces.h). The secret arithmetic is that of libsecp256k1 (constant time). */
#include "pieces.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <string.h>

/* 1/D mod n for D in {-2, -1, 1, 2}, big-endian */
static void inv_d(int D, uint8_t out[32])
{
    static const uint8_t HALF[32] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa1};
    static const uint8_t ONE[32] = {[31] = 1};
    static const uint8_t MINUS_ONE[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                                          0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b, 0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x40};
    static const uint8_t MINUS_HALF[32] = {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                           0x5d, 0x57, 0x6e, 0x73, 0x57, 0xa4, 0x50, 0x1d, 0xdf, 0xe9, 0x2f, 0x46, 0x68, 0x1b, 0x20, 0xa0};
    memcpy(out, D == 1 ? ONE : D == -1 ? MINUS_ONE : D == 2 ? HALF : MINUS_HALF, 32);   /* 1/2 = (n + 1) / 2, -1/2 = (n - 1) / 2 */
}

bool pieces_order(const record *a, const record *b, record pair[2])
{
    if (memcmp(a->x, b->x, 32) != 0 || a->t == b->t) return false;
    const int c = memcmp(a->owner, b->owner, 32);
    if (c == 0) return false;
    pair[0] = c < 0 ? *a : *b;
    pair[1] = c < 0 ? *b : *a;
    return true;
}

bool pieces_public(const secp256k1_context *ctx, const record_net *net, const uint8_t base32[32], const record pair[2], int who,
                   secp256k1_pubkey *out)
{
    uint8_t inv[32];
    inv_d(pair[0].t - pair[1].t, inv);
    if (!record_dpoint(ctx, net, &pair[who], out) || !secp256k1_ec_pubkey_tweak_mul(ctx, out, inv)) return false;   /* d_who/D * G */
    if (who == 1) return true;
    secp256k1_pubkey bg, neg = *out;                           /* X0 = base*G - d0/D * G */
    const secp256k1_pubkey *both[2] = {&bg, &neg};
    return secp256k1_ec_pubkey_negate(ctx, &neg) && secp256k1_ec_pubkey_create(ctx, &bg, base32) &&
           secp256k1_ec_pubkey_combine(ctx, out, both, 2);
}

bool pieces_secret(const secp256k1_context *ctx, const record_net *net, const uint8_t base32[32], const record pair[2], int me,
                   const uint8_t d[32], uint8_t piece[32])
{
    uint8_t inv[32];
    inv_d(pair[0].t - pair[1].t, inv);
    memcpy(piece, d, 32);
    bool ok = secp256k1_ec_seckey_tweak_mul(ctx, piece, inv);                                  /* d/D */
    if (ok && me == 0) ok = secp256k1_ec_seckey_negate(ctx, piece) && secp256k1_ec_seckey_tweak_add(ctx, piece, base32);
    secp256k1_pubkey mine, want;
    ok = ok && secp256k1_ec_pubkey_create(ctx, &mine, piece) && pieces_public(ctx, net, base32, pair, me, &want) &&
         secp256k1_ec_pubkey_cmp(ctx, &mine, &want) == 0;     /* the distance is the one of the record */
    if (!ok) pz_secure_zero(piece, 32);
    return ok;
}

void pieces_eid(const record_net *net, const record pair[2], uint8_t eid[32])
{
    uint8_t m[10 + 32 + 2 * RECORD_LEN];
    memcpy(m, "pznet1/eid", 10);
    memcpy(m + 10, net->fp, 32);
    record_encode(&pair[0], m + 42);
    record_encode(&pair[1], m + 42 + RECORD_LEN);
    pz_sha256(m, sizeof m, eid);
}
