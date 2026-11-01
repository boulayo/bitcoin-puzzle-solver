#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""Writes seal_vectors.h: the expected values of the tests of the key derivation functions and of the seal (seal.h). Computed here with
Python's own integers and hashlib (scrypt and PBKDF2 of OpenSSL, HMAC of the hmac module), by an implementation of secp256k1 and of the
ECIES construction that shares no code with the C library. The vectors published in the RFCs (4231, 5869, 7914) are written by hand in the C
tests; this file checks the ones it embeds against hashlib first, so a mistake in this file is caught before it reaches the tests.
usage: python3 gen_seal_vectors.py > seal_vectors.h"""
import hashlib, hmac, sys

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)

def add(p, q):
    if p is None: return q
    if q is None: return p
    if p[0] == q[0]:
        if (p[1] + q[1]) % P == 0: return None
        l = 3 * p[0] * p[0] * pow(2 * p[1], -1, P) % P
    else:
        l = (q[1] - p[1]) * pow(q[0] - p[0], -1, P) % P
    x = (l * l - p[0] - q[0]) % P
    return (x, (l * (p[0] - x) - p[1]) % P)

def mul(k, p=G):
    r = None
    while k:
        if k & 1: r = add(r, p)
        p = add(p, p)
        k >>= 1
    return r

def pub33(p): return bytes([2 + (p[1] & 1)]) + p[0].to_bytes(32, 'big')

# RIPEMD-160 in pure Python (the same code as gen_vectors.py, copied so that each generator stands alone)
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

def sha256(b): return hashlib.sha256(b).digest()
def sha256d(b): return sha256(sha256(b))
def hash160(b): return ripemd160(sha256(b))

B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58check(payload):
    d = payload + sha256d(payload)[:4]
    n = int.from_bytes(d, 'big'); s = ''
    while n: n, r = divmod(n, 58); s = B58[r] + s
    return '1' * (len(d) - len(d.lstrip(b'\0'))) + s

SALT = b'pzsolve-found-v1'

def hkdf(salt, ikm, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    t, okm = b'', b''
    for i in range(1, (length + 31) // 32 + 1):
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
    return okm[:length]

def seal(qpoint, plain, r):
    rp = mul(r)
    s = mul(r, qpoint)
    okm = hkdf(SALT, s[0].to_bytes(32, 'big'), pub33(rp) + pub33(qpoint), 64)
    ct = bytes(a ^ b for a, b in zip(plain, okm[:32]))
    tag = hmac.new(okm[32:], pub33(rp) + ct, hashlib.sha256).digest()[:16]
    return bytes([1]) + hash160(pub33(qpoint))[:4] + pub33(rp) + ct + tag

def key_from_password(pw, log2n):
    dk = hashlib.scrypt(pw, salt=SALT, n=1 << log2n, r=8, p=1, dklen=32, maxmem=1 << 30)
    return int.from_bytes(dk, 'big') % (N - 1) + 1

out = []
def emit(s=''): out.append(s)
def cstr(b): return '"' + b.replace('\\', '\\\\').replace('"', '\\"') + '"'

emit('/* SPDX-License-Identifier: AGPL-3.0-or-later */')
emit('/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */')
emit('/* Generated by gen_seal_vectors.py (independent Python implementation: integers, hashlib, hmac); do not edit. */')
emit('#define SVEC static const __attribute__((unused))')

# k * P for points other than G
emit('SVEC struct { const char *p_hex, *k_hex, *r_hex; } MUL_VEC[] = {')
pts = [(1, 3), (2, N - 1), (7, 0xdeadbeefcafe), (0x123456789abcdef, 0xfedcba9876543210fedcba9876543210),
       (N - 2, 5), (3, (1 << 255) + 12345), (0xc0ffee, N + 17)]
for m, k in pts:
    p = mul(m)
    r = mul(k % N, p)
    emit('    {"%s", "%x", "%s"},' % (pub33(p).hex(), k, pub33(r).hex() if r else ''))
emit('};')

# seal vectors: private key d (so Q = dG), plaintext, r, expected payload and text
emit('SVEC struct { const char *d_hex, *plain_hex, *r_hex, *payload_hex, *text; } SEAL_VEC[] = {')
cases = [
    (0x1, '00' * 31 + '01', 0x2),
    (0x4f5a6b7c8d9e0f1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192 % N, 'ab' * 32, 0x7777777777777777777777777777777777777777777777777777777777777777),
    (N - 1, '7f' * 32, N - 2),
    (0xabcdef0123456789, 'c0' * 16 + '3d' * 16, 0x10000),
]
for d, plain, r in cases:
    pl = bytes.fromhex(plain)
    pay = seal(mul(d), pl, r)
    emit('    {"%x", "%s", "%x", "%s", "PZENC1:%s"},' % (d, plain, r, pay.hex(), b58check(pay)))
emit('};')

# keys from passwords: the real cost (N = 2^18) once, and a reduced one for the tests that run often
emit('SVEC struct { const char *pw; unsigned log2n; const char *d_hex, *q_hex, *keyid_hex; } PWKEY_VEC[] = {')
for pw, lg in [(b'correct horse battery staple', 18), (b'correct horse battery staple', 4), (b'pa\xc3\xa7\xc3\xa3o', 4), (b'x', 1)]:
    d = key_from_password(pw, lg)
    q = pub33(mul(d))
    emit('    {%s, %d, "%x", "%s", "%s"},' % (cstr(pw.decode()), lg, d, q.hex(), hash160(q)[:4].hex()))
emit('};')

# the vectors of PBKDF2 and scrypt written by hand in the tests are checked here against hashlib
assert hashlib.pbkdf2_hmac('sha256', b'passwd', b'salt', 1, 64).hex().startswith('55ac046e56e3089fec1691c22544b605')
assert hashlib.scrypt(b'', salt=b'', n=16, r=1, p=1, dklen=64).hex().startswith('77d6576238657b20')
assert hashlib.scrypt(b'password', salt=b'NaCl', n=1024, r=8, p=16, dklen=64, maxmem=1 << 30).hex().startswith('fdbabe1c9d347200')
assert hashlib.scrypt(b'pleaseletmein', salt=b'SodiumChloride', n=16384, r=8, p=1, dklen=64, maxmem=1 << 30).hex().startswith('7023bdcb3afd7348')
print('\n'.join(out))
