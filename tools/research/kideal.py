#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The ideal pair model without geometry (docs/en/RESEARCH.md section 8.14): what a family of coefficients and a modulus can
give at best, before the zones take their share.

Species (b, r): walkers with E = b x + u, u = r (mod M), jumps multiples of M. A pair of species certifies x through
(b - b') x = u' - u and (b + b') x = -(u + u'); with d | M the pairs are all integer when d | (r' - r) (or d | (r + r')),
on the d residues of x mod M that solve the congruence, each worth M / d; on the other residues nothing. lambda(x) is the
sum over the pairs with their shares, the cost is the mean over x mod M of (sqrt(pi)/2) / sqrt(lambda(x)): the residues decide
both the mean value of the pairs (a cross pair is worth 2 at most, a pair of one coefficient 1, a tame pair 0) and how
evenly the residues of x receive them (the Jensen penalty). The shares of all (coefficient, residue) pairs are optimized by
exponentiated gradient from random starts; the mixture bound of bounds.py (c = 1 - a0^2 - sum of the wild shares squared / 2
when every pair is worth its maximum) is printed beside it.

    python3 -I tools/research/kideal.py 0,1,3 12 [starts]         the family of schema 3: 0.9908 (ALGORITHMS.md 8.10)
    python3 -I tools/research/kideal.py 0,1,3,5 120 [starts]      a fourth species
    python3 -I tools/research/kideal.py 0,1,3,5 120 1 seed        one start from the solution of schema 3's family (12 | M), the
                                                                  other coefficients at a trace: the first-order test at that optimum
At the best point the script prints, for every coefficient, the lowest gradient of its classes against the gradient shared by the
active classes: a coefficient whose best class has a lower gradient would take share. Needs numpy; minutes per start.
"""
import sys
import numpy as np


def pair_tensor(bs, M):
    combos = [(s, r) for s in range(len(bs)) for r in range(M)]
    n = len(combos)
    xr = np.arange(M)
    A = np.zeros((n, n, M))
    for i, (si, ri) in enumerate(combos):
        for j, (sj, rj) in enumerate(combos):
            if j < i:
                continue
            bi, bj = bs[si], bs[sj]
            d1, d2 = bi - bj, bi + bj
            v = np.zeros(M)
            if d1 != 0:
                v += ((ri + d1 * xr - rj) % M == 0) * (M / abs(d1))
            if d2 != 0:
                v += ((d2 * xr + ri + rj) % M == 0) * (M / abs(d2))
            A[i, j] = v * (0.5 if i == j else 1.0)
    return combos, A


def cost(w, A):
    lam = np.einsum('i,ijx,j->x', w, A, w)
    if lam.min() <= 0:
        return 9.0, lam
    return float(np.mean(np.sqrt(np.pi) / 2 / np.sqrt(lam))), lam


def eg(w, A, As, iters=3000):
    c, lam = cost(w, A)
    eta = 0.3
    for it in range(iters):
        g = -0.25 * np.sqrt(np.pi) * np.einsum('x,ijx,j->i', np.maximum(lam, 1e-12) ** -1.5, As, w) / A.shape[2]
        w2 = w * np.exp(-eta * g / np.abs(g).max())
        w2 /= w2.sum()
        c2, lam2 = cost(w2, A)
        if c2 < c:
            w, c, lam = w2, c2, lam2
            eta = min(eta * 1.2, 3)
        else:
            eta *= 0.5
        if eta < 1e-7:
            break
    return c, w


def mixture_bound(bs):
    """best c = sum of pair shares times their maximum value (2 cross, 1 same wild, 0 tame), and its cost"""
    best = (0, None)
    k = len(bs)
    rng = np.random.default_rng(1)
    V = np.array([[0.0 if (bi == 0 and bj == 0) else (1.0 if bi == bj else 2.0) for bj in bs] for bi in bs])
    for _ in range(200):
        a = rng.random(k)
        a /= a.sum()
        for _ in range(3000):
            g = V @ a
            a = a * g / (a @ g)
            a /= a.sum()
        c = 0.5 * a @ V @ a
        if c > best[0]:
            best = (c, a.copy())
    return best


if __name__ == '__main__':
    bs = [int(v) for v in sys.argv[1].split(',')]
    M = int(sys.argv[2])
    starts = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    cmax, amax = mixture_bound(bs)
    print('family %s mod %d: every pair at its maximum value gives c = %.4f, cost %.4f, shares %s'
          % (bs, M, cmax, np.sqrt(np.pi) / 2 / np.sqrt(cmax), np.round(amax, 3)))
    combos, A = pair_tensor(bs, M)
    As = A + np.transpose(A, (1, 0, 2))
    rng = np.random.default_rng(7)
    best = (9.0, None)
    seed3 = len(sys.argv) > 4 and sys.argv[4] == 'seed' and M % 12 == 0
    for s in range(starts):
        if seed3:
            # the solution of {0, 1, 3} mod 12 (tame on 3 and 9, wild on Q' on 2, 6, 10, wild on 3Q' on 6), spread over the classes mod M
            pat = {0: {3: 0.181, 9: 0.019}, 1: {2: 0.4 / 3, 6: 0.4 / 3, 10: 0.4 / 3}, 3: {6: 0.4}}
            w = np.full(len(combos), 1e-4)
            for i, (si, r) in enumerate(combos):
                w[i] += pat.get(bs[si], {}).get(r % 12, 0.0) / (M // 12)
        else:
            w = rng.random(len(combos)) ** 3
        w /= w.sum()
        c, w = eg(w, A, As)
        tot = [w[[i for i, (si, r) in enumerate(combos) if si == k]].sum() for k in range(len(bs))]
        print('  start %2d: cost %.4f, shares by coefficient %s' % (s, c, ' '.join('%d: %.3f' % (b, t) for b, t in zip(bs, tot))))
        sys.stdout.flush()
        if c < best[0]:
            best = (c, w)
    c, w = best
    _, lam = cost(w, A)
    print('BEST %.4f (mean lambda %.4f, min %.4f, max %.4f over the residues of x)' % (c, lam.mean(), lam.min(), lam.max()))
    # first-order test: at an optimum every active class has the same gradient (the multiplier of the simplex); a class with a
    # lower gradient would take share. By coefficient: the lowest gradient of its classes against the gradient of the active ones
    g = -0.25 * np.sqrt(np.pi) * np.einsum('x,ijx,j->i', lam ** -1.5, As, w) / M
    act = w > 1e-6
    ga = g[act].mean()
    for k, b in enumerate(bs):
        idx = np.array([i for i, (si, r) in enumerate(combos) if si == k])
        print('  coefficient %d: share %.3f, lowest gradient of its classes %.4f against %.4f of the active classes (%s)'
              % (b, w[idx].sum(), g[idx].min(), ga, 'would take share' if g[idx].min() < ga - 1e-3 * abs(ga) else 'stable'))
    for (s, r), x in sorted(zip(combos, w), key=lambda t: (t[0][0], t[0][1])):
        if x > 0.003:
            print('   b=%d r=%3d %.3f' % (bs[s], r, x))
