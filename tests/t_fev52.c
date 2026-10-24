/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_fev52.c - lane-parallel field arithmetic with AVX-512 IFMA (fev52.h) against the scalar implementation, with limbs
 * at the bounds of the "tight" form every operation accepts and returns. Skipped on CPUs without IFMA (PZ_ASSUME_IFMA=1
 * forces it, see cpu_has_ifma). */
#define VBYTES 64
#include "../src/fev52.h"
#include "../src/cpu_engine.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); if (++fails > 20) exit(1); } } while (0)

static u64 s = 0x9E3779B97F4A7C15ULL;
static u64 rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static const u64 LIMB_MAX = FEV_MASK;           /* limbs 0..3 of a tight element */
static u64 bound(int i) { return i < 4 ? LIMB_MAX : FEV_TOP_MAX; }

/* value of a lane, whatever the limbs are: sum l_i * 2^(52 i) mod p, with scalar field ops */
static void lane_value(fe *out, const fev *v, int lane)
{
    fe base, acc, t;
    fe_set_u64(&base, (u64)1 << 52);
    fe_zero(&acc);
    for (int i = 4; i >= 0; i--) {
        fe_mul(&acc, &acc, &base);
        fe_set_u64(&t, v->l[i][lane]);
        fe_add(&acc, &acc, &t);
    }
    fe_normalize(&acc);
    *out = acc;
}

/* limbs from a 320-bit integer w[0..4] (little-endian words); the value must be below 2^257 */
static void set_lane_words(fev *v, int lane, const u64 w[5])
{
    for (int i = 0; i < 5; i++) {
        int bit = 52 * i, wi = bit / 64, sh = bit % 64;
        u64 x = w[wi] >> sh;
        if (sh > 12 && wi + 1 < 5) x |= w[wi + 1] << (64 - sh);
        v->l[i][lane] = i < 4 ? x & FEV_MASK : x;
    }
}
static void words_add_small(u64 w[5], u64 d) { u128 c = d; for (int i = 0; i < 5 && c; i++) { c += w[i]; w[i] = (u64)c; c >>= 64; } }
static void words_sub_small(u64 w[5], u64 d) { u64 b = d; for (int i = 0; i < 5 && b; i++) { u64 t = w[i]; w[i] = t - b; b = t < b; } }
static void words_add(u64 w[5], const u64 v[5]) { u128 c = 0; for (int i = 0; i < 5; i++) { c += (u128)w[i] + v[i]; w[i] = (u64)c; c >>= 64; } }

/* values around multiples of p and powers of two, all at most 2^257 + 3 (limb 4 <= 2^49: still tight) */
static void crafted_value(u64 w[5], u64 sel)
{
    static const u64 P[5] = {0xFFFFFFFEFFFFFC2FULL, ~0ULL, ~0ULL, ~0ULL, 0};
    memset(w, 0, 5 * sizeof(u64));
    u64 kind = sel % 6, k = (sel / 6) % 3, d = (sel / 18) % 9;   /* k multiples of p (0..2), d in -4..4 */
    switch (kind) {
    case 0: case 1:
        for (u64 i = 0; i < k; i++) words_add(w, P);
        break;
    case 2: w[3] = w[2] = w[1] = w[0] = ~0ULL; break;                       /* 2^256 - 1 */
    case 3: w[4] = 1; break;                                                /* 2^256 */
    case 4: w[4] = 1; w[3] = w[2] = w[1] = w[0] = ~0ULL; break;            /* 2^257 - 1 */
    default: for (int i = 0; i < 4; i++) w[i] = 0x9E3779B97F4A7C15ULL * (sel + i + 1); w[4] = (sel >> 3) & 1; break;
    }
    if (d < 4) {
        if (w[0] | w[1] | w[2] | w[3] | w[4]) words_sub_small(w, 4 - d);
    } else {
        words_add_small(w, d - 4);
    }
}

