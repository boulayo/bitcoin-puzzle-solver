/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* seal.c - ECIES on secp256k1 for a 32-byte secret (see seal.h and docs/en/ALGORITHMS.md). */
#include <pzcore/seal.h>
#include <pzcore/hash.h>
#include <pzcore/address.h>

static const uint8_t kSalt[16] = {'p', 'z', 's', 'o', 'l', 'v', 'e', '-', 'f', 'o', 'u', 'n', 'd', '-', 'v', '1'};

void pz_seal_scalar_from_bytes(pz_u256 *d, const uint8_t b[32])
{
    pz_u256 v, n1;
    pz_u256_from_be(&v, b);
    pz_u256_sub(&n1, &pz_secp256k1_n, &(pz_u256){{1, 0, 0, 0}});
    if (pz_u256_cmp(&v, &n1) >= 0) pz_u256_sub(&v, &v, &n1);       /* v < 2^256 < 2 (n - 1): one subtraction reduces */
    pz_u256_add_u64(d, &v, 1);
    pz_secure_zero(&v, sizeof v);
}

bool pz_seal_key_from_password(pz_u256 *d, const uint8_t *pw, size_t pwlen, unsigned log2n, void *work, size_t worksize)
{
    if (log2n < 1 || log2n > 40) return false;
    uint8_t dk[32];
    const bool ok = pz_scrypt(pw, pwlen, kSalt, sizeof kSalt, 1ULL << log2n, PZ_SEAL_SCRYPT_R, PZ_SEAL_SCRYPT_P, dk, 32, work, worksize);
    if (ok) pz_seal_scalar_from_bytes(d, dk);
    pz_secure_zero(dk, sizeof dk);
    return ok;
}

void pz_seal_key_id(const pz_ge *q, uint8_t id[4])
{
    uint8_t pub[33], h[20];
    pz_ge_to_pub33(pub, q);
    pz_hash160(pub, 33, h);
    memcpy(id, h, 4);
}

/* The key stream (32 bytes) and the MAC key (32 bytes) of a payload, from the shared point's x coordinate. */
static void derive(uint8_t okm[64], const pz_ge *shared, const uint8_t r33[33], const uint8_t q33[33], const uint8_t *aad, size_t aadlen)
{
    uint8_t ikm[32], info[33 + 33 + PZ_SEAL_AAD_MAX];
    pz_fe_to_be(ikm, &shared->x);
    memcpy(info, r33, 33);
    memcpy(info + 33, q33, 33);
    if (aadlen) memcpy(info + 66, aad, aadlen);
    pz_hkdf_sha256(kSalt, sizeof kSalt, ikm, 32, info, 66 + aadlen, okm, 64);
    pz_secure_zero(ikm, sizeof ikm);
}

static void make_tag(uint8_t tag[16], const uint8_t mackey[32], const uint8_t r33[33], const uint8_t ct[32], const uint8_t *aad, size_t aadlen)
{
    uint8_t full[32];
    pz_hmac_sha256 c;
    pz_hmac_sha256_init(&c, mackey, 32);
    pz_hmac_sha256_update(&c, r33, 33);
    pz_hmac_sha256_update(&c, ct, 32);
    pz_hmac_sha256_update(&c, aad, aadlen);
    pz_hmac_sha256_final(&c, full);
    memcpy(tag, full, 16);
    pz_secure_zero(full, sizeof full);
}

static bool scalar_ok(const pz_u256 *k)
{
    return !pz_u256_is_zero(k) && pz_u256_cmp(k, &pz_secp256k1_n) < 0;
}

pz_seal_status pz_seal(uint8_t out[PZ_SEAL_PAYLOAD_LEN], const pz_ge *q, const uint8_t plain[32], const uint8_t *aad, size_t aadlen, const pz_u256 *r)
{
    if (!q || !plain || !r || aadlen > PZ_SEAL_AAD_MAX || (aadlen && !aad) || !scalar_ok(r) || !pz_ge_on_curve(q)) return PZ_SEAL_E_ARG;
    pz_ge rp, shared;
    if (!pz_ec_mul_g(&rp, r) || !pz_ec_mul(&shared, q, r)) return PZ_SEAL_E_ARG;
    uint8_t r33[33], q33[33], okm[64], payload[PZ_SEAL_PAYLOAD_LEN];
    pz_ge_to_pub33(r33, &rp);
    pz_ge_to_pub33(q33, q);
    derive(okm, &shared, r33, q33, aad, aadlen);
    payload[0] = PZ_SEAL_VERSION;
    pz_seal_key_id(q, payload + 1);
    memcpy(payload + 5, r33, 33);
    for (int i = 0; i < 32; i++) payload[38 + i] = (uint8_t)(plain[i] ^ okm[i]);
    make_tag(payload + 70, okm + 32, r33, payload + 38, aad, aadlen);
    memcpy(out, payload, sizeof payload);
    pz_secure_zero(okm, sizeof okm);
    pz_secure_zero(&shared, sizeof shared);
    return PZ_SEAL_OK;
}

