# The shared kangaroo network: implementation

This is the technical companion of the project document (the Italian *Progetto rete canguro condivisa*, version 7). It says how the
code is organized, what each module does, and the exact formats between modules. It is written for whoever works on the code; the
project document explains the reasons.

Status: under construction on the branch `rete-condivisa`. Section 9 lists what works and what does not yet.

## 1. What runs where

```
pzsolve (unchanged deps: libc only)          pzpeer (pzcore + lib/secp256k1)                 pzfirma (Rust, cggmp21)
  hunt --network FILE                           reads the network file and the home seed        the two-party ECDSA signer,
  walks rete-06 kangaroos                       publishes the distinguished points             one process per signing session,
  writes dp.bin as today          ---------->   stores the records of others (custodian)   <-> spoken to over stdin/stdout
                                                detects coincidences, sends alerts
                                                runs the signing session with the partner
                                                builds, checks and broadcasts the transactions
```

- **pzsolve** stays the program people use. In network mode (`hunt --network FILE`) the campaign takes its parameters from the
  network file, its jump table from the public jump seed of the file, and its starts from a secret key that pzpeer derives from the
  home seed. Nothing else changes in the search: the distinguished points go to `dp.bin` as in any campaign.
- **pzpeer** reads `dp.bin` as it grows, recomputes each point from its kind and distance, signs a record and publishes it. It is also
  a custodian for the records of others, and the one that pays.
- **pzfirma** is started by pzpeer for a signing session and dies at its end. It never touches the network or the disk except for its
  pregenerated primes: pzpeer relays its messages to the partner's pzfirma.

## 2. Source layout

| Path | Content |
|---|---|
| `lib/secp256k1/` | libsecp256k1 0.7.1, the library of Bitcoin Core (MIT), with the ecdh, extrakeys and schnorrsig modules. Used for every operation on a secret key: BIP340 signatures, ECDH, ECDSA of the coins of the home |
| `lib/pzcore/src/seed.c` | the generator of the home seed (several sources, health tests, HKDF) |
| `lib/pzcore/src/aead.c` | ChaCha20-Poly1305 (RFC 8439) |
| `src/peer/` | pzpeer, one file per layer (below) |
| `signer/` | pzfirma, a Rust crate |
| `tests/peer/` | unit tests of pzpeer (`make peer-test`) and the end-to-end test on regtest |

Layers of pzpeer, bottom up; each one only uses the ones above it in this list:

| File | Layer |
|---|---|
| `json.c` | a small JSON reader and writer (bitcoind, pzfirma) |
| `btc.c` | Bitcoin transactions: serialization, txid, legacy signature hash, scripts of addresses, DER |
| `btc_rpc.c` | the JSON-RPC client of bitcoind (HTTP/1.1, basic auth) |
| `netfile.c` | the network file: parsing, fingerprint, signatures of the maintainers |
| `keys.c` | the keys derived from the home seed |
| `noise.c` | Noise_XX_secp256k1_ChaChaPoly_SHA256 over a socket |
| `record.c` | the record of a distinguished point, its signature, the proof of possession |
| `store.c` | the custodian: records by key, on disk, coincidence detection |
| `dht.c` | Kademlia: routing table, lookups, store, alerts, bootstrap |
| `dpfeed.c` | reads the campaign's `dp.bin` and turns its records into points |
| `signer.c` | starts pzfirma and relays its frames |
| `session.c` | the session of a coincidence: channel, proofs, pieces, the two transactions |
| `main.c` | the daemon (event loop) and the commands |

## 3. Conventions

- C11 (`-std=gnu11`), the style of the rest of the repository; comments say why. Every file starts with the two SPDX lines
  (`AGPL-3.0-or-later`, `SPDX-FileCopyrightText: 2026 Leonardo Boulay`), the vendored library keeps its own.
- Secrets live in buffers that are wiped with `pz_secure_zero` when done. Operations on secret scalars use libsecp256k1, never the
  variable-time code of pzcore. pzcore is used for public data (points of the search, hashes).
- All integers on the wire are big-endian. Every hash has a domain tag: `SHA256(tag || data)` with an ASCII tag such as `"pznet1/record"`.
- Every input from the network is checked for length and range before use; a malformed message closes the connection.

## 4. Keys

