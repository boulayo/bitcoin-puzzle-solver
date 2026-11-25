/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* block.c - see block.h. A claim on the wire: index (8 bytes, big-endian), owner (32), n (1), n offsets (6 bytes each, big-endian), then
 * the BIP340 signature of the owner over SHA256("pznet1/block" || F || everything before it). */
#include "block.h"
#include <pzcore/hash.h>
#include <pzcore/u256.h>
#include <secp256k1_schnorrsig.h>
#include <stdlib.h>
#include <string.h>

bool block_bits_ok(unsigned bits) { return (bits >= 34 && bits <= 37) || (bits >= 42 && bits <= 45); }

static unsigned proof_bytes(unsigned bits) { return bits < 40 ? 4 : 5; }   /* bytes 8 to 11, or 8 to 12, of the hash160 */

int block_min_proofs(unsigned bits) { return block_bits_ok(bits) ? 1 << (bits - 8 * proof_bytes(bits) - 1) : 1; }

uint64_t block_count(const block_net *net)
{
    pz_u256 s, e;
    pz_u256_from_be(&s, net->start), pz_u256_from_be(&e, net->end);
    pz_u256_sub(&e, &e, &s);
    e = pz_u256_shr(&e, net->bits);
    return e.w[1] | e.w[2] | e.w[3] || e.w[0] == UINT64_MAX ? UINT64_MAX : e.w[0] + 1;
}

bool block_range(const block_net *net, uint64_t index, pz_u256 *first, pz_u256 *last)
{
    pz_u256 e, d = pz_u256_from_u64(index);
    if (!block_bits_ok(net->bits) || index >= block_count(net)) return false;
    pz_u256_from_be(first, net->start), pz_u256_from_be(&e, net->end);
    d = pz_u256_shl(&d, net->bits);
    pz_u256_add(first, first, &d);
    pz_u256_add_u64(last, first, (1ull << net->bits) - 1);
    if (pz_u256_cmp(last, &e) > 0) *last = e;
    return true;
}

void block_key(const block_net *net, uint64_t index, uint8_t key[32])
{
    uint8_t m[sizeof BLOCK_CLAIM_KEY_TAG - 1 + 32 + 8];
    memcpy(m, BLOCK_CLAIM_KEY_TAG, sizeof BLOCK_CLAIM_KEY_TAG - 1);
    memcpy(m + sizeof BLOCK_CLAIM_KEY_TAG - 1, net->fp, 32);
    for (int i = 0; i < 8; i++) m[sizeof m - 8 + i] = (uint8_t)(index >> (56 - 8 * i));
    pz_sha256(m, sizeof m, key);
}

bool block_proof_key(const secp256k1_context *ctx, const block_net *net, const uint8_t key[32])
{
    secp256k1_pubkey p;
    uint8_t pub[33], h[20];
    size_t n = sizeof pub;
    if (!secp256k1_ec_pubkey_create(ctx, &p, key)) return false;
    secp256k1_ec_pubkey_serialize(ctx, pub, &n, &p, SECP256K1_EC_COMPRESSED);
    pz_hash160(pub, sizeof pub, h);
    return !memcmp(h + 8, net->h160 + 8, proof_bytes(net->bits)) && memcmp(h, net->h160, 20) != 0;   /* never the key of the prize itself */
}

static size_t body(const block_claim *c, uint8_t *out)
{
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(c->index >> (56 - 8 * i));
    memcpy(out + 8, c->owner, 32);
    out[40] = (uint8_t)c->n;
    for (int k = 0; k < c->n; k++)
        for (int i = 0; i < 6; i++) out[41 + 6 * k + i] = (uint8_t)(c->off[k] >> (40 - 8 * i));
    return 41 + 6 * (size_t)c->n;
}

static void digest(const block_net *net, const block_claim *c, uint8_t out[32])
{
    static const char TAG[] = "pznet1/block";
    uint8_t m[sizeof TAG - 1 + 32 + BLOCK_CLAIM_MAX];
    memcpy(m, TAG, sizeof TAG - 1);
    memcpy(m + sizeof TAG - 1, net->fp, 32);
    pz_sha256(m, sizeof TAG - 1 + 32 + body(c, m + sizeof TAG - 1 + 32), out);
}

