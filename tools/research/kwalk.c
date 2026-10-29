/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* kwalk.c - abstract model of the kangaroo of schema 3 as the program runs it, with the decomposition of its cost
 * (docs/en/RESEARCH.md section 8.12). Points are integer exponents E = c*x + d, no curve. As in src/kangaroo.c: the group
 * (coefficient, residue) of a walker is fixed by the cumulated shares, every start draws a band of the group by its share,
 * the jumps are uniform in [2/3, 4/3] of the mean and rounded up to a multiple of the modulus, the 2-cycle rule and the
 * checkpoint exit of longer cycles, a distinguished point when a hash of the class has dp low zero bits, a restart of the
 * walker whose distinguished point is already in the table with the same kind, and the steps counted by whole rounds of the
 * herd (as 'pzsolve kstat' with one thread).
 *
 * Beside the table of distinguished points a second table keeps every class visited (shadow=1): it gives the step of the
 * first collision that solves (t_coll; the cost minus t_coll is the delay of the distinguished points and of the round),
 * the distinct classes visited until then (the elements of the coverage model, start points included) and the redundant
 * steps (on classes already visited: merged walkers before their next distinguished point, cycles). iid=1 replaces the
 * walk by independent points (every step a new start): the random coverage of the layout, without walks.
 *
 *   gcc -O2 -o kwalk tools/research/kwalk.c -lm
 *   ./kwalk bits=36 K=1024 dp=2 runs=2000                 the regime of 'pzsolve kstat 36 N 1'
 *   ./kwalk bits=36 K=1024 dp=0 iid=1 runs=2000           the same layout as independent points
 * Options: design=FILE (default tools/research/kang_schema3.txt), jm34=D (mean jump W^(3/4)/D, default 16) or jabs=A
 * (A sqrt(W)), spread=S (jumps uniform in [1-S, 1+S] of the mean), pow2=1 (jumps of 8 powers of two, same mean), R=256,
 * seed=N, out=FILE (appends one line per trial: cost, t_coll, distinct, redundant, in units of sqrt(W)). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef uint64_t u64;
typedef int64_t i64;

