/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_puzzles.c - consistency of the built-in puzzle database (independent of the generator script). */
#include "../src/puzzles.h"
#include "../src/envopts.h"
#include <pzcore/ec.h>
#include <pzcore/address.h>
#include <pzcore/hash.h>
#include "../src/campaign.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    pz_env_init();
    pz_ec_init();
    char err[200];
    CHECK(puzzles_verify(err, sizeof err) == 0);
    if (fails) printf("%s\n", err);

    CHECK(puzzle_find(0) == NULL && puzzle_find(PZ_NUM_PUZZLES + 1) == NULL && puzzle_find(-5) == NULL);
    for (int n = 1; n <= PZ_NUM_PUZZLES; n++) {
        const puzzle_info *p = puzzle_find(n);
        CHECK(p && p->n == n);
        if (!p) continue;
        /* addresses are unique */
        for (int m = n + 1; m <= PZ_NUM_PUZZLES; m++) CHECK(strcmp(p->address, puzzle_find(m)->address) != 0);
        /* the first 70 puzzles are public knowledge: solved and with a revealed public key */
        if (n <= 70) CHECK(p->privkey[0] && p->pubkey[0]);
    }
    /* known vectors from the original challenge */
    CHECK(!strcmp(puzzle_find(1)->address, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"));
    CHECK(!strcmp(puzzle_find(66)->address, "13zb1hQbWVsc2S7ZTZnP2G4undNNpdh5so"));
    CHECK(!strcmp(puzzle_find(66)->privkey, "2832ed74f2b5e35ee"));
    CHECK(!strcmp(puzzle_find(160)->address, "1NBC8uXJy1GiJ6drkiZa1WuKn51ps7EPTv"));

    /* withdrawn puzzles 161..256: addresses only, unique across both tables */
    CHECK(puzzle_retired_address(160) == NULL && puzzle_retired_address(257) == NULL && puzzle_retired_address(0) == NULL);
    for (int n = 161; n <= 256; n++) {
        const char *a = puzzle_retired_address(n);
        CHECK(a != NULL && puzzle_find(n) == NULL);
        if (!a) continue;
        u8 hr[20];
        CHECK(pz_address_to_hash160(a, hr));
        for (int m = 1; m <= PZ_NUM_PUZZLES; m++) CHECK(strcmp(a, puzzle_find(m)->address) != 0);
        for (int m = n + 1; m <= 256; m++) CHECK(strcmp(a, puzzle_retired_address(m)) != 0);
    }
    CHECK(!strcmp(puzzle_retired_address(161), "1JkqBQcC4tHcb1JfdCH6nrWYwTPGznHANh"));
    CHECK(!strcmp(puzzle_retired_address(256), "1FMcotmnqqE5M2x9DDX3VfPAPuBWArGisa"));
    /* ranges: [2^(n-1), 2^n - 1], for 256 up to the group order n - 1 */
    {
        pz_u256 a, b, one = pz_u256_from_u64(1), w;
        CHECK(puzzle_range(161, &a, &b));
        w = pz_u256_pow2(160);
        CHECK(pz_u256_cmp(&a, &w) == 0);
        CHECK(puzzle_range(256, &a, &b));
        w = pz_u256_pow2(255);
        CHECK(pz_u256_cmp(&a, &w) == 0);
        pz_u256_sub(&w, &pz_secp256k1_n, &one);
        CHECK(pz_u256_cmp(&b, &w) == 0);
        CHECK(!puzzle_range(0, &a, &b) && !puzzle_range(257, &a, &b));
    }

    /* a corrupted copy must be detected: flip one character of an address */
    puzzle_info bad = *puzzle_find(10);
    char addr[64];
    snprintf(addr, sizeof addr, "%s", bad.address);
    addr[5] = addr[5] == 'a' ? 'b' : 'a';
    u8 h[20];
    CHECK(!pz_address_to_hash160(addr, h));

    puts(fails ? "t_puzzles: FAILED" : "t_puzzles: ok");
    return fails != 0;
}
