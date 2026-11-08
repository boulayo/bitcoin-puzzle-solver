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
| `src/netfile.c` | the network file: its lines, its fingerprint `F`, the values of the immutable part. libc and pzcore only: pzsolve and pzpeer link the same object |
| `src/peer/` | pzpeer, one file per layer (below) |
| `signer/` | pzfirma, a Rust crate |
| `tests/peer/` | unit tests of pzpeer (`make peer-test`) and the end-to-end test on regtest |

Layers of pzpeer, bottom up; each one only uses the ones above it in this list:

| File | Layer |
|---|---|
| `json.c` | a small JSON reader and writer (bitcoind, pzfirma) |
| `btc.c` | Bitcoin transactions: serialization, txid, legacy signature hash, scripts of addresses, DER |
| `btc_rpc.c` | the JSON-RPC client of bitcoind (HTTP/1.1, basic auth) |
| `netfile.c` | the signatures of the maintainers over the network file (the parsing is `src/netfile.c`) |
| `keys.c` | the keys derived from the home seed |
| `noise.c` | Noise_XX_secp256k1_ChaChaPoly_SHA256 and the frames after it: a state machine, no I/O |
| `net.c` | TCP: one connection per exchange, the handshake, frames, timeouts. The only file that calls the sockets of the system |
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
| identity | `"identity" || F || j` | BIP340 key of the home: signs records, sessions; Noise static key. `j` (4 bytes, big-endian) is the first counter for which the node ID has 16 leading zero bits (section 7.1; the unit tests use fewer) |
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

A line is `key=value`; from `#` on it is a comment, and spaces at the ends of the key and of the value do not count. Canonical lines: the
lines of the part rebuilt as `key=value`, sorted bytewise, joined with `\n` (no `\n` at the end). An unknown or repeated key of the
immutable part refuses the file. `puzzle=0` is a target of its own (tests); `dpbits` is 8 to 60; `schema` is `rete-06`.

## 6. pzsolve in network mode

- `pzsolve hunt --network FILE --start-key FILE2`: pzpeer writes the start key (64 hex digits, the key *starts* of section 4) to `FILE2`
  and starts pzsolve. The network file sets target, range, method and `--dp-bits`; the options that would change them are refused with it.
  The campaign is `net-<net>`.
- **campaign.conf** has `network=`, `jumpseed=`, `start_key_id=` (the `seed_to_id` of the start key) and `requires=1.24.0`; `seed=` is
  empty. A resume needs `--network` with the same network and jump seed and a start key with the same id; the reports and `FOUND.txt`
  show the seed as not written.
- **Jump table**: from `jumpseed` (`krng_seed(jumpseed, "jumps4")`), not from the campaign seed. Everyone walks the same graph.
- **Species**: the table `rete-06` (864 rows, `src/kang_spec_net6.inc`, made by `tools/research/kspec_table.py` from
  `tools/research/kang_net6.txt`). As with schema 3 the rows are grouped by (coefficient, residue mod 12): a walker belongs to a group for
  its life, and every start draws a band of the group by its share, so the index of a walker stays one byte (at most 64 groups).
- **Starts**: each start draws from a generator of its own, a xoshiro state set to `HMAC-SHA256(start key, "pznet1/s" || session || n)`
  (session: 8 bytes, the session number of the campaign and `--node` above it; n: 8 bytes, a counter of the starts of the session). A
  record shows the point and the type of a start, never the state of a generator that made another one.
- **Distinguished points**: `dpbits` of the file, whatever the walkers and the memory of this home would pick. A point the table leaves
  out (thinned by the RAM limit, or the table full) still goes to `dp.bin`: the table serves the coincidences of this home, `dp.bin` those
  of the network.
- `dp.bin` is the usual log of the campaign. pzpeer reads it.
- `pzsolve kstat net BITS TRIALS` measures the cost of the walk of the network (section 9).

## 7. pzpeer

### 7.1 Identity and transport

