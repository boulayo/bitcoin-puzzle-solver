/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */
/* t_block.c - the claim of a block of a shared linear search, and the file of claims. The two keys of the test, 0x80000068b68 and
 * 0x800003d4338, have hash160s with the same bytes 8 to 12 (08 e1 d9 a1 9a: found by trying four million keys): with a target that has
 * those bytes, both are proof keys of the block that starts at 2^43. A claim is made from them, checked, sent and read back; then what
 * must be refused; then the blocks of a range as large as that of puzzle 71, and what a file of claims keeps, of a few blocks and of
 * thousands. */
#include "peertest.h"
#include "block.h"
#include "claims.h"
#include "store.h"
#include <pzcore/hash.h>

static void key_of(uint64_t k, uint8_t out[32])
{
    memset(out, 0, 32);
    for (int i = 0; i < 8; i++) out[31 - i] = (uint8_t)(k >> (8 * i));
}

/* the blocks of 2^44 keys of a range of 2^70 keys, as puzzle 71's, and of a range whose last block is short */
static void test_blocks(void)
{
    block_net big = {.bits = 44};
    pz_u256 first, last, want;
    big.start[23] = 0x40;                                      /* 2^70 */
    memset(big.end + 23, 0xff, 9), big.end[23] = 0x7f;         /* 2^71 - 1 */
    CHECK(block_count(&big) == 1ull << 26);
    CHECK(block_range(&big, (1ull << 26) - 1, &first, &last) && !block_range(&big, 1ull << 26, &first, &last));
    pz_u256_from_be(&want, big.end);
    CHECK(!pz_u256_cmp(&last, &want));                         /* the last key of the last block is the end of the range */
    CHECK(block_range(&big, (1ull << 20) + 1, &first, &last)); /* the blocks past 2^20 (refused before: 63 of 64 of puzzle 71) */
    pz_u256 off = pz_u256_from_u64((1ull << 20) + 1);
    off = pz_u256_shl(&off, 44);
    pz_u256_from_be(&want, big.start), pz_u256_add(&want, &want, &off);
    CHECK(!pz_u256_cmp(&first, &want));
    block_net s = {.bits = 34};
    s.start[27] = 1;                                           /* 2^32, and 2^34 + 6 keys: one block, then one of 6 keys */
    pz_u256_from_be(&want, s.start), pz_u256_add_u64(&want, &want, (1ull << 34) + 5), pz_u256_to_be(&want, s.end);
    CHECK(block_count(&s) == 2 && block_range(&s, 1, &first, &last) && !pz_u256_cmp(&last, &want) && !block_range(&s, 2, &first, &last));
}

/* A file of claims (claims.c): the owners of a block, a copy refused, the order kept on disk and in memory, a torn slot cut. The claims
 * here are not signed: the node checks them before they come to the file. */
