# The shared kangaroo network: implementation

This is the technical companion of the project document (the Italian *Progetto rete canguro condivisa*, version 7). It says how the
code is organized, what each module does, and the exact formats between modules. It is written for whoever works on the code; the
project document explains the reasons.

Status: under construction on the branch `rete-condivisa`. Section 9 lists what works and what does not yet.

## 1. What runs where

```
pzsolve (unchanged deps: libc only)          pzpeer (pzcore + lib/secp256k1)                 pzfirma (Rust, cggmp21)
  hunt --jump-seed --start-key                  reads the network file and the home seed        the two-party ECDSA signer,
  walks rete-06 kangaroos                       publishes the distinguished points             one process per signing session,
  writes dp.bin as today          ---------->   stores the records of others (custodian)   <-> spoken to over stdin/stdout
                                                detects coincidences, sends alerts
                                                runs the signing session with the partner
                                                builds, checks and broadcasts the transactions
```

- **pzsolve** stays the program people use, and knows nothing of the network. pzpeer starts it with the options of a shared walk
  (section 6): the range and the key of the network file, its jump seed, its dp bits, and a secret start key that pzpeer derives from the
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
| `netfile.c` | the network file: its lines, its fingerprint `F`, the values of the immutable part, the signatures of the maintainers |
| `btc.c` | Bitcoin transactions: serialization, txid, legacy signature hash, scripts of addresses, DER |
| `btc_rpc.c` | the JSON-RPC client of bitcoind (HTTP/1.1, basic auth) |
| `noise.c` | Noise_XX_secp256k1_ChaChaPoly_SHA256 and the frames after it: a state machine, no I/O |
| `keys.c` | the keys derived from the home seed |
| `record.c` | the record of a distinguished point, its signature, the proof of possession |
| `net.c` | TCP: one connection per exchange, the handshake, frames, timeouts. The only file that calls the sockets of the system |
| `store.c` | the custodian: records by key, on disk, coincidence detection, mailboxes |
| `dht.c` | Kademlia: routing table, lookups, store, alerts, bootstrap |
| `dpfeed.c` | reads the campaign's `dp.bin` and turns its records into points |
| `pieces.c` | the two pieces of the key of a coincidence |
| `signer.c` | starts pzfirma and relays its frames |
| `payout.c` | the transactions of the pair: the first, the second, the cancel |
| `session.c` | the session of a coincidence, from the hello to the payout (section 7.5) |
| `chain_rpc.c` | the chain of a session on bitcoind |
| `home.c` | the daemon of a home: seed and keys, pzsolve, the feed of `dp.bin`, a session for every useful coincidence |
| `main.c` | the commands |

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

### 4.1 The home seed

`pz_seed_generate` (`lib/pzcore/include/pzcore/seed.h`, `lib/pzcore/src/seed.c`). pzpeer calls it once, when a home is created;
pzsolve uses it for the seed of every new campaign.

| Source, in the order it is read | What is read | Bits credited | Counts for the 128 |
|---|---|---|---|
| CPU generator: RDSEED, else RDRAND (x86-64); RNDRRS, else RNDR (arm64) | 128 words; 1024 from RDRAND or RNDR | 128 (RNDR 64) | no |
| TPM 2.0: `TPM2_GetRandom` on `/dev/tpmrm0` or `/dev/tpm0` (Linux), TBS (Windows) | 8 × 32 bytes | 128 | yes |
| the caller's source (GPU timing jitter from pzsolve or pzpeer, later) | fixed by the caller, at most 64 KiB | what it claims, at most 256 | yes |
| CPU timing jitter: timer deltas around a fixed walk through 1 MiB | rounds of 4096 samples | 0.5 per sample that is not stuck, at most 256 | yes |
| OS generator: getrandom, getentropy, BCryptGenRandom | 2 × 512 bytes | 256 | mandatory |
| system context: process ids, clocks, host, boot id, OS, CPU | | 0: it is the public salt | |

**Rules.**
- The CPU generator is read first, while no other secret is in the process; the OS last, so that its output spends the least time in
  memory and comes after any snapshot of a virtual machine taken during the slower steps.
- Each source tests its own raw output and is excluded and wiped when it fails; the others go on. A TPM that is absent or that the user
  may not open (the `tss` group) is skipped.
