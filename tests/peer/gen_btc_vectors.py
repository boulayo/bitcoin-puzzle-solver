#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""Writes btc_vectors.h: the expected values of tests/peer/t_btc.c, computed here by an implementation that shares no code with
src/peer/btc.c: Python's integers for secp256k1 and ECDSA, hashlib for SHA-256, a pure-Python RIPEMD-160, and the serialization, the
legacy signature hash (SignatureHash before BIP143, as its description gives it), RFC 6979 and DER written out again.

The transactions of the main chain below are checked against their txids, and their signatures against their keys, before they are
written: bytes that differed by one bit would have another hash, and a signature hash computed wrongly would not verify. That pins
the legacy signature hash of this file to the one of the network, and the random vectors then test the C code against it.
    usage: python3 gen_btc_vectors.py > btc_vectors.h"""
import hashlib, hmac, random, sys

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

# RIPEMD-160 in pure Python (the same code as lib/pzcore/tests/gen_vectors.py, copied): OpenSSL 3 builds of hashlib may not have it
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
assert hash160(b'').hex() == 'b472a266d0bd89c13706a4132ccfb16f7c3b9fcb'

def pub33(k):
    x, y = mul(k)
    return bytes([2 + (y & 1)]) + x.to_bytes(32, 'big')

def pub_point(b):
    if b[0] == 4: return int.from_bytes(b[1:33], 'big'), int.from_bytes(b[33:65], 'big')
    x = int.from_bytes(b[1:33], 'big')
    y = pow((x ** 3 + 7) % P, (P + 1) // 4, P)
    return x, y if y % 2 == b[0] % 2 else P - y

# ---- serialization: a transaction is (version, [(txid, vout, scriptSig, sequence)], [(value, scriptPubKey)], locktime, witness)

def cs(n):
    if n < 0xfd: return bytes([n])
    if n <= 0xffff: return b'\xfd' + n.to_bytes(2, 'little')
    if n <= 0xffffffff: return b'\xfe' + n.to_bytes(4, 'little')
    return b'\xff' + n.to_bytes(8, 'little')

def ser(tx, with_witness=True):
    ver, ins, outs, lock, wit = tx
    seg = with_witness and wit is not None
    b = ver.to_bytes(4, 'little') + (b'\x00\x01' if seg else b'') + cs(len(ins))
    for txid, vout, sc, seq in ins: b += txid + vout.to_bytes(4, 'little') + cs(len(sc)) + sc + seq.to_bytes(4, 'little')
    b += cs(len(outs))
    for value, sc in outs: b += value.to_bytes(8, 'little') + cs(len(sc)) + sc
    if seg:
        for items in wit: b += cs(len(items)) + b''.join(cs(len(i)) + i for i in items)
    return b + lock.to_bytes(4, 'little')

def parse(raw):
    i = 0
    def take(n):
        nonlocal i
        assert i + n <= len(raw)
        i += n
        return raw[i - n:i]
    def size():
        b = take(1)[0]
        return b if b < 0xfd else int.from_bytes(take({0xfd: 2, 0xfe: 4, 0xff: 8}[b]), 'little')
    ver = int.from_bytes(take(4), 'little')
    seg = raw[4] == 0
    if seg: assert take(2) == b'\x00\x01'
    ins = []
    for _ in range(size()):
        txid = take(32); vout = int.from_bytes(take(4), 'little'); sc = take(size()); seq = int.from_bytes(take(4), 'little')
        ins.append((txid, vout, sc, seq))
    outs = []
    for _ in range(size()):
        value = int.from_bytes(take(8), 'little'); outs.append((value, take(size())))
    wit = [[take(size()) for _ in range(size())] for _ in ins] if seg else None
    lock = int.from_bytes(take(4), 'little')
    assert i == len(raw)
    return ver, ins, outs, lock, wit

def txid(tx): return sha256d(ser(tx, False))
def display(h): return h[::-1].hex()

# ---- the legacy signature hash, as described before BIP143: a copy of the transaction with every scriptSig empty except the one of the
# input being signed, which is the scriptCode (the script of the output it spends, without its OP_CODESEPARATORs); for SIGHASH_ALL the
# outputs and sequences stay; serialized, followed by the hashtype as a 4-byte integer, hashed twice.

def strip_codeseparators(script):
    out, i = b'', 0
    while i < len(script):
        op = script[i]
        n = 1
        if 1 <= op <= 75: n += op
        elif op == 76: n += 1 + script[i + 1]
        elif op == 77: n += 2 + int.from_bytes(script[i + 1:i + 3], 'little')
        elif op == 78: n += 4 + int.from_bytes(script[i + 1:i + 5], 'little')
        if op != 0xab: out += script[i:i + n]
        i += n
    return out

SIGHASH_ALL = 1
def sighash_legacy(tx, idx, script_code):
    ver, ins, outs, lock, wit = tx
    code = strip_codeseparators(script_code)
    copy = [(t, v, code if j == idx else b'', q) for j, (t, v, s, q) in enumerate(ins)]
    return sha256d(ser((ver, copy, outs, lock, None), False) + SIGHASH_ALL.to_bytes(4, 'little'))

def sighash_bip143(tx, idx, script_code, amount):   # only to confirm the bytes of the BIP143 example below
    ver, ins, outs, lock, wit = tx
    hp = sha256d(b''.join(t + v.to_bytes(4, 'little') for t, v, s, q in ins))
    hs = sha256d(b''.join(q.to_bytes(4, 'little') for t, v, s, q in ins))
    ho = sha256d(b''.join(v.to_bytes(8, 'little') + cs(len(s)) + s for v, s in outs))
    t, v, s, q = ins[idx]
    pre = (ver.to_bytes(4, 'little') + hp + hs + t + v.to_bytes(4, 'little') + cs(len(script_code)) + script_code +
           amount.to_bytes(8, 'little') + q.to_bytes(4, 'little') + ho + lock.to_bytes(4, 'little') + SIGHASH_ALL.to_bytes(4, 'little'))
    return sha256d(pre)

# ---- ECDSA

def der_int(v):
    b = v.to_bytes(32, 'big').lstrip(b'\0') or b'\0'
    return b'\x02' + bytes([len(b) + (b[0] >> 7)]) + (b'\0' if b[0] & 0x80 else b'') + b

def der(r, s):
    body = der_int(r) + der_int(s)
    return b'\x30' + bytes([len(body)]) + body

def der_parse(sig):
    assert sig[0] == 0x30 and sig[1] == len(sig) - 2 and sig[2] == 2
    lr = sig[3]; r = int.from_bytes(sig[4:4 + lr], 'big')
    assert sig[4 + lr] == 2
    ls = sig[5 + lr]; s = int.from_bytes(sig[6 + lr:6 + lr + ls], 'big')
    assert 6 + lr + ls == len(sig) and der(r, s) == sig   # strict DER: the minimal form
    return r, s

def verify(digest, sig, pub):
    r, s = der_parse(sig)
    if not (0 < r < N and 0 < s < N): return False
    z = int.from_bytes(digest, 'big') % N
    w = pow(s, -1, N)
    pt = add(mul(z * w % N), mul(r * w % N, pub_point(pub)))
    return pt is not None and pt[0] % N == r

def rfc6979(x, digest):
    """the nonce of RFC 6979 3.2 with HMAC-SHA256, the message reduced mod n (bits2octets), no extra data: libsecp256k1's default"""
    hm = lambda k, d: hmac.new(k, d, hashlib.sha256).digest()
    key = x.to_bytes(32, 'big') + (int.from_bytes(digest, 'big') % N).to_bytes(32, 'big')
    v, k = b'\x01' * 32, b'\x00' * 32
    k = hm(k, v + b'\x00' + key); v = hm(k, v)
    k = hm(k, v + b'\x01' + key); v = hm(k, v)
    while True:
        v = hm(k, v)
        t = int.from_bytes(v, 'big')
        if 1 <= t < N: return t
        k = hm(k, v + b'\x00'); v = hm(k, v)

