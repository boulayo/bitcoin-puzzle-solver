/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_gpufe.c - the GPU field arithmetic (kernels/pz_common.cl: the PTX carry-chain variant, with the carry flag emulated exactly as
 * the PTX ISA defines it; the OpenCL variant on add-with-carry, with the C equivalent of the builtins; the OpenCL variant in 64-bit C)
 * against the CPU implementation: random values and every combination of the limb patterns that trigger the rare carry/borrow paths
 * (values near 0, near p, near 2^256). */
#include "../src/common.h"
#include "../src/envopts.h"
#include <pzcore/fe.h>
#include <stdio.h>

typedef unsigned int uint;
#define GPUFE_DECL(p)                                                                                              \
    void p##add(uint *r, const uint *a, const uint *b);                                                           \
    void p##sub(uint *r, const uint *a, const uint *b);                                                           \
    void p##subn(uint *r, const uint *a, const uint *b);                                                          \
    void p##addn(uint *r, const uint *a, const uint *b);                                                          \
    void p##sub2(uint *r, const uint *a, const uint *b, const uint *c);                                           \
    void p##mul(uint *r, const uint *a, const uint *b);                                                           \
    void p##sqr(uint *r, const uint *a);                                                                          \
    void p##norm(uint *r, const uint *a);                                                                         \
    void p##cneg(uint *r, const uint *a, uint c);                                                                 \
    void p##inv(uint *r, const uint *a);                                                                          \
    void p##invpow(uint *r, const uint *a);
GPUFE_DECL(gp_)   /* OpenCL, 64-bit C */
GPUFE_DECL(gc_)   /* OpenCL, add-with-carry */
GPUFE_DECL(gf_)   /* PZ_FAST_FE (PTX chains, emulated) */

typedef struct {
    const char *name;
    void (*add)(uint *, const uint *, const uint *), (*sub)(uint *, const uint *, const uint *);
    void (*subn)(uint *, const uint *, const uint *), (*addn)(uint *, const uint *, const uint *);
    void (*sub2)(uint *, const uint *, const uint *, const uint *), (*mul)(uint *, const uint *, const uint *);
    void (*sqr)(uint *, const uint *), (*norm)(uint *, const uint *), (*cneg)(uint *, const uint *, uint);
    void (*inv)(uint *, const uint *), (*invpow)(uint *, const uint *);
} gpufe;
#define GPUFE(p, n) {n, p##add, p##sub, p##subn, p##addn, p##sub2, p##mul, p##sqr, p##norm, p##cneg, p##inv, p##invpow}
static const gpufe V[] = {GPUFE(gp_, "portable"), GPUFE(gc_, "carry"), GPUFE(gf_, "PTX")};
#define NV ((int)(sizeof V / sizeof *V))

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
        printf("FAIL %s (%s): a=", op, V[variant].name);
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
    for (int v = 0; v < NV; v++) {
        V[v].add(r, a, b); pz_fe_add(&w, &A, &B); expect("add", v, r, &w, a, b, false);
        V[v].sub(r, a, b); pz_fe_sub(&w, &A, &B); expect("sub", v, r, &w, a, b, false);
        V[v].sub(r, b, a); pz_fe_sub(&w, &B, &A); expect("sub", v, r, &w, b, a, false);
        /* a - b - b and b - a - a: up to two borrows, and the corrections after them */
        V[v].sub2(r, a, b, b); pz_fe_sub(&w, &A, &B); pz_fe_sub(&w, &w, &B); expect("sub2", v, r, &w, a, b, false);
        V[v].sub2(r, b, a, a); pz_fe_sub(&w, &B, &A); pz_fe_sub(&w, &w, &A); expect("sub2", v, r, &w, b, a, false);
        V[v].mul(r, a, b); pz_fe_mul(&w, &A, &B); expect("mul", v, r, &w, a, b, false);
        V[v].sqr(r, a); pz_fe_sqr(&w, &A); expect("sqr", v, r, &w, a, NULL, false);
        V[v].norm(r, a); w = A; expect("normalize", v, r, &w, a, NULL, true);
        /* p - a of a canonical, non-zero a */
        uint n[8], nb[8];
        V[v].norm(n, a);
        V[v].norm(nb, b);
        /* a - b and b - a with the subtrahend normalized, a + b of two normalized values */
        {
            pz_fe Nb;
            from32(&Nb, nb);
            V[v].subn(r, a, nb); pz_fe_sub(&w, &A, &Nb); expect("sub_norm", v, r, &w, a, nb, false);
            V[v].subn(r, b, n); pz_fe_sub(&w, &B, &A); expect("sub_norm", v, r, &w, b, n, false);
            V[v].addn(r, n, nb); pz_fe_add(&w, &A, &B); expect("add_norm", v, r, &w, n, nb, false);
        }
        if (n[0] | n[1] | n[2] | n[3] | n[4] | n[5] | n[6] | n[7]) {
            pz_fe N, z;
            from32(&N, n);
            memset(&z, 0, sizeof z);
            V[v].cneg(r, n, 1); pz_fe_sub(&w, &z, &N); expect("cneg", v, r, &w, n, NULL, true);
            V[v].cneg(r, n, 0); expect("cneg", v, r, &N, n, NULL, true);
        }
    }
}

int main(void)
{
    pz_env_init();
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
            for (int v = 0; v < NV; v++) {
                V[v].inv(r, a);
                expect("inv", v, r, &w, a, NULL, true);
                V[v].invpow(r, a);
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
                for (int v = 0; v < NV; v++) { V[v].inv(r, a); expect("inv", v, r, &w, a, NULL, true); }
            }
        }
        n += ninv;
    }
    if (fails) printf("t_gpufe: %d FAILED\n", fails);
    else printf("t_gpufe: OK (%ld combinations, portable, carry and PTX variants)\n", n);
    return fails != 0;
}
