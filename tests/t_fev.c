/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_fev.c - lane-parallel field arithmetic (pz_fev.h) against the scalar implementation, with limb patterns at the bounds.
 * Compiled once per vector width (see Makefile): PZ_VBYTES is 32 or 64. */
#include "../src/fev.h"
#include "../src/cpu_engine.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); if (++fails > 20) exit(1); } } while (0)

static u64 s = 0x9E3779B97F4A7C15ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

/* value of a lane, whatever the limbs are (not necessarily canonical): sum l_i * 2^(29 i) mod p, with scalar field ops */
static void lane_value(pz_fe *out, const pz_fev *v, int lane)
{
    pz_fe base, acc, t;
    pz_fe_set_u64(&base, (u64)1 << 29);
    pz_fe_zero(&acc);
    for (int i = 8; i >= 0; i--) {
        pz_fe_mul(&acc, &acc, &base);
        pz_fe_set_u64(&t, v->l[i][lane]);
        pz_fe_add(&acc, &acc, &t);
    }
    pz_fe_normalize(&acc);
    *out = acc;
}


/* ---- crafted values: limbs from a 320-bit integer w[0..4] (little-endian words) ---- */
static void words_add_small(u64 w[5], u64 d) { u128 c = d; for (int i = 0; i < 5 && c; i++) { c += w[i]; w[i] = (u64)c; c >>= 64; } }
static void words_sub_small(u64 w[5], u64 d) { u64 b = d; for (int i = 0; i < 5 && b; i++) { u64 t = w[i]; w[i] = t - b; b = t < b; } }
static void words_add(u64 w[5], const u64 v[5]) { u128 c = 0; for (int i = 0; i < 5; i++) { c += (u128)w[i] + v[i]; w[i] = (u64)c; c >>= 64; } }
static void set_lane_words(pz_fev *v, int lane, const u64 w[5])
{
    for (int i = 0; i < 9; i++) {
        int bit = 29 * i, wi = bit / 64, sh = bit % 64;
        u64 x = w[wi] >> sh;
        if (sh > 35 && wi + 1 < 5) x |= w[wi + 1] << (64 - sh);
        v->l[i][lane] = x & PZ_FEV_MASK;
    }
}
static void crafted_value(u64 w[5], u64 sel)
{
    static const u64 P[5] = {0xFFFFFFFEFFFFFC2FULL, ~0ULL, ~0ULL, ~0ULL, 0};
    memset(w, 0, 5 * sizeof(u64));
    u64 kind = sel % 8, k = (sel / 8) % 72, d = (sel / 576) % 9;     /* k multiples of p, d in -4..4 */
    switch (kind) {
    case 0: case 1: case 2:                    /* k*p +- d */
        for (u64 i = 0; i < k; i++) words_add(w, P);
        break;
    case 3: w[4] = 0; w[3] = w[2] = w[1] = w[0] = ~0ULL; break;                          /* 2^256 - 1 */
    case 4: w[4] = 1; break;                                                             /* 2^256 */
    case 5: w[4] = k % 32; w[3] = w[2] = w[1] = ~0ULL; w[0] = 0 - 977 * (k % 32);       /* near h*2^256 + 2^256 - h*(2^32+977) */
            { u64 h = k % 32; w[1] = ~0ULL; w[0] = ~0ULL; (void)h; }
            break;
    case 6: w[4] = 31; w[3] = w[2] = w[1] = w[0] = ~0ULL; w[4] = 0x1F; break;           /* 2^261 - 1 style maximum */
    default: for (int i = 0; i < 4; i++) w[i] = 0x9E3779B97F4A7C15ULL * (sel + i + 1); w[4] = (sel >> 3) & 3; break;
    }
    if (d < 4) words_sub_small(w, 4 - d); else words_add_small(w, d - 4);
}

