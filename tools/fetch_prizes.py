#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Makes data/prizes.csv: the coins the creator of the puzzles put at the address of every open
puzzle of data/puzzles.csv, outputs of his three transactions of 2015, 2017 and 2023, as two
independent block explorers list them unspent and confirmed. The two must agree on every coin
(txid, output, value, block), or nothing is written. A request carries nothing but the address;
there is a pause between requests.

    python3 -I tools/fetch_prizes.py --raw DIR    # the answers as they came go to DIR
    python3 -I tools/gen_puzzles.py               # then: checks the table, rewrites src/puzzles_data.c

Those coins are history: the table changes only when a puzzle is solved, and its rows go. What
others send to an address is not in it; servers show it, as every coin of a puzzle address is
above their checkpoint (docs/en/NETWORK.md 8.2).
"""
import csv, json, os, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = ['https://mempool.space/api', 'https://blockstream.info/api']
CREATOR = ('08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15',   # as in tools/gen_puzzles.py
           '5d45587cfd1d5b0fb826805541da7d94c61fe432259e68ee26f4a04544384164',
           '12f34b58b04dfb0233ce889f674781c0e0c7ba95482cca469125af41a78d13b3')
PAUSE = 2.0                        # seconds between two requests


def coins(base, addr, raw_dir, name):
    with urllib.request.urlopen(urllib.request.Request('%s/address/%s/utxo' % (base, addr), headers={'User-Agent': 'curl/8'}), timeout=60) as r:
        body = r.read()
    if raw_dir:
        with open(os.path.join(raw_dir, name), 'wb') as f:
            f.write(body)
    time.sleep(PAUSE)
    return {(u['txid'], int(u['vout']), int(u['value']), int(u['status']['block_height']))
            for u in json.loads(body) if u['status']['confirmed'] and u['txid'] in CREATOR}


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
        rows += [(n, txid, vout, value) for txid, vout, value, height in sorted(got[0], key=lambda c: (c[3], c[0], c[1]))]
        print('puzzle %d: %d coins' % (n, len(got[0])), file=sys.stderr)
    with open(os.path.join(ROOT, 'data', 'prizes.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['n', 'txid', 'vout', 'sats'])
        w.writerows(rows)
    print('wrote data/prizes.csv: %d coins of %d open puzzles' % (len(rows), len(unsolved)))


if __name__ == '__main__':
    main()