static void test_claims(const uint8_t fp[32])
{
    static const char PATH[] = PZ_TEST_DIR "/t_block.claims";
    char err[200];
    block_claim a = {.index = 5, .n = 2, .off = {1, 2}}, b = a, copy = a, mixed = a, low = a, got[CLAIMS_PER_BLOCK];
    memset(a.owner, 1, 32), memset(b.owner, 2, 32), memset(copy.owner, 3, 32), memset(mixed.owner, 4, 32), memset(low.owner, 5, 32);
    b.off[1] = 3, mixed.off[0] = 3, low.index = 4;
    remove(PATH);
    claims *c = claims_open(PATH, fp, err, sizeof err);
    CHECK(c && claims_put(c, &a) == CLAIM_STORED && claims_put(c, &a) == CLAIM_DUP);
    CHECK(claims_put(c, &copy) == CLAIM_COPY);                 /* the same proof keys under another identity: no scan behind it */
    CHECK(claims_put(c, &b) == CLAIM_STORED && claims_put(c, &mixed) == CLAIM_COPY);   /* b brings key 3; then 2 and 3 are both held */
    CHECK(claims_put(c, &low) == CLAIM_STORED && claims_count(c) == 3);
    CHECK(claims_get(c, 5, got, CLAIMS_PER_BLOCK) == 2 && block_claims_keys(got, 2) == 3 && claims_get(c, 4, got, 1) == 1);
    CHECK(claims_at(c, 0, &got[0]) && got[0].index == 5 && got[0].owner[0] == 1 && claims_at(c, 2, &got[0]) && got[0].index == 4 &&
          !claims_at(c, 3, &got[0]));
    for (int k = 0; k < CLAIMS_PER_BLOCK - 2; k++) {           /* the owners of a block: CLAIMS_PER_BLOCK at most */
        block_claim more = a;
        memset(more.owner, 10 + k, 32), more.off[1] = 10 + k;
        CHECK(claims_put(c, &more) == CLAIM_STORED);
    }
    b.off[1] = 99, memset(b.owner, 9, 32);
    CHECK(claims_put(c, &b) == CLAIM_FULL);
    claims_close(c);
    FILE *f = fopen(PATH, "ab");                               /* a slot written as zeros, then half a slot: a write cut by a crash */
    for (size_t i = 0; f && i < 3 * (2 + BLOCK_CLAIM_MAX) / 2; i++) fputc(0, f);
    if (f) fclose(f);
    c = claims_open(PATH, fp, err, sizeof err);
    CHECK(c && claims_count(c) == 5 && claims_get(c, 5, got, CLAIMS_PER_BLOCK) == 4 && claims_get(c, 4, got, 1) == 1);
    if (c) claims_close(c);
    f = fopen(PATH, "rb");
    if (f) fseek(f, 0, SEEK_END);
    CHECK(f && ftell(f) == STORE_HDR + 5 * (2 + BLOCK_CLAIM_MAX));
    if (f) fclose(f);
    remove(PATH);
}

/* Many blocks, 1 to CLAIMS_PER_BLOCK owners each, put in turns: each block gives all its claims, a claim again is a duplicate and one
 * owner more is refused, before and after the file is opened again (the table of the blocks grew many times), and claims_at gives them
 * in the order they came */
static void test_many_claims(const uint8_t fp[32])
{
    static const char PATH[] = PZ_TEST_DIR "/t_block.many";
    enum { BLOCKS = 5000 };
    char err[200];
    block_claim got[CLAIMS_PER_BLOCK], first, last;
    int stored = 0, put = 0;
    remove(PATH);
    claims *c = claims_open(PATH, fp, err, sizeof err);
    for (int turn = 0; c && turn < CLAIMS_PER_BLOCK; turn++)
        for (uint64_t i = 0; i < BLOCKS; i++) {
            block_claim cl = {.index = i * 0x9e3779b97f4a7c15ull >> 20, .n = 1, .off = {(uint64_t)turn + 1}};   /* a proof key of its own */
            memcpy(cl.owner, &i, sizeof i), cl.owner[31] = (uint8_t)turn;
            if (turn > (int)(i % CLAIMS_PER_BLOCK)) continue;
            stored += claims_put(c, &cl) == CLAIM_STORED, put++;
            if (put == 1) first = cl;
            last = cl;
        }
    CHECK(stored == put);
    for (int pass = 0; c && pass < 2; pass++) {
        int right = 0;
        for (uint64_t i = 0; i < BLOCKS; i++) {
            block_claim again = {.index = i * 0x9e3779b97f4a7c15ull >> 20, .n = 1, .off = {1}}, more = again;
            memcpy(again.owner, &i, sizeof i), again.owner[31] = 0;
            more.off[0] = 9, memset(more.owner, 0xee, 32);
            const int n = (int)(i % CLAIMS_PER_BLOCK) + 1;
            right += claims_get(c, again.index, got, CLAIMS_PER_BLOCK) == n && claims_put(c, &again) == CLAIM_DUP &&
                     (n < CLAIMS_PER_BLOCK || claims_put(c, &more) == CLAIM_FULL);
        }
        CHECK(right == BLOCKS && claims_count(c) == (uint64_t)put);
        CHECK(claims_at(c, 0, &got[0]) && got[0].index == first.index && !memcmp(got[0].owner, first.owner, 32));
        CHECK(claims_at(c, (uint64_t)put - 1, &got[0]) && got[0].index == last.index && !memcmp(got[0].owner, last.owner, 32));
        claims_close(c);
        c = pass ? NULL : claims_open(PATH, fp, err, sizeof err);
    }
    remove(PATH);
}

