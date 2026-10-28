/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kara_check.c - host check of kara.h: the Karatsuba products equal the schoolbook one on random limbs and on the carry edge cases.
 *   gcc -O2 -o /tmp/kara_check tools/research/sass/kara_check.c && /tmp/kara_check */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t uint;
typedef uint64_t ulong;
#define inline static inline
#include "kara.h"

static void ref(uint t[16], const uint *a, const uint *b)
{
    memset(t, 0, 64);
    for (int i = 0; i < 8; i++) {
        ulong c = 0;
        for (int j = 0; j < 8; j++) { ulong p = (ulong)a[i] * b[j] + t[i + j] + c; t[i + j] = (uint)p; c = p >> 32; }
        t[i + 8] = (uint)c;
    }
}
static uint64_t s = 88172645463325252ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

int main(void)
{
    int bad_add = 0, bad_sub = 0;
    for (int n = 0; n < 2000000; n++) {
        uint a[8], b[8], r[16], k[16];
        for (int i = 0; i < 8; i++) { a[i] = (uint)rnd(); b[i] = (uint)rnd(); }
        switch (n % 7) {
        case 1: for (int i = 0; i < 8; i++) a[i] = b[i] = 0xffffffffu; break;
        case 2: for (int i = 0; i < 4; i++) a[i] = 0; break;
        case 3: for (int i = 4; i < 8; i++) b[i] = 0; break;
        case 4: for (int i = 0; i < 4; i++) { a[i] = 0xffffffffu; b[4 + i] = 0xffffffffu; } break;
        case 5: for (int i = 0; i < 4; i++) { a[4 + i] = a[i]; b[4 + i] = b[i]; } break;      /* a1 = a0: a zero difference */
        }
        ref(r, a, b);
        mul512_kara_add(k, a, b); bad_add += memcmp(r, k, 64) != 0;
        mul512_kara_sub(k, a, b); bad_sub += memcmp(r, k, 64) != 0;
    }
    printf("Karatsuba additive: %d errors, subtractive: %d errors out of 2000000 products\n", bad_add, bad_sub);
    return bad_add || bad_sub;
}
