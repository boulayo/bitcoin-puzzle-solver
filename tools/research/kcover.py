#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Coverage of the keys by a layout of the kangaroo (docs/en/RESEARCH.md section 8.12), in the exact pair model of kspecies.py.

lambda(x) is the rate at which pairs of points certify the key x (points drawn independently from the layout, as kwalk.c iid=1);
the expected cost is the mean over x of (sqrt(pi)/2) / sqrt(lambda(x)), in units of sqrt(W). The script splits it in three:

    ideal      every pair certifies its values inside [0, W) (no geometry): the value of the shares and residues alone
    uniform    the pairs of the layout, with their mean lambda spread evenly over x: the cost of the values that fall outside [0, W)
    layout     the real lambda(x): the rest is the unevenness over x (Jensen)

and prints lambda(x) by tenths of the interval and by residue of x.

    python3 -I tools/research/kcover.py tools/research/kang_schema3.txt [W]     (W: grid of the model, default 768; needs numpy)
"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kspecies import geometry


def load(path):
    t = open(path).read().split()
    M = int(t[0])
    rows = [(int(t[i]), int(t[i + 1]), float(t[i + 2]), float(t[i + 3]), float(t[i + 4])) for i in range(1, len(t), 5)]
    return M, rows


def coverage(M, rows, W):
    bs = [r[0] for r in rows]
    G1, G2 = geometry(bs, [(r[2], r[3]) for r in rows], W)
    w = np.array([r[4] for r in rows])
    w /= w.sum()
    xr = np.arange(W) % M
    lam = np.zeros(W)
    ideal = np.zeros(W)
    for p in range(len(rows)):
        for q in range(p, len(rows)):
            bi, bj, ri, rj = bs[p], bs[q], rows[p][1], rows[q][1]
            d1, d2 = bi - bj, bi + bj
            f = w[p] * w[q] * (0.5 if p == q else 1.0)
            if d1 != 0:
                ok = ((ri + d1 * xr - rj) % M == 0) * M
                lam += f * G1[p, q] * ok
                ideal += f * ok / (abs(d1) * W)
            if d2 != 0:
                ok = ((d2 * xr + ri + rj) % M == 0) * M
                lam += f * G2[p, q] * ok
                ideal += f * ok / (abs(d2) * W)
    return lam, ideal


if __name__ == '__main__':
    M, rows = load(sys.argv[1])
    W = int(sys.argv[2]) if len(sys.argv) > 2 else 768
    lam, ideal = coverage(M, rows, W)
    k = np.sqrt(np.pi) / 2
    cost = lambda l: float(np.mean(k / np.sqrt(l)) / np.sqrt(W))
    print('ideal %.4f   uniform %.4f   layout %.4f   (mean lambda*W: ideal %.4f, layout %.4f)'
          % (cost(ideal), k / np.sqrt(lam.mean() * W), cost(lam), ideal.mean() * W, lam.mean() * W))
    for t in range(10):
        s = lam[t * W // 10:(t + 1) * W // 10] * W
        print('  x in [%.1f, %.1f) W: lambda*W mean %.3f, min %.3f, max %.3f' % (t / 10, (t + 1) / 10, s.mean(), s.min(), s.max()))
    print('  by residue of x mod %d: %s' % (M, ' '.join('%.3f' % (lam[r::M].mean() * W) for r in range(M))))
