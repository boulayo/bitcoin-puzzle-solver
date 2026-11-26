# Puzzle database

`puzzles.csv` holds the 160 puzzles of the Bitcoin Puzzle (the 1000 BTC puzzle): number `n`, P2PKH address, compressed public key (if it has ever appeared
on the blockchain) and private key (only for solved puzzles, which are public). `tools/gen_puzzles.py` turns this file into
`src/puzzles_data.c`, which is compiled into the executable.

## Provenance (acquired on 2026-10-21)

* List of addresses and keys: <https://btcpuzzle.info/puzzle> (structured data of the page) and
  <https://privatekeys.pw/puzzles/bitcoin-puzzle-tx> (HTML page). The two sources **agree on all 160 addresses and on all 83
  keys of the solved puzzles**.
* The known public keys come from the second source and were **recomputed** (not trusted): for solved puzzles they are `k·G`;
  for the 5 open puzzles that have a public key (140, 145, 150, 155, 160) the hash160 is checked against the address.

## Checks performed (at every regeneration and at every `pzsolve selftest`)

* Base58Check checksum and type (mainnet P2PKH) of every address;
* the key of a solved puzzle lies in its range `[2^(n-1), 2^n-1]`, `k·G` has the hash160 of the address and matches the stated
  public key;
* the public key of an open puzzle is a point of the curve and its hash160 (compressed form) matches the address.

`tools/gen_puzzles.py` uses the independent Python implementation `tools/ref.py` (no code in common with the C program);
`puzzles_verify()` (C) repeats the checks at every `selftest`. `tools/gen_puzzles.py --check` fails if `src/puzzles_data.c` is not
in line with the CSV.

Blockchain check (2026-10-21, public API of blockstream.info): **all 77 addresses of the open puzzles were still funded**, for a total
of about 903 BTC.

## Caveats

* The "solved / open" status is the one at the acquisition date: a puzzle may have been solved since. Before a long campaign
  check the address with a block explorer. The program does not access the network.
* A public key may appear on the blockchain in the future (when someone moves the funds): if you know a new one, pass it with
  `-k`. The program then chooses the suitable algorithm by itself.
* There is no secret in the file: solved puzzles have had their public key out for years; open puzzles have only an address (and, for five,
  a public key), that is, public data.

## Puzzles 161-256 (`puzzles_withdrawn.csv`, acquired on 2026-10-21)

The original transaction `08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15` (January 2015, block 339085) has
256 P2PKH outputs; output `i` was worth `(i+1)·0.001 BTC`. The file contains the addresses of outputs 161-256, taken from the public APIs of
**blockstream.info** and **mempool.space**, which agree byte for byte on all 256 outputs (script and amount).
Checks:

* outputs 1-160 coincide, in order, with the 160 addresses of `puzzles.csv` (obtained from other sources): output `n` is
  puzzle `n`;
* the amount of every output is `n` thousandths of a BTC;
* all outputs 161-256 were spent by **a single** transaction in block 475240 (July 2017): they are the funds that the creator
  withdrew to raise the prizes of puzzles 1-160;
* Base58Check of every address and no duplicate with `puzzles.csv` (`tools/gen_puzzles.py`, `puzzles_verify()`).

No public or private keys are known for these puzzles. They stay in the database only so that `hunt -p 161` works without `-a`.

## Coins of the addresses of the open puzzles (`prizes.csv`)

The coins at the address of every open puzzle (one without a private key in `puzzles.csv`, with or without a public key):
`n,txid,vout,sats`, the txid as explorers show it. Every confirmed unspent coin of the address worth at least 7400 satoshis, what an
input costs at 50 sat/vB, the lowest fee rate of a sweep (a payout of a pair takes a coin from 8880): the coins of the creator and what
strangers sent there. At the five addresses whose public key is known (140, 145, 150, 155, 160) the creator's are three:

* output `n - 1` of `08389f34c98c606322740c0be6a7125d9860bb8d5cb182c02f98461e5fa6cd15` (block 339085, January 2015), `n · 0.001` BTC;
* output `n - 52` of `5d45587cfd1d5b0fb826805541da7d94c61fe432259e68ee26f4a04544384164` (block 475240, July 2017), `n · 0.009` BTC;
* output `n - 76` of `12f34b58b04dfb0233ce889f674781c0e0c7ba95482cca469125af41a78d13b3` (block 785620, April 2023), `n · 0.09` BTC.

`tools/fetch_prizes.py` makes the file from the unspent coins of each address in the public APIs of **mempool.space** and
**blockstream.info** (`/api/address/ADDRESS/utxo`, nothing but the address in the request, a pause between requests): the two must
**agree on every coin** (txid, output, value, block), or nothing is written; `--raw DIR` keeps their answers. The rows of this revision
were acquired on 2026-11-26 for the 77 open puzzles: the two sources agreed on all 247 coins (the three of the creator at every
address, and 16 that strangers sent; puzzle 71 has the most, 7).

The rows become the `prize=` lines of the networks written in the program (`pzpeer netcheck 140` lists them) and the coins `pzpeer
sweep` looks up for the key of an address. A home without a node reads the chain from servers, which cannot show a coin older than
their checkpoint: with the coins named it pays them, builds the same first transaction as a home with a node, and counts them in what is
left at the address until each is spent (docs/en/NETWORK.md 5, 7.5, 8.2). The table is a snapshot: a coin spent since is found spent by
the program, and one that came later is above the checkpoint of this version, where a scan of the address lists it. A version that
moves the checkpoint forward needs the table made again, for the coins that came in between. `tools/gen_puzzles.py` checks that each row is of an open puzzle, with a txid of 64 hex digits, a value from 7400 satoshis
to 21 million BTC, no coin twice and at most 16 coins a puzzle (what a network names: it fails when an address has more);
`puzzles_verify()` repeats the first checks. A puzzle that is solved needs its rows removed.

## Updating the database

```sh
$EDITOR data/puzzles.csv                 # n,address,pubkey,privkey  (pubkey/privkey may be empty)
python3 -I tools/fetch_prizes.py --raw DIR   # data/prizes.csv: the coins of the addresses of the open puzzles, from two explorers
python3 -I tools/gen_puzzles.py          # checks every row and rewrites src/puzzles_data.c
make test                                # includes the check in C
```