def sign(x, digest):
    k = rfc6979(x, digest)
    r = mul(k)[0] % N
    s = pow(k, -1, N) * (int.from_bytes(digest, 'big') + r * x) % N
    assert r and s
    return r, min(s, N - s)   # low s

def p2pkh(h): return b'\x76\xa9\x14' + h + b'\x88\xac'

# ---- output

out = []
def emit(s=''): out.append(s)
def cstr(s): return '"' + s + '"'
def chex(b): return cstr(b.hex())
def long_cstr(b, indent='    '):   # a long hex string in pieces of 112 digits
    h = b.hex()
    return ('\n' + indent).join(cstr(h[i:i + 112]) for i in range(0, len(h), 112)) or '""'

emit('/* SPDX-License-Identifier: AGPL-3.0-or-later */')
emit('/* SPDX-FileCopyrightText: 2026 Leonardo Boulay */')
emit('/* Generated by gen_btc_vectors.py (an independent Python implementation: integers, hashlib, hmac); do not edit. */')
emit('#define VEC static const __attribute__((unused))')

# ---- transactions of the main chain: bytes pinned by their txids
GENESIS_CB = bytes.fromhex(
    '01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff4d04ffff001d0104455468652054696d65732030'
    '332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73ffff'
    'ffff0100f2052a01000000434104678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec1'
    '12de5c384df7ba0b8d578a4c702b6bf11d5fac00000000')
