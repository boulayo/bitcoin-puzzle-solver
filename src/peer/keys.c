/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* keys.c - the keys of a home (see keys.h). */
#include "keys.h"
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <string.h>

/* HKDF(salt "pznet1", seed, use || F || counter (4 bytes, big-endian, when has_n)) */
static void derive(const uint8_t seed[32], const char *use, const uint8_t fp[32], bool has_n, uint32_t n, uint8_t out[32])
{
    static const uint8_t SALT[] = "pznet1";
    uint8_t info[16 + 32 + 4];
    const size_t ul = strlen(use);
    memcpy(info, use, ul);
    memcpy(info + ul, fp, 32);
    size_t len = ul + 32;
    if (has_n) {
        for (int b = 0; b < 4; b++) info[len + (size_t)b] = (uint8_t)(n >> (24 - 8 * b));
        len += 4;
    }
    (void)pz_hkdf_sha256(SALT, sizeof SALT - 1, seed, 32, info, len, out, 32);
}

void keys_node_id(const uint8_t xonly[32], uint8_t id[32])
{
    uint8_t m[9 + 32] = "pznet1/id";
    memcpy(m + 9, xonly, 32);
    pz_sha256(m, sizeof m, id);
}

bool keys_id_ok(const uint8_t id[32])
{
    return id[0] == 0 && id[1] == 0;   /* NODE_ID_ZERO_BITS = 16 */
}

bool keys_identity(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], home_identity *out)
{
    memset(out, 0, sizeof *out);
    for (uint32_t j = 0; j < (1u << 26); j++) {                 /* 2^26 tries without an ID: impossible but for a broken hash */
        secp256k1_xonly_pubkey xo;
        derive(seed, "identity", fp, true, j, out->sec);
        if (!secp256k1_keypair_create(ctx, &out->kp, out->sec) || !secp256k1_keypair_xonly_pub(ctx, &xo, NULL, &out->kp) ||
            !secp256k1_xonly_pubkey_serialize(ctx, out->xonly, &xo))
            continue;
        keys_node_id(out->xonly, out->id);
        if (!keys_id_ok(out->id)) continue;
        secp256k1_pubkey pk;
        size_t n = 33;
        if (!secp256k1_keypair_pub(ctx, &pk, &out->kp) || !secp256k1_ec_pubkey_serialize(ctx, out->pub33, &n, &pk, SECP256K1_EC_COMPRESSED)) break;
        out->j = j;
        return true;
    }
    pz_secure_zero(out, sizeof *out);
    return false;
}

void keys_starts(const uint8_t seed[32], const uint8_t fp[32], uint8_t out[32]) { derive(seed, "starts", fp, false, 0, out); }

bool keys_coin(const secp256k1_context *ctx, const uint8_t seed[32], const uint8_t fp[32], uint32_t n, uint8_t out[32])
{
    derive(seed, "coin", fp, true, n, out);
    return secp256k1_ec_seckey_verify(ctx, out);
}
