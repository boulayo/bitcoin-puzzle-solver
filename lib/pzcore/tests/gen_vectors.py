#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes vectors.h: the expected values of the unit tests of pzcore, computed here with Python's own integers, hashlib's SHA-256 and a
pure-Python RIPEMD-160, by an implementation of secp256k1, Base58Check, bech32 and bech32m that shares no code with the C library. The
vectors of BIP173 and BIP350 that are written out by hand are checked against that implementation first, so a mistake in this file is
caught before it reaches the tests.   usage: python3 gen_vectors.py > vectors.h"""
import hashlib, random, sys

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

# RIPEMD-160 in pure Python, the same code as tools/ref.py (copied, so that lib/pzcore stays self-contained): OpenSSL 3 builds of
# hashlib (for example Python 3.10 on Ubuntu 22.04) no longer provide it
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
def rmd160(b): return ripemd160(b)
def hash160(b): return rmd160(sha256(b))

B58 = '123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'
def b58check(payload):
    d = payload + sha256d(payload)[:4]
    n = int.from_bytes(d, 'big'); s = ''
    while n: n, r = divmod(n, 58); s = B58[r] + s
    return '1' * (len(d) - len(d.lstrip(b'\0'))) + s

CS = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l'
def polymod(v):
    gen = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
    chk = 1
    for x in v:
        b = chk >> 25
        chk = (chk & 0x1ffffff) << 5 ^ x
        for i in range(5):
            if (b >> i) & 1: chk ^= gen[i]
    return chk
def hrp_expand(h): return [ord(c) >> 5 for c in h] + [0] + [ord(c) & 31 for c in h]
def bech_encode(hrp, data, const):
    mod = polymod(hrp_expand(hrp) + data + [0] * 6) ^ const
    return hrp + '1' + ''.join(CS[d] for d in data + [(mod >> 5 * (5 - i)) & 31 for i in range(6)])
def bech_decode(s):
    """-> (hrp, data, const) or None, as the BIPs say"""
    if any(ord(c) < 33 or ord(c) > 126 for c in s): return None
    if s.lower() != s and s.upper() != s: return None
    s = s.lower()
    pos = s.rfind('1')
    if pos < 1 or pos + 7 > len(s) or len(s) > 90: return None
    if not all(c in CS for c in s[pos + 1:]): return None
    data = [CS.find(c) for c in s[pos + 1:]]
    c = polymod(hrp_expand(s[:pos]) + data)
    if c not in (1, 0x2bc830a3): return None
    return s[:pos], data[:-6], c
def convert(data, f, t, pad=True):
    acc = bits = 0; ret = []; maxv = (1 << t) - 1
    for v in data:
        if v >> f: return None
        acc = ((acc << f) | v) & ((1 << (f + t - 1)) - 1); bits += f
        while bits >= t:
            bits -= t; ret.append((acc >> bits) & maxv)
    if pad:
        if bits: ret.append((acc << (t - bits)) & maxv)
    elif bits >= f or ((acc << (t - bits)) & maxv): return None
    return ret
def seg_decode(hrp, addr):
    d = bech_decode(addr)
    if d is None or d[0] != hrp.lower() or not d[1]: return None
    h, data, c = d
    if data[0] > 16: return None
    prog = convert(data[1:], 5, 8, False)
    if prog is None or not 2 <= len(prog) <= 40: return None
    if data[0] == 0 and len(prog) not in (20, 32): return None
    if (data[0] == 0 and c != 1) or (data[0] != 0 and c != 0x2bc830a3): return None
    return data[0], bytes(prog)
def seg_encode(hrp, ver, prog):
    return bech_encode(hrp, [ver] + convert(list(prog), 8, 5), 1 if ver == 0 else 0x2bc830a3)

out = []
def emit(s=''): out.append(s)
def cstr(b): return '"' + b + '"'

emit('/* Generated by gen_vectors.py (independent Python implementation: integers, hashlib); do not edit. */')
emit('#define VEC static const __attribute__((unused))')

# ---- u256: hex <-> decimal
rnd = random.Random(20260101)
emit('VEC struct { const char *hex, *dec; } U256_VEC[] = {')
vals = [0, 1, 9, 10, 255, 2**32, 2**63, 2**64 - 1, 2**64, 10**19 - 1, 10**19, 10**19 + 1, 10**38, 2**128, 2**255, 2**256 - 1, N, P, 10**76]
vals += [rnd.getrandbits(rnd.randint(1, 256)) for _ in range(40)]
for v in vals:
    emit('    {%s, %s},' % (cstr('%x' % v), cstr(str(v))))
emit('};')

# ---- EC: k -> x, y, pub33 hash160
emit('VEC struct { const char *k, *x, *y, *h160; } EC_VEC[] = {')
ks = [1, 2, 3, 7, 15, 16, 255, 256, 0x1a838b13505b26867, 2**64, 2**128 + 1, 2**255, N - 1, N - 2, (N - 1) // 2, (N + 1) // 2, P % N]
ks += [rnd.getrandbits(256) % (N - 1) + 1 for _ in range(14)]
ks += [rnd.getrandbits(rnd.randint(8, 128)) | 1 for _ in range(6)]
for k in ks:
    x, y = mul(k)
    pub = bytes([2 + (y & 1)]) + x.to_bytes(32, 'big')
    emit('    {%s, %s, %s, %s},' % (cstr('%x' % k), cstr('%064x' % x), cstr('%064x' % y), cstr(hash160(pub).hex())))
emit('};')

# a point whose y is odd and one whose y is even are both in the table above; invalid public keys follow
emit('/* public keys that must be refused: the first of the non-canonical ones was accepted by pzsolve 1.8.1 */')
emit('VEC char *const PUBKEY_BAD[] = {')
gx, gy = G
bad = []
bad.append('04' + '%064x' % gx + '%064x' % (gy ^ 1))                     # not on the curve
bad.append('04' + '%064x' % (gx + P) + '%064x' % gy) if gx + P < 2**256 else None   # x + p: non-canonical
bad.append('04' + '%064x' % gx + '%064x' % (gy + P)) if gy + P < 2**256 else None
# a point with a small x exists on the curve, so x + p still fits in 256 bits: the 65-byte form with that representative is not canonical
sx = next(x for x in range(1, 100) if pow((x**3 + 7) % P, (P - 1) // 2, P) == 1)
sy = pow((sx**3 + 7) % P, (P + 1) // 4, P)
assert (sy * sy - sx**3 - 7) % P == 0 and sx + P < 2**256
bad.append('04' + '%064x' % (sx + P) + '%064x' % sy)
bad.append('04' + '%064x' % sx + '%064x' % (sy + P)) if sy + P < 2**256 else None
bad.append('02' + '%064x' % (sx + P))
bad.append('02' + '%064x' % P)                                                   # x = p
bad.append('02' + '%064x' % (2**256 - 1))                                        # x >= p
bad.append('02' + '%064x' % 5)                                                   # x = 5: x^3 + 7 = 132 is not a square
bad.append('05' + '%064x' % gx)                                                  # bad prefix
bad.append('06' + '%064x' % gx + '%064x' % gy)                                   # hybrid
bad.append('07' + '%064x' % gx + '%064x' % (P - gy))
bad.append('04' + '%064x' % 0 + '%064x' % 0)
bad.append('00' + '%064x' % gx)
bad.append(('02' + '%064x' % gx)[:-2])                                           # too short
bad.append('02' + '%064x' % gx + '00')                                           # too long
bad.append('03' + '%064x' % gx + '%064x' % gy)                                   # 65 bytes with a 33-byte prefix
bad.append('')
for b in bad:
    if b is not None: emit('    %s,' % cstr(b))
emit('};')
# the x = 5 claim
assert pow((5**3 + 7) % P, (P - 1) // 2, P) == P - 1

# ---- hashes: digest of the digests of the messages of 0..520 bytes
def pat(n): return bytes((i * 167 + n * 13 + 5) & 0xff for i in range(n))
for name, fn, in (('sha256', sha256), ('sha256d', sha256d), ('ripemd160', rmd160), ('hash160', hash160)):
    acc = b''.join(fn(pat(n)) for n in range(521))
    emit('#define DIGEST_OF_DIGESTS_%s %s' % (name.upper(), cstr(sha256(acc).hex())))
emit('/* pattern of the messages: byte i of the message of length n is (i * 167 + n * 13 + 5) & 255 */')

# ---- Base58Check
emit('VEC struct { const char *payload_hex, *text; } B58_VEC[] = {')
cases = [b'', b'\0', b'\0\0', b'\x00' * 21, b'\x01', b'\xff' * 32, bytes(range(1, 40)), b'\x00' + hash160(b'abc'), b'\x80' + b'\x11' * 32 + b'\x01',
         b'\x00\x00\x01', b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01']
cases += [bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 70))) for _ in range(25)]
cases += [b'\0' * rnd.randint(1, 6) + bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 30))) for _ in range(12)]
cases += [bytes(rnd.getrandbits(8) for _ in range(96)), bytes(rnd.getrandbits(8) for _ in range(95)) + b'\0', b'\0' * 96]
for c in cases:
    emit('    {%s, %s},' % (cstr(c.hex()), cstr(b58check(c))))
emit('};')
emit('VEC char *const B58_BAD[] = {')
good = b58check(b'\x00' + hash160(b'abc'))
bads = ['', '1', '11', '1111', good[:-1], good + '1', good[:-1] + ('2' if good[-1] != '2' else '3'), good.replace(good[5], '0', 1), good.replace(good[5], 'O', 1),
        good.replace(good[5], 'I', 1), good.replace(good[5], 'l', 1), ' ' + good, good + ' ', good[:10] + ' ' + good[10:], good + '\n', good.lower(),
        '0' * 34, 'z' * 34, 'z' * 140, 'z' * 200, '1' * 100, '1' * 140, '1' * 2000, '\xe9' * 20, good[:15] + '\x80' + good[16:]]
# an over-long but otherwise valid string: a 97-byte payload
big = b58check(bytes(rnd.getrandbits(8) for _ in range(97)))
bads.append(big)
for b in bads:
    emit('    %s,' % ('"' + b.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').encode('latin-1').decode('latin-1') + '"' if all(ord(ch) < 128 for ch in b) else '"' + ''.join('\\x%02x' % ord(ch) if ord(ch) > 126 else ch for ch in b) + '"'))
emit('};')

# ---- WIF and P2PKH
emit('VEC struct { const char *key_hex; int compressed; const char *wif; } WIF_VEC[] = {')
for k, comp in ((1, 1), (1, 0), (N - 1, 1), (0x1a838b13505b26867, 1), (2**255, 0)) + tuple((rnd.getrandbits(256) % (N - 1) + 1, rnd.getrandbits(1)) for _ in range(6)):
    pl = b'\x80' + k.to_bytes(32, 'big') + (b'\x01' if comp else b'')
    emit('    {%s, %d, %s},' % (cstr('%064x' % k), comp, cstr(b58check(pl))))
emit('};')
emit('VEC struct { const char *h160_hex, *p2pkh, *p2wpkh; } ADDR_VEC[] = {')
for h in [hash160(b'abc'), b'\0' * 20, b'\xff' * 20, hash160(bytes([2]) + G[0].to_bytes(32, 'big'))] + [bytes(rnd.getrandbits(8) for _ in range(20)) for _ in range(8)]:
    emit('    {%s, %s, %s},' % (cstr(h.hex()), cstr(b58check(b'\0' + h)), cstr(seg_encode('bc', 0, h))))
emit('};')

# ---- bech32 / bech32m, BIP173 and BIP350
valid_bech32 = ['A12UEL5L', 'a12uel5l', 'an83characterlonghumanreadablepartthatcontainsthenumber1andtheexcludedcharactersbio1tt5tgs',
                'abcdef1qpzry9x8gf2tvdw0s3jn54khce6mua7lmqqqxw', '11' + 'q' * 82 + 'c8247j',
                'split1checkupstagehandshakeupstreamerranterredcaperred2y9e3w', '?1ezyfcl']
valid_bech32m = ['A1LQFN3A', 'a1lqfn3a', 'an83characterlonghumanreadablepartthatcontainsthetheexcludedcharactersbioandnumber11sg7hg6',
                 'abcdef1l7aum6echk45nj3s0wdvt2fg8x9yrzpqzd3ryx',
                 bech_encode('1', [31] * 82, 0x2bc830a3)[:0] or bech_encode('1', [31] * 82, 0x2bc830a3),
                 'split1checkupstagehandshakeupstreamerranterredcaperredlc445v', '?1v759aa']
invalid_bech32 = ['\x20' + '1nwldj5', '\x7f' + '1axkwrx', '\x80' + '1eym55h', 'an84characterslonghumanreadablepartthatcontainsthenumber1andtheexcludedcharactersbio1569pvx',
                  'pzry9x0s0muk', '1pzry9x0s0muk', 'x1b4n0q5v', 'li1dgmt3', 'de1lg7wt\xff', 'A1G7SGD8', '10a06t8', '1qzzfhee']
invalid_bech32m = ['\x20' + '1xj0phk', '\x7f' + '1g6xzxy', '\x80' + '1vctc34', 'an84characterslonghumanreadablepartthatcontainsthetheexcludedcharactersbioandnumber11d6pts4',
                   'qyrz8wqd2c9m', '1qyrz8wqd2c9m', 'y1b0jsk6g', 'lt1igcx5c0', 'in1muywd', 'mm1crxm3i', 'au1s5cgom', 'M1VUXWEZ', '16plkw9', '1p2gdwpf']
emit('/* strings the BIPs list as valid (with the variant of the checksum: 1 bech32, 2 bech32m) and as invalid */')
emit('VEC struct { const char *s; int variant; } BECH32_OK[] = {')
for s in valid_bech32:
    d = bech_decode(s); assert d and d[2] == 1, s
    emit('    {%s, 1},' % cstr(s))
for s in valid_bech32m:
    if not s: continue
    d = bech_decode(s)
    if not (d and d[2] == 0x2bc830a3):
        print('warning: bech32m vector not valid, skipped:', s, file=sys.stderr); continue
    emit('    {%s, 2},' % cstr(s))
emit('};')
emit('VEC char *const BECH32_BAD[] = {')
for s in invalid_bech32 + invalid_bech32m:
    if bech_decode(s) is not None:
        print('warning: vector not invalid, skipped:', repr(s), file=sys.stderr); continue
    emit('    "%s",' % ''.join(c if 32 <= ord(c) < 127 and c != '"' and c != '\\' else '\\x%02x""' % ord(c) for c in s))
emit('};')

seg_valid = [('BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4', 'bc', 0, '751e76e8199196d454941c45d1b3a323f1433bd6'),
             ('tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7', 'tb', 0, '1863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262'),
             ('bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7kt5nd6y', 'bc', 1, '751e76e8199196d454941c45d1b3a323f1433bd6751e76e8199196d454941c45d1b3a323f1433bd6'),
             ('BC1SW50QGDZ25J', 'bc', 16, '751e'),
             ('bc1zw508d6qejxtdg4y5r3zarvaryvaxxpcs', 'bc', 2, '751e76e8199196d454941c45d1b3a323'),
             ('tb1qqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesrxh6hy', 'tb', 0, '000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433'),
             ('tb1pqqqqp399et2xygdj5xreqhjjvcmzhxw4aywxecjdzew6hylgvsesf3hn0c', 'tb', 1, '000000c4a5cad46221b2a187905e5266362b99d5e91c6ce24d165dab93e86433'),
             ('bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0', 'bc', 1, '79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798')]
emit('VEC struct { const char *addr, *hrp; int witver; const char *prog_hex; } SEGWIT_OK[] = {')
for a, h, v, pr in seg_valid:
    r = seg_decode(h, a)
    if r is None or r[0] != v or r[1].hex() != pr:
        print('warning: segwit vector does not check, skipped:', a, r, file=sys.stderr); continue
    assert seg_encode(h, v, bytes.fromhex(pr)) == a.lower()
    emit('    {%s, %s, %d, %s},' % (cstr(a), cstr(h), v, cstr(pr)))
emit('};')
seg_bad = ['tc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq5zuyut', 'bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqh2y7hd',
           'tb1z0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqglt7rf', 'BC1S0XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ54WELL',
           'bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kemeawh', 'tb1q0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq24jc47',
           'bc1p38j9r5y49hruaue7wxjce0updqjuyyx0kh56v8s25huc6995vvpql3jow4', 'BC130XLXVLHEMJA6C4DQV22UAPCTQUPFHLXM9H8Z3K2E72Q4K9HCZ7VQ7ZWS8R',
           'bc1pw5dgrnzv', 'bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v8n0nx0muaewav253zgeav', 'BC1QR508D6QEJXTDG4Y5R3ZARVARYV98GJ9P',
           'tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vq47Zagq', 'bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7v07qwwzcrf',
           'tb1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vpggkg4j', 'bc1gmk9yu']
emit('/* SegWit addresses that must be refused for the human-readable part "bc" or "tb" (the first one has the wrong prefix for both) */')
emit('VEC char *const SEGWIT_BAD[] = {')
for a in seg_bad:
    if seg_decode('bc', a) is not None or seg_decode('tb', a) is not None:
        print('warning: segwit vector not invalid, skipped:', a, file=sys.stderr); continue
    emit('    %s,' % cstr(a))
emit('};')
print('\n'.join(out))