BLOCK9_CB = bytes.fromhex(
    '01000000010000000000000000000000000000000000000000000000000000000000000000ffffffff0704ffff001d0134ffffffff0100f2052a0100'
    '000043410411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6909a5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9d4c03f'
    '999b8643f656b412a3ac00000000')
BLOCK170_TX = bytes.fromhex(
    '0100000001c997a5e56e104102fa209c6a852dd90660a20b2d9c352423edce25857fcd3704000000004847304402204e45e16932b8af514961a1d3a1'
    'a25fdf3f4f7732e9d624c6c61548ab5fb8cd410220181522ec8eca07de4860a4acdd12909d831cc56cbbac4622082221a8768d1d0901ffffffff02'
    '00ca9a3b00000000434104ae1a62fe09c5f51b13905f07f06b99a2f7159b2225f374cd378d71302fa28414e7aab37397f554a7df5f142c21c1b730'
    '3b8a0626f1baded5c72a704f7e6cd84cac00286bee0000000043410411db93e1dcdb8a016b49840f8c53bc1eb68a382e97b1482ecad7b148a6909a'
    '5cb2e0eaddfb84ccf9744464f82e160bfa9b8b64f9d4c03f999b8643f656b412a3ac00000000')
MAIN = [('the coinbase of the genesis block', GENESIS_CB, '4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b'),
        ('the coinbase of block 9', BLOCK9_CB, '0437cd7f8525ceed2324359c2d0ba26006d92d856a9c20fa0241106ee5a597c9'),
        ('the first payment, in block 170', BLOCK170_TX, 'f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16')]
for name, raw, tid in MAIN:
    assert display(sha256d(raw)) == tid, name
    assert ser(parse(raw)) == raw, name
# the spend of block 170: its input 0 spends output 0 of the coinbase of block 9, a P2PK script; the signature must verify with the
# signature hash computed here
b9 = parse(BLOCK9_CB)
b170 = parse(BLOCK170_TX)
assert b170[1][0][0] == sha256d(BLOCK9_CB) and b170[1][0][1] == 0
b9_spk = b9[2][0][1]
hal_digest = sighash_legacy(b170, 0, b9_spk)
hal_sig = b170[1][0][2][1:1 + b170[1][0][2][0]]
assert hal_sig[-1] == SIGHASH_ALL and verify(hal_digest, hal_sig[:-1], b9_spk[1:66])

emit('/* Transactions of the main chain. Each one is checked by the generator against its txid, the double SHA-256 of its bytes: other')
emit(' * bytes would not have that hash. */')
emit('VEC struct { const char *what, *raw, *txid; } MAIN_TX[] = {')
for name, raw, tid in MAIN:
    emit('    {%s,' % cstr(name))
    emit('     ' + long_cstr(raw, '     ') + ',')
    emit('     %s},' % cstr(tid))
emit('};')
emit('/* Input 0 of the transaction of block 170 spends output 0 of the coinbase of block 9 (MAIN_TX[1], a P2PK script); its signature')
emit(' * verifies with this legacy signature hash, which the generator computed and checked with the key of that script. */')
emit('#define B170_SIGHASH %s' % chex(hal_digest))

# BIP143's example of a native P2WPKH spend, signed: input 0 is P2PK and signed with the legacy hash, input 1 is P2WPKH. Both signatures
# verify here, so these are the bytes of the BIP to the bit; it is a segwit serialization with a known legacy signature in it.
BIP143 = bytes.fromhex(
    '01000000000102fff7f7881a8099afa6940d42d1e7f6362bec38171ea3edf433541db4e4ad969f00000000494830450221008b9d1dc26ba6a9cb6212'
    '7b02742fa9d754cd3bebf337f7a55d114c8e5cdd30be022040529b194ba3f9281a99f2b1c0a19c0489bc22ede944ccf4ecbab4cc618ef3ed01eeffff'
    'ffef51e1b804cc89d182d279655c3aa89e815b1b309fe287d9b2b55d57b90ec68a0100000000ffffffff02202cb206000000001976a9148280b37df3'
    '78db99f66f85c95a783a76ac7a6d5988ac9093510d000000001976a9143bde42dbee7e4dbe6a21b2d50ce2f0167faa815988ac000247304402203609'
    'e17b84f6a7d30c80bfa610b5b4542f32a8a0d5447a12fb1366d7f01cc44a0220573a954c4518331561406f90300e8f3358f51928d43c212a8caed02d'
    'e67eebee0121025476c2e83188368da1ff3e292e7acafcdb3566bb0ad253f62fc70f07aeee635711000000')
