/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* noise.c - the handshake XX and the frames of the channel (see noise.h). The names follow the Noise specification: MixHash, MixKey,
 * EncryptAndHash, DecryptAndHash, Split. */
#include "noise.h"
#include <pzcore/aead.h>
#include <pzcore/hash.h>
#include <pzcore/seal.h>
#include <secp256k1_ecdh.h>
#include <string.h>

static const char PROTOCOL[] = "Noise_XX_secp256k1_ChaChaPoly_SHA256";   /* longer than 32 bytes: h starts as its hash */
#define ROTATE_AFTER 1000

/* the ChaChaPoly nonce of Noise: 4 zero bytes and the counter in little-endian */
static void nonce_of(uint64_t n, uint8_t out[12])
{
    memset(out, 0, 4);
    for (int i = 0; i < 8; i++) out[4 + i] = (uint8_t)(n >> (8 * i));
}

/* HKDF(ck, ikm) of Noise with two outputs: HKDF-SHA256 with ck as the salt and no info */
static void hkdf2(const uint8_t ck[32], const uint8_t *ikm, size_t ikmlen, uint8_t o1[32], uint8_t o2[32])
{
    uint8_t okm[64];
    (void)pz_hkdf_sha256(ck, 32, ikm, ikmlen, NULL, 0, okm, sizeof okm);
    memcpy(o1, okm, 32);
    memcpy(o2, okm + 32, 32);
    pz_secure_zero(okm, sizeof okm);
}

static void mix_hash(noise_hs *hs, const uint8_t *data, size_t len)
{
    uint8_t buf[32 + NOISE_HS_MAX];
    memcpy(buf, hs->h, 32);
    memcpy(buf + 32, data, len);
    pz_sha256(buf, 32 + len, hs->h);
}

static void mix_key(noise_hs *hs, const uint8_t ikm[32])
{
    hkdf2(hs->ck, ikm, 32, hs->ck, hs->k);
    hs->n = 0;
    hs->has_k = true;
}

/* the DH of a secret with a public key of the other side, mixed into the key; false for a public key that is not on the curve */
static bool mix_dh(noise_hs *hs, const uint8_t sec[32], const uint8_t pub[33])
{
    secp256k1_pubkey pk;
    uint8_t shared[32];
    if (!secp256k1_ec_pubkey_parse(hs->ctx, &pk, pub, 33) || !secp256k1_ecdh(hs->ctx, shared, &pk, sec, NULL, NULL)) return false;
    mix_key(hs, shared);
    pz_secure_zero(shared, sizeof shared);
    return true;
}

static size_t encrypt_and_hash(noise_hs *hs, const uint8_t *pt, size_t len, uint8_t *out)
{
    size_t n = len;
    if (hs->has_k) {
        uint8_t nonce[12];
        nonce_of(hs->n++, nonce);
        pz_aead_seal(hs->k, nonce, hs->h, 32, pt, len, out, out + len);
        n += NOISE_TAG;
    } else memmove(out, pt, len);
    mix_hash(hs, out, n);
    return n;
}

/* in: len bytes of ciphertext (with its tag when there is a key); the plain text into pt */
static bool decrypt_and_hash(noise_hs *hs, const uint8_t *in, size_t len, uint8_t *pt)
{
    if (hs->has_k) {
        uint8_t nonce[12];
        if (len < NOISE_TAG) return false;
        nonce_of(hs->n++, nonce);
        if (!pz_aead_open(hs->k, nonce, hs->h, 32, in, len - NOISE_TAG, in + len - NOISE_TAG, pt)) return false;
    } else memcpy(pt, in, len);
    mix_hash(hs, in, len);
    return true;
}

static bool pub_of(const secp256k1_context *ctx, const uint8_t sec[32], uint8_t pub[33])
{
    secp256k1_pubkey pk;
    size_t n = 33;
    return secp256k1_ec_pubkey_create(ctx, &pk, sec) && secp256k1_ec_pubkey_serialize(ctx, pub, &n, &pk, SECP256K1_EC_COMPRESSED);
}

bool noise_hs_init(noise_hs *hs, const secp256k1_context *ctx, bool initiator, const uint8_t *prologue, size_t plen, const uint8_t s[32],
                   const uint8_t e[32])
{
    memset(hs, 0, sizeof *hs);
    hs->ctx = ctx;
    hs->initiator = initiator;
    hs->msg = -1;
    if (plen > NOISE_HS_MAX || !pub_of(ctx, s, hs->spub) || !pub_of(ctx, e, hs->epub)) return false;
    memcpy(hs->s, s, 32);
    memcpy(hs->e, e, 32);
    pz_sha256((const uint8_t *)PROTOCOL, sizeof PROTOCOL - 1, hs->h);
    memcpy(hs->ck, hs->h, 32);
    mix_hash(hs, prologue, plen);
    hs->msg = 0;
    return true;
}

/* whether the next message is written by this side */
static bool my_turn(const noise_hs *hs) { return hs->msg >= 0 && hs->msg < 3 && (hs->msg % 2 == 0) == hs->initiator; }

