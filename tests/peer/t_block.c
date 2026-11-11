/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_block.c - the claim of a block of a shared linear search. The two keys of the test, 0x80000068b68 and 0x800003d4338, have hash160s
 * with the same bytes 8 to 12 (08 e1 d9 a1 9a: found by trying four million keys): with a target that has those bytes, both are proof
 * keys of the block that starts at 2^43. A claim is made from them, checked, sent and read back; then what must be refused. */
#include "peertest.h"
#include "block.h"
#include <pzcore/hash.h>

static void key_of(uint64_t k, uint8_t out[32])
{
    memset(out, 0, 32);
    for (int i = 0; i < 8; i++) out[31 - i] = (uint8_t)(k >> (8 * i));
}

int main(void)
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    secp256k1_keypair id, other;
    uint8_t sec[32], keys[6][32], enc[BLOCK_CLAIM_MAX];
    block_net net = {.bits = 41};
    block_claim c, d;
    memset(net.fp, 7, 32), memset(net.h160, 0x11, 20);
    memcpy(net.h160 + 8, "\x08\xe1\xd9\xa1\x9a", 5);
    key_of(1ull << 43, net.start);
    pz_sha256((const uint8_t *)"owner", 5, sec);
    CHECK(secp256k1_keypair_create(ctx, &id, sec));
    pz_sha256((const uint8_t *)"another", 7, sec);
    CHECK(secp256k1_keypair_create(ctx, &other, sec));
    CHECK(block_min_proofs(40) == 1 && block_min_proofs(43) == 2 && block_min_proofs(44) == 4 && block_min_proofs(48) == 64);

    key_of(0x800003d4338, keys[0]), key_of(0x80000068b68, keys[1]), key_of(0x80000068b68, keys[2]);   /* out of order, and one twice */
    key_of(0x80000068b69, keys[3]);                                                                  /* not a proof key */
    key_of(0x7ffffffffff, keys[4]), key_of((1ull << 43) + (1ull << 41) + 5, keys[5]);               /* before and after the block */
    CHECK(block_proof_key(ctx, &net, keys[0]) && block_proof_key(ctx, &net, keys[1]) && !block_proof_key(ctx, &net, keys[3]));
    CHECK(block_claim_make(ctx, &net, &id, 0, (const uint8_t(*)[32])keys, 6, &c));
    CHECK(c.n == 2 && c.off[0] == 0x68b68 && c.off[1] == 0x3d4338 && c.index == 0);
    CHECK(block_claim_check(ctx, &net, &c));
    const size_t len = block_claim_encode(&c, enc);
    CHECK(len == 8 + 32 + 1 + 12 + 64 && block_claim_decode(enc, len, &d) && block_claim_check(ctx, &net, &d));
    CHECK(d.n == 2 && d.off[1] == 0x3d4338 && !memcmp(d.owner, c.owner, 32) && !memcmp(d.sig, c.sig, 64));

    /* refused: any byte changed (the block, the owner, the count, an offset, the signature), a cut or a longer claim */
    for (size_t i = 0; i < len; i++) {
        uint8_t bad[BLOCK_CLAIM_MAX];
        memcpy(bad, enc, len);
        bad[i] ^= 1;
        CHECK(!block_claim_decode(bad, len, &d) || !block_claim_check(ctx, &net, &d));
    }
    CHECK(!block_claim_decode(enc, len - 1, &d) && !block_claim_decode(enc, len + 6, &d) && !block_claim_decode(enc, 40, &d));
    /* another network: the fingerprint, the target, the size of the blocks */
    block_net n2 = net;
    n2.fp[0] ^= 1;
    CHECK(!block_claim_check(ctx, &n2, &c));
    n2 = net, n2.h160[9] ^= 1;
    CHECK(!block_claim_check(ctx, &n2, &c));
    n2 = net, n2.bits = 44;                                                      /* four proof keys at least: two are too few */
    CHECK(!block_claim_check(ctx, &n2, &c) && !block_claim_make(ctx, &n2, &id, 0, (const uint8_t(*)[32])keys, 6, &d));
    n2.bits = 39;
    CHECK(!block_claim_make(ctx, &n2, &id, 0, (const uint8_t(*)[32])keys, 6, &d));
    /* the claim of another block from the same keys, and a claim with the signature of someone else */
    CHECK(!block_claim_make(ctx, &net, &id, 1, (const uint8_t(*)[32])keys, 6, &d) && !block_claim_make(ctx, &net, &id, 0, NULL, 0, &d));
    CHECK(block_claim_make(ctx, &net, &other, 0, (const uint8_t(*)[32])keys, 6, &d) && memcmp(d.owner, c.owner, 32));
    memcpy(d.owner, c.owner, 32);
    CHECK(!block_claim_check(ctx, &net, &d));
    /* offsets that are not increasing: the same proof twice is one proof */
    d = c, d.off[1] = d.off[0];
    CHECK(!block_claim_check(ctx, &net, &d));
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_block");
}