- Node ID = `SHA256("pznet1/id" || xonly identity pubkey)`. It must have 16 leading zero bits: generating an identity costs about 65 000
  key derivations, a few seconds; checking one costs one hash. (S/Kademlia's static puzzle.)
- Transport: TCP, `Noise_XX_secp256k1_ChaChaPoly_SHA256`.
  - DH: libsecp256k1 ECDH, output `SHA256(compressed shared point)`, as in BOLT 8.
  - Static keys are the identity keys (compressed points on the wire, x-only identity = their x).
  - Prologue: `"pznet1" || F`, so peers of another network fail the handshake.
  - The three handshake messages have fixed lengths, so they travel without a length: 33 bytes (`e`), 98 (`e`, `ee`, `s`, `es`, an
    empty payload), 67 (`s`, `se`, and as payload the port the initiator listens on, 2 bytes, 0 when it does not accept connections).
    The responder puts the initiator in its routing table with the address it sees and that port.
  - Both sides check that the node ID of the other's static key has the 16 zero bits. An initiator that dials a known contact also
    checks that the responder is that identity, before it sends the third message (the one that shows its own key).
  - After the handshake, frames of at most 65 535 bytes: a 2-byte encrypted length and the encrypted body, each with its own nonce, as in
    BOLT 8; keys rotate every 1000 messages.
  - One TCP connection per exchange: connect, handshake, one request frame, one answer frame, close. A session (messages 16–31) keeps its
    connection. Each connect, handshake message and frame has a deadline (10 s by default): a peer that stops talking costs a timeout.
    A malformed message, or a frame longer than the receiver takes, closes the connection without an answer.
  - Ephemeral keys: 32 bytes from the system (`getrandom`, or `/dev/urandom`).

### 7.2 The record of a distinguished point

Fixed 138 bytes:

| Bytes | Field |
|---|---|
| 1 | version, 1 |
| 1 | signed type t: 0, +1 or −1 (as two's complement byte) |
| 32 | x of the point; the point is the representative with even y |
| 32 | x-only identity of the owner |
| 8 | sequence number of the owner (strictly increasing: pzpeer uses the microseconds of the clock, always above the last it used) |
| 64 | BIP340 signature of the owner over `SHA256("pznet1/record" || F || the 74 bytes above)` |

The point is `R = t·Q' + d·G` with `Q' = P − base·G`, `base` the start of the range, `d` the owner's exact distance (an integer of up to
192 bits, kept at home). Checks on arrival: version, t in range, x on the curve with the zero bits of `dpbits` (on `x.n[1]` as pzsolve
counts them), signature.

**Proof of possession** of a record: a BIP340 signature by the key `d` (the scalar `d mod n`, adjusted so that `d·G = R − t·Q'`), over
`SHA256("pznet1/possess" || F || record || the identity of the asker || a nonce of the asker)`. It is asked before an alert has any effect.

### 7.3 Custody and coincidences

- Key of a record: `K = SHA256("pznet1/key" || x)`. A record goes to the 5 nodes whose IDs are closest to K (XOR distance).
- A custodian keeps records on disk and an index in memory by the first 8 bytes of K. The file, `store.bin` in the data folder, is
  append-only: a 40-byte header (`PZSTORE1` and F: a file of another network is refused) and the 138-byte records one after the other.
  At start the index is rebuilt from the file, and a record cut by a crash at its end is dropped.
- A custodian refuses a record that does not pass the checks of 7.2 or whose owner's node ID has not the zero bits (an owner is a node:
  the alerts find it by its ID), a second record with the same x **from the same owner** (duplicate: the first is kept), and a new
  record whose sequence number is not above the last one it has of that owner. So an owner sends its records to each custodian in the
  order of their numbers, and a republish signs them again with new numbers (7.4).
- Same x, different owners: a coincidence. Same type: a merge (both walkers should restart). Different types: useful. Both kinds are
  alerted the same way; the owners tell them apart by the types. The custodian sends `ALERT(record1, record2)` to both owners (found
  by their node IDs) and to the mailboxes of each (the 5 nodes closest to `SHA256("pznet1/mbox" || owner)`). A record that comes again
  (a duplicate) with a coincidence alerts again, at most once an hour for a pair: an alert lost on the way comes back with the next
  republish.
