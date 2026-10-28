/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* t_fev52.c - lane-parallel field arithmetic with AVX-512 IFMA (fev52.h) against the scalar implementation, with limbs
 * at the bounds of the "tight" form every operation accepts and returns. Skipped on CPUs without IFMA (PZ_ASSUME_IFMA=1
 * forces it, the PZ_ASSUME_IFMA environment variable). */
#define PZ_VBYTES 64
#include <pzcore/fev52.h>
#include <pzcore/cpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); if (++fails > 20) exit(1); } } while (0)

static uint64_t s = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static const uint64_t LIMB_MAX = PZ_FEV_MASK;           /* limbs 0..3 of a tight element */
static uint64_t bound(int i) { return i < 4 ? LIMB_MAX : PZ_FEV_TOP_MAX; }

/* value of a lane, whatever the limbs are: sum l_i * 2^(52 i) mod p, with scalar field ops */
static void lane_value(pz_fe *out, const pz_fev *v, int lane)
{
    pz_fe base, acc, t;
    pz_fe_set_u64(&base, (uint64_t)1 << 52);
    pz_fe_zero(&acc);
    for (int i = 4; i >= 0; i--) {
        pz_fe_mul(&acc, &acc, &base);
        pz_fe_set_u64(&t, v->l[i][lane]);
        pz_fe_add(&acc, &acc, &t);
    }
    pz_fe_normalize(&acc);
    *out = acc;
}

/* limbs from a 320-bit integer w[0..4] (little-endian words); the value must be below 2^257 */
static void set_lane_words(pz_fev *v, int lane, const uint64_t w[5])
{
    for (int i = 0; i < 5; i++) {
        int bit = 52 * i, wi = bit / 64, sh = bit % 64;
        uint64_t x = w[wi] >> sh;
        if (sh > 12 && wi + 1 < 5) x |= w[wi + 1] << (64 - sh);
        v->l[i][lane] = i < 4 ? x & PZ_FEV_MASK : x;
    }
}
static void words_add_small(uint64_t w[5], uint64_t d) { pz_u128 c = d; for (int i = 0; i < 5 && c; i++) { c += w[i]; w[i] = (uint64_t)c; c >>= 64; } }
static void words_sub_small(uint64_t w[5], uint64_t d) { uint64_t b = d; for (int i = 0; i < 5 && b; i++) { uint64_t t = w[i]; w[i] = t - b; b = t < b; } }
static void words_add(uint64_t w[5], const uint64_t v[5]) { pz_u128 c = 0; for (int i = 0; i < 5; i++) { c += (pz_u128)w[i] + v[i]; w[i] = (uint64_t)c; c >>= 64; } }

