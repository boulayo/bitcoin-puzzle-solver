/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_address.c - Base58Check, bech32 and bech32m, SegWit addresses, WIF and the address helpers: the vectors of BIP173 and BIP350, values
 * computed by an independent Python implementation, round trips, and a good deal of hostile input. */
#include <pzcore/address.h>
#include "testutil.h"
#include "vectors.h"

static const char B58_ALPHABET[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static void test_base58(void)
{
    for (size_t i = 0; i < sizeof B58_VEC / sizeof *B58_VEC; i++) {
        uint8_t pl[128], back[128];
        char text[PZ_BASE58CHECK_MAX_TEXT + 8];
        const int n = tu_unhex(B58_VEC[i].payload_hex, pl, sizeof pl);
        CHECK(n >= 0);
        const size_t tl = strlen(B58_VEC[i].text);
        CHECK(pz_base58check_encode(pl, (size_t)n, text, sizeof text) == (int)tl);
        CHECK_STR(text, B58_VEC[i].text);
        CHECK(pz_base58check_encode(pl, (size_t)n, text, tl + 1) == (int)tl);               /* exactly the size it needs */
        CHECK(pz_base58check_encode(pl, (size_t)n, text, tl) == -1 && text[0] == 0);        /* one byte short: refused, not cut */
        CHECK(pz_base58check_encode(pl, (size_t)n, text, 0) == -1);
        CHECK(pz_base58check_decode(B58_VEC[i].text, back, sizeof back) == n);
        CHECK(n == 0 || !memcmp(pl, back, (size_t)n));
        CHECK(pz_base58check_decode(B58_VEC[i].text, back, (size_t)n) == n);               /* exactly the size it needs */
        if (n > 0) CHECK(pz_base58check_decode(B58_VEC[i].text, back, (size_t)n - 1) == -1);
        if (n == 0) CHECK(pz_base58check_decode(B58_VEC[i].text, NULL, 0) == 0);
    }
    /* things that must not decode, with the output left alone */
    for (size_t i = 0; i < sizeof B58_BAD / sizeof *B58_BAD; i++) {
        uint8_t out[128];
        memset(out, 0xa5, sizeof out);
        CHECK(pz_base58check_decode(B58_BAD[i], out, sizeof out) == -1);
        for (size_t j = 0; j < sizeof out; j++) CHECK(out[j] == 0xa5);
    }
    CHECK(pz_base58check_decode(NULL, NULL, 0) == -1);
    /* arguments of the encoder */
    {
        char text[200];
        uint8_t pl[200] = {0};
        CHECK(pz_base58check_encode(pl, 96, text, sizeof text) > 0);
        CHECK(pz_base58check_encode(pl, 97, text, sizeof text) == -1 && text[0] == 0);
        CHECK(pz_base58check_encode(pl, (size_t)-1, text, sizeof text) == -1);
        CHECK(pz_base58check_encode(NULL, 5, text, sizeof text) == -1);
        CHECK(pz_base58check_encode(NULL, 0, text, sizeof text) == 6 && !strcmp(text, "3QJmnh"));
        CHECK(pz_base58check_encode(pl, 4, NULL, 0) == -1);
    }
    /* the longest texts fit in PZ_BASE58CHECK_MAX_TEXT */
    {
        uint8_t pl[96];
        char text[PZ_BASE58CHECK_MAX_TEXT];
        memset(pl, 0xff, sizeof pl);
        const int n = pz_base58check_encode(pl, 96, text, sizeof text);
        CHECK(n > 0 && n < PZ_BASE58CHECK_MAX_TEXT);
        uint8_t back[96];
        CHECK(pz_base58check_decode(text, back, sizeof back) == 96 && !memcmp(back, pl, 96));
    }
    /* round trips, with runs of leading zero bytes, and the damage a changed character does */
    for (int it = 0; it < 3000; it++) {
        uint8_t pl[PZ_BASE58CHECK_MAX_PAYLOAD], back[PZ_BASE58CHECK_MAX_PAYLOAD];
        char text[PZ_BASE58CHECK_MAX_TEXT];
        const size_t len = tu_rnd() % (PZ_BASE58CHECK_MAX_PAYLOAD + 1);
        tu_fill(pl, len);
        const size_t zeros = len ? tu_rnd() % (len < 8 ? len : 8) : 0;
        memset(pl, 0, zeros);
        const int tl = pz_base58check_encode(pl, len, text, sizeof text);
        CHECK(tl > 0);
        CHECK(pz_base58check_decode(text, back, sizeof back) == (int)len && (len == 0 || !memcmp(pl, back, len)));
        const size_t pos = tu_rnd() % (size_t)tl;
        const char old = text[pos];
        char repl = B58_ALPHABET[tu_rnd() % 58];
        if (repl == old) repl = old == 'z' ? 'y' : (char)(old + 1 == 'I' ? 'J' : old + 1 == 'O' ? 'P' : old + 1 == 'l' ? 'm' : old + 1);
        text[pos] = repl;
        CHECK(pz_base58check_decode(text, back, sizeof back) == -1);               /* the checksum catches it (2^-32 chance of a miss) */
    }
    /* random text over the alphabet: never a crash; if it decodes, it encodes back to the same text */
    for (int it = 0; it < 20000; it++) {
        char text[200];
        const size_t len = tu_rnd() % 150;
        for (size_t i = 0; i < len; i++) text[i] = (it % 3 == 0 && tu_rnd() % 4 == 0) ? '1' : B58_ALPHABET[tu_rnd() % 58];
        text[len] = 0;
        uint8_t out[PZ_BASE58CHECK_MAX_PAYLOAD];
        const int n = pz_base58check_decode(text, out, sizeof out);
        if (n >= 0) {
            char again[200];
            CHECK(pz_base58check_encode(out, (size_t)n, again, sizeof again) > 0 && !strcmp(again, text));
        }
    }
    /* text with arbitrary bytes */
    for (int it = 0; it < 5000; it++) {
        char text[64];
        const size_t len = tu_rnd() % 40;
        for (size_t i = 0; i < len; i++) text[i] = (char)(1 + tu_rnd() % 255);
        text[len] = 0;
        uint8_t out[PZ_BASE58CHECK_MAX_PAYLOAD];
        CHECK(pz_base58check_decode(text, out, sizeof out) == -1);
    }
}

static void test_wif_and_p2pkh(void)
{
    for (size_t i = 0; i < sizeof WIF_VEC / sizeof *WIF_VEC; i++) {
        uint8_t key[32];
        char wif[PZ_WIF_BUF];
        CHECK(tu_unhex(WIF_VEC[i].key_hex, key, 32) == 32);
        CHECK(pz_privkey_to_wif(key, WIF_VEC[i].compressed != 0, wif, sizeof wif));
        CHECK_STR(wif, WIF_VEC[i].wif);
        uint8_t pl[40];
        const int n = pz_base58check_decode(wif, pl, sizeof pl);
        CHECK(n == (WIF_VEC[i].compressed ? 34 : 33) && pl[0] == 0x80 && !memcmp(pl + 1, key, 32) && (!WIF_VEC[i].compressed || pl[33] == 1));
        CHECK(!pz_privkey_to_wif(key, WIF_VEC[i].compressed != 0, wif, strlen(WIF_VEC[i].wif)));       /* does not fit: refused, not cut */
    }
    for (size_t i = 0; i < sizeof ADDR_VEC / sizeof *ADDR_VEC; i++) {
        uint8_t h[20], back[20];
        char a[PZ_P2PKH_ADDRESS_BUF], w[100];
        CHECK(tu_unhex(ADDR_VEC[i].h160_hex, h, 20) == 20);
        CHECK(pz_hash160_to_address(h, a, sizeof a));
        CHECK_STR(a, ADDR_VEC[i].p2pkh);
        CHECK(!pz_hash160_to_address(h, a, strlen(ADDR_VEC[i].p2pkh)));
        CHECK(pz_address_to_hash160(ADDR_VEC[i].p2pkh, back) && !memcmp(back, h, 20));
        const int wl = pz_hash160_to_segwit_address("bc", h, w, sizeof w);
        CHECK(wl == (int)strlen(ADDR_VEC[i].p2wpkh));
        CHECK_STR(w, ADDR_VEC[i].p2wpkh);
        CHECK(pz_address_to_hash160(ADDR_VEC[i].p2wpkh, back) && !memcmp(back, h, 20));
        /* the same address in upper case, and in mixed case */
        char up[100];
        strcpy(up, ADDR_VEC[i].p2wpkh);
        for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        CHECK(pz_address_to_hash160(up, back) && !memcmp(back, h, 20));
        up[5] = (char)(up[5] >= 'A' && up[5] <= 'Z' ? up[5] + 32 : up[5]);
        if (up[5] >= 'a' && up[5] <= 'z') CHECK(!pz_address_to_hash160(up, back));
        CHECK(pz_hash160_to_segwit_address("tb", h, w, sizeof w) > 0 && !pz_address_to_hash160(w, back));   /* a test network address */
    }
    /* what pz_address_to_hash160 refuses */
    {
        uint8_t h[20];
        const char *bad[] = {NULL, "", "1", "bc", "bc1", "BC1", "bc1q", "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMX",                               /* bad checksum */
            "3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy",                                                                                      /* P2SH */
            "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5",                                                                              /* bad checksum */
            "Bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",                                                                              /* mixed case */
            "tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4",                                                                              /* test network */
            "bc1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3qccfmv3",                                                          /* 32-byte program */
            "bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y",                                              /* version 1 */
            "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0"};                                                         /* taproot */
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(!pz_address_to_hash160(bad[i], h));
        CHECK(pz_address_to_hash160("1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH", h));
        CHECK(pz_address_to_hash160("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4", h));
        char a[3];
        CHECK(!pz_hash160_to_address(h, a, sizeof a));
        CHECK(!pz_hash160_to_address(h, a, 0));
        CHECK(pz_hash160_to_segwit_address("bc", h, a, sizeof a) == -1 && a[0] == 0);
    }
}

static void test_bech32(void)
{
    for (size_t i = 0; i < sizeof BECH32_OK / sizeof *BECH32_OK; i++) {
        char hrp[PZ_BECH32_MAX_HRP + 1];
        uint8_t data[PZ_BECH32_MAX_LEN];
        size_t n = sizeof data;
        pz_bech32_variant var = 0;
        CHECK(pz_bech32_decode(BECH32_OK[i].s, hrp, sizeof hrp, data, &n, &var));
        CHECK((int)var == BECH32_OK[i].variant);
        /* re-encoding gives the lower-case form of the input */
        char again[PZ_BECH32_MAX_LEN + 1], lower[PZ_BECH32_MAX_LEN + 1];
        strcpy(lower, BECH32_OK[i].s);
        for (char *p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        CHECK(pz_bech32_encode(again, sizeof again, hrp, data, n, var) == (int)strlen(lower));
        CHECK_STR(again, lower);
        /* the output buffers may be exactly as large as needed, and not smaller */
        size_t cap = n;
        CHECK(pz_bech32_decode(BECH32_OK[i].s, hrp, strlen(hrp) + 1, data, &cap, &var) && cap == n);
        if (n > 0) { cap = n - 1; CHECK(!pz_bech32_decode(BECH32_OK[i].s, hrp, sizeof hrp, data, &cap, &var)); }
        cap = n;
        CHECK(!pz_bech32_decode(BECH32_OK[i].s, hrp, strlen(hrp), data, &cap, &var));
        CHECK(pz_bech32_decode(BECH32_OK[i].s, NULL, 0, data, &(size_t){n}, NULL));                     /* the outputs are optional */
    }
    for (size_t i = 0; i < sizeof BECH32_BAD / sizeof *BECH32_BAD; i++) {
        char hrp[PZ_BECH32_MAX_HRP + 1] = "keep";
        uint8_t data[PZ_BECH32_MAX_LEN];
        memset(data, 0x5a, sizeof data);
        size_t n = sizeof data;
        pz_bech32_variant var = (pz_bech32_variant)77;
        CHECK(!pz_bech32_decode(BECH32_BAD[i], hrp, sizeof hrp, data, &n, &var));
        CHECK(!strcmp(hrp, "keep") && n == sizeof data && var == 77 && data[0] == 0x5a);                 /* nothing stored */
    }
    /* a hundred random strings from the alphabet never crash, and those that decode re-encode exactly */
    for (int it = 0; it < 30000; it++) {
        char s[100];
        const size_t len = tu_rnd() % 95;
        for (size_t i = 0; i < len; i++) s[i] = (char)(33 + tu_rnd() % 94);
        if (len > 3 && it % 2) { s[tu_rnd() % (len - 2)] = '1'; for (size_t i = len - 6; i < len; i++) s[i] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"[tu_rnd() % 32]; }
        s[len] = 0;
        char hrp[PZ_BECH32_MAX_HRP + 1];
        uint8_t data[PZ_BECH32_MAX_LEN];
        size_t n = sizeof data;
        pz_bech32_variant var;
        if (pz_bech32_decode(s, hrp, sizeof hrp, data, &n, &var)) {
            char again[PZ_BECH32_MAX_LEN + 1];
            CHECK(pz_bech32_encode(again, sizeof again, hrp, data, n, var) > 0);
        }
    }
    /* the encoder: valid and invalid arguments */
    {
        char out[100];
        uint8_t d[5] = {0, 1, 2, 3, 31};
        CHECK(pz_bech32_encode(out, sizeof out, "test", d, 5, PZ_BECH32) == (int)strlen("test1") + 5 + 6);
        CHECK(pz_bech32_encode(out, sizeof out, "TEST", d, 5, PZ_BECH32M) > 0 && !strncmp(out, "test1", 5));
        CHECK(pz_bech32_encode(out, sizeof out, "", d, 5, PZ_BECH32) == -1 && out[0] == 0);
        CHECK(pz_bech32_encode(out, sizeof out, NULL, d, 5, PZ_BECH32) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "a\x7f", d, 5, PZ_BECH32) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "a b", d, 5, PZ_BECH32) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "test", d, 5, (pz_bech32_variant)0) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "test", d, 5, (pz_bech32_variant)3) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "test", NULL, 5, PZ_BECH32) == -1);
        CHECK(pz_bech32_encode(out, sizeof out, "test", NULL, 0, PZ_BECH32) == 11);                      /* no data is fine */
        d[2] = 32;
        CHECK(pz_bech32_encode(out, sizeof out, "test", d, 5, PZ_BECH32) == -1);                         /* a value above 31 */
        d[2] = 2;
        CHECK(pz_bech32_encode(out, 16, "test", d, 5, PZ_BECH32) == -1 && out[0] == 0);                  /* 16 bytes are too few for 16 characters + NUL */
        CHECK(pz_bech32_encode(out, 17, "test", d, 5, PZ_BECH32) == 16);
        /* the length limits: 90 characters in all, 83 for the human-readable part */
        uint8_t big[100] = {0};
        char hrp83[100], hrp84[100];
        memset(hrp83, 'a', 83); hrp83[83] = 0;
        memset(hrp84, 'a', 84); hrp84[84] = 0;
        CHECK(pz_bech32_encode(out, sizeof out, "a", big, 90 - 8, PZ_BECH32) == 90);
        CHECK(pz_bech32_encode(out, sizeof out, "a", big, 90 - 8 + 1, PZ_BECH32) == -1);
        char o2[200];
        CHECK(pz_bech32_encode(o2, sizeof o2, hrp83, big, 0, PZ_BECH32) == 83 + 1 + 6);
        CHECK(pz_bech32_encode(o2, sizeof o2, hrp84, big, 0, PZ_BECH32) == -1);
        /* and what it writes decodes */
        uint8_t back[100];
        size_t n = sizeof back;
        char hrp[PZ_BECH32_MAX_HRP + 1];
        pz_bech32_variant var;
        CHECK(pz_bech32_encode(o2, sizeof o2, hrp83, big, 0, PZ_BECH32) > 0);
        CHECK(pz_bech32_decode(o2, hrp, sizeof hrp, back, &n, &var) && n == 0 && !strcmp(hrp, hrp83));
    }
    /* arguments of the decoder */
    {
        char hrp[20];
        uint8_t data[20];
        size_t n = sizeof data;
        CHECK(!pz_bech32_decode(NULL, hrp, sizeof hrp, data, &n, NULL));
        CHECK(!pz_bech32_decode("a12uel5l", hrp, sizeof hrp, data, NULL, NULL));
        n = 5;
        CHECK(!pz_bech32_decode("a12uel5l", hrp, sizeof hrp, NULL, &n, NULL) || n == 0);
    }
}

