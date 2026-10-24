/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_gpufe.c - the GPU field arithmetic (kernels/pz_common.cl, portable and PTX carry-chain variants, the latter with
 * the carry flag emulated exactly as the PTX ISA defines it) against the CPU implementation: random values and every
 * combination of the limb patterns that trigger the rare carry/borrow paths (values near 0, near p, near 2^256). */
#include "../src/fe.h"
#include <stdio.h>

typedef unsigned int uint;
void gf_add(uint *r, const uint *a, const uint *b);   /* PZ_FAST_FE (PTX chains, emulated) */
void gf_sub(uint *r, const uint *a, const uint *b);
void gf_mul(uint *r, const uint *a, const uint *b);
void gf_sqr(uint *r, const uint *a);
void gf_norm(uint *r, const uint *a);
void gf_inv(uint *r, const uint *a);
void gf_invpow(uint *r, const uint *a);
void gp_add(uint *r, const uint *a, const uint *b);   /* portable */
void gp_sub(uint *r, const uint *a, const uint *b);
void gp_mul(uint *r, const uint *a, const uint *b);
void gp_sqr(uint *r, const uint *a);
void gp_norm(uint *r, const uint *a);
void gp_inv(uint *r, const uint *a);
void gp_invpow(uint *r, const uint *a);

static int fails;
static u64 rs = 0x6A09E667F3BCC908ULL;
static u64 rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }

static void from32(pz_fe *f, const uint o[8]) { for (int i = 0; i < 4; i++) f->n[i] = (u64)o[2 * i] | (u64)o[2 * i + 1] << 32; }

/* r must be congruent to want and (lazy representation) any value < 2^256; canonical results must be < p */
static void expect(const char *op, int variant, const uint got[8], const pz_fe *want, const uint a[8], const uint b[8], bool canonical)
{
    pz_fe g, w = *want;
    from32(&g, got);
    if (canonical) {
        pz_fe n = g;
        pz_fe_normalize(&n);
        if (memcmp(&n, &g, sizeof g) != 0) goto bad;
    }
    if (pz_fe_equiv(&g, &w)) return;
bad:
    if (fails++ < 10) {
        printf("FAIL %s (%s): a=", op, variant ? "PTX" : "portable");
        for (int i = 7; i >= 0; i--) printf("%08x", a[i]);
        printf(" b=");
        for (int i = 7; i >= 0; i--) printf("%08x", b ? b[i] : 0);
        printf("\n");
    }
}

static void check(const uint a[8], const uint b[8])
{
    pz_fe A, B, w;
    from32(&A, a);
    from32(&B, b);
    uint r[8];
    void (*add[2])(uint *, const uint *, const uint *) = {gp_add, gf_add};
    void (*sub[2])(uint *, const uint *, const uint *) = {gp_sub, gf_sub};
    void (*mul[2])(uint *, const uint *, const uint *) = {gp_mul, gf_mul};
    void (*sqr[2])(uint *, const uint *) = {gp_sqr, gf_sqr};
    void (*nrm[2])(uint *, const uint *) = {gp_norm, gf_norm};
    for (int v = 0; v < 2; v++) {
        add[v](r, a, b); pz_fe_add(&w, &A, &B); expect("add", v, r, &w, a, b, false);
        sub[v](r, a, b); pz_fe_sub(&w, &A, &B); expect("sub", v, r, &w, a, b, false);
        sub[v](r, b, a); pz_fe_sub(&w, &B, &A); expect("sub", v, r, &w, b, a, false);
        mul[v](r, a, b); pz_fe_mul(&w, &A, &B); expect("mul", v, r, &w, a, b, false);
        sqr[v](r, a); pz_fe_sqr(&w, &A); expect("sqr", v, r, &w, a, NULL, false);
        nrm[v](r, a); w = A; expect("normalize", v, r, &w, a, NULL, true);
    }
}