- If the OS fails there is no seed (`PZ_SEED_E_OS`).
- The TPM, the caller's source and the jitter must give at least 128 bits together. The CPU generator does not count: nobody can check
  it, and the kernel often seeds its own generator from it (`random.trust_cpu`). When they fall short, the jitter source collects more
  rounds, for at most 3 s; still short, there is no seed (`PZ_SEED_E_SHORT`, "make it on another computer"). A campaign seed of pzsolve
  accepts the OS alone in that case, as before (`PZ_SEED_ALLOW_DEGRADED`, at most 0.2 s more of jitter).
- Health tests (SP 800-90B 4.4, cutoffs from the claimed entropy): for the outputs that claim full entropy (OS, CPU generator, TPM) the
  repetition count and adaptive proportion tests at H = 8, α = 2^-40, the count of every byte value and a monobit test; for the CPU
  generator also the words all ones (the AMD RDRAND erratum), zero, or repeated, and the retry limits; for the TPM well-formed answers of
  the size asked, all different; for the jitter a timer that advances and is monotonic, jitterentropy's stuck test, the repetition
  count and adaptive proportion tests at H = 0.5, α = 2^-30, and a live estimate, min(MCV, 8 × Markov), of at least 1 bit per sample:
  twice the credit.
- Mixing: HKDF-SHA256 over labelled records that carry their lengths, salt = SHA-256 of the context (the format is in `seed.h`). One
  honest source with enough entropy keeps the seed unpredictable; a dishonest one that sees everything else can only grind.
- Memory: the raw outputs live in one region locked in RAM, left out of core dumps and seen as zeros by a forked child, wiped before it
  is unmapped; `PZ_SEED_HARDEN` also turns core files off and makes the process non-dumpable. The report gives the bits and the reasons
  of each source, never data.

**Measured** on 2026-11-08 on the development machine, a VM (Intel Xeon at 2.3 GHz, 4 vCPUs under KVM, Linux 6.18), with
`tools/research/seed_assess.c`, which runs the generator's own collector:

| | |
|---|---|
| Min-entropy of the jitter, 10^6 samples | **6.61 bits per 8-bit sample** (lowest estimator: t-tuple; MCV 6.89, 8 × Markov 6.78). The prototype measured 6.44 on the same kind of VM |
| Samples for 128 bits | 39 at a safety factor of 2, 78 at 4. The generator credits 0.5 bit per sample, 13 times less, and needs 256 |
| Restart test (SP 800-90B 3.1.4), 1000 processes × 1000 samples, H_I = 6 | sanity check passed (a value at most 30 times at one position, cutoff 36); min(H_rows, H_columns) = 6.44 |
| Live estimate, 1000 generations | 5.70 to 6.13 bits per sample, median 5.99 (the threshold is 1); 4058 to 4090 samples of 4096 not stuck; no second round ever needed |
| Time of a seed | median 9.7 ms, 99% under 14 ms (the jitter is 97% of it); with the other 3 vCPUs busy, median 22 ms, live estimate unchanged |
| Elsewhere | under Wine 5.9 to 6.0 bits per sample, and TBS answers "no TPM"; under qemu-aarch64 (RNDRRS, emulated counter) 3.0, which says nothing of real arm64 |
| `lib/pzcore/tests/t_seed.c` | 39 cases of broken, missing and attacker-known sources, all as the rules say; a malicious CPU generator sets a XOR combiner to its target, and with every other input known 2^16 tries fix 13 to 17 bits of the HKDF seed (about log2 of the work); 2000 seeds with only the OS honest: no duplicate, bits and bytes balanced |

**Limits.**
- One machine was measured, and it is a VM: its TSC is always even (at most 7 bits in an 8-bit sample), and part of the noise comes
  from the host, which can observe it. ARM machines with a 24 MHz counter (Apple M, many Neoverse) and quiet desktops need their own
  assessment; the 0.5-bit credit is a policy with a wide margin, not a certified value.
- No TPM here: the TPM code was tested with a file that stands for the device. RNDR on Windows is not detected.
- The CPU generator and the TPM cannot be checked from outside, and the tests on conditioned output (OS, CPU generator, TPM) catch only
  gross failures. That is why no source has to be trusted alone.