static u64 rs[4], salt0;
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
/* the "curve": a hash of the class for the jump, the sign of the representative, the distinguished points and the tags */
static inline u64 mixh(u64 z, u64 salt)
{
    z += (salt + salt0) * 0x9e3779b97f4a7c15ULL + 0x632be59bd9b4e019ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static inline i64 uni(i64 lo, i64 hi) { return lo + (i64)(rnd() % (u64)(hi - lo + 1)); }

/* the layout: rows "coefficient residue start end share" (zone in units of W), the modulus in the first line */
typedef struct { int coef, r, grp; double lo, hi, w; } band;
static band des[512];
static int ndes, MOD = 1, ngrp, grp_first[512];
static double grp_w[512];

static void load_design(const char *f)
{
    FILE *fp = fopen(f, "r");
    if (!fp) { perror(f); exit(1); }
    if (fscanf(fp, "%d", &MOD) != 1) exit(1);
    while (ndes < 512 && fscanf(fp, "%d %d %lf %lf %lf", &des[ndes].coef, &des[ndes].r, &des[ndes].lo, &des[ndes].hi, &des[ndes].w) == 5) ndes++;
    fclose(fp);
    double s = 0;
    for (int i = 0; i < ndes; i++) s += des[i].w;
    for (int i = 0; i < ndes; i++) {
        des[i].w /= s;
        int g = -1;
        for (int k = 0; k < ngrp; k++)
            if (des[grp_first[k]].coef == des[i].coef && des[grp_first[k]].r == des[i].r) { g = k; break; }
        if (g < 0) { g = ngrp++; grp_first[g] = i; grp_w[g] = 0; }
        des[i].grp = g;
        grp_w[g] += des[i].w;
    }
}

typedef struct { i64 E, d; int coef, grp, esc; u64 prev, prev2, ck, mn, since; } kang;

static int bits = 36, runs = 1000, K = 1024, dpbits = 2, R = 256, iid = 0, shadow = 1, pow2 = 0;
static double jm34 = 16, jabs = 0, spread = 1.0 / 3, capm = 24;
#define CK_PERIOD 64
#define N_ESC 16
static i64 jumps[4096 + N_ESC], W, X;
static double n_esc, n_merge, n_cap;

static void new_start(kang *k)
{
    const band *b = NULL;
    const double p = (double)(rnd() >> 11) * 0x1p-53 * grp_w[k->grp];
    double acc = 0;
    for (int i = 0; i < ndes; i++)
        if (des[i].grp == k->grp) { b = &des[i]; acc += des[i].w; if (p < acc) break; }
    i64 u = uni((i64)floor(b->lo * (double)W), (i64)floor(b->hi * (double)W));
    u -= ((u - b->r) % MOD + MOD) % MOD;                      /* down to the residue */
    k->coef = b->coef; k->d = u; k->E = (i64)b->coef * X + u;
    const u64 a = (u64)llabs(k->E);
    const i64 c = (mixh(a, 1) & 1) ? -(i64)a : (i64)a;
    if (c != k->E) { k->E = c; k->d = -k->d; k->coef = -k->coef; }
    k->prev = k->prev2 = k->ck = k->mn = ~0ULL; k->esc = 0; k->since = 0;
}

/* open addressing on the class; an entry of an earlier trial (another gen) is empty */
typedef struct { u64 key; i64 d; int kind; unsigned gen; } ent;
static ent *tab, *sh;
static u64 tmask, smask;
static unsigned gen;
static ent *find(ent *t, u64 m, u64 key)
{
    for (u64 h = mixh(key, 4) & m;; h = (h + 1) & m) {
        if (t[h].gen != gen) { t[h].key = 0; return &t[h]; }
        if (t[h].key == key) return &t[h];
    }
}

typedef struct { u64 ops, tcoll, ndist, nred, nred_at; } track;

/* the walker of coefficient coef and distance d is on class cls (a step, or a start) */
static void visit(track *t, u64 cls, int coef, i64 d)
{
    if (!shadow || t->tcoll || t->ndist > smask / 10 * 7) return;
    ent *e = find(sh, smask, cls + 1);
    if (!e->key) { *e = (ent){cls + 1, d, coef, gen}; t->ndist++; return; }
    t->nred++;
    if (e->kind != coef) { t->tcoll = t->ops; t->nred_at = t->nred; }   /* different coefficients on one class: x */
}

static double one_run(track *t)
{
    W = (i64)1 << bits;
    X = uni(0, W - 1);
    double mean = jabs > 0 ? jabs * sqrt((double)W) : pow((double)W, 0.75) / jm34;
    if (mean < 2 * MOD) mean = 2 * MOD;
    for (int i = 0; i < R + N_ESC; i++) {
        i64 v = pow2 ? (i64)(mean * 8 / 255 * ldexp(1, (int)(rnd() % 8)))
                     : (i64)(mean * (1 - spread)) + (i64)(rnd() % (u64)(2 * spread * mean + 1));
        jumps[i] = (v / MOD + 1) * MOD;
    }
    gen++;
    memset(t, 0, sizeof *t);
    kang *ks = malloc(sizeof(kang) * (size_t)K);
    for (int i = 0; i < K; i++) {                              /* the groups as in kang_worker_new */
        const double pos = (2.0 * i + 1) / (2.0 * K);
        double acc = 0;
        int g = 0;
        while (g < ngrp - 1 && pos >= acc + grp_w[g]) acc += grp_w[g], g++;
        ks[i].grp = g;
        new_start(&ks[i]);
        visit(t, (u64)llabs(ks[i].E), ks[i].coef, ks[i].d);
    }
    const u64 dpmask = ((u64)1 << dpbits) - 1, cap = (u64)(capm * (double)(1ULL << dpbits)) + 16;
    const u64 limit = (u64)(40.0 * sqrt((double)W)) + 100000;
    for (u64 rd = 0; t->ops < limit; rd++) {
        for (int i = 0; i < K; i++) {
            kang *k = &ks[i];
            if (iid) {
                new_start(k);
                t->ops++;
                visit(t, (u64)llabs(k->E), k->coef, k->d);
                if (t->tcoll) { free(ks); return (double)(rd + 1) * K; }
                continue;
            }
            const u64 a = (u64)llabs(k->E);
            unsigned j = (unsigned)(mixh(a, 2) % (u64)R);
            if (a == k->prev2) j = (j + 1) % (unsigned)R;
            {   /* checkpoint every CK_PERIOD rounds; back on it = a cycle, left from its point of least tag */
                const u64 tg = mixh(a, 5);
                const int bnd = (rd & (CK_PERIOD - 1)) == 0;
                int out = 0;
                if (k->esc || tg == k->ck) {
                    if (tg == k->mn) out = 1;
                    else if (k->esc && bnd) { k->esc = 0; k->ck = k->mn = tg; }
                    else k->esc = 1;
                } else if (bnd) k->ck = k->mn = tg;
                else if (tg < k->mn) k->mn = tg;
                if (out) { j = (unsigned)R + (unsigned)(mixh(a, 6) % N_ESC); k->esc = 0; k->ck = k->mn = ~0ULL; n_esc++; }
            }
            i64 e = k->E + jumps[j];
            k->d += jumps[j];
            const u64 na = (u64)llabs(e);
            const i64 c = (mixh(na, 1) & 1) ? -(i64)na : (i64)na;
            if (c != e) { e = c; k->d = -k->d; k->coef = -k->coef; }
            k->E = e;
            k->prev2 = k->prev; k->prev = a;
            t->ops++; k->since++;
            visit(t, na, k->coef, k->d);
            if ((mixh(na, 3) & dpmask) == 0) {
                ent *f = find(tab, tmask, na + 1);
                if (!f->key) *f = (ent){na + 1, k->d, k->coef, gen};
                else if (f->kind != k->coef) {
                    const i64 dc = f->kind - k->coef, dd = k->d - f->d;
                    if (dd % dc != 0 || dd / dc != X) { fprintf(stderr, "wrong solution\n"); exit(1); }
                    if (!t->tcoll) t->tcoll = t->ops, t->nred_at = t->nred;
                    free(ks);
                    return (double)(rd + 1) * K;
                } else { new_start(k); n_merge++; visit(t, (u64)llabs(k->E), k->coef, k->d); }
                k->since = 0;
            } else if ((rd & (CK_PERIOD - 1)) == 0 && k->since > cap) { new_start(k); n_cap++; visit(t, (u64)llabs(k->E), k->coef, k->d); }
        }
    }
    free(ks);
    return -1;
}

int main(int argc, char **argv)
{
    u64 seed = 1;
    double shm = 16;
    const char *dfile = "tools/research/kang_schema3.txt", *outf = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define OPT(n, fmt, var) if (!strncmp(a, n "=", sizeof n)) { sscanf(a + sizeof n, fmt, &var); continue; }
        OPT("bits", "%d", bits) OPT("runs", "%d", runs) OPT("K", "%d", K) OPT("dp", "%d", dpbits) OPT("R", "%d", R) OPT("iid", "%d", iid)
        OPT("shadow", "%d", shadow) OPT("jm34", "%lf", jm34) OPT("jabs", "%lf", jabs) OPT("spread", "%lf", spread) OPT("pow2", "%d", pow2)
        OPT("cap", "%lf", capm) OPT("seed", "%lu", seed) OPT("shm", "%lf", shm)
        if (!strncmp(a, "design=", 7)) { dfile = a + 7; continue; }
        if (!strncmp(a, "out=", 4)) { outf = a + 4; continue; }
        fprintf(stderr, "unknown option %s\n", a);
        return 1;
    }
    if (R < 1 || R > 4096 || bits < 16 || bits > 52) { fprintf(stderr, "R in 1..4096, bits in 16..52\n"); return 1; }
    load_design(dfile);
    salt0 = seed * 0x1234567ULL;
    const double sw = sqrt(ldexp(1.0, bits));
    int b = 10;
    while ((double)(1ULL << b) < 32.0 * sw / (double)(1ULL << dpbits) + 1024) b++;
    tab = calloc(1ULL << b, sizeof(ent)); tmask = (1ULL << b) - 1;
    if (shadow) { b = 10; while ((double)(1ULL << b) < shm * sw + 4.0 * K) b++; sh = calloc(1ULL << b, sizeof(ent)); smask = (1ULL << b) - 1; }
    if (!tab || (shadow && !sh)) { fprintf(stderr, "out of memory\n"); return 1; }
    seed_rng(seed);
    int n = 0, fail = 0;
    double s[5] = {0}, q[5] = {0};
    for (int r = 0; r < runs; r++) {
        track t;
        const double c = one_run(&t);
        if (c < 0) { fail++; continue; }
        const double v[5] = {c / sw, (double)t.tcoll / sw, (double)t.ndist / sw, (double)t.nred_at / sw, (c - (double)t.tcoll) / sw};
        for (int k = 0; k < 5; k++) s[k] += v[k], q[k] += v[k] * v[k];
        if (outf) { FILE *fo = fopen(outf, "a"); if (fo) { fprintf(fo, "%.6f %.6f %.6f %.6f\n", v[0], v[1], v[2], v[3]); fclose(fo); } }
        n++;
    }
    if (!n) { printf("no trial succeeded\n"); return 1; }
    double m[5], se[5];
    for (int k = 0; k < 5; k++) { m[k] = s[k] / n; se[k] = n > 1 ? sqrt((q[k] / n - m[k] * m[k]) / (n - 1)) : 0; }
    printf("bits=%d K=%d dp=%d R=%d iid=%d trials=%d failed=%d: cost %.4f +- %.4f", bits, K, dpbits, R, iid, n, fail, m[0], se[0]);
    if (shadow) printf(" | t_coll %.4f, distinct %.4f, redundant %.4f, delay %.4f +- %.4f", m[1], m[2], m[3], m[4], se[4]);
    printf(" | per trial: merges %.1f, cycle exits %.1f, restarts without DP %.1f\n", n_merge / runs, n_esc / runs, n_cap / runs);
    return 0;
}