- A node that receives an `ALERT` checks both records (signatures, same x, different owners). It keeps the pair as a mailbox (in
  memory, the last 4096 pairs), and when one owner is itself it checks the proofs of possession: its own (the daemon gives the distance
  of the point) and the other owner's (`POSSESS`, with a nonce of its own). Only then the daemon gets the alert. An alert whose other
  owner does not answer is retried every minute for a day; a wrong proof drops it.
- An owner asks its 5 mailboxes for its pairs (`MBOX_GET`): at start, every hour, and every minute when it does not listen.

### 7.4 Kademlia

k = 20, α = 3, 256 buckets. Messages (inside Noise frames, first byte the type; an answer is its request + 1; `n` is one byte):

| Type | Message | Answer |
|---|---|---|
| 1 | PING | 2 PONG |
| 3 | FIND_NODE(target 32) | 4 NODES(n, n × (node id 32, xonly 32, ip 16, port 2)), n ≤ 20 |
| 5 | STORE(n, n × record), 1 ≤ n ≤ 255 | 6 STORED(n, n × status byte) |
| 7 | ALERT(record, record) | 8 OK |
| 9 | MBOX_GET(owner 32) | 10 MBOX(n, n × (record, record)), n ≤ 64, the newest first |
| 11 | POSSESS(record, nonce 32) | 12 PROOF(sig 64) |
| 16–31 | session messages (section 7.5) | |

- The lengths are exact. In NODES each contact must have the ID of its identity, with the zero bits, and a port; the ip is IPv6, an
  IPv4 address as `::ffff:a.b.c.d`. Any other answer is refused whole.
- STORED statuses: 0 stored, 1 duplicate, 2 invalid, 3 sequence number not above the last, 4 stored and a coincidence (alerts follow),
  5 the custodian could not write it. A record counts as held for 0, 1 and 4.
- POSSESS is answered only for a record of the node itself, with its signature; the proof binds the asker's identity, which the
  handshake told. A node that cannot prove closes the connection.
- The routing table: buckets by the number of leading bits an ID shares with the node's own, the least recently seen contact first. A
  contact goes to the tail when it answers or asks something. A full bucket replaces a contact that failed; otherwise it pings its
  oldest contact and keeps it if it answers (the newcomer waits as the candidate). Three failed exchanges in a row drop a contact. A
  node that does not listen (port 0) is never put in a table: it dials out, publishes, polls its mailboxes, and is never a custodian.
- Lookups are iterative: rounds of up to α FIND_NODE to the closest candidates not asked yet, among the `want` closest that did not fail;
  they end when those `want` have all answered. The result counts the node itself when it listens. `want` is 20 to find nodes, 5 to
  find the custodians of a record or the mailboxes of an owner.
- Publishing: a lookup for each record, then one STORE per custodian with all its records, in the order of their sequence numbers.
  pzpeer signs the records itself (owner, sequence number, signature) and sends them under one lock, so the numbers of one owner reach
  every custodian in order. Its own records are also kept in `own.bin` (the format of `store.bin`). Every day it publishes them again,
  signed with new numbers: a node that became a custodian since takes them, the others answer duplicate. Custodians do not copy
  records to each other.
- Bootstrap: the `entry=` lines of the network file, addresses given with `--peer`, and the saved `peers.txt` (whose lines name the
  identity too, which is checked). A PING to each, 16 at a time, until 8 answer; then a lookup of the own ID fills the buckets. The
  contacts that answered are saved to `peers.txt` (`address xonly-hex`, one per line) then, every 5 minutes and at stop. A node with an
  empty table tries again every 30 s.
- Every hour each bucket that had no lookup in its range gets one, for a random ID in it (from the first bucket a node ID can be in to
  the one after the deepest that has contacts).
