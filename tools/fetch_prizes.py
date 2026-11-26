#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Makes data/prizes.csv: the confirmed unspent coins of the address of every open puzzle of
data/puzzles.csv that are worth an input of a sweep (7400 satoshis), as two independent block
explorers list them. The two must agree on every coin (txid, output, value, block), or nothing
is written. A request carries nothing but the address; there is a pause between requests.

    python3 -I tools/fetch_prizes.py --raw DIR    # the answers as they came go to DIR
    python3 -I tools/gen_puzzles.py               # then: checks the table, rewrites src/puzzles_data.c

The table is a snapshot: a coin spent since is found spent by the program, and one that came
later is above every checkpoint, where a scan of the address lists it (docs/en/NETWORK.md 8.2).
"""
import csv, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ['https://mempool.space/api', 'https://blockstream.info/api']
PRIZE_MIN, PRIZES_MAX, PAUSE = 7400, 16, 2.0   # as in tools/gen_puzzles.py; seconds between two requests


def coins(base, addr, raw_dir, name):
    with urllib.request.urlopen(urllib.request.Request('%s/address/%s/utxo' % (base, addr), headers={'User-Agent': 'curl/8'}), timeout=60) as r:
        body = r.read()
    if raw_dir:
        with open(os.path.join(raw_dir, name), 'wb') as f:
            f.write(body)
    time.sleep(PAUSE)
    return {(u['txid'], int(u['vout']), int(u['value']), int(u['status']['block_height']))
            for u in json.loads(body) if u['status']['confirmed'] and int(u['value']) >= PRIZE_MIN}


def main():
    args = sys.argv[1:]
    raw_dir = args[args.index('--raw') + 1] if '--raw' in args else None
    sources = args[args.index('--sources') + 1].split(',') if '--sources' in args else SOURCES   # for a test against local servers
    if raw_dir:
        os.makedirs(raw_dir, exist_ok=True)
    rows = []
    with open(os.path.join(ROOT, 'data', 'puzzles.csv'), newline='') as f:
        unsolved = [(int(r['n']), r['address']) for r in csv.DictReader(f) if not r['privkey']]
    for n, addr in unsolved:
        got = [coins(base, addr, raw_dir, '%03d-%d.json' % (n, k)) for k, base in enumerate(sources)]
        if any(g != got[0] for g in got):
            sys.exit('puzzle %d (%s): the sources do not agree: %s' % (n, addr, sorted(got[0] ^ got[1])))
        if len(got[0]) > PRIZES_MAX:
            sys.exit('puzzle %d (%s) has %d coins: a network names at most %d' % (n, addr, len(got[0]), PRIZES_MAX))
        rows += [(n, txid, vout, value) for txid, vout, value, height in sorted(got[0], key=lambda c: (c[3], c[0], c[1]))]
        print('puzzle %d: %d coins' % (n, len(got[0])), file=sys.stderr)
    with open(os.path.join(ROOT, 'data', 'prizes.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['n', 'txid', 'vout', 'sats'])
        w.writerows(rows)
    print('wrote data/prizes.csv: %d coins of %d open puzzles' % (len(rows), len(unsolved)))


if __name__ == '__main__':
    main()