int main(void)
{
    /* limb patterns around the carry boundaries of p = 2^256 - 2^32 - 977 */
    static const uint pat[] = {0u, 1u, 2u, 0x3D0u, 0x3D1u, 0x3D2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFC2Eu, 0xFFFFFC2Fu, 0xFFFFFC30u,
                               0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu};
    const int NP = (int)(sizeof pat / sizeof *pat);
    long n = 0;
    /* structured values: limb 0, limb 1 and the high limbs chosen independently from the patterns */
    uint vals[4096][8];
    int nv = 0;
    for (int x = 0; x < NP; x++)
        for (int y = 0; y < NP; y++)
            for (int z = 0; z < NP && nv < 4096; z++) {
                vals[nv][0] = pat[x];
                vals[nv][1] = pat[y];
                for (int k = 2; k < 8; k++) vals[nv][k] = pat[z];
                nv++;
            }
    /* every pair of values whose high limbs are close to 2^32 (where the second carry/borrow folds happen), a sample of
     * the other pairs */
    for (int i = 0; i < nv; i++)
        for (int j = 0; j < nv; j++) {
            bool big = vals[i][7] >= 0xFFFFFC2Eu && vals[j][7] >= 0xFFFFFC2Eu;
            bool small = vals[i][7] <= 2u || vals[j][7] <= 2u;
            if (big || small || j % 7 == 0) { check(vals[i], vals[j]); n++; }
        }
    /* random values, and random values with the high limbs saturated */
    for (int it = 0; it < 300000; it++) {
        uint a[8], b[8];
        for (int k = 0; k < 8; k++) { a[k] = (uint)rnd(); b[k] = (uint)rnd(); }
        if (it & 1) for (int k = 2; k < 8; k++) a[k] = 0xFFFFFFFFu;
        if (it & 2) for (int k = 2; k < 8; k++) b[k] = 0xFFFFFFFFu;
        check(a, b);
        n++;
    }
    /* inversion (divsteps, and the power a^(p-2) it replaces): random values, small ones, values around p and 2^256,
     * the limb patterns, zero. The result must be canonical (< p). */
    {
        uint a[8], r[8];
        long ninv = 0;
        for (int it = 0; it < 400000 + nv; it++) {
            if (it < nv) memcpy(a, vals[it], sizeof a);
            else {
                for (int k = 0; k < 8; k++) a[k] = (uint)rnd();
                switch (it % 8) {
                case 1: for (int k = 1; k < 8; k++) a[k] = 0; break;                                   /* small */
                case 2: for (int k = 2; k < 8; k++) a[k] = 0xFFFFFFFFu; a[1] = 0xFFFFFFFEu - (a[1] & 3); break;   /* around p, 2^256 */
                case 3: for (int k = 2; k < 8; k++) a[k] = 0; break;                                   /* 64 bits */
                case 4: a[7] &= 0x0000FFFFu; break;                                                   /* 240 bits */
                case 5: a[0] |= 1u; for (int k = 1; k < 8; k++) a[k] = (k & 1) ? 0 : 0xFFFFFFFFu; break;  /* long runs of 0 and 1 */
                default: break;
                }
            }
            pz_fe A, w;
            from32(&A, a);
            pz_fe_inv(&w, &A);
            for (int v = 0; v < 2; v++) {
                (v ? gf_inv : gp_inv)(r, a);
                expect("inv", v, r, &w, a, NULL, true);
                (v ? gf_invpow : gp_invpow)(r, a);
                expect("invpow", v, r, &w, a, NULL, false);
            }
            ninv++;
        }
        for (int z = 0; z < 4; z++) {          /* 0, 1, p - 1, p, p + 1, 2^256 - 1 */
            static const uint sp[6][8] = {{0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0},
                                          {0xFFFFFC2Eu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                          {0xFFFFFC2Fu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                          {0xFFFFFC30u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                          {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
            for (int s = 0; s < 6; s++) {
                memcpy(a, sp[s], sizeof a);
                pz_fe A, w;
                from32(&A, a);
                pz_fe_inv(&w, &A);
                for (int v = 0; v < 2; v++) { (v ? gf_inv : gp_inv)(r, a); expect("inv", v, r, &w, a, NULL, true); }
            }
        }
        n += ninv;
    }
    if (fails) printf("t_gpufe: %d FAILED\n", fails);
    else printf("t_gpufe: OK (%ld combinations, portable and PTX variants)\n", n);
    return fails != 0;
}