bt = parse(BIP143)
assert ser(bt) == BIP143 and bt[4] is not None
bip143_spk0 = bytes.fromhex('2103c9f4836b9a4f77fc0d81f7bcb01b7f1b35916864b9476c241ce9fc198bd25432ac')
assert pub33(0xbbc27228ddcb9209d7fd6f36b02f7dfa6252af40bb2f1cbc7a557da8027ff866) == bip143_spk0[1:34]   # its key, from the BIP
sig0 = bt[1][0][2][1:1 + bt[1][0][2][0]]
assert sig0[-1] == SIGHASH_ALL and verify(sighash_legacy(bt, 0, bip143_spk0), sig0[:-1], bip143_spk0[1:34])
wsig, wpub = bt[4][1]
assert wsig[-1] == SIGHASH_ALL and verify(sighash_bip143(bt, 1, p2pkh(hash160(wpub)), 600000000), wsig[:-1], wpub)
assert bt[4][0] == []
emit('/* The signed example of BIP143 (native P2WPKH): a segwit serialization. Its input 0 is a P2PK spend with a legacy signature; the')
emit(' * generator verified that one and the BIP143 one of input 1, so these are the bytes of the BIP. Its txid and its serialization')
emit(' * without the witness were computed by the generator. */')
emit('#define BIP143_TX \\')
emit('    ' + long_cstr(BIP143).replace('\n', ' \\\n'))
emit('#define BIP143_STRIPPED \\')
emit('    ' + long_cstr(ser(bt, False)).replace('\n', ' \\\n'))
emit('#define BIP143_TXID %s' % cstr(display(txid(bt))))
emit('#define BIP143_IN0_SPK %s' % chex(bip143_spk0))

# ---- random transactions, within the capacities of a btc_tx (BTC_MAX_IN, BTC_SCRIPTSIG_MAX, BTC_MAX_OUT, BTC_SPK_MAX): serialization,
# txid and the legacy signature hash of some of their inputs
rnd = random.Random(20261108)
def rbytes(n): return bytes(rnd.getrandbits(8) for _ in range(n))
def rscript_code():
    kind = rnd.randrange(4)
    if kind == 0: return p2pkh(rbytes(20))
    if kind == 1: return bytes([33]) + pub33(rnd.randrange(1, N)) + b'\xac'          # P2PK
    if kind == 2: return b''                                                         # empty
    return b'\x4c' + bytes([200]) + rbytes(200) + b'\xac'                            # a long push with an opcode after it
