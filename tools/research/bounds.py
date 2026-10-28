#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounds of docs/en/RESEARCH.md section 8.7: counting of the pairs of elements that certify the unknown x of an interval-DLP search.

Elements are classes {E, -E} with E = b*x + a. Two of them collide in two ways, E1 = E2 and E1 = -E2, which give x = (a2-a1)/(b1-b2)
and x = -(a1+a2)/(b1+b2); each is an integer with probability 1/|delta| unless the offsets are chosen to make it one (the 'even distance'
trick for b = +-1: wild-wild pairs then give 1 value). A pair of a tame element (b = 0) and a wild one (b = 1) gives 2 values, always integers.
Types: b = 0 tame, b = 1 wild, b = 2, 3, ... walkers on 2x, 3x, ...

  cover(n) = c * n^2 / W   values of x certified after n elements, c from the mixture of types that maximises it
  structured coverage (no overlaps, as in baby-step giant-step):  E = (2/3) / sqrt(c)       sqrt(W)
  random coverage (independent pairs, Poisson):                   E = (sqrt(pi)/2) / sqrt(c)  sqrt(W)

Run: python3 tools/research/bounds.py     (needs numpy)"""
import numpy as np


def value(bi, bj, same_pair_exact):
    """x values certified by one pair of elements of types bi, bj."""
    if bi == bj:
        return 0.0 if bi == 0 else (1.0 if same_pair_exact else 1.0 / (2 * bi))
    v = 1.0 / abs(bi - bj)
    if bi + bj > 0:
        v += 1.0 / (bi + bj)
    return v


def best_mixture(types, same_pair_exact, all_pairs_two=False):
    m = len(types)
    V = np.zeros((m, m))
    for i, bi in enumerate(types):
        for j, bj in enumerate(types):
            V[i, j] = 2.0 if (all_pairs_two and bi != bj) else value(bi, bj, same_pair_exact)
    best_c, best_w = -1.0, None
    rng = np.random.default_rng(1)
    for _ in range(40):
        w = rng.random(m)
        w /= w.sum()
        for _ in range(4000):                       # replicator iteration: maximises w'Vw/2 on the simplex
            g = V @ w
            w = w * g / (w @ g) if w @ g > 0 else w
            w /= w.sum()
        c = 0.5 * (w @ (V @ w))
        if c > best_c:
            best_c, best_w = c, w.copy()
    return best_c, best_w


def trapz(y, x):
    return np.trapezoid(y, x) if hasattr(np, 'trapezoid') else np.trapz(y, x)


def interleaved_bsgs(T, interleave):
    """BSGS with +-: t baby steps (memory T at most), g giants, coverage 2 t g; babies first or interleaved (t = g until T)."""
    n = np.linspace(0, 4, 400001)
    t = np.minimum(n / 2 if interleave else n, T)
    return trapz(1 - np.minimum(2 * t * (n - t), 1), n)


if __name__ == '__main__':
    print('mixture of walker types (c = coverage coefficient; costs in units of sqrt(W))')
    for types in ([0, 1], [0, 1, 2, 3, 4, 5]):
        for exact, name in ((True, 'wild-wild pairs worth 1 (even distances)'), (False, 'wild-wild pairs worth 1/(2b) (random offsets)')):
            c, w = best_mixture(types, exact)
            print('  types %-22s %-46s c=%.4f  structured %.4f  random %.4f  weights %s'
                  % (types, name, c, (2 / 3) / np.sqrt(c), (np.sqrt(np.pi) / 2) / np.sqrt(c), np.round(w, 3)))
    c, _ = best_mixture(list(range(6)), True, True)
    print('  every pair of different types worth 2 values (the old counting bound, 6 types): structured %.4f (-> 0.6667 with unboundedly many)' % ((2 / 3) / np.sqrt(c)))
    print('classic kangaroo with +-: tame + wild only, c = 1/2: random coverage %.4f = sqrt(pi/2)' % ((np.sqrt(np.pi) / 2) / np.sqrt(0.5)))
    print('baby-step giant-step with +-, total memory M (entries, in sqrt(W)); interleaved keeps the giants too, so its babies are at most M/2:')
    for M in (0.25, 0.35, 0.5, 0.7071, 1.0, 1.4142, 2.0):
        bf = min(interleaved_bsgs(T, False) for T in np.linspace(0.05, M, 60))
        il = min(interleaved_bsgs(T, True) for T in np.linspace(0.05, M / 2, 60))
        print('  M=%.4f  babies first %.4f   interleaved %.4f' % (M, bf, il))
