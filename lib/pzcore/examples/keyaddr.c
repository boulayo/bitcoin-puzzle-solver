/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* keyaddr.c - from a private key (hex, default 1) to the compressed public key, its HASH160 and the addresses and WIF that go with it.
 *   usage: keyaddr [private-key-hex] */
#include <pzcore/pzcore.h>
#include <stdio.h>

static void hex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

int main(int argc, char **argv)
{
    pz_u256 k;
    if (!pz_u256_from_hex(&k, argc > 1 ? argv[1] : "1")) { fprintf(stderr, "not a hexadecimal number of at most 256 bits\n"); return 2; }
    pz_ge point;
    if (!pz_ec_mul_g(&point, &k)) { fprintf(stderr, "the key is 0 or a multiple of the group order\n"); return 2; }

    uint8_t pub[33], h160[20], keybytes[32];
    pz_ge_to_pub33(pub, &point);
    pz_hash160(pub, sizeof pub, h160);
    pz_u256_to_be(&k, keybytes);

    char p2pkh[PZ_P2PKH_ADDRESS_BUF], p2wpkh[100], wif[PZ_WIF_BUF];
    if (!pz_hash160_to_address(h160, p2pkh, sizeof p2pkh) || pz_hash160_to_segwit_address("bc", h160, p2wpkh, sizeof p2wpkh) < 0 ||
        !pz_privkey_to_wif(keybytes, true, wif, sizeof wif)) {
        fprintf(stderr, "cannot build the text forms\n");
        return 1;
    }
    printf("public key  "); hex(pub, sizeof pub); printf("\n");
    printf("hash160     "); hex(h160, sizeof h160); printf("\n");
    printf("p2pkh       %s\n", p2pkh);
    printf("p2wpkh      %s\n", p2wpkh);
    printf("wif         %s\n", wif);

    /* the other direction: both addresses lead back to the same hash */
    uint8_t back1[20], back2[20];
    if (!pz_address_to_hash160(p2pkh, back1) || !pz_address_to_hash160(p2wpkh, back2) || memcmp(back1, h160, 20) || memcmp(back2, h160, 20)) {
        fprintf(stderr, "round trip failed\n");
        return 1;
    }
    /* the well-known first key */
    if (argc <= 1 && strcmp(p2pkh, "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH") != 0) { fprintf(stderr, "unexpected address for key 1\n"); return 1; }
    return 0;
}