- A seed copied with a VM image, or generated in a VM that is then cloned, is the same in every copy; the context does not tell clones
  apart. The home seed belongs on a physical computer.
- The platform is trusted: a malicious kernel, CPU or hypervisor can read the seed. Registers and copies made by the compiler are not
  wiped.

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

pzsolve knows nothing of the network: pzpeer reads the network file and starts pzsolve with the options of a **shared walk**:

```
pzsolve hunt -r START:END -k PUBKEY --dp-bits N --jump-seed HEX --start-key FILE -c net-NAME -d DIR --ui log
```

- `--jump-seed`: the jump table comes from this public seed (`krng_seed(jumpseed, "jumps4")`), not from the campaign seed: every home
  walks the same graph. `--start-key`: the 64 hex digits of the key *starts* of section 4, written by pzpeer to a file only this user reads.
- **campaign.conf** has `jumpseed=`, `start_key_id=` (the `seed_to_id` of the start key) and `requires=1.24.0`; `seed=` is empty. A resume
  needs the same jump seed and a start key with the same id; the reports and `FOUND.txt` show the seed as not written.
- **Species**: the table `rete-06` (864 rows, `src/kang_spec_net6.inc`, made by `tools/research/kspec_table.py` from
  `tools/research/kang_net6.txt`). As with schema 3 the rows are grouped by (coefficient, residue mod 12): a walker belongs to a group for
  its life, and every start draws a band of the group by its share, so the index of a walker stays one byte (at most 64 groups).
- **Starts**: each start draws from a generator of its own, a xoshiro state set to `HMAC-SHA256(start key, "pznet1/s" || session || n)`
  (session: 8 bytes, the session number of the campaign and `--node` above it; n: 8 bytes, a counter of the starts of the session). A
  record shows the point and the type of a start, never the state of a generator that made another one.
- **Distinguished points**: exactly `--dp-bits`, whatever the walkers and the memory of this home would pick. A point the table leaves out
  (thinned by the RAM limit, or the table full) still goes to `dp.bin`: the table serves the coincidences of this home, `dp.bin` those
  of the network.
- `dp.bin` is the usual log of the campaign. pzpeer reads it.
- `pzsolve kstat net BITS TRIALS` measures the cost of the walk of the network. On the curve, W = 2^40, 3000 trials each, one thread:
  schema 3 1.061 ± 0.011, rete-06 1.182 ± 0.012 steps per √W: rete-06 costs 11.4 % more, as the simulator predicted (+11 %), the price
  of the schema that makes a theft pay only above 51 % of the power of the network (project 2.3).

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

Between the two homes, over a Noise channel between their identities (`src/peer/session.c`, run in a thread with the channel and the
chain given as interfaces). Party 0 is the owner with the smaller identity; `eid = SHA256("pznet1/eid" || F || record_0 || record_1)`;
F, the party whose tiny coin goes into the second transaction, is the last bit of `eid`, S the other. Every step is a message both sides
send and then read (types 16 to 22: `S_HELLO`, `S_PROOF`, `S_PLAN`, `S_SIGNER`, `S_SIGNED`, `S_TINYSIG`, `S_ABORT`; an `S_ABORT` with its
reason can come instead of any of them):

1. `S_HELLO`: my record, your record, a nonce, the fee rate I propose (8 bytes), the script of my tiny coin, my payout script. Each side
   checks the records, that they are a useful coincidence, that they belong to the two ends of the channel and that the other hello is
   about the same pair. The fee rate is the higher of the two, refused above the home's limit; the payout scripts must be of standard
   addresses. The nonce is `SHA256("pznet1/nonce" || coin key || eid)`: unpredictable to the partner, no randomness needed.
2. `S_PROOF`: the proof of possession of my record over your nonce. It shows the distance, hence the piece, so no other proof of the piece
   is needed. The pieces (`src/peer/pieces.c`): with `D = t0 − t1`, `x0 = base − d0/D`, `x1 = d1/D` (mod n); either public piece is
   computed by anyone from the record (`d·G = R − t·Q'`), and `X0 + X1 = P`.
3. `S_PLAN`: `SHA256("pznet1/plan" || the first transaction)`, which each side built on its own from the confirmed coins of the puzzle
   (section 8); a different hash stops the session. Then the two-party signing of its inputs (step 5), the broadcast by both, and one
   confirmation.