/* random limbs bounded by `bound`, with a fraction of lanes at the extremes */
static void fill(pz_fev *v, u64 bound, int mode)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) {
        int m = (mode + lane) % 6;
        for (int i = 0; i < 9; i++) {
            u64 x;
            switch (m) {
            case 0: x = bound; break;                        /* every limb at the bound */
            case 1: x = 0; break;
            case 2: x = i & 1 ? bound : 0; break;
            case 3: x = rnd() % (bound + 1); break;
            case 4: x = bound - (rnd() & 7); break;
            default: x = (rnd() % (bound + 1)) | 1; break;
            }
            v->l[i][lane] = x;
        }
    }
}

static void check_lanes(const pz_fev *r, const pz_fe *want, u64 bound, const char *what)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) {
        pz_fe got;
        lane_value(&got, r, lane);
        if (!pz_fe_eq(&got, &want[lane])) { printf("  %s: lane %d differs\n", what, lane); fails++; }
        for (int i = 0; i < 9; i++)
            if (r->l[i][lane] > bound) { printf("  %s: limb %d of lane %d exceeds the bound (%llu)\n", what, i, lane, (unsigned long long)r->l[i][lane]); fails++; }
    }
}

int main(void)
{
#if PZ_VBYTES == 64
    if (!cpu_has_avx512()) { printf("t_fev (avx512): CPU without AVX-512, skipped\n"); return 0; }
    const char *name = "avx512";
#else
    if (!cpu_has_avx2()) { printf("t_fev (avx2): CPU without AVX2, skipped\n"); return 0; }
    const char *name = "avx2";
#endif
    const u64 TIGHT = PZ_FEV_TIGHT, LOOSE = PZ_FEV_LOOSE;
    /* the subtraction constants really are multiples of p and dominate their subtrahends */
    {
        pz_fe base, acc, t;
        pz_fe_set_u64(&base, (u64)1 << 29);
        for (int w = 0; w < 2; w++) {
            const u64 *K = w ? pz_fev_k2 : pz_fev_k1;
            pz_fe_zero(&acc);
            for (int i = 8; i >= 0; i--) { pz_fe_mul(&acc, &acc, &base); pz_fe_set_u64(&t, K[i]); pz_fe_add(&acc, &acc, &t); }
            pz_fe_normalize(&acc);
            CHECK(pz_fe_is_zero(&acc));
            for (int i = 0; i < 9; i++) CHECK(K[i] >= (w ? 2 * LOOSE : TIGHT));
        }
    }
    for (int it = 0; it < 60000; it++) {
        pz_fev a, b, c, r;
        pz_fe va[PZ_FEV_LANES], vb[PZ_FEV_LANES], vc[PZ_FEV_LANES], want[PZ_FEV_LANES];
        const u64 ba = it % 3 == 0 ? LOOSE : TIGHT, bb = it % 5 == 0 ? LOOSE : TIGHT;
        fill(&a, ba, it);
        fill(&b, bb, it / 2 + it);
        fill(&c, TIGHT, it / 3 + 2 * it);
        for (int l = 0; l < PZ_FEV_LANES; l++) { lane_value(&va[l], &a, l); lane_value(&vb[l], &b, l); lane_value(&vc[l], &c, l); }

        pz_fev_mul(&r, &a, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_mul(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "mul");

        pz_fev_sqr(&r, &a);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sqr(&want[l], &va[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "sqr");

        pz_fev_add(&r, &a, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_add(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, 2 * LOOSE, "add");

        if (bb == TIGHT) {
            pz_fev_sub(&r, &a, &b);
            for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sub(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
            check_lanes(&r, want, TIGHT, "sub");
        }
        if (ba == TIGHT && bb == TIGHT) {
            pz_fev_sub2(&r, &a, &b, &c);
            for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe t; pz_fe_sub(&t, &va[l], &vb[l]); pz_fe_sub(&want[l], &t, &vc[l]); pz_fe_normalize(&want[l]); }
            check_lanes(&r, want, TIGHT, "sub2");
        }
        /* loose operands fed into a product: (a + b) * (a - b) */
        if (ba == TIGHT && bb == TIGHT) {
            pz_fev sum, dif;
            pz_fev_add(&sum, &a, &b);
            pz_fev_sub(&dif, &a, &b);
            pz_fev_mul(&r, &sum, &dif);
            for (int l = 0; l < PZ_FEV_LANES; l++) {
                pz_fe t1, t2;
                pz_fe_add(&t1, &va[l], &vb[l]);
                pz_fe_sub(&t2, &va[l], &vb[l]);
                pz_fe_mul(&want[l], &t1, &t2);
                pz_fe_normalize(&want[l]);
            }
            check_lanes(&r, want, TIGHT, "(a+b)(a-b)");
        }
        /* normalization is canonical and keeps the value */
        {
            r = a;
            pz_fev_normalize(&r);
            for (int l = 0; l < PZ_FEV_LANES; l++) {
                pz_fe got;
                pz_fev_get_lane(&got, &r, l);
                pz_fe w = va[l];
                pz_fe_normalize(&w);
                CHECK(pz_fe_eq(&got, &w));
                CHECK(r.l[8][l] < ((u64)1 << 24));
                for (int i = 0; i < 8; i++) CHECK(r.l[i][l] <= PZ_FEV_MASK);
            }
        }
        /* inversion */
        if (it % 50 == 0) {
            pz_fev inv, one;
            pz_fev_inv(&inv, &a);
            pz_fev_mul(&one, &inv, &a);
            for (int l = 0; l < PZ_FEV_LANES; l++) {
                if (pz_fe_is_zero(&va[l])) continue;
                pz_fe got, want1;
                lane_value(&got, &one, l);
                pz_fe_set_u64(&want1, 1);
                CHECK(pz_fe_eq(&got, &want1));
            }
        }
        /* chained use: a long dependent computation stays correct and bounded */
        {
            pz_fev x = a, y = b, t;
            pz_fev_normalize(&x); pz_fev_normalize(&y);
            pz_fe wx[PZ_FEV_LANES], wy[PZ_FEV_LANES];
            for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fev_get_lane(&wx[l], &x, l); pz_fev_get_lane(&wy[l], &y, l); }
            for (int step = 0; step < 6; step++) {
                pz_fev_mul(&t, &x, &y);
                pz_fev_sub2(&y, &t, &x, &y);
                pz_fev_sqr(&x, &t);
                for (int l = 0; l < PZ_FEV_LANES; l++) {
                    pz_fe tt, y2;
                    pz_fe_mul(&tt, &wx[l], &wy[l]);
                    pz_fe_sub(&y2, &tt, &wx[l]);
                    pz_fe_sub(&y2, &y2, &wy[l]);
                    pz_fe_sqr(&wx[l], &tt);
                    wy[l] = y2;
                    pz_fe_normalize(&wx[l]); pz_fe_normalize(&wy[l]);
                }
            }
            for (int l = 0; l < PZ_FEV_LANES; l++) {
                pz_fe gx, gy;
                lane_value(&gx, &x, l); lane_value(&gy, &y, l);
                CHECK(pz_fe_eq(&gx, &wx[l]) && pz_fe_eq(&gy, &wy[l]));
            }
        }
    }
    /* crafted values around multiples of p and powers of two must normalize to the exact canonical residue */
    for (u64 sel = 0; sel < 5000; sel++) {
        pz_fev x, r;
        pz_fe vx[PZ_FEV_LANES];
        for (int l = 0; l < PZ_FEV_LANES; l++) {
            u64 w[5];
            crafted_value(w, sel * PZ_FEV_LANES + (u64)l);
            set_lane_words(&x, l, w);
        }
        for (int l = 0; l < PZ_FEV_LANES; l++) lane_value(&vx[l], &x, l);
        r = x;
        pz_fev_normalize(&r);
        for (int l = 0; l < PZ_FEV_LANES; l++) {
            pz_fe got;
            pz_fev_get_lane(&got, &r, l);
            CHECK(pz_fe_eq(&got, &vx[l]));
            CHECK(r.l[8][l] < ((u64)1 << 24));
            for (int i = 0; i < 8; i++) CHECK(r.l[i][l] <= PZ_FEV_MASK);
        }
        /* and the same values through a product keep their residue */
        pz_fev one, pr;
        pz_fe f1;
        pz_fe_set_u64(&f1, 1);
        pz_fev_broadcast(&one, &f1);
        pz_fev_mul(&pr, &x, &one);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe g; lane_value(&g, &pr, l); CHECK(pz_fe_eq(&g, &vx[l])); }
    }
    /* lane conversion of lazy values (p, p+1, 2^256-1) and broadcast */
    {
        pz_fe lazies[4];
        lazies[0] = pz_fe_p;
        lazies[1] = pz_fe_p; lazies[1].n[0] += 1;
        memset(&lazies[2], 0xff, sizeof lazies[2]);
        pz_fe_zero(&lazies[3]);
        for (int k = 0; k < 4; k++) {
            pz_fev v;
            pz_fev_broadcast(&v, &lazies[k]);
            pz_fe want = lazies[k];
            pz_fe_normalize(&want);
            for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe got; pz_fev_get_lane(&got, &v, l); CHECK(pz_fe_eq(&got, &want)); }
        }
    }
    /* in-place calls and loose subtrahends */
    for (int it = 0; it < 4000; it++) {
        pz_fev a, b, c, r, r2;
        pz_fe va[PZ_FEV_LANES], vb[PZ_FEV_LANES], vc[PZ_FEV_LANES], want[PZ_FEV_LANES];
        fill(&a, TIGHT, it);
        fill(&b, TIGHT, it / 2 + 3 * it);
        fill(&c, TIGHT, it / 3 + 5 * it);
        for (int l = 0; l < PZ_FEV_LANES; l++) { lane_value(&va[l], &a, l); lane_value(&vb[l], &b, l); lane_value(&vc[l], &c, l); }
        r = a; pz_fev_mul(&r, &r, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_mul(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "mul in place (a)");
        r = b; pz_fev_mul(&r, &a, &r);
        check_lanes(&r, want, TIGHT, "mul in place (b)");
        r = a; pz_fev_sqr(&r, &r);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sqr(&want[l], &va[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "sqr in place");
        r = a; pz_fev_sub(&r, &r, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sub(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "sub in place (a)");
        r = b; pz_fev_sub(&r, &a, &r);
        check_lanes(&r, want, TIGHT, "sub in place (b)");
        /* sub2 with loose subtrahends: a - (b + c) - (b + b) */
        pz_fev bc, bb;
        pz_fev_add(&bc, &b, &c);
        pz_fev_add(&bb, &b, &b);
        pz_fev_sub2(&r2, &a, &bc, &bb);
        for (int l = 0; l < PZ_FEV_LANES; l++) {
            pz_fe t, u, w;
            pz_fe_add(&t, &vb[l], &vc[l]);
            pz_fe_add(&u, &vb[l], &vb[l]);
            pz_fe_sub(&w, &va[l], &t);
            pz_fe_sub(&want[l], &w, &u);
            pz_fe_normalize(&want[l]);
        }
        check_lanes(&r2, want, TIGHT, "sub2 with loose subtrahends");
        r = a; pz_fev_sub2(&r, &r, &b, &c);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe t; pz_fe_sub(&t, &va[l], &vb[l]); pz_fe_sub(&want[l], &t, &vc[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, TIGHT, "sub2 in place");
    }
    printf(fails ? "t_fev (%s): %d FAILED\n" : "t_fev (%s): OK\n", name, fails);
    return fails != 0;
}
