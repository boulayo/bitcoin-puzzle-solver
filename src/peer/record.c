/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* record.c - the record of a distinguished point and its proof of possession (see record.h). */
#include "record.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <secp256k1_schnorrsig.h>
#include <string.h>

bool record_net_init(const secp256k1_context *ctx, record_net *net, const uint8_t fp[32], unsigned dpbits, const uint8_t pubkey33[33],
                     const uint8_t base32[32])
{
    secp256k1_pubkey p, bg;
    const secp256k1_pubkey *both[2] = {&p, &bg};
    memcpy(net->fp, fp, 32);
    net->dpbits = dpbits;
    return secp256k1_ec_pubkey_parse(ctx, &p, pubkey33, 33) && secp256k1_ec_pubkey_create(ctx, &bg, base32) &&
           secp256k1_ec_pubkey_negate(ctx, &bg) && secp256k1_ec_pubkey_combine(ctx, &net->qp, both, 2);
}

void record_encode(const record *r, uint8_t out[RECORD_LEN])
{
    out[0] = RECORD_VERSION;
    out[1] = (uint8_t)(int8_t)r->t;
    memcpy(out + 2, r->x, 32);
    memcpy(out + 34, r->owner, 32);
    for (int b = 0; b < 8; b++) out[66 + b] = (uint8_t)(r->seq >> (56 - 8 * b));
    memcpy(out + RECORD_BODY, r->sig, 64);
}

bool record_decode(const uint8_t in[RECORD_LEN], record *r)
{
    const int t = (int8_t)in[1];
    if (in[0] != RECORD_VERSION || t < -1 || t > 1) return false;
    r->t = t;
    memcpy(r->x, in + 2, 32);
    memcpy(r->owner, in + 34, 32);
    r->seq = 0;
    for (int b = 0; b < 8; b++) r->seq = r->seq << 8 | in[66 + b];
    memcpy(r->sig, in + RECORD_BODY, 64);
    return true;
}

bool record_dp_ok(const uint8_t x[32], unsigned dpbits)
{
    for (unsigned b = 64; b < 64 + dpbits; b++)
        if (x[31 - b / 8] >> (b % 8) & 1) return false;
    return true;
}

/* SHA256(tag || F || data) */
static void tagged(const char *tag, const uint8_t fp[32], const uint8_t *data, size_t len, uint8_t out[32])
{
    uint8_t m[24 + 32 + RECORD_LEN + 64];
    const size_t tl = strlen(tag);
    memcpy(m, tag, tl);
    memcpy(m + tl, fp, 32);
    memcpy(m + tl + 32, data, len);
    pz_sha256(m, tl + 32 + len, out);
}

static void digest(const record_net *net, const record *r, uint8_t out[32])
{
    uint8_t enc[RECORD_LEN];
    record_encode(r, enc);
    tagged("pznet1/record", net->fp, enc, RECORD_BODY, out);
}

bool record_sign(const secp256k1_context *ctx, const record_net *net, const secp256k1_keypair *id, record *r)
{
    uint8_t h[32];
    digest(net, r, h);
    return secp256k1_schnorrsig_sign32(ctx, r->sig, h, id, NULL);   /* BIP340 nonces are derived from the key and the message */
}

bool record_check(const secp256k1_context *ctx, const record_net *net, const record *r)
{
    secp256k1_xonly_pubkey xr, owner;
    uint8_t h[32];
    if (!record_dp_ok(r->x, net->dpbits) || !secp256k1_xonly_pubkey_parse(ctx, &xr, r->x) || !secp256k1_xonly_pubkey_parse(ctx, &owner, r->owner))
        return false;
    digest(net, r, h);
    return secp256k1_schnorrsig_verify(ctx, r->sig, h, 32, &owner);
}

/* R - t*Q', the point whose scalar the owner knows; false at infinity (d = 0) or for an x off the curve */
static bool point_of_d(const secp256k1_context *ctx, const record_net *net, const record *r, secp256k1_pubkey *out)
{
    uint8_t c[33] = {2};
    memcpy(c + 1, r->x, 32);
    if (!secp256k1_ec_pubkey_parse(ctx, out, c, 33)) return false;
    if (r->t == 0) return true;
    secp256k1_pubkey q = net->qp, rr = *out;
    if (r->t > 0 && !secp256k1_ec_pubkey_negate(ctx, &q)) return false;
    const secp256k1_pubkey *both[2] = {&rr, &q};
    return secp256k1_ec_pubkey_combine(ctx, out, both, 2);
}

static void possess_msg(const record_net *net, const record *r, const uint8_t asker[32], const uint8_t nonce[32], uint8_t out[32])
{
    uint8_t m[RECORD_LEN + 64];
    record_encode(r, m);
    memcpy(m + RECORD_LEN, asker, 32);
    memcpy(m + RECORD_LEN + 32, nonce, 32);
    tagged("pznet1/possess", net->fp, m, sizeof m, out);
}

bool record_prove(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t d32[32], const uint8_t asker[32],
                  const uint8_t nonce[32], uint8_t sig[64])
{
    secp256k1_pubkey want;
    secp256k1_keypair kp;
    secp256k1_xonly_pubkey xw, xd;
    uint8_t h[32];
    bool ok = point_of_d(ctx, net, r, &want) && secp256k1_keypair_create(ctx, &kp, d32) && secp256k1_keypair_xonly_pub(ctx, &xd, NULL, &kp) &&
              secp256k1_xonly_pubkey_from_pubkey(ctx, &xw, NULL, &want) && secp256k1_xonly_pubkey_cmp(ctx, &xw, &xd) == 0;   /* the right d */
    if (ok) {
        possess_msg(net, r, asker, nonce, h);
        ok = secp256k1_schnorrsig_sign32(ctx, sig, h, &kp, NULL);
    }
    pz_secure_zero(&kp, sizeof kp);
    return ok;
}

bool record_proof_ok(const secp256k1_context *ctx, const record_net *net, const record *r, const uint8_t asker[32], const uint8_t nonce[32],
                     const uint8_t sig[64])
{
    secp256k1_pubkey p;
    secp256k1_xonly_pubkey x;
    uint8_t h[32];
    if (!point_of_d(ctx, net, r, &p) || !secp256k1_xonly_pubkey_from_pubkey(ctx, &x, NULL, &p)) return false;
    possess_msg(net, r, asker, nonce, h);
    return secp256k1_schnorrsig_verify(ctx, sig, h, 32, &x);
}