pz_seal_status pz_seal_peek(const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], uint8_t keyid[4])
{
    if (payload[0] != PZ_SEAL_VERSION) return PZ_SEAL_E_VERSION;
    pz_ge rp;
    if (!pz_ge_from_pubkey(&rp, payload + 5, 33)) return PZ_SEAL_E_POINT;
    if (keyid) memcpy(keyid, payload + 1, 4);
    return PZ_SEAL_OK;
}

pz_seal_status pz_seal_open(uint8_t plain[32], const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], const pz_u256 *d, const uint8_t *aad, size_t aadlen)
{
    pz_seal_status st = pz_seal_peek(payload, NULL);
    if (st != PZ_SEAL_OK) return st;
    if (!d || !scalar_ok(d) || aadlen > PZ_SEAL_AAD_MAX || (aadlen && !aad)) return PZ_SEAL_E_ARG;
    pz_ge rp, q, shared;
    pz_ge_from_pubkey(&rp, payload + 5, 33);
    if (!pz_ec_mul_g(&q, d)) return PZ_SEAL_E_ARG;
    uint8_t id[4];
    pz_seal_key_id(&q, id);
    if (!pz_ct_equal(id, payload + 1, 4)) return PZ_SEAL_E_KEYID;
    if (!pz_ec_mul(&shared, &rp, d)) return PZ_SEAL_E_POINT;
    uint8_t q33[33], okm[64], tag[16];
    pz_ge_to_pub33(q33, &q);
    derive(okm, &shared, payload + 5, q33, aad, aadlen);
    make_tag(tag, okm + 32, payload + 5, payload + 38, aad, aadlen);
    if (pz_ct_equal(tag, payload + 70, 16)) {
        for (int i = 0; i < 32; i++) plain[i] = (uint8_t)(payload[38 + i] ^ okm[i]);
        st = PZ_SEAL_OK;
    } else st = PZ_SEAL_E_TAG;
    pz_secure_zero(okm, sizeof okm);
    pz_secure_zero(&shared, sizeof shared);
    return st;
}

int pz_seal_encode(const uint8_t payload[PZ_SEAL_PAYLOAD_LEN], char *out, size_t outcap)
{
    if (outcap < PZ_SEAL_TEXT_PREFIX_LEN + 1) return -1;
    memcpy(out, PZ_SEAL_TEXT_PREFIX, PZ_SEAL_TEXT_PREFIX_LEN);
    const int n = pz_base58check_encode(payload, PZ_SEAL_PAYLOAD_LEN, out + PZ_SEAL_TEXT_PREFIX_LEN, outcap - PZ_SEAL_TEXT_PREFIX_LEN);
    if (n < 0) { out[0] = 0; return -1; }
    return n + PZ_SEAL_TEXT_PREFIX_LEN;
}

pz_seal_status pz_seal_decode(uint8_t payload[PZ_SEAL_PAYLOAD_LEN], const char *token)
{
    if (!token || strncmp(token, PZ_SEAL_TEXT_PREFIX, PZ_SEAL_TEXT_PREFIX_LEN) != 0) return PZ_SEAL_E_FORMAT;
    uint8_t buf[PZ_BASE58CHECK_MAX_PAYLOAD];
    const int n = pz_base58check_decode(token + PZ_SEAL_TEXT_PREFIX_LEN, buf, sizeof buf);
    if (n != PZ_SEAL_PAYLOAD_LEN) return PZ_SEAL_E_FORMAT;
    memcpy(payload, buf, PZ_SEAL_PAYLOAD_LEN);
    return PZ_SEAL_OK;
}