size_t block_claim_encode(const block_claim *c, uint8_t out[BLOCK_CLAIM_MAX])
{
    const size_t n = body(c, out);
    memcpy(out + n, c->sig, 64);
    return n + 64;
}

bool block_claim_decode(const uint8_t *in, size_t len, block_claim *c)
{
    memset(c, 0, sizeof *c);
    if (len < 41 + 64 || in[40] > BLOCK_PROOFS_MAX || len != 41 + 6 * (size_t)in[40] + 64) return false;
    for (int i = 0; i < 8; i++) c->index = c->index << 8 | in[i];
    memcpy(c->owner, in + 8, 32);
    c->n = in[40];
    for (int k = 0; k < c->n; k++)
        for (int i = 0; i < 6; i++) c->off[k] = c->off[k] << 8 | in[41 + 6 * k + i];
    memcpy(c->sig, in + len - 64, 64);
    return true;
}

static int by_offset(const void *a, const void *b) { return (*(const uint64_t *)a > *(const uint64_t *)b) - (*(const uint64_t *)a < *(const uint64_t *)b); }

bool block_claim_make(const secp256k1_context *ctx, const block_net *net, const secp256k1_keypair *id, uint64_t index,
                      const uint8_t (*keys)[32], size_t nkeys, block_claim *out)
{
    secp256k1_xonly_pubkey xo;
    pz_u256 first, last, k, d;
    uint8_t h[32];
    memset(out, 0, sizeof *out);
    out->index = index;
    if (!block_range(net, index, &first, &last) || !secp256k1_keypair_xonly_pub(ctx, &xo, NULL, id) ||
        !secp256k1_xonly_pubkey_serialize(ctx, out->owner, &xo))
        return false;
    for (size_t i = 0; i < nkeys && out->n < BLOCK_PROOFS_MAX; i++) {
        pz_u256_from_be(&k, keys[i]);
        if (pz_u256_cmp(&k, &first) < 0 || pz_u256_cmp(&k, &last) > 0 || !block_proof_key(ctx, net, keys[i])) continue;   /* another block, or 32 bits only */
        pz_u256_sub(&d, &k, &first);
        bool seen = false;
        for (int j = 0; j < out->n && !seen; j++) seen = out->off[j] == d.w[0];
        if (!seen) out->off[out->n++] = d.w[0];                /* a block scanned twice writes its near misses twice */
    }
    qsort(out->off, (size_t)out->n, sizeof *out->off, by_offset);
    digest(net, out, h);
    return secp256k1_schnorrsig_sign32(ctx, out->sig, h, id, NULL);
}

bool block_claim_check(const secp256k1_context *ctx, const block_net *net, const block_claim *c)
{
    secp256k1_xonly_pubkey owner;
    pz_u256 first, last, k;
    uint8_t h[32], key[32];
    if (c->n < block_min_proofs(net->bits) || c->n > BLOCK_PROOFS_MAX || !block_range(net, c->index, &first, &last) ||
        !secp256k1_xonly_pubkey_parse(ctx, &owner, c->owner))
        return false;
    digest(net, c, h);
    if (!secp256k1_schnorrsig_verify(ctx, c->sig, h, 32, &owner)) return false;
    pz_u256_sub(&k, &last, &first);
    const uint64_t span = k.w[0];
    for (int i = 0; i < c->n; i++) {
        if (c->off[i] > span || (i && c->off[i] <= c->off[i - 1])) return false;
        pz_u256_add_u64(&k, &first, c->off[i]);
        pz_u256_to_be(&k, key);
        if (!block_proof_key(ctx, net, key)) return false;
    }
    return true;
}

/* whether off is a proof key of one of the n claims */
static bool has_proof(const block_claim *c, int n, uint64_t off)
{
    for (int a = 0; a < n; a++)
        for (int i = 0; i < c[a].n; i++)
            if (c[a].off[i] == off) return true;
    return false;
}

int block_claims_keys(const block_claim *c, int n)
{
    int k = 0;
    for (int a = 0; a < n; a++)
        for (int i = 0; i < c[a].n; i++) k += !has_proof(c, a, c[a].off[i]);
    return k;
}
