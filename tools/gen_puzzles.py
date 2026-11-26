#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Generates src/puzzles_data.c from data/puzzles.csv after re-verifying every row
with the independent reference implementation (tools/ref.py), plus the addresses of the
withdrawn puzzles 161..256 from data/puzzles_withdrawn.csv (n,address) and the coins of the
addresses of the open puzzles from data/prizes.csv (n,txid,vout,sats; see data/README.md,
tools/fetch_prizes.py).

    python3 -I tools/gen_puzzles.py            # rewrite src/puzzles_data.c
    python3 -I tools/gen_puzzles.py --check    # fail if the committed file is stale

Row format: n,address,pubkey,privkey   (pubkey/privkey may be empty; privkey = solved).
"""
import csv, hashlib, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import ref

CSV = os.path.join(ROOT, 'data', 'puzzles.csv')
CSV_RET = os.path.join(ROOT, 'data', 'puzzles_withdrawn.csv')
CSV_PRIZES = os.path.join(ROOT, 'data', 'prizes.csv')
PRIZES_MAX = 16                    # the prize= lines a network takes (SESSION_PRIZES_MAX of src/peer/session.h)
PRIZE_MIN = 7400                   # satoshis: what an input costs at 50 sat/vB, the lowest fee rate of a sweep (src/peer/sweep.c)
ADDR_RE = r'1[1-9A-HJ-NP-Za-km-z]{24,33}'
OUT = os.path.join(ROOT, 'src', 'puzzles_data.c')


def b58_to_h160(addr):
    n = 0
    for c in addr:
        n = n * 58 + ref.B58.index(c)
    raw = n.to_bytes(25, 'big')
    if raw[-4:] != hashlib.sha256(hashlib.sha256(raw[:-4]).digest()).digest()[:4]:
        raise ValueError('bad Base58Check checksum: ' + addr)
    if raw[0] != 0:
        raise ValueError('not a P2PKH mainnet address: ' + addr)
    return raw[1:21]


def load():
    rows = []
    with open(CSV, newline='') as f:
        rd = csv.DictReader(f)
        if rd.fieldnames != ['n', 'address', 'pubkey', 'privkey']:
            raise ValueError('puzzles.csv: header must be n,address,pubkey,privkey')
        for r in rd:
            if None in r or None in r.values():
                raise ValueError('puzzles.csv: malformed row %r' % (r,))
            rows.append((int(r['n']), r['address'], r['pubkey'], r['privkey']))
    if [r[0] for r in rows] != list(range(1, 161)):
        raise ValueError('puzzles.csv must contain exactly puzzles 1..160 in order')
    if len({r[1] for r in rows}) != len(rows):
        raise ValueError('puzzles.csv: duplicate addresses')
    for n, addr, pub, priv in rows:
        # the values end up inside C string literals: accept nothing but the exact expected alphabets
        if not re.fullmatch(ADDR_RE, addr):
            raise ValueError('puzzle %d: malformed address %r' % (n, addr))
        if pub and not re.fullmatch(r'0[23][0-9a-f]{64}', pub):
            raise ValueError('puzzle %d: public key must be 66 lowercase hex digits (compressed)' % n)
        if priv and not re.fullmatch(r'[0-9a-f]{1,64}', priv):
            raise ValueError('puzzle %d: private key must be lowercase hex without prefix' % n)
        h = b58_to_h160(addr)
        if priv:
            k = int(priv, 16)
            if not (1 << (n - 1)) <= k < (1 << n):
                raise ValueError('puzzle %d: key outside its range' % n)
            p = ref.pub33(k)
            if ref.h160(p) != h:
                raise ValueError('puzzle %d: key does not produce the address' % n)
            if pub and pub != p.hex():
                raise ValueError('puzzle %d: pubkey differs from key*G' % n)
        elif pub:
            if ref.h160(bytes.fromhex(pub)) != h:
                raise ValueError('puzzle %d: pubkey does not hash to the address' % n)
        if pub:
            x = int(pub[2:], 16)
            if x >= ref.P:
                raise ValueError('puzzle %d: invalid public key (x >= p)' % n)
            y2 = (x ** 3 + 7) % ref.P
            y = pow(y2, (ref.P + 1) // 4, ref.P)
            if y * y % ref.P != y2:
                raise ValueError('puzzle %d: public key is not on the curve' % n)
    return rows


def load_retired(main_rows):
    rows = []
    with open(CSV_RET, newline='') as f:
        rd = csv.DictReader(f)
        if rd.fieldnames != ['n', 'address']:
            raise ValueError('puzzles_withdrawn.csv: header must be n,address')
        for r in rd:
            if None in r or None in r.values():
                raise ValueError('puzzles_withdrawn.csv: malformed row %r' % (r,))
            rows.append((int(r['n']), r['address']))
    if [r[0] for r in rows] != list(range(161, 257)):
        raise ValueError('puzzles_withdrawn.csv must contain exactly puzzles 161..256 in order')
    seen = {r[1] for r in main_rows}
    for n, addr in rows:
        if not re.fullmatch(ADDR_RE, addr):
            raise ValueError('puzzle %d: malformed address %r' % (n, addr))
        b58_to_h160(addr)
        if addr in seen:
            raise ValueError('puzzle %d: duplicate address' % n)
        seen.add(addr)
    return rows


def load_prizes(main_rows):
    rows = []
    with open(CSV_PRIZES, newline='') as f:
        rd = csv.DictReader(f)
        if rd.fieldnames != ['n', 'txid', 'vout', 'sats']:
            raise ValueError('prizes.csv: header must be n,txid,vout,sats')
        for r in rd:
            if None in r or None in r.values() or not all(re.fullmatch(r'[0-9]{1,16}', r[k]) for k in ('n', 'vout', 'sats')):
                raise ValueError('prizes.csv: malformed row %r' % (r,))
            rows.append((int(r['n']), r['txid'], int(r['vout']), int(r['sats'])))
    # the coins of the address of an open puzzle, with or without a public key
    unsolved = {n for n, addr, pub, priv in main_rows if not priv}
    seen = set()
    for n, txid, vout, sats in rows:
        if n not in unsolved:
            raise ValueError('coin of puzzle %d: not an open puzzle' % n)
        if not re.fullmatch(r'[0-9a-f]{64}', txid):
            raise ValueError('coin of puzzle %d: txid must be 64 lowercase hex digits' % n)
        if not PRIZE_MIN <= sats <= 21000000 * 100000000 or (txid, vout) in seen:
            raise ValueError('coin of puzzle %d: value out of range (%d satoshis at least), or a coin listed twice' % (n, PRIZE_MIN))
        seen.add((txid, vout))
    if [r[0] for r in rows] != sorted(r[0] for r in rows):
        raise ValueError('prizes.csv: rows in the order of the puzzles')
    for n in unsolved:
        if sum(r[0] == n for r in rows) > PRIZES_MAX:
            raise ValueError('puzzle %d has more than %d coins: a network names at most that many (SESSION_PRIZES_MAX)' % (n, PRIZES_MAX))
    return rows


def render(rows, retired, prizes):
    out = ['/* puzzles_data.c - GENERATED by tools/gen_puzzles.py from data/puzzles.csv, puzzles_withdrawn.csv and prizes.csv. Do not edit.',
           ' * Every row was verified (Base58Check, key range, key*G -> address, pubkey -> address,',
           ' * point on curve) against the independent implementation in tools/ref.py. */',
           '#include "puzzles.h"', '',
           'const puzzle_info PZ_PUZZLES[PZ_NUM_PUZZLES] = {']
    for n, addr, pub, priv in rows:
        out.append('    {%d, "%s", "%s", "%s"},' % (n, addr, pub, priv))
    out.append('};')
    out += ['', '/* puzzles 161..256: funds withdrawn by the creator in 2017 (data/puzzles_withdrawn.csv) */',
            'const char *const PZ_RETIRED[PZ_NUM_RETIRED] = {']
    for n, addr in retired:
        out.append('    "%s", /* %d */' % (addr, n))
    out.append('};')
    out += ['', '/* the coins of the addresses of the open puzzles (data/prizes.csv), ended by n = 0 */',
            'const puzzle_prize PZ_PRIZES[] = {']
    for n, txid, vout, sats in prizes:
        out.append('    {%d, "%s", %d, %d},' % (n, txid, vout, sats))
    out.append('    {0, "", 0, 0},')
    out.append('};')
    return '\n'.join(out) + '\n'


def main():
    rows = load()
    retired = load_retired(rows)
    prizes = load_prizes(rows)
    text = render(rows, retired, prizes)
    if '--check' in sys.argv:
        cur = open(OUT).read() if os.path.exists(OUT) else ''
        if cur != text:
            print('src/puzzles_data.c is out of date: run tools/gen_puzzles.py')
            return 1
        print('puzzles_data.c in sync (%d puzzles, %d solved, %d unsolved with public key; %d withdrawn; %d coins of open puzzles)' % (
            len(rows), sum(1 for r in rows if r[3]), sum(1 for r in rows if not r[3] and r[2]), len(retired), len(prizes)))
        return 0
    with open(OUT, 'w') as f:
        f.write(text)
    print('wrote', OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main())
