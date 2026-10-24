/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* blsim.c - abstract model of the kangaroo with precomputation (Bernstein-Lange, "Computing small discrete logarithms
 * faster", 2012) on an interval of W keys: points are their integer exponents, no curve arithmetic.
 *
 * Precomputation: N = beta*T walks from random tame starts in [0, 2W), each until a distinguished point (DP, one point
 * in L on average); the T DPs reached by most walks are kept (the table, T entries of RAM).
 * Online: for a key x in [0, W), walks from x + r (r random in [0, W)) until a DP; a DP in the table gives x. A walk
 * that misses starts again from a new r.
 * Walk: classic kangaroo (positive jumps, mean W/(4L)), or with negation map (sym=1: classes {E, -E}, random sign).
 *
 *   gcc -O2 -o blsim tools/blsim.c -lm && ./blsim bits=36 T=4096 beta=8 targets=400
 * prints precomputation and online steps, also in units of sqrt(W*T) and sqrt(W/T). */
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

#define R 1024                 /* many jumps: with the negation map, 4-cycles come about once every 4 R^2 steps */
static int bits = 36, sym = 0, targets = 400;
static double T_d = 4096, beta = 8, alpha = 1.0;
static i64 W, jumps[R];
static u64 dpmask, cap;

/* open-addressing map: DP class -> (count, exponent with sign) */
typedef struct { u64 key; u64 cnt; i64 e; } ent;
static ent *tab; static u64 tmask;
static ent *find(u64 k, int add)
{
    u64 h = mixh(k, 9) & tmask;
    while (tab[h].key && tab[h].key != k) h = (h + 1) & tmask;
    if (!tab[h].key && add) tab[h].key = k;
    return tab[h].key ? &tab[h] : NULL;
}

/* walk from exponent e until a DP; returns the DP exponent (sign kept), *steps; 0 when the cap is hit */
static i64 walk(i64 e, u64 *steps, int *ok)
{
    u64 n = 0, p1 = ~0ULL, p2 = ~0ULL;
    for (;;) {
        u64 a = sym ? (u64)llabs(e) : (u64)e;
        if ((mixh(a, 3) & dpmask) == 0) { *steps += n; *ok = 1; return e; }
        if (++n > cap) { *steps += n; *ok = 0; return 0; }
        u64 j = mixh(a, 2) % R;
        if (a == p2) j = (j + 1) % R;                   /* back where it was two steps ago: the 2-cycle rule of the program */
        p2 = p1; p1 = a;
        e += jumps[j];
        if (sym && (mixh((u64)llabs(e), 1) & 1)) e = -e;
    }
}

static int cmp_cnt(const void *a, const void *b)
{
    const ent *x = a, *y = b;
    return x->cnt < y->cnt ? 1 : x->cnt > y->cnt ? -1 : 0;
}

int main(int argc, char **argv)
{
    u64 seed = 1;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
#define OPT(n, fmt, var) if (!strncmp(a, n "=", sizeof n)) { sscanf(a + sizeof n, fmt, &var); continue; }
        OPT("bits", "%d", bits) OPT("T", "%lf", T_d) OPT("beta", "%lf", beta) OPT("alpha", "%lf", alpha) OPT("sym", "%d", sym)
        OPT("targets", "%d", targets) OPT("seed", "%lu", seed)
        fprintf(stderr, "unknown option %s\n", a); return 1;
    }
    seed_rng(seed);
    W = (i64)1 << bits;
    const u64 T = (u64)T_d;
    /* walk length L = alpha * sqrt(W/T), rounded to a power of two (the DP rule) */
    double Lf = alpha * sqrt((double)W / (double)T);
    int dp = (int)floor(log2(Lf) + 0.5);
    if (dp < 1) dp = 1;
    const u64 L = (u64)1 << dp;
    dpmask = L - 1;
    cap = 20 * L;
    const double mu = sym ? (double)W / (4.0 * sqrt((double)L)) : (double)W / (4.0 * (double)L);   /* sym: a random walk, so jumps ~ W/sqrt(L) to spread over the interval */
    for (int j = 0; j < R; j++) jumps[j] = 1 + (i64)(rnd() % (u64)(2 * mu));
    const u64 N = (u64)(beta * (double)T);
    u64 tb = 1; while (tb < 4 * N) tb <<= 1;
    tab = calloc(tb, sizeof(ent)); tmask = tb - 1;
    /* precomputation */
    u64 pre = 0, distinct = 0;
    for (u64 w = 0; w < N; w++) {
        i64 e = (i64)(rnd() % (u64)(2 * W));
        int ok;
        i64 d = walk(e, &pre, &ok);
        if (!ok) continue;
        u64 k = (sym ? (u64)llabs(d) : (u64)d) + 1;
        ent *t = find(k, 1);
        if (!t->cnt) { distinct++; t->e = d; }
        t->cnt++;
    }
    /* keep the T most popular */
    ent *all = malloc(sizeof(ent) * (distinct + 1));
    u64 m = 0;
    for (u64 i = 0; i < tb; i++) if (tab[i].key) all[m++] = tab[i];
    qsort(all, m, sizeof(ent), cmp_cnt);
    if (m > T) m = T;
    memset(tab, 0, tb * sizeof(ent));
    for (u64 i = 0; i < m; i++) { ent *t = find(all[i].key, 1); t->cnt = all[i].cnt; t->e = all[i].e; }
    /* online */
    double on = 0, on2 = 0;
    for (int q = 0; q < targets; q++) {
        const i64 x = (i64)(rnd() % (u64)W);
        u64 st = 0;
        for (;;) {
            const i64 r = (i64)(rnd() % (u64)W);
            int ok;
            i64 e0 = x + r;
            i64 d = walk(e0, &st, &ok);
            if (!ok) continue;
            ent *t = find((sym ? (u64)llabs(d) : (u64)d) + 1, 0);
            if (!t) continue;
            /* d = sign*(x + r + walked): the table has t->e with |t->e| == |d|; the program would solve x from it */
            (void)t;
            break;
        }
        on += (double)st;
        on2 += (double)st * (double)st;
    }
    on /= targets;
    const double sd = sqrt(on2 / targets - on * on) / sqrt((double)targets);
    const double sWT = sqrt((double)W * (double)T), sWoT = sqrt((double)W / (double)T);
    printf("bits=%d T=%llu beta=%.1f alpha=%.2f sym=%d L=2^%d: precomputation %.3g steps = %.2f sqrt(W*T); online %.3g +- %.2g steps = %.2f sqrt(W/T); "
           "product %.2f W; without precomputation the kangaroo costs ~%.3g\n",
           bits, (unsigned long long)T, beta, alpha, sym, dp, (double)pre, (double)pre / sWT, on, sd, on / sWoT, (double)pre * on / (double)W,
           (sym ? 1.09 : 2.0) * sqrt((double)W));
    return 0;
}
