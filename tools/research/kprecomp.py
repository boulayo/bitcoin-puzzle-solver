#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A table of distinguished points of tame walks prepared before the search (Bernstein-Lange precomputation) against the
kangaroo of schema 3, at the scale of the open puzzles (docs/en/RESEARCH.md section 8.14; the first pricing is in 8.9).

Two models of the online cost of one key, in units of sqrt(W), with P tame points laid before the search (P in units of
sqrt(W)), T entries of RAM (one per tame trail) and K wild walkers:
  * pairs: every tame-wild pair certifies two keys, every wild-wild pair one (the even-distance trick), the pairs are random:
    online = integral of exp(-(2 P n + n^2 / 2)) dn + delay, the delay K * L / sqrt(W) with L = P sqrt(W) / T the length of a trail
    (a wild walker that lands on a tame trail walks to its end before the match is seen, and the whole herd walks meanwhile).
    This is the floor of any table of tame trails: it counts every tame point as useful (no merged trails, no trail cut short).
  * Bernstein-Lange as measured: precomputation 1.21 sqrt(W T), online 1.93 sqrt(W / T) (their interval experiments, without the
    negation map; blsim.c found 2.3 to 2.6 W for the product on the abstract model, so 1.21 x 1.93 = 2.34 W is taken as their best).
The precomputation itself costs P sqrt(W) steps at least (the pairs model) or 1.21 sqrt(W T) (Bernstein-Lange), once per width W;
a table for width W serves every narrower interval at the online cost of W.

    python3 -I tools/research/kprecomp.py [ram_gib] [bytes_per_entry] [walkers_log2]
prints, for puzzles 135 to 140 (W = 2^134 to 2^139): the table size, the precomputation in kangaroo units (1.04 sqrt(W) is one
kangaroo search), the online cost per key in the two models, and the number of keys of the same width from which the
precomputation pays. Needs numpy.
"""
import sys
import numpy as np
from math import sqrt, exp, log2

KANG = 1.04                      # cost of one kangaroo search of schema 3 at the scale of the puzzles, in sqrt(W) (8.12)


def online_pairs(P, dn=1e-4, nmax=8.0):
    """integral of exp(-(2 P n + n^2 / 2)) dn: wild steps until the key, P tame points laid before, both in sqrt(W)."""
    n = np.arange(dn / 2, nmax, dn)
    return float(np.sum(np.exp(-(2 * P * n + n * n / 2))) * dn)


def table(ram_gib=64.0, entry=16, kbits=20):
    T = ram_gib * 2 ** 30 / entry
    print('table of %.0f GiB at %d bytes per entry: T = 2^%.1f trails; %d wild walkers' % (ram_gib, entry, log2(T), 1 << kbits))
    print('%-7s %-9s %-24s %-32s %-26s %s' % ('puzzle', 'sqrt(W)', 'Bernstein-Lange (1.21/1.93)', 'pairs floor, P tame points (sqrt W)',
                                             'online, pairs floor', 'keys to pay off'))
    for bits in range(134, 140):
        W = 2.0 ** bits
        sW = sqrt(W)
        pre_bl = 1.21 * sqrt(W * T) / sW                    # in sqrt(W)
        on_bl = 1.93 * sqrt(W / T) / sW
        # the pairs floor: P chosen so that the trail length L = P sqrt(W) / T keeps the delay K L / sqrt(W) at 1 % of sqrt(W)
        K = 1 << kbits
        parts = []
        for P in (1.0, 4.0, 16.0):
            L = P * sW / T
            delay = K * L / sW
            on = online_pairs(P) + delay
            parts.append((P, L, on))
        # keys m of the same width from which m kangaroo searches cost more than the precomputation plus m online searches
        def payoff(pre, on):
            return pre / (KANG - on) if on < KANG else float('inf')
        print('%-7d 2^%-6.1f pre %8.0f x kang, online %.2e' % (bits + 1, bits / 2, pre_bl / KANG, on_bl), end='   ')
        print(' '.join('P=%2.0f: L=2^%4.1f online %.3f' % (P, log2(L), on) for P, L, on in parts), end='   ')
        print('BL %.0f keys; P=1: %.1f, P=4: %.1f, P=16: %.1f' % (payoff(pre_bl, on_bl), payoff(1.0, parts[0][2]), payoff(4.0, parts[1][2]),
                                                                 payoff(16.0, parts[2][2])))
    print()
    print('one key, tame points first (pairs floor, no delay): P + online(P) for P in sqrt(W):')
    for P in (0.0, 0.1, 0.2, 0.3, 0.5, 1.0, 2.0):
        on = online_pairs(P)
        print('  P = %.1f: online %.4f, total %.4f' % (P, on, P + on))
    print('(the simultaneous herd with a third of tame walkers and the same pair values costs %.4f, and schema 3 %.3f)'
          % (sqrt(np.pi) / 2 / sqrt(2.0 / 3), KANG))
    print()
    print('several puzzles served by one table of the widest (W = 2^139), Bernstein-Lange: total against separate kangaroos')
    tot_k = sum(KANG * sqrt(2.0 ** b) for b in range(135, 140))
    pre = 1.21 * sqrt(2.0 ** 139 * T)
    on = 1.93 * sqrt(2.0 ** 139 / T)
    print('  puzzles 136 to 140: kangaroos 2^%.2f steps; precomputation 2^%.2f plus 5 online searches 2^%.2f'
          % (log2(tot_k), log2(pre), log2(5 * on)))
    multi_target()


def multi_target():
    """one tame herd shared by the wild herds of puzzles 136 to 140 (widths 2^135 to 2^139), counting pairs deterministically:
    target i is found when 2 n_T n_i + n_i^2 / 2 = W_i (tame-wild pairs worth two keys, wild-wild of one target one key,
    wild-wild of two targets nothing: one equation, two unknowns). Units: sqrt(W_140)."""
    ws = [2.0 ** (b - 139) for b in range(135, 140)]
    def total(a):
        return a + sum(-2 * a + sqrt(4 * a * a + 2 * w) for w in ws)
    grid = np.linspace(0.01, 3, 30000)
    tots = np.array([total(a) for a in grid])
    i = int(tots.argmin())
    single = sum(3 * sqrt(w / 6) for w in ws)                  # the same count for one target alone: 1.2247 sqrt(W_i) each
    print()
    print('one tame herd for puzzles 136 to 140 (deterministic pairs, units of sqrt(W_140) = 2^69.5):')
    print('  separate searches %.3f; shared tame herd of %.3f and wild herds %s: total %.3f (%.0f %% of the separate searches)'
          % (single, grid[i], ' '.join('%.3f' % (-2 * grid[i] + sqrt(4 * grid[i] ** 2 + 2 * w)) for w in ws), tots[i], 100 * tots[i] / single))
    # the first key (puzzle 136): with the shared herd its wild walkers are a share of the whole, so it comes later in wall time
    a = grid[i]
    n136 = -2 * a + sqrt(4 * a * a + 2 * ws[0])
    frac = n136 / (tots[i] - a)                             # share of the wild steps that belong to puzzle 136
    print('  puzzle 136 alone: %.4f; in the shared campaign its collision comes after %.3f wild steps of its own, laid at a share %.2f of'
          ' the herd while the tame herd takes %.0f %%: wall time about %.1f times its own search' % (3 * sqrt(ws[0] / 6), n136, frac,
          100 * a / tots[i], (a + n136 / frac) / (3 * sqrt(ws[0] / 6))))


if __name__ == '__main__':
    ram = float(sys.argv[1]) if len(sys.argv) > 1 else 64.0
    entry = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    kb = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    table(ram, entry, kb)