/* random tight limbs, a fraction of the lanes at the extremes */
static void fill(fev *v, int mode)
{
    for (int lane = 0; lane < FEV_LANES; lane++) {
        int m = (mode + lane) % 6;
        for (int i = 0; i < 5; i++) {
            const u64 b = bound(i);
            u64 x;
            switch (m) {
            case 0: x = b; break;
            case 1: x = 0; break;
            case 2: x = i & 1 ? b : 0; break;
            case 3: x = rnd() % (b + 1); break;
            case 4: x = b - (rnd() & 7); break;
            default: x = (rnd() % (b + 1)) | 1; break;
            }
            v->l[i][lane] = x;
        }
    }
}

static void check_lanes(const fev *r, const fe *want, const char *what)
{
    for (int lane = 0; lane < FEV_LANES; lane++) {
        fe got;
        lane_value(&got, r, lane);
        if (!fe_eq(&got, &want[lane])) { printf("  %s: lane %d differs\n", what, lane); fails++; }
        for (int i = 0; i < 5; i++)
            if (r->l[i][lane] > bound(i)) { printf("  %s: limb %d of lane %d exceeds the bound (%llu)\n", what, i, lane, (unsigned long long)r->l[i][lane]); fails++; }
    }
}

static void check_canonical(const fev *r, const fe *want)
{
    for (int l = 0; l < FEV_LANES; l++) {
        fe got, w = want[l];
        fev_get_lane(&got, r, l);
        fe_normalize(&w);
        CHECK(fe_eq(&got, &w));
        CHECK(r->l[4][l] < ((u64)1 << FEV_TOPBITS));
        for (int i = 0; i < 4; i++) CHECK(r->l[i][l] <= FEV_MASK);
    }
}

