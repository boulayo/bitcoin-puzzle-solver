/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_core.c - known-answer tests for field/curve/hash primitives, vectors from tools/ref.py */
#include <pzcore/ec.h>
#include <pzcore/hash.h>
#include "../src/platform.h"
#include <assert.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static u64 s = 88172645463325252ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

int main(void)
{
    pz_ec_init();
    /* SHA-256 / RIPEMD-160 KATs */
    u8 d[32], r[20];
    char hx[80];
    pz_sha256((const u8 *)"abc", 3, d); pz_hex_encode(d, 32, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    pz_ripemd160((const u8 *)"abc", 3, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"));
    pz_ripemd160((const u8 *)"", 0, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "9c1185a5c5e9fc54612808977ee8f548b2258d31"));
    u8 big[200];
    for (int i = 0; i < 200; i++) big[i] = (u8)i;
    pz_sha256(big, 200, d); pz_hex_encode(d, 32, hx);
    CHECK(!strcmp(hx, "1901da1c9f699b48f6b2636e65cbf73abf99d0441ef67f5c540a42f7051dec6f"));
    pz_ripemd160(big, 200, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "c315823ea8fe07a2dd18de4e545255afe3af0738"));
    pz_ripemd160(big, 100, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "8ae5d2e6b1f3a514257f2469b637454931844aeb"));
    /* EC: 1*G -> address, 7*G, puzzle 66 key */
    struct { const char *k, *x, *h160, *addr; } v[] = {
        {"1", "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", "751e76e8199196d454941c45d1b3a323f1433bd6", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"},
        {"7", "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc", "5dedfbf9ea599dd4e3ca6a80b333c472fd0b3f69", "19ZewH8Kk1PDbSNdJ97FP4EiCjTRaZMZQA"},
        {"1a838b13505b26867", "30210c23b1a047bc9bdbb13448e67deddc108946de6de639bcc75d47c0216b1b", "52e763a7ddc1aa4fa811578c491c1bc7fd570137", "18ZMbwUFLMHoZBbfpCjUJQTCMCbktshgpe"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        pz_u256 k; pz_ge p; u8 pub[33], xb[32];
        CHECK(pz_u256_from_hex(&k, v[i].k));
        CHECK(pz_ec_mul_g(&p, &k));
        CHECK(pz_ge_on_curve(&p));
        pz_fe_to_be(xb, &p.x); pz_hex_encode(xb, 32, hx);
        CHECK(!strcmp(hx, v[i].x));
        pz_ge_to_pub33(pub, &p);
        pz_hash160(pub, 33, r); pz_hex_encode(r, 20, hx);
        CHECK(!strcmp(hx, v[i].h160));
        char addr[64];
        pz_hash160_to_address(r, addr, sizeof addr);
        CHECK(!strcmp(addr, v[i].addr));
        u8 h2[20];
        CHECK(pz_address_to_hash160(v[i].addr, h2) && !memcmp(h2, r, 20));
        pz_ge q;
        CHECK(pz_ge_from_pubkey(&q, pub, 33) && pz_ge_equal(&p, &q));
    }
    CHECK(!pz_address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMX", r));   /* bad checksum */
    {   /* native SegWit (BIP173): the address of the public key of key 1 has the hash160 of the legacy address of key 1 */
        u8 w[20], l[20];
        CHECK(pz_address_to_hash160("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", w));
        CHECK(pz_address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", l) && !memcmp(w, l, 20));
        CHECK(pz_address_to_hash160("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", w) && !memcmp(w, l, 20));            /* all upper case */
        CHECK(!pz_address_to_hash160("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5", w));                                /* bad checksum */
        CHECK(!pz_address_to_hash160("Bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", w));                                /* mixed case */
        CHECK(!pz_address_to_hash160("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", w));                                /* testnet */
        CHECK(!pz_address_to_hash160("bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3", w));            /* P2WSH: 32 bytes */
        CHECK(!pz_address_to_hash160("bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y", w)); /* version 1 (bech32m) */
    }
    {   /* WIF of key 1 */
        u8 kb[32] = {0}; kb[31] = 1; char w[64];
        pz_privkey_to_wif(kb, true, w, sizeof w);
        CHECK(!strcmp(w, "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn"));
    }
    /* field: mul/sqr/inv consistency on random values; (a*b)*inv(b) == a */
    for (int it = 0; it < 20000; it++) {
        pz_fe a, b, c, ab, bi, t;
        for (int j = 0; j < 4; j++) { a.n[j] = rnd(); b.n[j] = rnd(); }
        if (it % 7 == 0) { a.n[3] = ~0ULL; a.n[2] = ~0ULL; }          /* near p */
        if (it % 11 == 0) { b.n[1] = b.n[2] = b.n[3] = ~0ULL; }
        pz_fe_mul(&ab, &a, &b);
        pz_fe_inv(&bi, &b);
        pz_fe_mul(&c, &ab, &bi);
        CHECK(pz_fe_equiv(&c, &a));
        pz_fe_sqr(&t, &a); pz_fe_mul(&c, &a, &a);
        CHECK(pz_fe_equiv(&t, &c));
        pz_fe_add(&t, &a, &b); pz_fe_sub(&t, &t, &b);
        CHECK(pz_fe_equiv(&t, &a));
        pz_fe_neg(&t, &a); pz_fe_add(&t, &t, &a); pz_fe_normalize(&t);
        CHECK(pz_fe_is_zero(&t));
    }
    /* batch inversion */
    {
        enum { N = 37 };
        pz_fe a[N], o[N], tmp[N];
        for (int i = 0; i < N; i++) { for (int j = 0; j < 4; j++) a[i].n[j] = rnd(); o[i] = a[i]; }
        CHECK(pz_fe_inv_batch(a, tmp, N));
        for (int i = 0; i < N; i++) { pz_fe m; pz_fe_mul(&m, &a[i], &o[i]); pz_fe one; pz_fe_set_u64(&one, 1); CHECK(pz_fe_equiv(&m, &one)); }
    }
    /* group law: k*G + j*G == (k+j)*G */
    for (int it = 0; it < 200; it++) {
        pz_u256 k = pz_u256_zero(), j = pz_u256_zero(), sum;
        for (int i = 0; i < 3; i++) { k.w[i] = rnd(); j.w[i] = rnd(); }
        pz_ge a, b, c, e;
        CHECK(pz_ec_mul_g(&a, &k)); CHECK(pz_ec_mul_g(&b, &j));
        pz_u256_addmod_n(&sum, &k, &j);
        CHECK(pz_ec_mul_g(&c, &sum));
        CHECK(pz_ge_add(&e, &a, &b) && pz_ge_equal(&e, &c));
        CHECK(pz_ge_sub(&e, &c, &b) && pz_ge_equal(&e, &a));
        CHECK(pz_ge_add(&e, &a, &a));
        pz_u256 k2; pz_u256_addmod_n(&k2, &k, &k);
        CHECK(pz_ec_mul_g(&c, &k2) && pz_ge_equal(&e, &c));
    }
    {   /* (n-1)*G == -G ; n*G == infinity */
        pz_u256 nm1; pz_u256_sub(&nm1, &pz_secp256k1_n, &(pz_u256){{1, 0, 0, 0}});
        pz_ge a, g = pz_ec_g, ng;
        CHECK(pz_ec_mul_g(&a, &nm1));
        pz_ge_neg(&ng, &g);
        CHECK(pz_ge_equal(&a, &ng));
        CHECK(!pz_ec_mul_g(&a, &pz_secp256k1_n));
    }
#ifndef _WIN32
    {   /* a file that holds a secret is created private (FOUND.txt carries the private key); an ordinary one stays readable */
        char dir[600], path[700];
        if (pz_make_temp_dir(dir, sizeof dir, "pzperm")) {
            struct stat st;
            snprintf(path, sizeof path, "%s/secret", dir);
            CHECK(pz_write_file_atomic(path, "k", 1, 0600) == 0);
            CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0);
            snprintf(path, sizeof path, "%s/ordinary", dir);
            CHECK(pz_write_file_atomic(path, "k", 1, 0644) == 0);
            CHECK(stat(path, &st) == 0 && (st.st_mode & 0600) == 0600);
            pz_remove_tree(dir);
        }
    }
#endif
    printf(fails ? "t_core: %d FAILED\n" : "t_core: OK\n", fails);
    return fails != 0;
}
