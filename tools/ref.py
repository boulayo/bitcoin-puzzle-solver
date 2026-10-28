#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent pure-Python secp256k1 / Bitcoin reference used to generate and
cross-check test vectors for pzsolve. It shares no code with the C sources."""
import hashlib, sys

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
GX = 0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798
GY = 0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8

def inv(a, m=P):
    return pow(a, -1, m)

def add(p, q):
    if p is None: return q
    if q is None: return p
    if p[0] == q[0]:
        if (p[1] + q[1]) % P == 0: return None
        l = 3 * p[0] * p[0] * inv(2 * p[1]) % P
    else:
        l = (q[1] - p[1]) * inv(q[0] - p[0]) % P
    x = (l * l - p[0] - q[0]) % P
    return (x, (l * (p[0] - x) - p[1]) % P)

def mul(k, p=(GX, GY)):
    r = None
    while k:
        if k & 1: r = add(r, p)
        p = add(p, p)
        k >>= 1
    return r

def pub33(k):
    x, y = mul(k)
    return bytes([2 + (y & 1)]) + x.to_bytes(32, 'big')

# RIPEMD-160 in pure Python: OpenSSL 3 builds of hashlib (for example Python 3.10 on Ubuntu 22.04) no longer provide it
_RL = [list(range(16)), [7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8], [3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12],
       [1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2], [4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13]]
_RR = [[5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12], [6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2],
       [15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13], [8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14],
       [12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11]]
_SL = [[11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8], [7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12],
       [11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5], [11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12],
       [9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6]]
_SR = [[8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6], [9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11],
       [9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5], [15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8],
       [8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11]]
_KL = [0x00000000, 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xA953FD4E]
_KR = [0x50A28BE6, 0x5C4DD124, 0x6D703EF3, 0x7A6D76E9, 0x00000000]

def _rmd_f(j, x, y, z):
    return [x ^ y ^ z, (x & y) | (~x & z), (x | ~y) ^ z, (x & z) | (y & ~z), x ^ (y | ~z)][j] & 0xFFFFFFFF

def _rol(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF

def ripemd160(data):
    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0]
    m = data + b'\x80' + b'\x00' * ((55 - len(data)) % 64) + (8 * len(data)).to_bytes(8, 'little')
    for off in range(0, len(m), 64):
        x = [int.from_bytes(m[off + 4 * i:off + 4 * i + 4], 'little') for i in range(16)]
        al, bl, cl, dl, el = h
        ar, br, cr, dr, er = h
        for r in range(5):
            for i in range(16):
                t = (_rol((al + _rmd_f(r, bl, cl, dl) + x[_RL[r][i]] + _KL[r]) & 0xFFFFFFFF, _SL[r][i]) + el) & 0xFFFFFFFF
                al, el, dl, cl, bl = el, dl, _rol(cl, 10), bl, t
                t = (_rol((ar + _rmd_f(4 - r, br, cr, dr) + x[_RR[r][i]] + _KR[r]) & 0xFFFFFFFF, _SR[r][i]) + er) & 0xFFFFFFFF
                ar, er, dr, cr, br = er, dr, _rol(cr, 10), br, t
        t = (h[1] + cl + dr) & 0xFFFFFFFF
        h[1] = (h[2] + dl + er) & 0xFFFFFFFF
        h[2] = (h[3] + el + ar) & 0xFFFFFFFF
        h[3] = (h[4] + al + br) & 0xFFFFFFFF
        h[4] = (h[0] + bl + cr) & 0xFFFFFFFF
        h[0] = t
    return b''.join(v.to_bytes(4, 'little') for v in h)

def h160(b):
    return ripemd160(hashlib.sha256(b).digest())

B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58check(payload):
    d = payload + hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    n = int.from_bytes(d, 'big'); s = ''
    while n: n, r = divmod(n, 58); s = B58[r] + s
    return '1' * (len(d) - len(d.lstrip(b'\0'))) + s

def address(k): return b58check(b'\x00' + h160(pub33(k)))
def wif(k): return b58check(b'\x80' + k.to_bytes(32, 'big') + b'\x01')

if __name__ == '__main__':
    for k in map(lambda s: int(s, 0), sys.argv[1:]):
        x, y = mul(k)
        print(f"key  {k:#x}\npub  {pub33(k).hex()}\nh160 {h160(pub33(k)).hex()}\naddr {address(k)}\nwif  {wif(k)}\nx    {x:064x}\ny    {y:064x}")
