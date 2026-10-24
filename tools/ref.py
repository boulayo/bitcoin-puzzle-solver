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

def h160(b):
    return hashlib.new('ripemd160', hashlib.sha256(b).digest()).digest()

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