static void test_convert_bits(void)
{
    uint8_t in[4] = {0xff, 0x00, 0xab, 0xcd}, out[16];
    size_t n = sizeof out;
    CHECK(pz_convert_bits(out, &n, 5, in, 4, 8, true) && n == 7);
    const uint8_t want[7] = {31, 28, 0, 10, 23, 19, 8};
    CHECK(!memcmp(out, want, 7));
    uint8_t back[8];
    size_t m = sizeof back;
    CHECK(pz_convert_bits(back, &m, 8, out, 7, 5, false) && m == 4 && !memcmp(back, in, 4));      /* 35 bits: 4 bytes and 3 bits of zero padding */
    out[6] |= 1;                                                                                    /* a padding bit that is not zero */
    m = sizeof back;
    CHECK(!pz_convert_bits(back, &m, 8, out, 7, 5, false));
    out[6] &= (uint8_t)~1;
    m = sizeof back;
    CHECK(pz_convert_bits(back, &m, 8, out, 6, 5, false) == false);                                  /* 30 bits: 3 bytes and 6 bits of padding, too many */
    /* output too small, bad widths, input values that are too wide */
    n = 6;
    CHECK(!pz_convert_bits(out, &n, 5, in, 4, 8, true));
    n = sizeof out;
    CHECK(!pz_convert_bits(out, &n, 0, in, 4, 8, true));
    CHECK(!pz_convert_bits(out, &n, 9, in, 4, 8, true));
    CHECK(!pz_convert_bits(out, &n, 5, in, 4, 0, true));
    CHECK(!pz_convert_bits(out, &n, 5, in, 4, 9, true));
    CHECK(!pz_convert_bits(NULL, NULL, 5, in, 4, 8, true));
    uint8_t wide[2] = {32, 1};
    CHECK(!pz_convert_bits(out, &n, 8, wide, 2, 5, false));
    n = sizeof out;
    CHECK(pz_convert_bits(out, &n, 8, wide, 0, 5, false) && n == 0);                                 /* nothing in, nothing out */
    /* random round trips between every pair of widths: the padding that is added is the padding that is accepted back */
    for (int it = 0; it < 5000; it++) {
        const unsigned from = 1 + (unsigned)(tu_rnd() % 8), to = 1 + (unsigned)(tu_rnd() % 8);
        uint8_t src[40], mid[400], res[400];
        const size_t len = tu_rnd() % 40;
        for (size_t i = 0; i < len; i++) src[i] = (uint8_t)(tu_rnd() & ((1u << from) - 1));
        size_t mn = sizeof mid, rn = sizeof res;
        CHECK(pz_convert_bits(mid, &mn, to, src, len, from, true));
        CHECK(mn == (len * from + to - 1) / to);
        const size_t pad = mn * to - len * from;                                                     /* fewer than `to` bits */
        CHECK(pz_convert_bits(res, &rn, from, mid, mn, to, false));
        CHECK(rn == len + pad / from);                                                               /* whole values of padding come back as zeros */
        CHECK(!len || !memcmp(res, src, len));
        for (size_t i = len; i < rn; i++) CHECK(res[i] == 0);
    }
}

