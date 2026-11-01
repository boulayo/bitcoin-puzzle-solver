#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The endomorphism of secp256k1 against an interval of exponents (docs/en/RESEARCH.md section 8.14).

phi(x, y) = (beta x, y) is multiplication by lambda, a cube root of 1 mod n: a point P of exponent e gives lambda e mod n at the
cost of one field product. The classes {+-P, +-phi(P), +-phi^2(P)} have 6 elements, and a collision of a walker of exponent
b x + u with phi^i of another certifies x only if lambda^i (b' x + u') mod n is again a number of the interval. This script
counts, for the widths of the open puzzles, the exponents x in [0, W) whose image lambda x mod n lies in (-W, W): the lattice
{(x, lambda x mod n)} has determinant n, so a window of width W on both coordinates holds about 2 W^2 / n of its points; the
same count with the interval moved to [b, b + W) (the image is lambda b + lambda x, a translation) and with the shortest vector
of the lattice (the first x > 0 whose image is small, from the Gauss reduction).

    python3 -I tools/research/klambda.py
"""
n = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
lam = 0x5363AD4CC05C30E0A5261C028812645A122E22EA20816678DF02967C1B23BD72
beta = 0x7AE96A2B657C07106E64479EAC3434E99CF0497512F58995C1396C28719501EE
p = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F


def gauss_reduce(u, v):
    """shortest basis of the lattice spanned by u and v (integer Gauss reduction)"""
    while True:
        if u[0] * u[0] + u[1] * u[1] > v[0] * v[0] + v[1] * v[1]:
            u, v = v, u
        m = round((u[0] * v[0] + u[1] * v[1]) / (u[0] * u[0] + u[1] * u[1]))
        if m == 0:
            return u, v
        v = (v[0] - m * u[0], v[1] - m * u[1])


if __name__ == '__main__':
    assert pow(lam, 3, n) == 1 and pow(beta, 3, p) == 1
    u, v = gauss_reduce((1, lam), (0, n))
    print('lattice {(x, lambda x mod n)}: determinant n = 2^%.1f, reduced basis of lengths 2^%.1f and 2^%.1f'
          % ((n).bit_length() - 1 + 0.0, (u[0] * u[0] + u[1] * u[1]).bit_length() / 2, (v[0] * v[0] + v[1] * v[1]).bit_length() / 2))
    print('  shortest vector: x = %d (2^%.1f), lambda x mod n = %d (2^%.1f)' % (abs(u[0]), abs(u[0]).bit_length(), (lam * u[0]) % n if (lam * u[0]) % n < n // 2 else (lam * u[0]) % n - n, abs(u[1]).bit_length()))
    print('exponents x in [0, W) whose image lambda x mod n lies in (-W, W): about 2 W^2 / n, and the image of the rest is a 256-bit number')
    for bits in range(134, 140):
        W = 1 << bits
        print('  puzzle %d, W = 2^%d: %.1e of 2^%d (one in 2^%.1f)' % (bits + 1, bits, 2.0 * W * W / n, bits, bits - (2 * bits + 1 - 256)))
    print('the images of an interval [b, b + W) are lambda b + the images above: a translation moves the window, not the count')
    print('smallest W for which the image of some x < W is itself below W (W^2 of the order of n): 2^%.1f, the length of the shortest vector'
          % ((u[0] * u[0] + u[1] * u[1]).bit_length() / 2))
    # the GLV decomposition of a short exponent k: k = k1 + k2 lambda with k1, k2 of 128 bits does not shorten the search
    for bits in (134, 139):
        k = (1 << bits) - 12345
        # k1 + k2 lambda = k mod n, with (k1, k2) the vector of the lattice {(a, b): a + b lambda = k} closest to the origin: Babai on (u, v)
        # here u = (x1, y1) means x1 + ... we use the basis of {(a, b): a + b lambda = 0 mod n}: (n, 0), (-lambda, 1)
        a1, a2 = gauss_reduce((n, 0), (-lam, 1))
        # express (k, 0) in that basis and round
        det = a1[0] * a2[1] - a1[1] * a2[0]
        c1 = round((k * a2[1]) / det)
        c2 = round((-k * a1[1]) / det)
        k1 = k - c1 * a1[0] - c2 * a2[0]
        k2 = -c1 * a1[1] - c2 * a2[1]
        assert (k1 + k2 * lam - k) % n == 0
        print('GLV of k = 2^%d - 12345: k1 of %d bits, k2 of %d bits (two unknowns of 128 bits for one of %d: no shorter search)'
              % (bits, abs(k1).bit_length(), abs(k2).bit_length(), bits))
