#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Shares of the bands that change with the progress of the search, in the exact pair model of kspecies.py
(docs/en/RESEARCH.md section 8.14).

The static layout minimizes the mean over the keys x of (sqrt(pi)/2) / sqrt(lambda(x)); the unevenness of lambda(x) costs the
Jensen term of kcover.py (layout against uniform). The only information a running search gains is the time elapsed without a
collision, so the most an "adaptive" herd can do is follow a schedule: the shares w(n) of the bands depend on the points n laid
so far. With N_p(n) = integral of w_p over [0, n] (points of band p), the pairs that certify x are
Lambda(x, n) = sum_{p<=q} N_p N_q A_pq(x) and the expected cost is the mean over x of the integral of exp(-Lambda(x, n)) dn.
The schedule is piecewise constant over E epochs of the progress (in units of sqrt(W)); every epoch is optimized by
exponentiated gradient (the analytic gradient of the cost), starting from the static shares.

    python3 -I tools/research/ksched.py tools/research/kang_schema3.txt [W] [E] [iters]      (W grid 768, E epochs 6, 300 iterations)

prints the cost of the static layout computed by the same integral (a check against kcover.py), the optimized schedule's cost
on its grid and on a grid twice as fine, and the shares of the three species in every epoch. Needs numpy; a few minutes.
"""
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kspecies import geometry
from kcover import load

NMAX, NSTEP = 4.0, 0.02          # the integral over the progress n, in units of sqrt(W)


def pair_rates(M, rows, W):
    """A[p, q, x] for p <= q: certifying rate of a pair of points of bands p and q (1/2 on the diagonal), per W."""
    bs = [r[0] for r in rows]
    G1, G2 = geometry(bs, [(r[2], r[3]) for r in rows], W)
    xr = np.arange(W) % M
    n = len(rows)
    A = np.zeros((n, n, W))
    for p in range(n):
        for q in range(p, n):
            bi, bj, ri, rj = bs[p], bs[q], rows[p][1], rows[q][1]
            d1, d2 = bi - bj, bi + bj
            v = np.zeros(W)
            if d1 != 0:
                v += G1[p, q] * (((ri + d1 * xr - rj) % M == 0) * M)
            if d2 != 0:
                v += G2[p, q] * (((d2 * xr + ri + rj) % M == 0) * M)
            A[p, q] = v * (0.5 if p == q else 1.0)
    return A * W                  # lambda in units of 1/W: n in units of sqrt(W) gives Lambda = lambda*W * n^2


def schedule_points(w, edges, ns):
    """N[p, k]: points of band p laid before progress ns[k], for the shares w[e, p] of the epochs [edges[e], edges[e+1])."""
    E = w.shape[0]
    N = np.zeros((w.shape[1], len(ns)))
    for e in range(E):
        dur = np.clip(ns - edges[e], 0, edges[e + 1] - edges[e])
        N += np.outer(w[e], dur)
    return N


def cost_and_grad(w, A, As, edges, ns, want_grad=True):
    N = schedule_points(w, edges, ns)                       # (P, K)
    Lam = np.einsum('ik,ijx,jk->kx', N, A, N)               # (K, X)
    S = np.exp(-Lam)                                        # survival of every x at every n
    cost = float(S.mean(axis=1).sum() * NSTEP)
    if not want_grad:
        return cost, None
    # d cost / d N_p(k) = -NSTEP * mean_x S(k, x) * (As N)_p(k, x), As the symmetrized rates
    G = -NSTEP * np.einsum('kx,ijx,jk->ik', S, As, N) / A.shape[2]   # (P, K)
    grad = np.zeros_like(w)
    for e in range(w.shape[0]):
        dur = np.clip(ns - edges[e], 0, edges[e + 1] - edges[e])
        grad[e] = G @ dur
    return cost, grad


def optimize(w0, A, edges, ns, iters):
    As = A + np.transpose(A, (1, 0, 2))
    w = w0.copy()
    c, g = cost_and_grad(w, A, As, edges, ns)
    eta = 0.3
    for it in range(iters):
        w2 = w * np.exp(-eta * g / np.abs(g).max())
        w2 /= w2.sum(axis=1, keepdims=True)
        c2, g2 = cost_and_grad(w2, A, As, edges, ns)
        if c2 < c:
            w, c, g = w2, c2, g2
            eta = min(eta * 1.2, 3)
        else:
            eta *= 0.5
        if eta < 1e-7:
            break
    return c, w


if __name__ == '__main__':
    M, rows = load(sys.argv[1])
    W = int(sys.argv[2]) if len(sys.argv) > 2 else 768
    E = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    iters = int(sys.argv[4]) if len(sys.argv) > 4 else 300
    ns = np.arange(NSTEP / 2, NMAX, NSTEP)
    A = pair_rates(M, rows, W)
    As = A + np.transpose(A, (1, 0, 2))
    wst = np.array([r[4] for r in rows])
    wst /= wst.sum()
    edges1 = np.array([0.0, NMAX])
    c_static, _ = cost_and_grad(wst[None, :], A, As, edges1, ns, False)
    print('static layout: cost %.4f by the integral over n (kcover.py: mean of k/sqrt(lambda))' % c_static)
    # epochs: finer at the start, where most of the probability mass of the collision is
    edges = np.concatenate([np.linspace(0, 1.6, E, endpoint=False), [NMAX]])
    w0 = np.repeat(wst[None, :], E, axis=0)
    c, w = optimize(w0, A, edges, ns, iters)
    print('schedule of %d epochs (edges %s): cost %.4f (%+.2f %%)' % (E, ' '.join('%.2f' % e for e in edges), c, 100 * (c / c_static - 1)))
    A2 = pair_rates(M, rows, 2 * W)
    As2 = A2 + np.transpose(A2, (1, 0, 2))
    c2s, _ = cost_and_grad(wst[None, :], A2, As2, edges1, ns, False)
    c2, _ = cost_and_grad(w, A2, As2, edges, ns, False)
    print('on the grid of %d points: static %.4f, schedule %.4f (%+.2f %%)' % (2 * W, c2s, c2, 100 * (c2 / c2s - 1)))
    bs = np.array([r[0] for r in rows])
    for e in range(E):
        sh = ' '.join('c=%d %.3f' % (b, w[e][bs == b].sum()) for b in sorted(set(bs)))
        print('  epoch [%.2f, %.2f): %s' % (edges[e], edges[e + 1], sh))
    if len(sys.argv) > 5:
        with open(sys.argv[5], 'w') as f:
            for e in range(E):
                f.write('# epoch %.3f %.3f\n' % (edges[e], edges[e + 1]))
                for r, x in zip(rows, w[e]):
                    f.write('%d %d %.5f %.5f %.5f\n' % (r[0], r[1], r[2], r[3], x))