int main(void)
{
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    secp256k1_keypair id, other;
    uint8_t sec[32], keys[6][32], enc[BLOCK_CLAIM_MAX];
    block_net net = {.bits = 42};
    block_claim c, d;
    memset(net.fp, 7, 32), memset(net.h160, 0x11, 20);
    memcpy(net.h160 + 8, "\x08\xe1\xd9\xa1\x9a", 5);
    key_of(1ull << 43, net.start), key_of((1ull << 43) + (1ull << 44) - 1, net.end);   /* four blocks */
    pz_sha256((const uint8_t *)"owner", 5, sec);
    CHECK(secp256k1_keypair_create(ctx, &id, sec));
    pz_sha256((const uint8_t *)"another", 7, sec);
    CHECK(secp256k1_keypair_create(ctx, &other, sec));
    CHECK(block_min_proofs(36) == 8 && block_min_proofs(42) == 2 && block_min_proofs(43) == 4 && block_min_proofs(44) == 8 && block_min_proofs(45) == 16);
    CHECK(!block_bits_ok(33) && block_bits_ok(34) && block_bits_ok(37) && !block_bits_ok(38) && !block_bits_ok(41) && block_bits_ok(42) && block_bits_ok(45) &&
          !block_bits_ok(46) && block_min_proofs(34) == 2 && block_count(&net) == 4);

    key_of(0x800003d4338, keys[0]), key_of(0x80000068b68, keys[1]), key_of(0x80000068b68, keys[2]);   /* out of order, and one twice */
    key_of(0x80000068b69, keys[3]);                                                                  /* not a proof key */
    key_of(0x7ffffffffff, keys[4]), key_of((1ull << 43) + (1ull << 42) + 5, keys[5]);               /* before and after the block */
    CHECK(block_proof_key(ctx, &net, keys[0]) && block_proof_key(ctx, &net, keys[1]) && !block_proof_key(ctx, &net, keys[3]));
    {                                                     /* the key of the prize itself is never a proof: it is never to be published */
        block_net prize = net;
        secp256k1_pubkey P;
        uint8_t pub[33];
        size_t pl = sizeof pub;
        CHECK(secp256k1_ec_pubkey_create(ctx, &P, keys[0]) && secp256k1_ec_pubkey_serialize(ctx, pub, &pl, &P, SECP256K1_EC_COMPRESSED));
        pz_hash160(pub, sizeof pub, prize.h160);
        CHECK(!block_proof_key(ctx, &prize, keys[0]) && block_proof_key(ctx, &prize, keys[1]));
    }
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
    n2 = net, n2.bits = 44;                                    /* eight proof keys at least: two are too few, so the claim is made, not taken */
    CHECK(!block_claim_check(ctx, &n2, &c) && block_claim_make(ctx, &n2, &id, 0, (const uint8_t(*)[32])keys, 6, &d) && d.n == 2 &&
          !block_claim_check(ctx, &n2, &d));
    n2.bits = 39;
    CHECK(!block_claim_make(ctx, &n2, &id, 0, (const uint8_t(*)[32])keys, 6, &d));
    /* the claim of another block from the same keys, of none, past the range, and a claim with the signature of someone else */
    CHECK(block_claim_make(ctx, &net, &id, 1, (const uint8_t(*)[32])keys, 6, &d) && d.n == 0 && !block_claim_check(ctx, &net, &d));
    CHECK(block_claim_make(ctx, &net, &id, 0, NULL, 0, &d) && d.n == 0 && !block_claim_make(ctx, &net, &id, 4, NULL, 0, &d));
    CHECK(block_claim_make(ctx, &net, &other, 0, (const uint8_t(*)[32])keys, 6, &d) && memcmp(d.owner, c.owner, 32));
    memcpy(d.owner, c.owner, 32);
    CHECK(!block_claim_check(ctx, &net, &d));
    /* offsets that are not increasing: the same proof twice is one proof */
    d = c, d.off[1] = d.off[0];
    CHECK(!block_claim_check(ctx, &net, &d));
    d = c;
    CHECK(block_claims_keys(&c, 1) == 2 && block_claims_keys((const block_claim[]){c, d}, 2) == 2);
    test_blocks();
    test_claims(net.fp);
    test_many_claims(net.fp);
    secp256k1_context_destroy(ctx);
    TEST_DONE("t_block");
}
