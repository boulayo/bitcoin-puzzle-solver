/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_core.c - known-answer tests for field/curve/hash primitives, vectors from tools/ref.py */
#include "../src/ec.h"
#include "../src/hash.h"
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
    ec_init();
    /* SHA-256 / RIPEMD-160 KATs */
    u8 d[32], r[20];
    char hx[80];
    sha256((const u8 *)"abc", 3, d); pz_hex_encode(d, 32, hx);
    CHECK(!strcmp(hx, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ripemd160((const u8 *)"abc", 3, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc"));
    ripemd160((const u8 *)"", 0, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "9c1185a5c5e9fc54612808977ee8f548b2258d31"));
    u8 big[200];
    for (int i = 0; i < 200; i++) big[i] = (u8)i;
    sha256(big, 200, d); pz_hex_encode(d, 32, hx);
    CHECK(!strcmp(hx, "1901da1c9f699b48f6b2636e65cbf73abf99d0441ef67f5c540a42f7051dec6f"));
    ripemd160(big, 200, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "c315823ea8fe07a2dd18de4e545255afe3af0738"));
    ripemd160(big, 100, r); pz_hex_encode(r, 20, hx);
    CHECK(!strcmp(hx, "8ae5d2e6b1f3a514257f2469b637454931844aeb"));
    /* EC: 1*G -> address, 7*G, puzzle 66 key */
    struct { const char *k, *x, *h160, *addr; } v[] = {
        {"1", "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", "751e76e8199196d454941c45d1b3a323f1433bd6", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH"},
        {"7", "5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc", "5dedfbf9ea599dd4e3ca6a80b333c472fd0b3f69", "19ZewH8Kk1PDbSNdJ97FP4EiCjTRaZMZQA"},
        {"1a838b13505b26867", "30210c23b1a047bc9bdbb13448e67deddc108946de6de639bcc75d47c0216b1b", "52e763a7ddc1aa4fa811578c491c1bc7fd570137", "18ZMbwUFLMHoZBbfpCjUJQTCMCbktshgpe"},
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        u256 k; ge p; u8 pub[33], xb[32];
        CHECK(u256_from_hex(&k, v[i].k));
        CHECK(ec_mul_g(&p, &k));
        CHECK(ge_on_curve(&p));
        fe_to_be(xb, &p.x); pz_hex_encode(xb, 32, hx);
        CHECK(!strcmp(hx, v[i].x));
        ge_to_pub33(pub, &p);
        hash160(pub, 33, r); pz_hex_encode(r, 20, hx);
        CHECK(!strcmp(hx, v[i].h160));
        char addr[64];
        hash160_to_address(r, addr, sizeof addr);
        CHECK(!strcmp(addr, v[i].addr));
        u8 h2[20];
        CHECK(address_to_hash160(v[i].addr, h2) && !memcmp(h2, r, 20));
        ge q;
        CHECK(ge_from_pubkey(&q, pub, 33) && ge_equal(&p, &q));
    }
    CHECK(!address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMX", r));   /* bad checksum */
    {   /* WIF of key 1 */
        u8 kb[32] = {0}; kb[31] = 1; char w[64];
        privkey_to_wif(kb, true, w, sizeof w);
        CHECK(!strcmp(w, "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn"));
    }
    /* field: mul/sqr/inv consistency on random values; (a*b)*inv(b) == a */
    for (int it = 0; it < 20000; it++) {
        fe a, b, c, ab, bi, t;
        for (int j = 0; j < 4; j++) { a.n[j] = rnd(); b.n[j] = rnd(); }
        if (it % 7 == 0) { a.n[3] = ~0ULL; a.n[2] = ~0ULL; }          /* near p */
        if (it % 11 == 0) { b.n[1] = b.n[2] = b.n[3] = ~0ULL; }
        fe_mul(&ab, &a, &b);
        fe_inv(&bi, &b);
        fe_mul(&c, &ab, &bi);
        CHECK(fe_equiv(&c, &a));
        fe_sqr(&t, &a); fe_mul(&c, &a, &a);
        CHECK(fe_equiv(&t, &c));
        fe_add(&t, &a, &b); fe_sub(&t, &t, &b);
        CHECK(fe_equiv(&t, &a));
        fe_neg(&t, &a); fe_add(&t, &t, &a); fe_normalize(&t);
        CHECK(fe_is_zero(&t));
    }
    /* batch inversion */
    {
        enum { N = 37 };
        fe a[N], o[N], tmp[N];
        for (int i = 0; i < N; i++) { for (int j = 0; j < 4; j++) a[i].n[j] = rnd(); o[i] = a[i]; }
        CHECK(fe_inv_batch(a, tmp, N));
        for (int i = 0; i < N; i++) { fe m; fe_mul(&m, &a[i], &o[i]); fe one; fe_set_u64(&one, 1); CHECK(fe_equiv(&m, &one)); }
    }
    /* group law: k*G + j*G == (k+j)*G */
    for (int it = 0; it < 200; it++) {
        u256 k = u256_zero(), j = u256_zero(), sum;
        for (int i = 0; i < 3; i++) { k.w[i] = rnd(); j.w[i] = rnd(); }
        ge a, b, c, e;
        CHECK(ec_mul_g(&a, &k)); CHECK(ec_mul_g(&b, &j));
        u256_addmod_n(&sum, &k, &j);
        CHECK(ec_mul_g(&c, &sum));
        CHECK(ge_add(&e, &a, &b) && ge_equal(&e, &c));
        CHECK(ge_sub(&e, &c, &b) && ge_equal(&e, &a));
        CHECK(ge_add(&e, &a, &a));
        u256 k2; u256_addmod_n(&k2, &k, &k);
        CHECK(ec_mul_g(&c, &k2) && ge_equal(&e, &c));
    }
    {   /* (n-1)*G == -G ; n*G == infinity */
        u256 nm1; u256_sub(&nm1, &SECP_N, &(u256){{1, 0, 0, 0}});
        ge a, g = EC_G, ng;
        CHECK(ec_mul_g(&a, &nm1));
        ge_neg(&ng, &g);
        CHECK(ge_equal(&a, &ng));
        CHECK(!ec_mul_g(&a, &SECP_N));
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