The home seed is 32 bytes (section 3 of the project). Everything is derived from it with HKDF-SHA256, salt `"pznet1"`, and an info that
names the use and binds the network fingerprint `F` (section 5):

| Key | info | Use |
|---|---|---|
| identity | `"identity" || F || j` | BIP340 key of the home: signs records, sessions; Noise static key. `j` is the first counter for which the node ID has 16 leading zero bits (section 7.1) |
| starts | `"starts" || F` | the 32-byte secret seed of the starts of pzsolve (section 6) |
| coin n | `"coin" || F || n` | secp256k1 key of the n-th tiny coin of the home (P2PKH) |

## 5. The network file

A text file of `key=value` lines. The immutable part, whose fingerprint `F = SHA256("pznet1/file" || canonical lines)` binds keys and
records:

```
net=p140
puzzle=140
pubkey=03...                 # compressed public key of the puzzle
range=8000...:ffff...        # hex, inclusive
jumpseed=<64 hex>            # public seed of the jump table, the same for everyone
schema=rete-06
dpbits=30
chain=main                   # main, test, signet or regtest
```

The updatable part (`prize=txid:vout:sats`, one per line; `feecap=sats`; `entry=host:port`, one per line; `minversion=`) and the
signatures (`sig=<xonly pubkey hex>:<bip340 sig hex>`, over `SHA256("pznet1/sig" || F || canonical updatable lines)`). A file is
accepted when it has valid signatures from a threshold of the maintainer keys compiled into pzpeer; tests set their own keys with
`PZNET_TRUST=hexkey,...` and `PZNET_THRESHOLD=n`.

Canonical lines: the lines of the part, without comments and spaces at the ends, sorted, joined with `\n`.

## 6. pzsolve in network mode

- `pzsolve hunt --network FILE --start-key FILE2` (pzpeer passes the second file; `hunt --network` alone asks pzpeer for it). The campaign
  name is `net-<net>`.
- **Jump table**: from `jumpseed`, not from the campaign seed. Everyone walks the same graph.
- **Species**: the table `rete-06` (864 rows, in `src/kang_spec_net6.inc`). The species index of a walker is 16 bits.
- **Starts**: the draws of a start come from HMAC-SHA256 of the start key with `(stream, walker, start number)`, a fresh value for each
  start, instead of xoshiro: knowing one start says nothing about the others.
- **dpbits** from the file; the memory limit never thins the table in network mode (a thinned point is a lost coincidence for someone).
- `dp.bin` is the usual log of the campaign. pzpeer reads it.

## 7. pzpeer

### 7.1 Identity and transport