- Threads: one accepts connections, 8 answer them (a connection that finds them all busy and 64 waiting is closed), one runs the jobs
  that take time (the alerts of a custodian, the proofs of an alert, the ping of a full bucket), one does the periodic work above.

### 7.5 The session of a coincidence

Between the two homes, over a Noise channel between their identities:

1. `S_HELLO(record_mine, record_yours)`; each side checks both records and asks the other's proof of possession.
2. The pieces. With `D = t1 − t2` (party 0 is the owner with the smaller identity):
   `x0 = −d1/D + base`, `x1 = d2/D` (mod n), `X0 = x0·G`, `X1 = x1·G`; both check `X0 + X1 = P`.
3. A proof of knowledge of the piece: a BIP340 signature by `x_i` over the transcript of the session.
4. The two transactions (section 8): each side builds them on its own and they compare the hashes.
5. pzfirma for each input of the puzzle: the frames of type 0x81 of one side go to the other as `S_SIGNER(more, bytes)`. A message of
   pzfirma can be larger than a Noise frame (the auxiliary data, about 200 KB): it travels in pieces of at most 65 000 bytes, `more` = 1
   on all but the last, and the receiver joins them before it hands the message to its pzfirma as one frame 0x02.
   The session id `eid` is `SHA256("pznet1/eid" || F || record_0 || record_1)`, the same on both sides and different for every pair of
   records; pzfirma relies on it to keep the messages of one run out of another.
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
`{"sigs": [{"r": hex32, "s": hex32}, ...]}`, `s` low and verified against `key` before it is given. Commands: `pzfirma primes FILE`
(pregenerates the two safe primes of a session: seconds to a minute; a file serves one session, and pzpeer deletes it after);
`pzfirma selftest` (both parties in one process). The session runs the auxiliary data generation with the execution id
`SHA256("pznet1/aux" || eid)`, then one signing per digest with `SHA256("pznet1/eid" || eid || u32_be(index))`; no presignatures.

- The messages between the two pzfirma are an envelope (version, sender, phase, broadcast flag, the library's message) that pzpeer
  relays as it is. On a failure pzfirma first sends an envelope of phase *abort* with the reason, then its 0x83: the other side stops at
  once instead of waiting. 0x82 or 0x83 is always the last frame.
- `blame` lists the parties the library names as culprits, or the other party when its message is malformed. It can be empty (no culprit
  is known) and, when a broadcast was not reliable, each side blames the other.
- The last message can be lost after one side already has the signatures: one side ends with 0x82 and the other with 0x83. This is the
  case of the cancel lever of step 6 of section 7.5.
- pzfirma has no timeouts: pzpeer ends a process that does not answer.

## 8. Transactions

- Inputs: P2PKH only (the puzzle address and the tiny coins), legacy signature hash, `SIGHASH_ALL`, low `s`.
- Outputs to any standard address of the chain of the network file.
- Amounts are verified from the previous transactions (their hash is their id).
- The rules of the project document (section 4.5): at most 8 inputs, coins worth less than 3 times their fee are left, half each.

## 9. Status

| Part | State |
|---|---|
| build of pzpeer, libsecp256k1 | done |
| TCP and the channel (`net.c`): one exchange per connection, timeouts, IPv4 and IPv6 | done (`tests/peer/t_net.c`); POSIX only, the Windows shim is to come |
| custody (`store.c`): records on disk, index, duplicates, sequence numbers, coincidences, mailbox | done (`tests/peer/t_store.c`) |
| Kademlia (`dht.c`): table, lookups, bootstrap, `peers.txt`, publish and republish, alerts, mailboxes, proofs | done (`tests/peer/t_dht.c`: 30 nodes in one process); no limit of contacts per address block yet (project 4.2), no UPnP or NAT-PMP |
| `pzpeer node` | a node with no points of its own (custodian and mailbox only), for manual tests |
| the daemon: `dp.bin` fed to `dht_publish`, alerts to sessions over `dht_dial` and `on_session` | to do |
| everything else | in progress |
