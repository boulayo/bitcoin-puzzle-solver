/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_core.c - the platform layer's file helpers. (The primitives - field, curve, hashes, addresses, permutation - are tested in
 * lib/pzcore/tests, which link nothing but the library.) */
#include "../src/platform.h"
#include <pzcore/ec.h>
#include <pzcore/hash.h>
#include <assert.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    {   /* the library is linked and gives the well-known answer for key 1 */
        pz_u256 k = pz_u256_from_u64(1);
        pz_ge p;
        uint8_t pub[33], h[20];
        char hx[41];
        CHECK(pz_ec_mul_g(&p, &k));
        pz_ge_to_pub33(pub, &p);
        pz_hash160(pub, 33, h);
        pz_hex_encode(h, 20, hx);
        CHECK(!strcmp(hx, "751e76e8199196d454941c45d1b3a323f1433bd6"));
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