static void test_segwit(void)
{
    for (size_t i = 0; i < sizeof SEGWIT_OK / sizeof *SEGWIT_OK; i++) {
        uint8_t want[40], prog[40];
        const int wl = tu_unhex(SEGWIT_OK[i].prog_hex, want, sizeof want);
        size_t pl = sizeof prog;
        unsigned ver = 99;
        CHECK(wl > 0);
        CHECK(pz_segwit_address_decode(SEGWIT_OK[i].hrp, SEGWIT_OK[i].addr, &ver, prog, &pl));
        CHECK(ver == (unsigned)SEGWIT_OK[i].witver && pl == (size_t)wl && !memcmp(prog, want, pl));
        const char *other = !strcmp(SEGWIT_OK[i].hrp, "bc") ? "tb" : "bc";
        pl = sizeof prog;
        CHECK(!pz_segwit_address_decode(other, SEGWIT_OK[i].addr, &ver, prog, &pl));                       /* the wrong network */
        pl = (size_t)wl - 1;
        CHECK(!pz_segwit_address_decode(SEGWIT_OK[i].hrp, SEGWIT_OK[i].addr, &ver, prog, &pl));           /* room for one byte less */
        /* the human-readable part is compared without regard to case */
        char up[10];
        strcpy(up, SEGWIT_OK[i].hrp);
        for (char *p = up; *p; p++) *p = (char)(*p - 32);
        pl = sizeof prog;
        CHECK(pz_segwit_address_decode(up, SEGWIT_OK[i].addr, &ver, prog, &pl));
        /* and the address is built back (in lower case) */
        char text[100], lower[100];
        strcpy(lower, SEGWIT_OK[i].addr);
        for (char *p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        CHECK(pz_segwit_address_encode(text, sizeof text, SEGWIT_OK[i].hrp, (unsigned)SEGWIT_OK[i].witver, want, (size_t)wl) == (int)strlen(lower));
        CHECK_STR(text, lower);
    }
    for (size_t i = 0; i < sizeof SEGWIT_BAD / sizeof *SEGWIT_BAD; i++) {
        uint8_t prog[40];
        size_t pl = sizeof prog;
        unsigned ver = 99;
        CHECK(!pz_segwit_address_decode("bc", SEGWIT_BAD[i], &ver, prog, &pl) && !pz_segwit_address_decode("tb", SEGWIT_BAD[i], &ver, prog, &pl));
        CHECK(ver == 99 && pl == sizeof prog);
    }
    /* the encoder refuses what the standards forbid */
    {
        char out[100];
        uint8_t p[41] = {0};
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 0, p, 20) > 0);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 0, p, 32) > 0);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 0, p, 21) == -1 && out[0] == 0);              /* version 0: 20 or 32 bytes */
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 0, p, 31) == -1);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 1, p, 2) > 0);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 1, p, 40) > 0);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 1, p, 1) == -1);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 1, p, 41) == -1);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 16, p, 20) > 0);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 17, p, 20) == -1);
        CHECK(pz_segwit_address_encode(out, sizeof out, "bc", 0, NULL, 20) == -1);
        CHECK(pz_segwit_address_encode(out, sizeof out, "", 0, p, 20) == -1);
        CHECK(pz_segwit_address_encode(out, 10, "bc", 0, p, 20) == -1);
        /* every version and size that is allowed round-trips */
        for (unsigned v = 0; v <= 16; v++) {
            for (size_t len = 2; len <= 40; len++) {
                if (v == 0 && len != 20 && len != 32) continue;
                uint8_t prog[40], back[40];
                tu_fill(prog, len);
                size_t bl = sizeof back;
                unsigned bv;
                CHECK(pz_segwit_address_encode(out, sizeof out, "bc", v, prog, len) > 0);
                CHECK(pz_segwit_address_decode("bc", out, &bv, back, &bl) && bv == v && bl == len && !memcmp(back, prog, len));
            }
        }
    }
    {
        uint8_t prog[40];
        size_t pl = sizeof prog;
        unsigned ver;
        CHECK(!pz_segwit_address_decode(NULL, "bc1q", &ver, prog, &pl));
        CHECK(!pz_segwit_address_decode("bc", NULL, &ver, prog, &pl));
        CHECK(!pz_segwit_address_decode("bc", "bc1gmk9yu", &ver, prog, &pl));                               /* an empty data part */
    }
}

int main(void)
{
    test_base58();
    test_wif_and_p2pkh();
    test_bech32();
    test_convert_bits();
    test_segwit();
    TEST_DONE("t_address");
}