- Node ID = `SHA256("pznet1/id" || xonly identity pubkey)`. It must have 16 leading zero bits: generating an identity costs about 65 000
  key derivations, a few seconds; checking one costs one hash. (S/Kademlia's static puzzle.)
- Transport: TCP, `Noise_XX_secp256k1_ChaChaPoly_SHA256`.
  - DH: libsecp256k1 ECDH, output `SHA256(compressed shared point)`, as in BOLT 8.
  - Static keys are the identity keys (compressed points on the wire, x-only identity = their x).
  - Prologue: `"pznet1" || F`, so peers of another network fail the handshake.
  - After the handshake, frames of at most 65 535 bytes: a 2-byte encrypted length and the encrypted body, each with its own nonce, as in
    BOLT 8; keys rotate every 1000 messages.

### 7.2 The record of a distinguished point

Fixed 138 bytes:

| Bytes | Field |
|---|---|
| 1 | version, 1 |
| 1 | signed type t: 0, +1 or −1 (as two's complement byte) |
| 32 | x of the point; the point is the representative with even y |
| 32 | x-only identity of the owner |
| 8 | sequence number of the owner (strictly increasing) |
| 64 | BIP340 signature of the owner over `SHA256("pznet1/record" || F || the 74 bytes above)` |

The point is `R = t·Q' + d·G` with `Q' = P − base·G`, `base` the start of the range, `d` the owner's exact distance (an integer of up to
192 bits, kept at home). Checks on arrival: version, t in range, x on the curve with the zero bits of `dpbits` (on `x.n[1]` as pzsolve
counts them), signature.

**Proof of possession** of a record: a BIP340 signature by the key `d` (the scalar `d mod n`, adjusted so that `d·G = R − t·Q'`), over
`SHA256("pznet1/possess" || F || record || the identity of the asker || a nonce of the asker)`. It is asked before an alert has any effect.

### 7.3 Custody and coincidences

- Key of a record: `K = SHA256("pznet1/key" || x)`. A record goes to the 5 nodes whose IDs are closest to K (XOR distance).
- A custodian keeps records on disk (append-only file plus an index in memory by the first 8 bytes of K) and refuses a second record
  with the same x **from the same owner**.
- Same x, different owners: a coincidence. Same type: a merge (both walkers should restart). Different types: useful; the custodian sends
  `ALERT(record1, record2)` to both owners, and stores it in their mailboxes (the 5 nodes closest to `SHA256("pznet1/mbox" || owner)`).

### 7.4 Kademlia

k = 20, α = 3, 256 buckets. Messages (inside Noise frames, first byte the type):

| Type | Message | Answer |
|---|---|---|
| 1 | PING | 2 PONG |
| 3 | FIND_NODE(target 32) | 4 NODES(n, n × (node id 32, xonly 32, ip 16, port 2)) |
| 5 | STORE(n, n × record) | 6 STORED(n, n × status byte) |
| 7 | ALERT(record, record) | 8 OK |
| 9 | MBOX_GET(owner) | 10 MBOX(n, n × (record, record)) |
| 11 | POSSESS(record, nonce 32) | 12 PROOF(sig 64) |
| 16–31 | session messages (section 7.5) | |

Bootstrap: the `entry=` lines of the network file, the saved `peers.txt`, and addresses given with `--peer`. After the first contact
the node looks up its own ID to fill its buckets, and saves the contacts that answered.

### 7.5 The session of a coincidence

Between the two homes, over a Noise channel between their identities:

1. `S_HELLO(record_mine, record_yours)`; each side checks both records and asks the other's proof of possession.
2. The pieces. With `D = t1 − t2` (party 0 is the owner with the smaller identity):
   `x0 = −d1/D + base`, `x1 = d2/D` (mod n), `X0 = x0·G`, `X1 = x1·G`; both check `X0 + X1 = P`.
3. A proof of knowledge of the piece: a BIP340 signature by `x_i` over the transcript of the session.
4. The two transactions (section 8): each side builds them on its own and they compare the hashes.
5. pzfirma for each input of the puzzle: the frames of type 0x81 of one side go to the other as `S_SIGNER(bytes)`.
6. The first sender of the last message keeps its tiny coin as a cancel lever (project 2.4).

### 7.6 pzfirma

Frames on stdin and stdout: a 4-byte big-endian length L, then L bytes: a type byte and its payload.

| Type | Direction | Payload |
|---|---|---|
| 0x01 | pzpeer → pzfirma | JSON job |
| 0x02 | pzpeer → pzfirma | a message of the other party, opaque |
| 0x81 | pzfirma → pzpeer | a message for the other party, opaque |
| 0x82 | pzfirma → pzpeer | JSON result |
| 0x83 | pzfirma → pzpeer | JSON error, `{"error": "...", "blame": [i...]}` |

Job: `{"op": "sign", "i": 0|1, "eid": hex32, "share": hex32, "pub": [hex33, hex33], "key": hex33, "primes": path, "digests": [hex32, ...]}`.
`share` is the own additive piece (party 0's includes `base`), `pub` the two public pieces, `key` the puzzle key. Result:
`{"sigs": [{"r": hex32, "s": hex32}, ...]}`, `s` low. Commands: `pzfirma primes FILE` (pregenerates the primes of a session, minutes);
`pzfirma selftest` (both parties in one process). The session runs the auxiliary data generation, then one signing per digest with the
execution id `SHA256("pznet1/eid" || eid || i)`; no presignatures.

## 8. Transactions

- Inputs: P2PKH only (the puzzle address and the tiny coins), legacy signature hash, `SIGHASH_ALL`, low `s`.
- Outputs to any standard address of the chain of the network file.
- Amounts are verified from the previous transactions (their hash is their id).
- The rules of the project document (section 4.5): at most 8 inputs, coins worth less than 3 times their fee are left, half each.

## 9. Status

| Part | State |
|---|---|
| build of pzpeer, libsecp256k1 | done |
| everything else | in progress |
