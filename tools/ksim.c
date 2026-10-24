/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* ksim.c - abstract-model simulator for interval-DLP random-walk algorithms.
 * Points are represented by their integer exponent, so no elliptic-curve
 * arithmetic is needed and thousands of runs can be averaged in seconds.
 * Used to tune the parameters of the symmetric Gaudry-Schost kangaroo. A walker has E = coef*x + d with coef 0 (tame),
 * +-1 (wild) or +-2 (wild on 2x, option w2 = share of them, B2 = their zone): any collision with different coefficients
 * gives (c1 - c2) x = d2 - d1. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint64_t u64;
typedef int64_t i64;

static u64 rs[4];
static inline u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }
static u64 rnd(void)
{
    u64 r = rotl(rs[1] * 5, 7) * 9, t = rs[1] << 17;
    rs[2] ^= rs[0]; rs[3] ^= rs[1]; rs[1] ^= rs[2]; rs[0] ^= rs[3]; rs[2] ^= t; rs[3] = rotl(rs[3], 45);
    return r;
}
static void seed_rng(u64 s)
{
    for (int i = 0; i < 4; i++) { s += 0x9e3779b97f4a7c15ULL; u64 z = s; z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL; z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL; rs[i] = z ^ (z >> 31); }
}
static inline u64 mixh(u64 z, u64 salt)
{
    z += salt * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static inline i64 uni(i64 lo, i64 hi) { return lo + (i64)(rnd() % (u64)(hi - lo + 1)); }   /* inclusive */

typedef struct { i64 E, d; int coef, kind; u64 prev, prev2, ck, mn; uint32_t since; int esc; u64 age; } kang;   /* E = coef*x + d; kind 0 tame, 1 wild x, 2 wild 2x */

/* configuration */
static int bits = 30, runs = 200, K = 256, dpbits = 4, R = 256, symmetric = 1, reseed_same = 1;
static double jump_mult = 1.0;       /* mean jump = jump_mult * W / (K-ish scaling) set below */
static double A_f = 0.75, B_f = 0.25, tame_frac = 0.5, w2_frac = 0.0, B2_f = 0.125;   /* w2_frac: share of wild-2x walkers */
static double cap_mult = 24.0;
static int cyc = 1;
/* rck = 1: the layout of RCKangaroo's "SOTA" method instead of ours: the key x' = x + W/32 with x uniform in [0, W),
 * tame exponents in [0, W/32), wild exponents -x' + d with d even in [0, 34 W/32), a third tame; jabs > 0: mean jump
 * jabs * sqrt(W) (RCKangaroo: 12) */
static int rck = 0;
/* design: walker species E = coef*x + u, u = r (mod MOD) uniform in [lo, hi]*W, share w; jumps multiples of MOD */
typedef struct { int coef, r; double lo, hi, w; } species;
static species des[256]; static int ndes = 0, MOD = 1;
static void load_design(const char *f)
{
    FILE *fp = fopen(f, "r"); if (!fp) { perror(f); exit(1); }
    if (fscanf(fp, "%d", &MOD) != 1) exit(1);
    while (ndes < 256 && fscanf(fp, "%d %d %lf %lf %lf", &des[ndes].coef, &des[ndes].r, &des[ndes].lo, &des[ndes].hi, &des[ndes].w) == 5) ndes++;
    double s = 0; for (int i = 0; i < ndes; i++) s += des[i].w;
    for (int i = 0; i < ndes; i++) des[i].w /= s;
    fclose(fp);
}
static double rsmin = 0;              /* > 0: a walker that reaches a new DP after at least rsmin steps since its start starts again */
static double jabs = 0;                 /* fruitless-cycle detection and escape (as in the program) */
#define CK_PERIOD 64
#define N_ESC 16
static i64 jumps[4096 + N_ESC];
static unsigned long n_escapes, n_reseed, n_cap, n_rsdp;
static i64 W, X;                     /* interval width and unknown (centered) */
static i64 A, B, B2;

#define TBL_BITS 24
static u64 *tkey; static signed char *tkind; static i64 *stored_d;

static void new_start(kang *k, int kind)
{
    k->kind = kind; k->since = 0; k->age = 0; k->prev = k->prev2 = ~0ULL; k->ck = k->mn = ~0ULL; k->esc = 0;
    if (ndes) {
        const species *sp = &des[kind];
        i64 lo = (i64)floor(sp->lo * (double)W), hi = (i64)ceil(sp->hi * (double)W);
        i64 u = uni(lo, hi);
        u -= ((u - sp->r) % MOD + MOD) % MOD;          /* round down to the residue */
        k->coef = sp->coef; k->d = u;
    } else if (rck) {
        const i64 x32 = W / 32;
        if (kind == 0) { k->coef = 0; k->d = uni(0, x32 - 1); }
        else { k->coef = -1; k->d = uni(0, 34 * x32 - 1) & ~(i64)1; }
    } else if (kind == 0) { k->coef = 0; k->d = uni(-A, A); }
    else if (kind == 1) { k->coef = 1; k->d = uni(-B, B); }
    else { k->coef = 2; k->d = uni(-B2, B2); }
    k->E = k->coef * X + k->d;
    u64 a = (u64)llabs(k->E);
    i64 c = (mixh(a, 1) & 1) ? -(i64)a : (i64)a;
    if (c != k->E) { k->E = c; k->d = -k->d; k->coef = -k->coef; }
}

/* (c1 - c2) x = d2 - d1 */
static i64 solve_pair(int ca, i64 da, int cb, i64 db, int *ok)
{
    i64 c = ca - cb, r = db - da;
    *ok = c != 0 && r % c == 0;
    return *ok ? r / c : 0;
}



static double one_run(unsigned long *dps_out, unsigned long *bad)
{
    W = (i64)1 << bits;
    X = ndes ? uni(0, W - 1) : rck ? uni(0, W - 1) + W / 32 : uni(-W / 2, W / 2 - 1);
    A = (i64)(A_f * (double)W); B = (i64)(B_f * (double)W); B2 = (i64)(B2_f * (double)W);
    double steps_per = 1.5 * sqrt((double)W) / K * 3.0;                 /* planned steps per kangaroo (x3 margin) */
    double mean_jump = jump_mult * (double)(A) / (sqrt(steps_per) * 8.0);   /* drift ~ 1/8 of zone */
    if (jabs > 0) mean_jump = jabs * sqrt((double)W);
    if (mean_jump < 2) mean_jump = 2;
    for (int i = 0; i < R + N_ESC; i++) {
        jumps[i] = 1 + (i64)(rnd() % (u64)(2 * mean_jump));
        if (ndes) { jumps[i] = (i64)(mean_jump * 2 / 3) + (i64)(rnd() % (u64)(mean_jump * 2 / 3 + 1)); jumps[i] = (jumps[i] / MOD + 1) * MOD; }
        else if (rck) jumps[i] = (i64)(mean_jump * 2 / 3) + (i64)(rnd() % (u64)(mean_jump * 2 / 3 + 1)), jumps[i] = (jumps[i] & ~(i64)1) + 2;   /* [8, 16) sqrt(W) for jabs = 12, even */
    }
    memset(tkey, 0xff, sizeof(u64) << TBL_BITS);
    kang *ks = malloc(sizeof(kang) * (size_t)K);
    int ntame = (int)(tame_frac * K), nw2 = (int)(w2_frac * K);
    if (ndes) {
        int i = 0;
        for (int s = 0; s < ndes; s++) { int cnt = (int)(des[s].w * K + 0.5); if (s == ndes - 1) cnt = K - i; for (int c = 0; c < cnt && i < K; c++) new_start(&ks[i++], s); }
    } else
    for (int i = 0; i < K; i++) new_start(&ks[i], i < ntame ? 0 : (i < ntame + nw2 ? 2 : 1));
    u64 dpmask = ((u64)1 << dpbits) - 1;
    u64 ops = 0, dps = 0;
    i64 cap = (i64)(cap_mult * (double)(1u << dpbits)) + 16;
    for (;;) {
        for (int i = 0; i < K; i++) {
            kang *k = &ks[i];
            u64 a = (u64)llabs(k->E);
            unsigned j = (unsigned)(mixh(a, 2) % (u64)R);
            if (a == k->prev2) j = (j + 1) % (unsigned)R;
            if (cyc) {
                /* checkpoint every CK_PERIOD steps; back on it = cycle; leave it from its point of least tag */
                const u64 t = mixh(a, 5);
                const int bnd = (k->since & (CK_PERIOD - 1)) == 0;
                int out = 0;
                if (k->esc) {
                    if (t == k->mn) out = 1;
                    else if (bnd) { k->esc = 0; k->ck = k->mn = t; }
                } else if (t == k->ck) {
                    k->esc = 1;
                    out = t == k->mn;
                } else if (bnd) k->ck = k->mn = t;
                else if (t < k->mn) k->mn = t;
                if (out) { j = (unsigned)R + (unsigned)(mixh(a, 6) % N_ESC); k->esc = 0; k->ck = k->mn = ~0ULL; n_escapes++; }
            }
            i64 e = k->E + jumps[j];
            k->d += jumps[j];
            u64 na = (u64)llabs(e);
            i64 c = (symmetric && (mixh(na, 1) & 1)) ? -(i64)na : (symmetric ? (i64)na : e);
            if (c != e) { e = c; k->d = -k->d; k->coef = -k->coef; }
            k->E = e;
            k->prev2 = k->prev; k->prev = a;
            ops++; k->since++; k->age++;
            u64 cls = symmetric ? na : (u64)e;
            if ((mixh(cls, 3) & dpmask) == 0) {
                dps++;
                int kind = k->coef;
                u64 h = mixh(cls, 4) & (((u64)1 << TBL_BITS) - 1);
                while (tkey[h] != ~0ULL && tkey[h] != cls) h = (h + 1) & (((u64)1 << TBL_BITS) - 1);
                if (tkey[h] == ~0ULL) { tkey[h] = cls; tkind[h] = (signed char)kind; stored_d[h] = k->d; if (rsmin > 0 && (double)k->age >= rsmin) { new_start(k, k->kind); n_rsdp++; } }
                else if (tkind[h] != kind) {
                    int ok; i64 xc = solve_pair(tkind[h], stored_d[h], kind, k->d, &ok);
                    if (ok && xc == X) { free(ks); *dps_out = dps; return (double)ops; }
                    (*bad)++;
                } else if (reseed_same) { new_start(k, k->kind); n_reseed++; }
                k->since = 0;
            } else if ((i64)k->since > cap) { new_start(k, k->kind); n_cap++; }
        }
        if (ops > (u64)(60.0 * sqrt((double)W) + 100000)) { free(ks); *dps_out = dps; return -1; }
    }
}

int main(int argc, char **argv)
{
    unsigned long seed = 1;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
#define OPT(n, fmt, var) if (!strncmp(a, n "=", sizeof n)) { sscanf(a + sizeof n, fmt, &var); continue; }
        OPT("bits", "%d", bits) OPT("runs", "%d", runs) OPT("K", "%d", K) OPT("dp", "%d", dpbits) OPT("R", "%d", R)
        OPT("sym", "%d", symmetric) OPT("jm", "%lf", jump_mult) OPT("A", "%lf", A_f) OPT("B", "%lf", B_f) OPT("tf", "%lf", tame_frac)
        OPT("w2", "%lf", w2_frac) OPT("B2", "%lf", B2_f) OPT("cap", "%lf", cap_mult) OPT("cyc", "%d", cyc) OPT("rck", "%d", rck) OPT("jabs", "%lf", jabs) OPT("rsmin", "%lf", rsmin) OPT("reseed", "%d", reseed_same) OPT("seed", "%lu", seed) if (!strncmp(a, "design=", 7)) { load_design(a + 7); continue; }
        fprintf(stderr, "unknown option %s\n", a); return 1;
    }
    tkey = malloc(sizeof(u64) << TBL_BITS); tkind = malloc(1 << TBL_BITS); stored_d = malloc(sizeof(i64) << TBL_BITS);
    seed_rng(seed);
    double *res = malloc(sizeof(double) * (size_t)runs);
    int fail = 0; unsigned long badtot = 0;
    for (int r = 0; r < runs; r++) {
        unsigned long dps = 0;
        double o = one_run(&dps, &badtot);
        if (o < 0) { fail++; res[r] = 60.0 * 1e9; } else res[r] = o;
    }
    double sum = 0, sw = sqrt(ldexp(1.0, bits));
    for (int r = 0; r < runs; r++) sum += res[r];
    int cmp(const void *x, const void *y) { double a = *(const double *)x, b = *(const double *)y; return a < b ? -1 : a > b; }
    qsort(res, (size_t)runs, sizeof(double), cmp);
    double mean = sum / runs, var = 0;
    for (int r = 0; r < runs; r++) var += (res[r] - mean) * (res[r] - mean);
    printf("rck=%d jabs=%.2f cyc=%d esc/run=%.1f merges/run=%.1f noDP/run=%.1f restarts/run=%.1f ", rck, jabs, cyc, (double)n_escapes / runs, (double)n_reseed / runs, (double)n_cap / runs, (double)n_rsdp / runs);
    printf("w2=%.2f B2=%.3f bits=%d K=%d dp=%d R=%d sym=%d A=%.2f B=%.2f tf=%.2f jm=%.2f  mean=%.3f median=%.3f p90=%.3f  (x sqrt(W))  fail=%d bad=%lu\n",
           w2_frac, B2_f, bits, K, dpbits, R, symmetric, A_f, B_f, tame_frac, jump_mult, mean / sw, res[runs / 2] / sw, res[runs * 9 / 10] / sw, fail, badtot);
    return 0;
}