size_t noise_hs_write(noise_hs *hs, const uint8_t *payload, size_t plen, uint8_t *out)
{
    if (!my_turn(hs) || plen > NOISE_PAYLOAD_MAX) return 0;
    size_t o = 0;
    bool ok = true;
    if (hs->msg < 2) {                                         /* e */
        memcpy(out, hs->epub, 33);
        mix_hash(hs, hs->epub, 33);
        o = 33;
    }
    if (hs->msg == 1) {                                        /* ee, s, es (the responder's s with the initiator's e) */
        ok = mix_dh(hs, hs->e, hs->re);
        o += encrypt_and_hash(hs, hs->spub, 33, out + o);
        ok = ok && mix_dh(hs, hs->s, hs->re);
    } else if (hs->msg == 2) {                                 /* s, se (the initiator's s with the responder's e) */
        o += encrypt_and_hash(hs, hs->spub, 33, out + o);
        ok = mix_dh(hs, hs->s, hs->re);
    }
    o += encrypt_and_hash(hs, payload, plen, out + o);
    hs->msg = ok ? hs->msg + 1 : -1;
    return ok ? o : 0;
}

bool noise_hs_read(noise_hs *hs, const uint8_t *in, size_t len, uint8_t *payload, size_t *plen)
{
    if (hs->msg < 0 || hs->msg > 2 || my_turn(hs)) return false;
    const int m = hs->msg;
    hs->msg = -1;                                              /* until the message is read whole */
    size_t o = 0;
    uint8_t rs[33];
    if (m < 2) {                                               /* e */
        if (len < 33) return false;
        memcpy(hs->re, in, 33);
        mix_hash(hs, hs->re, 33);
        o = 33;
    }
    if (m == 1) {                                              /* ee, s, es */
        if (len < o + 33 + NOISE_TAG || !mix_dh(hs, hs->e, hs->re) || !decrypt_and_hash(hs, in + o, 33 + NOISE_TAG, rs)) return false;
        o += 33 + NOISE_TAG;
        memcpy(hs->rs, rs, 33);
        if (!mix_dh(hs, hs->e, hs->rs)) return false;
    } else if (m == 2) {                                       /* s, se */
        if (len < 33 + NOISE_TAG || !decrypt_and_hash(hs, in, 33 + NOISE_TAG, rs)) return false;
        o = 33 + NOISE_TAG;
        memcpy(hs->rs, rs, 33);
        if (!mix_dh(hs, hs->e, hs->rs)) return false;
    }
    const size_t rest = len - o;
    if (rest > NOISE_PAYLOAD_MAX + (hs->has_k ? NOISE_TAG : 0) || !decrypt_and_hash(hs, in + o, rest, payload)) return false;
    *plen = hs->has_k ? rest - NOISE_TAG : rest;
    hs->msg = m + 1;
    return true;
}

bool noise_hs_split(noise_hs *hs, noise_cs *tx, noise_cs *rx, uint8_t remote_static[33], uint8_t hash[32])
{
    if (hs->msg != 3) return false;
    uint8_t k1[32], k2[32];
    static const uint8_t empty[1];
    hkdf2(hs->ck, empty, 0, k1, k2);
    noise_cs *first = hs->initiator ? tx : rx, *second = hs->initiator ? rx : tx;   /* the initiator sends with the first key */
    memcpy(first->k, k1, 32), memcpy(second->k, k2, 32);
    memcpy(first->ck, hs->ck, 32), memcpy(second->ck, hs->ck, 32);
    first->n = second->n = 0;
    memcpy(remote_static, hs->rs, 33);
    memcpy(hash, hs->h, 32);
    pz_secure_zero(k1, sizeof k1);
    pz_secure_zero(k2, sizeof k2);
    pz_secure_zero(hs, sizeof *hs);
    return true;
}

/* after ROTATE_AFTER encryptions with a key: ck, k = HKDF(ck, k), as in BOLT 8 */
static void cs_next(noise_cs *c)
{
    if (++c->n < ROTATE_AFTER) return;
    uint8_t k[32];
    memcpy(k, c->k, 32);
    hkdf2(c->ck, k, 32, c->ck, c->k);
    pz_secure_zero(k, sizeof k);
    c->n = 0;
}

static void cs_seal(noise_cs *c, const uint8_t *pt, size_t len, uint8_t *out)
{
    uint8_t nonce[12];
    nonce_of(c->n, nonce);
    pz_aead_seal(c->k, nonce, NULL, 0, pt, len, out, out + len);
    cs_next(c);
}

static bool cs_open(noise_cs *c, const uint8_t *in, size_t len, uint8_t *pt)
{
    uint8_t nonce[12];
    nonce_of(c->n, nonce);
    if (!pz_aead_open(c->k, nonce, NULL, 0, in, len, in + len, pt)) return false;
    cs_next(c);
    return true;
}

size_t noise_frame_seal(noise_cs *tx, const uint8_t *body, size_t len, uint8_t *out)
{
    if (len > NOISE_FRAME_MAX) return 0;
    const uint8_t l[2] = {(uint8_t)(len >> 8), (uint8_t)len};
    cs_seal(tx, l, 2, out);
    cs_seal(tx, body, len, out + NOISE_HEAD);
    return NOISE_HEAD + len + NOISE_TAG;
}

bool noise_frame_head(noise_cs *rx, const uint8_t head[NOISE_HEAD], size_t *len)
{
    uint8_t l[2];
    if (!cs_open(rx, head, 2, l)) return false;
    *len = (size_t)l[0] << 8 | l[1];
    return true;
}

bool noise_frame_body(noise_cs *rx, const uint8_t *in, size_t len, uint8_t *body) { return cs_open(rx, in, len, body); }