int main(void)
{
    if (!cpu_has_ifma()) { printf("t_fev52: CPU without AVX-512 IFMA, skipped\n"); return 0; }
    /* the subtraction constants are multiples of p whose limbs dominate one (K1) or two (K2) tight subtrahends */
    {
        fe base, acc, t;
        fe_set_u64(&base, (u64)1 << 52);
        for (int w = 0; w < 2; w++) {
            const u64 *K = w ? FEV_K2 : FEV_K1;
            fe_zero(&acc);
            for (int i = 4; i >= 0; i--) { fe_mul(&acc, &acc, &base); fe_set_u64(&t, K[i]); fe_add(&acc, &acc, &t); }
            fe_normalize(&acc);
            CHECK(fe_is_zero(&acc));
            for (int i = 0; i < 5; i++) CHECK(K[i] >= (w ? 2 : 1) * bound(i));
        }
    }
    for (int it = 0; it < 100000; it++) {
        fev a, b, c, r;
        fe va[FEV_LANES], vb[FEV_LANES], vc[FEV_LANES], want[FEV_LANES];
        fill(&a, it);
        fill(&b, it / 2 + it);
        fill(&c, it / 3 + 2 * it);
        for (int l = 0; l < FEV_LANES; l++) { lane_value(&va[l], &a, l); lane_value(&vb[l], &b, l); lane_value(&vc[l], &c, l); }

        fev_mul(&r, &a, &b);
        for (int l = 0; l < FEV_LANES; l++) { fe_mul(&want[l], &va[l], &vb[l]); fe_normalize(&want[l]); }
        check_lanes(&r, want, "mul");

        fev_sqr(&r, &a);
        for (int l = 0; l < FEV_LANES; l++) { fe_sqr(&want[l], &va[l]); fe_normalize(&want[l]); }
        check_lanes(&r, want, "sqr");

        fev_add(&r, &a, &b);
        for (int l = 0; l < FEV_LANES; l++) { fe_add(&want[l], &va[l], &vb[l]); fe_normalize(&want[l]); }
        check_lanes(&r, want, "add");

        fev_sub(&r, &a, &b);
        for (int l = 0; l < FEV_LANES; l++) { fe_sub(&want[l], &va[l], &vb[l]); fe_normalize(&want[l]); }
        check_lanes(&r, want, "sub");

        fev_sub2(&r, &a, &b, &c);
        for (int l = 0; l < FEV_LANES; l++) { fe t; fe_sub(&t, &va[l], &vb[l]); fe_sub(&want[l], &t, &vc[l]); fe_normalize(&want[l]); }
        check_lanes(&r, want, "sub2");

        r = a;
        fev_normalize(&r);
        check_canonical(&r, va);

        if (it % 50 == 0) {
            fev inv, one;
            fev_inv(&inv, &a);
            fev_mul(&one, &inv, &a);
            for (int l = 0; l < FEV_LANES; l++) {
                if (fe_is_zero(&va[l])) continue;
                fe got, want1;
                lane_value(&got, &one, l);
                fe_set_u64(&want1, 1);
                CHECK(fe_eq(&got, &want1));
            }
        }
        /* chained use: a long dependent computation stays correct and within the bounds */
        {
            fev x = a, y = b, t;
            fe wx[FEV_LANES], wy[FEV_LANES];
            for (int l = 0; l < FEV_LANES; l++) { wx[l] = va[l]; wy[l] = vb[l]; }
            for (int step = 0; step < 6; step++) {
                fev_mul(&t, &x, &y);
                fev_sub2(&y, &t, &x, &y);
                fev_add(&x, &t, &y);
                fev_sqr(&x, &x);
                for (int l = 0; l < FEV_LANES; l++) {
                    fe tt, y2;
                    fe_mul(&tt, &wx[l], &wy[l]);
                    fe_sub(&y2, &tt, &wx[l]);
                    fe_sub(&y2, &y2, &wy[l]);
                    fe_add(&wx[l], &tt, &y2);
                    fe_sqr(&wx[l], &wx[l]);
                    wy[l] = y2;
                    fe_normalize(&wx[l]); fe_normalize(&wy[l]);
                }
            }
            check_lanes(&x, wx, "chain x");
            check_lanes(&y, wy, "chain y");
        }
    }
    /* crafted values around multiples of p and powers of two: normalization is exact, products keep the residue */
    for (u64 sel = 0; sel < 20000; sel++) {
        fev x, r;
        fe vx[FEV_LANES];
        for (int l = 0; l < FEV_LANES; l++) {
            u64 w[5];
            crafted_value(w, sel * FEV_LANES + (u64)l);
            set_lane_words(&x, l, w);
        }
        for (int l = 0; l < FEV_LANES; l++) {
            lane_value(&vx[l], &x, l);
            for (int i = 0; i < 5; i++) CHECK(x.l[i][l] <= bound(i));
        }
        r = x;
        fev_normalize(&r);
        check_canonical(&r, vx);
        fev one, pr;
        fe f1;
        fe_set_u64(&f1, 1);
        fev_broadcast(&one, &f1);
        fev_mul(&pr, &x, &one);
        check_lanes(&pr, vx, "x*1");
        fev_sqr(&pr, &x);
        for (int l = 0; l < FEV_LANES; l++) { fe w; fe_sqr(&w, &vx[l]); fe_normalize(&w); vx[l] = w; }
        check_lanes(&pr, vx, "x^2");
    }
    /* lane conversion of lazy scalar values (p, p+1, 2^256-1, 0) */
    {
        fe lazies[4];
        lazies[0] = FE_P;
        lazies[1] = FE_P; lazies[1].n[0] += 1;
        memset(&lazies[2], 0xff, sizeof lazies[2]);
        fe_zero(&lazies[3]);
        for (int k = 0; k < 4; k++) {
            fev v;
            fev_broadcast(&v, &lazies[k]);
            fe want = lazies[k];
            fe_normalize(&want);
            for (int l = 0; l < FEV_LANES; l++) {
                fe got;
                fev_get_lane(&got, &v, l);
                CHECK(fe_eq(&got, &want));
            }
        }
    }
    printf(fails ? "t_fev52: %d FAILED\n" : "t_fev52: OK\n", fails);
    return fails != 0;
}