4. `S_PLAN` again for the second transaction, built from the first; then F sends `S_TINYSIG`, the scriptSig of its tiny coin, which S checks.
5. The two-party signing: pzfirma on each side, its messages (0x81) relayed as `S_SIGNER(more, bytes)` in pieces of at most 65 000 bytes,
   `more` = 1 on all but the last piece; `S_SIGNED(status)` ends the messages of a side. **S sends its k-th message only after the k-th of F
   has arrived**: F's last message reaches S before S's reaches F, so S has the whole signature first, and F, whose tiny coin is in the
   transaction, holds the cancel. Each signing has its id `SHA256("pznet1/run/" || 'A' or 'B' || eid)`; both use the primes file of the
   session.
6. S sends the second transaction. F waits 3 blocks for the prize to move; if it does not, F sends the cancel, its tiny coin to its payout
   address, and the transaction S may be holding is worth nothing. F also sends the second transaction itself when it got the whole
   signature.

`tests/peer/t_session.c` runs both homes against each other with the real pzfirma and a chain in memory that checks every input: the
payout (half each, F's tiny coin back to F, S keeps its own), and a partner that keeps the second transaction and its last message, which
ends with F's cancel and the prize still under the joint key.

Not yet handled: a first transaction whose signature encoding a third party changes before it is mined (its txid changes: the session waits
for its own txid and gives up); the state of a session is in memory only (after a restart the pair starts again from the coins as they are).

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
- Amounts: the signature of a legacy input does not cover the value it spends, and a value too low would give the difference to the
  miners. Where a value comes from a source that is not trusted, it is verified from the previous transaction (its hash is its id):
  `btc_prevout` takes the raw bytes of the previous transaction, in either serialization, and gives the value and the script of the
  output only when the hash of those bytes is the txid. With the home's own node (8.1) the node itself is the verification.
- Fees: in satoshis per 1000 virtual bytes, as `estimatesmartfee` gives them (BTC per kvB, read exactly, never through a double). Our
  transactions are legacy, so their virtual size is their size; it is counted with every signature at its longest (71 bytes of DER
  with a low `s`, and the hashtype), and the fee is rounded up: the rate paid is never below the one agreed.
- What we build is standard: version 2, P2PKH inputs, every output above the dust limit of P2PKH (546 satoshis, the highest of the
  standard forms), at most 16 inputs and 16 outputs, far below the size limit.
- The vectors of `tests/peer/t_btc.c` come from transactions of the main chain, pinned by their txids, and from
  `tests/peer/gen_btc_vectors.py`, an independent implementation in Python of the serialization, the legacy signature hash, RFC 6979 and
  DER (`python3 tests/peer/gen_btc_vectors.py > tests/peer/btc_vectors.h`).
- The rules of the project document (section 4.5): at most 8 inputs, coins worth less than 3 times their fee are left, half each.
- `tests/peer/fuzz_peer.c` mutates JSON documents, raw transactions and HTTP replies and checks that the readers agree with each other
  (`make peer-fuzz`, under the sanitizers: `make BUILD=build_asan OPT="-O1 -fsanitize=address,undefined" peer-fuzz FUZZ_SECONDS=300`).

### 8.1 The home's node

`pzpeer run --rpc` is the home's own bitcoind, on the same machine or over a link the user trusts (the JSON-RPC is plain HTTP with basic
authentication). It is trusted as a wallet trusts its node: it validated every block, and what it says of its own set of unspent outputs
is the chain, values included. So the coins of the puzzle address are taken with their values from `scantxoutset`, which reads that set:
a pruned node serves as well as a full one. (The puzzle coins are of 2015: a pruned node no longer has their blocks, nor the bytes of
the transactions that made them, and those bytes would come from the same node anyway.) A node that is not trusted could hide coins,
report spent ones, or give a value too low: a node of someone else is not supported, and 8.2 is the way without a node of one's own.
Every reply is still read as hostile input: it is refused, never read past its buffers, when it is malformed.

On the main chain:

- Credentials: `--rpc HOST:PORT --rpc-cookie FILE` takes them from the `.cookie` that bitcoind writes in its data directory when it has
  no `rpcpassword` (its default; `~/.bitcoin/.cookie` on the main chain, `~/.bitcoin/testnet3/.cookie`, `signet/`, `regtest/`), and no
  password goes on the command line, where the list of processes shows it. Only the user of bitcoind reads the file, unless bitcoind
  runs with `-rpccookieperms=group` (Bitcoin Core 28). bitcoind writes a new cookie each time it starts: the client reads the file
  again when the node refuses it. `--rpc HOST:PORT:USER:PASSWORD` still works (the password is the rest of the text; `[::1]:8332`
  for IPv6). `pzpeer run` checks the option when it starts and says where the node is; a node that is not running yet (no cookie)
  only delays the sessions.
- A node that is behind: a session starts only on a node of the chain of the network file (`chain` of `getblockchaininfo`, where
  `testnet4` counts as `test`), out of its initial download, with its blocks at most one behind the headers it knows
  (`chain_rpc_ready`). A node on another chain stops `pzpeer run` at its start; one still syncing only delays the sessions.
- The scan reads the whole set of unspent outputs, minutes on the main chain, and the node runs one at a time: a session waits for the
  scan of another (the partner on the same node, a wallet) as long as a scan may take (30 minutes). A scan it gives up on is aborted:
  the node would go on with it, and refuse the next one meanwhile.
- Thousands of tiny coins may be sent to a puzzle address. The scan keeps the 64 largest (equal values by outpoint, the order of
  `payout_first`, so that both homes keep the same), not the first 64 the node lists: it lists them in the order of their txids,
  where the prize may come last.
- When the node does not answer for a while, the height is the last it gave, never 0: a session counts blocks from a height, and F
  would cancel at once.

Not yet handled: a puzzle address with more than about 45 000 coins makes the reply of the scan larger than the 16 MB of the JSON
reader, and no session starts. It would cost an attacker about 0.3 BTC (the coins of 546 satoshis and their fees), much less than a
prize. The prizes of the network file (`prize=txid:vout:sats`) can each be checked with `gettxout`, with no scan: the session does not
use them yet.

### 8.2 Without a node of one's own (to come)

The light verification of project 4.4: the coins come from several servers, and nothing they say is believed without a proof. The
shortest sound design is on the Electrum protocol, which the servers of most wallets speak; plain TCP is enough, as every answer is
proven (and TLS would be a new dependency).

1. **Headers.** A checkpoint (the height and hash of a block, a line of the signed network file), and every header after it from 3 or
   more servers chosen at random among the `electrum=` lines of the file and those of the user. Each header is checked: the hash of the
   one before, its proof of work below its target, the target of every 2016 blocks, its time above the median of the 11 before. The
   valid chain with the most work is kept on disk, 80 bytes a block; a server that sends an invalid header is dropped. It is also the
   clock of the session: the blocks F waits count on it, and no server can make them come faster without the work.
2. **Coins.** `blockchain.scripthash.listunspent` of the puzzle address from each server, and the union of what they list. For each
   coin, the transaction that made it (`blockchain.transaction.get`), whose hash must be its txid (`btc_prevout`: its value and its
   script), and its Merkle branch (`blockchain.transaction.get_merkle`) to the header of its height in the kept chain: then it is
   confirmed, as deep as that header. A "transaction" of 64 bytes is refused, as it could be an inner node of the tree passed off
   as a leaf (CVE-2017-12842).
3. What a lying server can still do: hide a coin, which is taken later by another session, or list a coin already spent, which makes
   the first transaction invalid: it is refused, and the session starts again with other servers. The two homes compare the hash of
   the first transaction as they do now, so different views stop the session before any signing.
4. **Our transactions** go to every server, and are confirmed when a Merkle branch of their txid links to the kept chain. For the wait
   of F, the prize has moved only when a transaction that spends it is confirmed that way (the second, or the same with another
   encoding of a signature): a server that just stops listing the prize proves nothing, and the partner could run it. Servers that
   hide a confirmed second transaction only make F send a cancel that is invalid, its coin being spent already.
5. **Fee rate:** the median of `blockchain.estimatefee` of the servers, under the cap of the file.

The code: the headers and their rules (a few hundred lines), the Merkle branch (twenty), a client of the Electrum JSON-RPC over TCP (one
JSON object per line: simpler than `btc_rpc.c`), and `chain_electrum.c`, a second `session_chain` beside `chain_rpc.c`. The main chain
first: the minimum difficulty of testnet and the signed blocks of signet are more rules for little use.