def rtx():
    ins = [(rbytes(32), rnd.choice([0, 1, 2, 0xffffffff, rnd.getrandbits(32)]), rbytes(rnd.choice([0, 0, 25, 107, 140, rnd.randrange(141)])),
            rnd.choice([0xffffffff, 0xfffffffe, 0, rnd.getrandbits(32)])) for _ in range(rnd.randint(1, 16))]
    outs = [(rnd.choice([0, 546, rnd.randrange(2100000000000000 // 16)]), rbytes(rnd.choice([0, 22, 23, 25, 34, 67, 83, rnd.randrange(84)])))
            for _ in range(rnd.randint(1, 16))]
    return (rnd.choice([1, 2, rnd.getrandbits(32)]), ins, outs, rnd.choice([0, 500000000, rnd.getrandbits(32)]), None)

emit('/* random transactions: their legacy serialization and txid, and the signature hash of some of their inputs with a scriptCode */')
emit('VEC struct { const char *tx, *txid; } TX_VEC[] = {')
# the first one fills a btc_tx: 16 inputs with the longest scriptSig, 16 outputs with the longest script (BTC_TX_MAX_SIZE)
full = (2, [(rbytes(32), k, rbytes(140), 0xffffffff - k) for k in range(16)], [(10**8 + k, rbytes(83)) for k in range(16)], 0, None)
assert len(ser(full)) == 4 + 1 + 16 * (36 + 1 + 140 + 4) + 1 + 16 * (8 + 1 + 83) + 4
txs = [full] + [rtx() for _ in range(17)]
for t in txs:
    emit('    {' + long_cstr(ser(t), '     ') + ',')
    emit('     %s},' % cstr(display(txid(t))))
emit('};')
emit('VEC struct { unsigned tx, in; const char *code, *digest; } SIGHASH_VEC[] = {')
for k, t in enumerate(txs):
    for i in sorted(set(rnd.randrange(len(t[1])) for _ in range(3))):
        code = rscript_code()
        assert strip_codeseparators(code) == code   # the C code takes the scriptCode as it is: none of these has the opcode
        emit('    {%d, %d, %s, %s},' % (k, i, chex(code), chex(sighash_legacy(t, i, code))))
emit('};')

# ---- signed P2PKH spends: RFC 6979 nonces as libsecp256k1 makes them by default, low s
emit('/* Spends of P2PKH outputs of known keys, signed here (ECDSA, RFC 6979 nonces, low s, SIGHASH_ALL): the unsigned transaction, the')
emit(' * keys and the scripts of the outputs spent, and the signed transaction, which the C code must reproduce byte for byte. */')
emit('VEC struct { const char *unsigned_tx; unsigned n; const char *key[4], *spk[4]; const char *signed_tx; } SIGN_VEC[] = {')
keysets = [[1], [1, 2, 3], [N - 1, rnd.randrange(1, N)], [rnd.randrange(1, N) for _ in range(4)], [0x1a838b13505b26867, 7]]
for ks in keysets:
    pubs = [pub33(k) for k in ks]
    spks = [p2pkh(hash160(p)) for p in pubs]
    ins = [(rbytes(32), rnd.randrange(4), b'', rnd.choice([0xffffffff, 0xfffffffd])) for _ in ks]
    outs = [(rnd.randrange(1, 10**8), rnd.choice([p2pkh(rbytes(20)), b'\x00\x14' + rbytes(20), b'\x51\x20' + rbytes(32), b'\xa9\x14' + rbytes(20) + b'\x87']))
            for _ in range(rnd.randint(1, 3))]
    t = (2, ins, outs, rnd.choice([0, 840000]), None)
    signed = []
    for i, (k, pub, spk) in enumerate(zip(ks, pubs, spks)):
        d = sighash_legacy(t, i, spk)
        r, s = sign(k, d)
        sig = der(r, s) + bytes([SIGHASH_ALL])
        assert verify(d, sig[:-1], pub)
        signed.append(bytes([len(sig)]) + sig + bytes([33]) + pub)
    ts = (t[0], [(a, b, sc, q) for (a, b, _, q), sc in zip(ins, signed)], outs, t[3], None)
    pad = lambda xs: ', '.join(xs + ['NULL'] * (4 - len(xs)))
    emit('    {' + long_cstr(ser(t), '     ') + ',')
    emit('     %d, {%s},' % (len(ks), pad([cstr('%064x' % k) for k in ks])))
    emit('     {%s},' % pad([chex(s) for s in spks]))
    emit('     ' + long_cstr(ser(ts), '     ') + '},')
emit('};')

# ---- DER with low s: (r, s) -> the encoding, or "" when (r, s) is not a signature
emit('/* (r, s) and their DER encoding with s made low; "" where r or s is 0 or not below n */')
emit('VEC struct { const char *r, *s, *der; } DER_VEC[] = {')
pairs = [(1, 1), (0x7f, 0x80), (0x80, 0x7f), (2**255, 1), (2**248 - 1, 2**247), (N - 1, N - 1), (N - 1, 1), (1, N // 2), (1, N // 2 + 1),
         (0x00ff << 200, 0x0080 << 200), (2**8, 2**16 - 1), (N - 1, 2**255)]
pairs += [(rnd.randrange(1, N), rnd.randrange(1, N)) for _ in range(12)]
pairs += [(rnd.randrange(1, 2**rnd.randint(1, 256)) % (N - 1) + 1, rnd.randrange(1, 2**rnd.randint(1, 256)) % (N - 1) + 1) for _ in range(8)]
for r, s in pairs:
    emit('    {%s, %s, %s},' % (cstr('%064x' % r), cstr('%064x' % s), chex(der(r, min(s, N - s)))))
for r, s in [(0, 1), (1, 0), (N, 1), (1, N), (2**256 - 1, 1), (1, 2**256 - 1), (0, 0)]:
    emit('    {%s, %s, ""},' % (cstr('%064x' % r), cstr('%064x' % s)))
emit('};')

sys.stdout.write('\n'.join(out) + '\n')