/* values around multiples of p and powers of two, all at most 2^257 + 3 (limb 4 <= 2^49: still tight) */
static void crafted_value(uint64_t w[5], uint64_t sel)
{
    static const uint64_t P[5] = {0xFFFFFFFEFFFFFC2FULL, ~0ULL, ~0ULL, ~0ULL, 0};
    memset(w, 0, 5 * sizeof(uint64_t));
    uint64_t kind = sel % 6, k = (sel / 6) % 3, d = (sel / 18) % 9;   /* k multiples of p (0..2), d in -4..4 */
    switch (kind) {
    case 0: case 1:
        for (uint64_t i = 0; i < k; i++) words_add(w, P);
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
static void fill(pz_fev *v, int mode)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) {
        int m = (mode + lane) % 6;
        for (int i = 0; i < 5; i++) {
            const uint64_t b = bound(i);
            uint64_t x;
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

static void check_lanes(const pz_fev *r, const pz_fe *want, const char *what)
{
    for (int lane = 0; lane < PZ_FEV_LANES; lane++) {
        pz_fe got;
        lane_value(&got, r, lane);
        if (!pz_fe_eq(&got, &want[lane])) { printf("  %s: lane %d differs\n", what, lane); fails++; }
        for (int i = 0; i < 5; i++)
            if (r->l[i][lane] > bound(i)) { printf("  %s: limb %d of lane %d exceeds the bound (%llu)\n", what, i, lane, (unsigned long long)r->l[i][lane]); fails++; }
    }
}

static void check_canonical(const pz_fev *r, const pz_fe *want)
{
    for (int l = 0; l < PZ_FEV_LANES; l++) {
        pz_fe got, w = want[l];
        pz_fev_get_lane(&got, r, l);
        pz_fe_normalize(&w);
        CHECK(pz_fe_eq(&got, &w));
        CHECK(r->l[4][l] < ((uint64_t)1 << PZ_FEV_TOPBITS));
        for (int i = 0; i < 4; i++) CHECK(r->l[i][l] <= PZ_FEV_MASK);
    }
}

/* PZ_ASSUME_IFMA: on with any value except "" and "0", as for pzsolve */
static int assume_ifma(void)
{
    const char *v = getenv("PZ_ASSUME_IFMA");
    return v && v[0] && strcmp(v, "0") != 0;
}

int main(void)
{
    if (!pz_cpu_has_ifma() && !assume_ifma()) { printf("t_fev52: CPU without AVX-512 IFMA, skipped\n"); return 0; }
    /* the subtraction constants are multiples of p whose limbs dominate one (K1) or two (K2) tight subtrahends */
    {
        pz_fe base, acc, t;
        pz_fe_set_u64(&base, (uint64_t)1 << 52);
        for (int w = 0; w < 2; w++) {
            const uint64_t *K = w ? pz_fev_k2 : pz_fev_k1;
            pz_fe_zero(&acc);
            for (int i = 4; i >= 0; i--) { pz_fe_mul(&acc, &acc, &base); pz_fe_set_u64(&t, K[i]); pz_fe_add(&acc, &acc, &t); }
            pz_fe_normalize(&acc);
            CHECK(pz_fe_is_zero(&acc));
            for (int i = 0; i < 5; i++) CHECK(K[i] >= (w ? 2 : 1) * bound(i));
        }
    }
    for (int it = 0; it < 100000; it++) {
        pz_fev a, b, c, r;
        pz_fe va[PZ_FEV_LANES], vb[PZ_FEV_LANES], vc[PZ_FEV_LANES], want[PZ_FEV_LANES];
        fill(&a, it);
        fill(&b, it / 2 + it);
        fill(&c, it / 3 + 2 * it);
        for (int l = 0; l < PZ_FEV_LANES; l++) { lane_value(&va[l], &a, l); lane_value(&vb[l], &b, l); lane_value(&vc[l], &c, l); }

        pz_fev_mul(&r, &a, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_mul(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, "mul");

        pz_fev_sqr(&r, &a);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sqr(&want[l], &va[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, "sqr");

        pz_fev_add(&r, &a, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_add(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, "add");

        pz_fev_sub(&r, &a, &b);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe_sub(&want[l], &va[l], &vb[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, "sub");

        pz_fev_sub2(&r, &a, &b, &c);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe t; pz_fe_sub(&t, &va[l], &vb[l]); pz_fe_sub(&want[l], &t, &vc[l]); pz_fe_normalize(&want[l]); }
        check_lanes(&r, want, "sub2");

        r = a;
        pz_fev_normalize(&r);
        check_canonical(&r, va);

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
        /* chained use: a long dependent computation stays correct and within the bounds */
        {
            pz_fev x = a, y = b, t;
            pz_fe wx[PZ_FEV_LANES], wy[PZ_FEV_LANES];
            for (int l = 0; l < PZ_FEV_LANES; l++) { wx[l] = va[l]; wy[l] = vb[l]; }
            for (int step = 0; step < 6; step++) {
                pz_fev_mul(&t, &x, &y);
                pz_fev_sub2(&y, &t, &x, &y);
                pz_fev_add(&x, &t, &y);
                pz_fev_sqr(&x, &x);
                for (int l = 0; l < PZ_FEV_LANES; l++) {
                    pz_fe tt, y2;
                    pz_fe_mul(&tt, &wx[l], &wy[l]);
                    pz_fe_sub(&y2, &tt, &wx[l]);
                    pz_fe_sub(&y2, &y2, &wy[l]);
                    pz_fe_add(&wx[l], &tt, &y2);
                    pz_fe_sqr(&wx[l], &wx[l]);
                    wy[l] = y2;
                    pz_fe_normalize(&wx[l]); pz_fe_normalize(&wy[l]);
                }
            }
            check_lanes(&x, wx, "chain x");
            check_lanes(&y, wy, "chain y");
        }
    }
    /* crafted values around multiples of p and powers of two: normalization is exact, products keep the residue */
    for (uint64_t sel = 0; sel < 20000; sel++) {
        pz_fev x, r;
        pz_fe vx[PZ_FEV_LANES];
        for (int l = 0; l < PZ_FEV_LANES; l++) {
            uint64_t w[5];
            crafted_value(w, sel * PZ_FEV_LANES + (uint64_t)l);
            set_lane_words(&x, l, w);
        }
        for (int l = 0; l < PZ_FEV_LANES; l++) {
            lane_value(&vx[l], &x, l);
            for (int i = 0; i < 5; i++) CHECK(x.l[i][l] <= bound(i));
        }
        r = x;
        pz_fev_normalize(&r);
        check_canonical(&r, vx);
        pz_fev one, pr;
        pz_fe f1;
        pz_fe_set_u64(&f1, 1);
        pz_fev_broadcast(&one, &f1);
        pz_fev_mul(&pr, &x, &one);
        check_lanes(&pr, vx, "x*1");
        pz_fev_sqr(&pr, &x);
        for (int l = 0; l < PZ_FEV_LANES; l++) { pz_fe w; pz_fe_sqr(&w, &vx[l]); pz_fe_normalize(&w); vx[l] = w; }
        check_lanes(&pr, vx, "x^2");
    }
    /* lane conversion of lazy scalar values (p, p+1, 2^256-1, 0) */
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
            for (int l = 0; l < PZ_FEV_LANES; l++) {
                pz_fe got;
                pz_fev_get_lane(&got, &v, l);
                CHECK(pz_fe_eq(&got, &want));
            }
        }
    }
    printf(fails ? "t_fev52: %d FAILED\n" : "t_fev52: OK\n", fails);
    return fails != 0;
}