## 9. Status

Everything below works and is tested; `tests/peer/e2e_net.sh` runs the whole of it on one machine with a bitcoind in regtest: a custodian
node and three homes (`pzpeer run`, each with its pzsolve), a puzzle of 58 bits funded with 1 BTC. With Bitcoin Core 28.1 a coincidence
between two homes ended in the payout: 0.49999366 and 0.49989366 BTC (the difference is F's tiny coin, back to F), nothing left on the
puzzle address. With so few homes a home may find the key on its own first (its own walkers meet too): the script then starts a new try
with a new puzzle. The cases of the session (the payout, and the cancel when a partner keeps the second transaction) also run on the node
in `t_session` with `PZ_REGTEST`.

| Part | State |
|---|---|
| pzsolve in network mode (section 6) | done; `tests/e2e.sh`, `kstat net` (rete-06 measured on the curve) |
| pzcore: ChaCha20-Poly1305, the generator of the home seed (section 4.1) | done (`t_aead`: RFC 8439 vectors; `t_seed`); the GPU source of the seed to come |
| `noise.c`, `keys.c`, `record.c`, `dpfeed.c` | done (`t_noise`: byte for byte against the noiseprotocol package of Python; `t_record`; `pzpeer dpcheck` on real dp.bin files) |
| `net.c`, `store.c`, `dht.c` | done (`t_net`, `t_store`, `t_dht`: 30 nodes in one process); POSIX only (the Windows shim is to come); no limit of contacts per address block yet (project 4.2), no UPnP or NAT-PMP |
| `json.c`, `btc.c`, `btc_rpc.c`, `chain_rpc.c` | done (`t_json`, `t_btc`, `t_btc_rpc`, `t_chain_rpc`, `fuzz_peer`; `t_session` with `PZ_REGTEST` on Bitcoin Core 28.1; the coins also on a pruned node); the light verification of section 8.2 to come |
| pzfirma, `signer.c`, `pieces.c`, `payout.c`, `session.c` | done (`cargo test`; `t_pieces`, `t_payout`, `t_session`: the payout and the cancel, in memory and on regtest) |
| `netfile.c`, `pzpeer netsign`, `netcheck` | done (`t_trust`); no maintainer keys compiled in yet: only test networks run (`PZNET_TRUST`) |
| `home.c`, `pzpeer run` | done (`tests/peer/e2e_net.sh`) |
| to come | the restart of the walkers of a merge (now only logged); a session that survives a restart of pzpeer; the light verification of the chain without a node of one's own (project 4.4, section 8.2); the spot checks of the walks (project 4.2); the Windows and macOS builds of pzpeer; the public test |

## 10. A test network on one machine

```sh
make && make peer && make signer            # pzsolve, pzpeer, pzfirma (Rust: cargo builds GMP the first time)
make peer-primes && make peer-test          # the unit tests, the signing ones with the real pzfirma
bitcoind -regtest -daemon -rpcuser=pz -rpcpassword=pz -fallbackfee=0.0002
PZ_REGTEST=127.0.0.1:18443:pz:pz build/peer/t_session    # the session on the node (with PZFIRMA and the primes, as make peer-test sets them)
PZ_REGTEST=127.0.0.1:18443:pz:pz tests/peer/e2e_net.sh   # the whole network: a custodian, two homes, a payout
```

By hand, a network file (section 5) is signed with a key of 64 hex digits (`pzpeer netsign KEY FILE >> FILE`), that key's x-only form goes
into `PZNET_TRUST`, and every home runs

```sh
pzpeer run --network FILE --dir HOME --payout ADDRESS --rpc 127.0.0.1:18443:pz:pz --listen PORT --peer 127.0.0.1:OTHER -- -t 2 --no-gpu
```

With a bitcoind of its own and no `rpcpassword`, `--rpc 127.0.0.1:8332 --rpc-cookie ~/.bitcoin/.cookie` takes its credentials from its
cookie file instead (section 8.1).

The home seed is made on the first run in `HOME/home.seed` (keep a copy: it is the key of every point of the home); `HOME/start.key` is
the start key pzpeer gives to pzsolve; the campaign is in `HOME/campaigns/net-NAME`, its output in `HOME/pzsolve.out`.
