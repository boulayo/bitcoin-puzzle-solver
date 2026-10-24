/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* roundtrip.c - Base58Check and bech32 in both directions, and what happens with damaged text. */
#include <pzcore/pzcore.h>
#include <stdio.h>

int main(void)
{
    /* Base58Check: a version byte and a 20-byte hash */
    uint8_t payload[21] = {0x00, 0x75, 0x1e, 0x76, 0xe8, 0x19, 0x91, 0x96, 0xd4, 0x54, 0x94, 0x1c, 0x45, 0xd1, 0xb3, 0xa3, 0x23, 0xf1, 0x43, 0x3b, 0xd6};
    char text[PZ_BASE58CHECK_MAX_TEXT];
    int n = pz_base58check_encode(payload, sizeof payload, text, sizeof text);
    printf("base58check  %s (%d characters)\n", text, n);
    uint8_t back[PZ_BASE58CHECK_MAX_PAYLOAD];
    int m = pz_base58check_decode(text, back, sizeof back);
    if (n < 0 || m != (int)sizeof payload || memcmp(back, payload, sizeof payload)) { fprintf(stderr, "base58check round trip failed\n"); return 1; }
    text[7] = text[7] == 'a' ? 'b' : 'a';                     /* one character changed: the checksum notices */
    if (pz_base58check_decode(text, back, sizeof back) != -1) { fprintf(stderr, "damaged text was accepted\n"); return 1; }

    /* bech32 and bech32m: 5-bit groups behind a human-readable part */
    uint8_t groups[6] = {1, 2, 3, 30, 31, 0};
    char bech[PZ_BECH32_MAX_LEN + 1];
    for (int variant = PZ_BECH32; variant <= PZ_BECH32M; variant++) {
        n = pz_bech32_encode(bech, sizeof bech, "demo", groups, sizeof groups, (pz_bech32_variant)variant);
        printf("%-12s %s\n", variant == PZ_BECH32 ? "bech32" : "bech32m", bech);
        char hrp[PZ_BECH32_MAX_HRP + 1];
        uint8_t data[PZ_BECH32_MAX_LEN];
        size_t cnt = sizeof data;
        pz_bech32_variant got;
        if (n < 0 || !pz_bech32_decode(bech, hrp, sizeof hrp, data, &cnt, &got) || got != (pz_bech32_variant)variant || strcmp(hrp, "demo") ||
            cnt != sizeof groups || memcmp(data, groups, cnt)) { fprintf(stderr, "bech32 round trip failed\n"); return 1; }
    }

    /* a native SegWit address: witness version 0 and a 20-byte program */
    char seg[100];
    n = pz_segwit_address_encode(seg, sizeof seg, "bc", 0, payload + 1, 20);
    printf("segwit       %s\n", seg);
    uint8_t prog[40];
    size_t plen = sizeof prog;
    unsigned ver;
    if (n < 0 || !pz_segwit_address_decode("bc", seg, &ver, prog, &plen) || ver != 0 || plen != 20 || memcmp(prog, payload + 1, 20)) {
        fprintf(stderr, "segwit round trip failed\n");
        return 1;
    }
    return 0;
}
