# The shared kangaroo network: implementation

This is the technical companion of the project document ([PROGETTO_RETE.md](../it/PROGETTO_RETE.md), in Italian; the decisions still
open are in [DA_FARE.md](../it/DA_FARE.md)). It says how the code is organized, what each module does, and the exact formats between
modules. It is written for whoever works on the code; the project document explains the reasons and states the assumptions.

Status: under construction on the branch `rete-condivisa`. Section 9 lists what works and what does not yet.

## 1. What runs where

```
pzsolve (unchanged deps: libc only)          pzpeer (pzcore + lib/secp256k1)                 pzsign (Rust, cggmp21)
  hunt --jump-seed --start-key                  reads the network file and the home seed        the two-party ECDSA signer,
  walks net-06 kangaroos                       publishes the distinguished points             one process per signing session,
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
- **pzsign** is started by pzpeer for a signing session and dies at its end. It never touches the network or the disk except for its
  pregenerated primes: pzpeer relays its messages to the partner's pzsign.

## 2. Source layout

| Path | Content |
|---|---|
| `lib/secp256k1/` | libsecp256k1 0.7.1, the library of Bitcoin Core (MIT), with the ecdh, extrakeys and schnorrsig modules. Used for every operation on a secret key: BIP340 signatures, ECDH, ECDSA of the coins of the home |
| `lib/pzcore/src/seed.c` | the generator of the home seed (several sources, health tests, HKDF) |
| `lib/pzcore/src/aead.c` | ChaCha20-Poly1305 (RFC 8439) |
| `src/peer/` | pzpeer, one file per layer (below) |
| `signer/` | pzsign, a Rust crate |
| `tests/peer/` | unit tests of pzpeer (`make peer-test`) and the end-to-end test on regtest |

Layers of pzpeer, bottom up; each one only uses the ones above it in this list:

| File | Layer |
|---|---|
| `json.c` | a small JSON reader and writer (bitcoind, pzsign) |
| `netfile.c` | the network file: its lines, its fingerprint `F`, its values |
| `sys.c`, `sys_posix.c`, `sys_win.c` | what pzpeer asks of the operating system (`sys.h`): the clock, randomness, sockets, child processes, the files of secrets, the stop signals. One implementation for Linux and macOS, one for Windows; every other file is the same code on the three |
| `dbg.c` | the debug log (`--debug FILE`) |
| `btc.c` | Bitcoin transactions: serialization, txid, legacy signature hash, scripts of addresses, DER |
| `btc_rpc.c` | the JSON-RPC client of bitcoind (HTTP/1.1, basic auth) |
| `noise.c` | Noise_XX_secp256k1_ChaChaPoly_SHA256 and the frames after it: a state machine, no I/O |
| `keys.c` | the keys derived from the home seed |
| `record.c` | the record of a distinguished point, its signature, the proof of possession |
| `net.c` | TCP: one connection per exchange, the handshake, frames, timeouts |
| `store.c` | the custodian: records by key, on disk, coincidence detection, mailboxes |
| `dht.c` | Kademlia: routing table, lookups, store, alerts, bootstrap |
| `dpfeed.c` | reads the campaign's `dp.bin` and turns its records into points |
| `pieces.c` | the two pieces of the key of a coincidence |
| `signer.c` | starts pzsign and relays its frames |
| `payout.c` | the transactions of the pair: the first, the second, the cancel |
| `session.c` | the session of a coincidence, from the hello to the payout (section 7.5) |
| `bt.c` | the rendezvous in the Mainline DHT of BitTorrent: announce and lookup of the network's key |
| `upnp.c` | the request to the home router to let the listening port in |
| `chain_rpc.c` | the chain of a session on bitcoind |
| `spv.c` | block headers checked by their proof of work, Merkle branches |
| `electrum.c` | a client of the Electrum protocol (plain TCP) |
| `chain_spv.c` | the chain of a session without a node: several Electrum servers, every answer proven |
| `sweep.c` | `pzpeer sweep`: the transaction that moves the coins of a key found by a linear search, signed and not sent |
| `block.c` | the claim of a block of a shared linear search and its proof of work (section 11; not used by the daemon yet) |
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

A text file of `key=value` lines. The lines that define the network:

```
net=p140
puzzle=140
pubkey=03...                 # compressed public key of the puzzle
range=8000...:ffff...        # hex, inclusive
jumpseed=<64 hex>            # public seed of the jump table, the same for everyone
schema=net-06
dpbits=30
chain=main                   # main, test, signet or regtest
```

Their fingerprint `F = SHA256("pznet1/file" || canonical lines)` **is** the network: every key, record, message and the rendezvous key
are bound to it, so two nodes are on the same network exactly when their files have the same `F`. Nobody signs a file. Whoever changes
one of those lines is on another network, as a torrent with another hash is another torrent; the file of a public network comes with
the program, as the parameters of a chain come with a node of that chain.

The other lines are settings of the node that reads them, and each is checked where it is used: `entry=host:port` (a node to start
from), `electrum=host:port` and `checkpoint=HEIGHT:HASH` (section 8.2), `prize=txid:vout:sats` (a coin of the puzzle, found even when
the scan of its address fails; its value and script are read from the chain). Any number of each.

A line is `key=value`; from `#` on it is a comment, and spaces at the ends of the key and of the value do not count. Canonical lines: the
defining lines rebuilt as `key=value`, sorted bytewise, joined with `\n` (no `\n` at the end). An unknown key, or a repeated or missing
defining key, refuses the file. `puzzle=0` is a target of its own (tests); `dpbits` is 8 to 60; `schema` is `net-06`.

## 6. pzsolve in network mode

pzsolve knows nothing of the network: pzpeer reads the network file and starts pzsolve with the options of a **shared walk**:

```
pzsolve hunt -r START:END -k PUBKEY --dp-bits N --jump-seed HEX --start-key FILE -c net-NAME -d DIR --ui log
```

- `--jump-seed`: the jump table comes from this public seed (`krng_seed(jumpseed, "jumps4")`), not from the campaign seed: every home
  walks the same graph. `--start-key`: the 64 hex digits of the key *starts* of section 4, written by pzpeer to a file only this user reads.
- **campaign.conf** has `jumpseed=`, `start_key_id=` (the `seed_to_id` of the start key) and `requires=1.24.0`; `seed=` is empty. A resume
  needs the same jump seed and a start key with the same id; the reports and `FOUND.txt` show the seed as not written.
- **Species**: the table `net-06` (864 rows, `src/kang_spec_net6.inc`, made by `tools/research/kspec_table.py` from
  `tools/research/kang_net6.txt`). As with schema 3 the rows are grouped by (coefficient, residue mod 12): a walker belongs to a group for
  its life, and every start draws a band of the group by its share, so the index of a walker stays one byte (at most 64 groups).
- **Starts**: each start draws from a generator of its own, a xoshiro state set to `HMAC-SHA256(start key, "pznet1/s" || session || n)`
  (session: 8 bytes, the session number of the campaign and `--node` above it; n: 8 bytes, a counter of the starts of the session). A
  record shows the point and the type of a start, never the state of a generator that made another one. The session number is saved
  when the session starts, not at its first checkpoint: a run cut before that would leave its number to the next, which would draw the
  same starts again.
- **Distinguished points**: exactly `--dp-bits`, whatever the walkers and the memory of this home would pick. A point the table leaves out
  (thinned by the RAM limit, or the table full) still goes to `dp.bin`: the table serves the coincidences of this home, `dp.bin` those
  of the network.
- `dp.bin` is the usual log of the campaign. pzpeer reads it.
- `pzsolve kstat net BITS TRIALS` measures the cost of the walk of the network. On the curve, W = 2^40, 3000 trials each, one thread:
  schema 3 1.061 ± 0.011, net-06 1.182 ± 0.012 steps per √W: net-06 costs 11.4 % more, as the simulator predicted (+11 %), the price
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
    BOLT 8; a key rotates after 1000 encryptions (500 frames).
  - One TCP connection per exchange: connect, handshake, one request frame, one answer frame, close. A session (messages 16–31) keeps its
    connection. The connect has a deadline (10 s by default), the whole handshake one more (a peer that sends a byte now and then gains
    nothing: before, each of its three messages had a deadline of its own), and each frame one: a peer that stops talking costs a
    timeout. A malformed message, or a frame longer than the receiver takes, closes the connection without an answer.
  - A relayed channel (7.7) runs a second handshake, end to end, on the same socket after the relay joined it to the partner's; there the
    responder checks the initiator's identity too.
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
- What a custodian takes is bounded (a record costs its owner one signature, with any x that has the zero bits: no discrete log is
  needed, so a flood is cheap to make):
  - only records of the node that sends them (an owner stores its own; custodians do not copy), and only records whose key it may be a
    custodian of: it refuses one when its table knows 10 nodes closer to K (a sum over buckets: a contact of bucket j is closer exactly
    when bit j of `ID ⊕ K` is 1). An honest publisher found it among the 5 closest, so it knows at most 4 closer; a flood sprayed at
    random nodes is refused, and one aimed at a node costs about (nodes / 10) tries per record;
  - within a rate per address block of the sender (the table of the limits in 7.4);
  - at most `store_max` records in all (`--store-gb`, 37 GB by default, 2^28 records; the index in memory is about 1/8 of the file), at
    most 1/16 of them of one owner, at most 8 owners of one x.
  The checks that cost nothing come first, the signature last.
- `store.bin` is the custodian's own file, so its records are not checked again when it is opened, except the last 1024: a crash can
  leave a record whole in length and wrong in content, which could give its owner a huge sequence number and refuse all its next ones.
  A record that fails there is not indexed. The records a coincidence is made of are checked again when they are read back, so a record
  damaged in the middle of the file never becomes an alert.
- Same x, different owners: a coincidence. Same type: a merge (both walkers should restart). Different types: useful. Both kinds are
  alerted the same way; the owners tell them apart by the types. The custodian sends `ALERT(record1, record2)` to both owners (found
  by their node IDs) and to the mailboxes of each (the 5 nodes closest to `SHA256("pznet1/mbox" || owner)`). A record that comes again
  (a duplicate) with a coincidence alerts again, at most once an hour for a pair: an alert lost on the way comes back with the next
  republish. A record the custodian does not keep because it is full is still alerted. Each alert costs the custodian four lookups and a
  dozen connections, so a record alerts at most 2 pairs, and the coincidences of the records of one address block are limited (7.4).
- A node that receives an `ALERT` checks both records (signatures, same x, different owners). It keeps the pair as a mailbox when it may
  be a mailbox of one of the owners (the rule of the custodian above, for the key of the mailbox), in memory: the last 4096 pairs, at
  most 64 of one owner (a flood of the pairs of one owner pushes out its own). When one owner is itself it checks the proofs of
  possession: its own (the daemon gives the distance of the point) and the other owner's. Only then the daemon gets the alert.
- The other owner's proof comes one of two ways. `POSSESS`, with a nonce of the asker, when the other owner listens. Or the other owner
  sends it: once its own proof checks out, an owner sends `ALERT(its record, the other record, proof)` to the other owner when it can
  dial it and to the other owner's mailboxes, and again every hour while the alert waits. That proof is over the pair: the asker is the
  other owner and the nonce is the name of the pair, `SHA256("pznet1/pair" || the two records in the order of their owners)`. It binds
  the asker, so it proves nothing to anyone else, and a fixed nonce lets it wait in a mailbox; a copier cannot make it without the
  distance. A mailbox checks it and keeps it with the pair, and `MBOX` carries it. So two homes that do not listen prove their points to
  each other through their mailboxes (before, both alerts waited for a day and no session could start).
- An alert that waits is tried again after a minute, then twice as late each time up to an hour, for a day; a wrong proof drops it. A
  node remembers 1024 alerts: when they are all waiting, the oldest goes, so a flood of alerts that will never be proved (copies of a
  home's points by identities that do not answer) cannot shut out the next real one, which is tried at once; an alert pushed out comes
  back with the next republish or poll.
- An owner asks its 5 mailboxes for its pairs (`MBOX_GET`): at start, every hour, and every minute when it does not listen.

### 7.4 Kademlia

k = 20, α = 3, 256 buckets. Messages (inside Noise frames, first byte the type; an answer is its request + 1; `n` is one byte):

| Type | Message | Answer |
|---|---|---|
| 1 | PING | 2 PONG |
| 3 | FIND_NODE(target 32) | 4 NODES(n, n × (node id 32, xonly 32, ip 16, port 2)), n ≤ 20 |
| 5 | STORE(n, n × record), 1 ≤ n ≤ 255 | 6 STORED(n, n × status byte) |
| 7 | ALERT(record, record), or ALERT(record, record, proof 64): the proof of the first record's owner for the second's (7.3) | 8 OK |
| 9 | MBOX_GET(owner 32) | 10 MBOX(n, n × (record, record, flags, proof 64, proof 64)), n ≤ 64, the newest first; the records in the order of their owners, bit i of flags set when the proof of the owner of record i is there (else zeros) |
| 11 | POSSESS(record, nonce 32) | 12 PROOF(sig 64) |
| 13 | RELAY(name 32, partner 32) (7.7) | 14 RELAYED, when the partner came; then the bytes of the partner's connection |
| 16–31 | session messages (section 7.5) | |

- The lengths are exact. In NODES each contact must have the ID of its identity, with the zero bits, a port, and an address the node may
  dial; no address twice (one address with two identities is no honest table), at most 4 contacts of one address block. The ip is IPv6,
  an IPv4 address as `::ffff:a.b.c.d`. Any other answer is refused whole.
- STORED statuses: 0 stored, 1 duplicate, 2 invalid (also: a record of another owner than the sender), 3 sequence number not above the
  last, 4 stored and a coincidence (alerts follow), 5 the custodian could not write it, 6 refused for now (the custodian is full, or
  the owner's share, or the owners of its x, or its address block is over its rate), 7 not a custodian of it (it knows 10 nodes closer
  to its key). A record counts as held for 0, 1 and 4.
- Addresses. On a public network (the chain of the network file is `main`) a node dials and keeps only public addresses: not
  unspecified, loopback, private (RFC 1918, RFC 4193), shared (RFC 6598), link-local, benchmarking, documentation, multicast or reserved.
  A NODES answer that points elsewhere is refused (it would make the node connect into the user's own LAN), and so is a contact that
  asks from such an address (it is answered, not kept). On a test network (`test`, `signet`, `regtest`, or `--allow-private`) they are
  taken, and the limits by block below do not apply to them: the nodes of a test are on one machine.
- The limits by address block: the /24 of an IPv4 address, the /48 of an IPv6 one, the whole address of one that is not public (on a
  public network). They apply to public addresses everywhere, and to all addresses on a public network.

  | Limit | Value |
  |---|---|
  | contacts of one block in a bucket, in the table, in a NODES answer | 2, 4, 4 |
  | queries of one lookup, to one block | 64, 4 (a candidate past them counts as failed) |
  | connections of one block queued or being answered at a time; new ones | 4; 20 a second, 100 at once (a peer that opens connections and says nothing holds 4 handlers at most) |
  | records stored from one block | 50 a second, 100 000 at once (a republish sends them all) |
  | ALERTs received from one block | 2 a second, 64 at once (checked before the signatures) |
  | coincidences alerted for the records of one block | one a minute, 8 at once; and 256 alerts of a custodian waiting in its jobs |
  | relays (waiting or under way) of the homes of one block; waits of one identity | 2; 2 |

  The rates are buckets of tokens, kept for 4096 blocks (an entry with no connection is reused, the one used longest ago).
- POSSESS is answered only for a record of the node itself, with its signature; the proof binds the asker's identity, which the
  handshake told. A node that cannot prove closes the connection.
- The routing table: buckets by the number of leading bits an ID shares with the node's own, the least recently seen contact first. A
  contact goes to the tail when it answers or asks something. A full bucket pings its weakest contact (the one that failed most, the
  oldest among equals) and keeps it if it answers; the newcomer waits as the candidate, and a candidate that answered a request of this
  node is not pushed aside by one that only asked. Before, a contact that had missed one answer was replaced at once, which an attacker
  on the path could use to swap good contacts for its own. Three failed exchanges in a row drop a contact. A contact enters the table
  only when it answers or asks, never from a NODES answer, and only within the limits by block: one attacker's nodes, on its few
  networks, cannot fill a bucket or the table (project 4.2). A known contact that moves to a block that has its share keeps its old
  address. A node that does not listen (port 0) is never put in a table: it dials out, publishes, polls its mailboxes, and is never a
  custodian.
- Lookups are iterative: rounds of up to α FIND_NODE to the closest candidates not asked yet, among the `want` closest that did not fail;
  they end when those `want` have all answered, or after 64 queries (answers that keep pointing at new nodes cannot make a lookup
  endless). A lookup asks at most 4 nodes of one address block: answers that all point at one network, a victim or the attacker's own,
  do not make the node dial it more. The result counts the node itself when it listens. `want` is 20 to find nodes, 5 to find the
  custodians of a record or the mailboxes of an owner.
- Publishing: a lookup for each record, then one STORE per custodian with all its records, in the order of their sequence numbers.
  pzpeer signs the records itself (owner, sequence number, signature) and sends them under one lock, so the numbers of one owner reach
  every custodian in order. Its own records are also kept in `own.bin` (the format of `store.bin`). Every day it publishes them again,
  signed with new numbers: a node that became a custodian since takes them, the others answer duplicate. Custodians do not copy
  records to each other. The last number signed is kept in `own.seq`: `own.bin` keeps the numbers of the first publication only, and a
  node that restarted with its clock behind its last republish would sign below what the custodians saw, and be refused by all of them.
- Bootstrap: the `entry=` lines of the network file, addresses given with `--peer`, and the saved `peers.txt` (whose lines name the
  identity too, which is checked). A PING to each, 16 at a time, until 8 answer; then a lookup of the own ID fills the buckets. The
  contacts that answered are saved to `peers.txt` (`address xonly-hex`, one per line) then, every 5 minutes and at stop. A node with an
  empty table tries again every 30 s.
- Every hour each bucket that had no lookup in its range gets one, for a random ID in it (from the first bucket a node ID can be in to
  the one after the deepest that has contacts).
- Threads: one accepts connections, 8 answer them (a connection that finds them all busy and 64 waiting is closed, and so is one whose
  block is over its limits), one runs the jobs that take time (the alerts of a custodian, the proofs of an alert, the ping of a full
  bucket), one does the periodic work above, one copies the bytes of each relay under way (7.7).

### 7.5 The session of a coincidence

Between the two homes, over a Noise channel between their identities (`src/peer/session.c`, run in a thread with the channel and the
chain given as interfaces). Party 0 is the owner with the smaller identity; `eid = SHA256("pznet1/eid" || F || record_0 || record_1)`;
F, the party whose tiny coin goes into the second transaction, is the last bit of `eid`, S the other. Every step is a message both sides
send and then read (types 16 to 22: `S_HELLO`, `S_PROOF`, `S_PLAN`, `S_SIGNER`, `S_SIGNED`, `S_TINYSIG`, `S_ABORT`; an `S_ABORT` with its
reason can come instead of any of them; type 23, `S_ALIVE`, empty, goes every minute while a side waits for blocks, so that NATs and
relays do not cut the channel as idle, and the reader skips it):

1. `S_HELLO`: my record, your record, a nonce, the fee rate I propose (8 bytes), the script of my tiny coin, my payout script. Each side
   checks the records, that they are a useful coincidence, that they belong to the two ends of the channel and that the other hello is
   about the same pair. The fee rate is the higher of the two. Each proposes twice the estimate of its chain for a confirmation within 2 blocks, at
   least 20 and at most 1000 sat/vB: these transactions move a prize, and no such fee is large beside it. A rate above the home's
   limit ends the session. The tiny coins grow with the rate: each is twice the fee of F's cancel and the dust limit, 10 000 satoshis
   at least, so F can cancel at any rate (were its coin worth less than that fee, S could hold the second transaction back with
   nothing to fear); the payout scripts must be of standard addresses. The nonce is 32 bytes from the system, new in every session: the partner's
   proof is made over it, and the two nonces make the ids of the signings (step 5), which must never repeat.
2. `S_PROOF`: the proof of possession of my record over your nonce. It shows the distance, hence the piece, so no other proof of the piece
   is needed. The pieces (`src/peer/pieces.c`): with `D = t0 − t1`, `x0 = base − d0/D`, `x1 = d1/D` (mod n); either public piece is
   computed by anyone from the record (`d·G = R − t·Q'`), and `X0 + X1 = P`. The hello and the proof of the partner are awaited 30 s at
   most (it makes them at once); every later message 5 minutes.
3. `S_PLAN`: `SHA256("pznet1/plan" || the first transaction)`, which each side built on its own from the confirmed coins of the puzzle
   (section 8); a different hash stops the session. Then the primes of pzsign (`pzsign primes`, seconds to a minute of CPU: only now, when
   the partner has proved its distance and builds the same transaction), the two-party signing of its inputs (step 5), the broadcast by
   both, and one confirmation.
4. `S_PLAN` again for the second transaction, built from the first; then F sends `S_TINYSIG`, the scriptSig of its tiny coin, which S checks.
5. The two-party signing: pzsign on each side, its messages (0x81) relayed as `S_SIGNER(more, bytes)` in pieces of at most 65 000 bytes,
   `more` = 1 on all but the last piece; `S_SIGNED(status)` ends the messages of a side. Each signing has its id
   `SHA256("pznet1/run/" || 'A' or 'B' || eid || nonce_0 || nonce_1)` (the nonces of party 0 and party 1); both use the primes file of the
   session. In the signing of the second transaction **S holds each of its messages until F's message of the same rank has reached its
   pzsign and its pzsign has gone on past it**, with its next message or with the signatures: F's last message reaches S before S's
   last leaves, and S's leaves only once S has the whole signature. A last message of F that is garbage, or the end of F's messages instead
   of it, gets nothing in exchange (counting messages is not enough: any bytes, or an `S_ABORT`, in place of F's last would get S's). So S has the whole
   signature first, and F, whose tiny coin is in the transaction, holds the cancel. The first transaction only gives the prize back to the
   puzzle: there both send at once. pzpeer never relays the envelope with which pzsign tells why it gave up: its reason would say which
   check failed (project 4.6); the `S_SIGNED` that follows stops the partner as well.
6. S sends the second transaction. F waits 3 blocks for the prize to move; if it does not, F sends the cancel, its tiny coin to its payout
   address, and the transaction S may be holding is worth nothing. F also sends the second transaction itself when it got the whole
   signature, and then does not cancel. A wait for n blocks counts them from the first height the node gives, and gives up after
   (n + 6) × 20 minutes if the chain stops (a node that stops answering keeps its last height): the session ends instead of waiting for
   ever, and F, then, tries its cancel.

**The limit of failed signings** (project 4.6). A bug of the signer could leak a bit of a piece through each signing that fails (a selective
failure), so the home counts them in `HOME/failed-signings`, lines `<hex of F or of an eid> <failures>`: every run of pzsign counts as failed
before it starts (written through a new file, synced, that replaces the old one) and is taken off when it gives valid signatures, so a crash
or a kill in the middle counts. At 10 failures on a network, with any partner, or 3 for one pair, the home signs no more on it, and says so
before the plan of the first transaction, so that the partner's pzsign does not start (it would count a failure of its own). Runs are one
at a time; a session that does not get its turn within 2 minutes gives up without a run. Not done yet: the count published in the network
and countersigned by the partner, so that it survives a restore of the seed; the file is next to the seed and goes with it in a copy.

**The daemon** (`src/peer/home.c`). A session starts on the alert of the table (both proofs checked there) or on a hello that names the
pair; anyone can copy the x of a published record into a record of its own, so a session started by a hello alone costs little before
the proofs (the primes come after them, step 3), at most 16 of them run at a time, one per partner, and their slots are given back when
they end unless the partner proved its distance: at least 48 of the 64 slots are always there for the pairs the table alerted. A pair
that the table alerted, or whose partner proved its distance, and whose session did not pay starts again 10 minutes after, for a day.
The primes file of a session is removed when it ends, and at the start of the daemon those left by a crash (a Paillier key: with the
messages of the session the partner could find the piece).

`tests/peer/t_session.c` runs both homes against each other with the real pzsign and a chain in memory that checks every input: the
payout (half each, F's tiny coin back to F, S keeps its own; with a chain that loses its first answer about the height, and the primes
made by the session); a partner that keeps the second transaction and its last message, which ends with F's cancel and the prize still
under the joint key; a partner F that stops, or sends garbage, in place of its last message, which gets nothing and cancels; the same at a
high fee rate, where the tiny coins are larger and F still cancels; the limit of failed signings, refused before anything is signed; a partner that copied the x of a
record and cannot prove it, for which no primes are made; a pzsign that stops reading while the other floods it; a scan of the puzzle
address that fails, where the prize of the network file is paid; a chain that stops after the first transaction, where the wait ends.
The counts of the limit are checked in every case, and every channel carries `S_ALIVE` before the messages that are read.

Not yet handled: a first transaction whose signature encoding a third party changes before it is mined (its txid changes: the session waits
for its own txid and gives up); the state of a session is in memory only (after a restart the pair starts again from the coins as they are);
F takes a spend of the prize in its node's mempool for the payout (chain_rpc asks `gettxout` with the mempool), so a second transaction
that S sends and that then leaves the mempools unconfirmed would be S's to hold again; two sessions of one puzzle at once build their
first transactions from the same coins, and the one that loses waits its 144 blocks.

### 7.6 pzsign

Frames on stdin and stdout: a 4-byte big-endian length L, then L bytes: a type byte and its payload.

| Type | Direction | Payload |
|---|---|---|
| 0x01 | pzpeer → pzsign | JSON job |
| 0x02 | pzpeer → pzsign | a message of the other party, opaque |
| 0x81 | pzsign → pzpeer | a message for the other party, opaque |
| 0x82 | pzsign → pzpeer | JSON result |
| 0x83 | pzsign → pzpeer | JSON error, `{"error": "...", "blame": [i...]}` |

Job: `{"op": "sign", "i": 0|1, "eid": hex32, "share": hex32, "pub": [hex33, hex33], "key": hex33, "primes": path, "digests": [hex32, ...]}`.
`share` is the own additive piece (party 0's includes `base`), `pub` the two public pieces, `key` the puzzle key. Result:
`{"sigs": [{"r": hex32, "s": hex32}, ...]}`, `s` low and verified against `key` before it is given. Commands: `pzsign primes FILE`
(pregenerates the two safe primes of a session: seconds to a minute; a file serves one session, and pzpeer deletes it after);
`pzsign selftest` (both parties in one process). The session runs the auxiliary data generation with the execution id
`SHA256("pznet1/aux" || eid)`, then one signing per digest with `SHA256("pznet1/eid" || eid || u32_be(index))`; no presignatures.

- The messages between the two pzsign are an envelope (version, sender, phase, broadcast flag, the library's message) that pzpeer
  relays as it is. On a failure pzsign first sends an envelope of phase *abort* with the reason, then its 0x83; pzpeer does not relay
  that envelope (section 7.5, step 5) and ends the messages of its side with `S_SIGNED`, which stops the other side at once instead of
  waiting. 0x82 or 0x83 is always the last frame.
- `blame` lists the parties the library names as culprits, or the other party when its message is malformed. It can be empty (no culprit
  is known) and, when a broadcast was not reliable, each side blames the other.
- The last message can be lost after one side already has the signatures: one side ends with 0x82 and the other with 0x83. This is the
  case of the cancel lever of step 6 of section 7.5.
- pzsign has no timeouts: pzpeer ends a process that does not answer, and kills it once its last frame came or did not come in time
  (a pzsign that stopped reading would otherwise keep the relay of the partner's messages waiting for ever). Its pipes are not
  inherited by any other program pzpeer starts, nor are dp.bin and the sockets.

**The review of how pzsign uses the library** (by the author of pzsign, not an outside audit). What was read: the whole of
`signer/src`, and in cggmp21 0.6.3 the auxiliary data generation and the interactive signing.

- The key share is built from the two additive pieces only after `x_i·G` equals the own public piece and the two pieces add up to
  the puzzle key; the partner's piece is not taken from the partner (pzpeer computes it, section 7.5).
- The auxiliary data of the partner is taken only with a modulus of the full size and its three proofs (ring-Pedersen parameters,
  Paillier-Blum modulus, no small factor), all bound to the execution id and to the party; the own primes are checked to be two
  different safe primes of 1536 bits and are used for one session.
- Signing is the interactive protocol with the reliable broadcast on (the default); pzsign calls no presignature and no key
  derivation function, so CVE-2025-66017 (presignatures) has no code path here. Every execution id comes from the session id, which
  two homes never repeat (section 7.5).
- A signature leaves pzsign only after it verifies against the puzzle key and the own digest, with code that is not the library's
  in the tests.
- `signer/tests/session.rs` plays the partner that cheats: one byte changed in each message it sends, in turn; a session sent again
  message by message, under the same and under a new execution id; another digest; another execution id; a share that is not its
  piece; pieces that are not the key. In every case the honest party ends without a signature.

What this does not give: a proof that the library has no flaw. Its protocol has a published security proof, but an implementation
can differ from it (CVE-2025-66016 was such a case, fixed in 0.6.3), and finding that kind of flaw is the work of an outside audit.
The limit of failed signings (section 7.5) bounds what a flaw that leaks a little at each abort can give.

### 7.7 The relay

Most homes are behind a router that takes no connection from outside, and a session needs a channel between the two homes of a pair. When
neither can dial the other, a node that listens joins their connections (`src/peer/dht.c`, the copy in `net_splice`).

- **Which relay.** Time is cut in slots of 60 s (`s = unix time / 60`). The relay of a pair in slot s is the node closest to
  `SHA256("pznet1/relay" || name || s)` (s as 8 bytes, big-endian; name: the eid of the session) that is neither home, found by a
  lookup: both homes find the same one without a word between them. A relay that is down, full or lying costs one slot, and the next
  slot has a relay of its own under another key. A fixed list tried in order would not do: its first relay could hold both homes for
  ever by answering nothing, and two homes that fail on different relays of the list would never meet. So no node placed near one key
  can stop a pair. The clocks of the two homes must agree within a few seconds (a wait lasts a slot and a quarter).
- **The joins.** Each home dials the relay (checking its identity), sends `RELAY(name, partner)` and waits until the end of its slot.
  The relay matches two joins with the same name where each names the other as its partner (the handshake told the relay who joined),
  so a third party that names the pair is joined to nobody. It answers `RELAYED` to both, and from then on copies the bytes of each
  connection to the other as they come. It closes a join over its limits, and both joins of a pair it cannot serve; a home whose
  partner did not come is closed after a slot and a quarter, and a join whose partner left meanwhile is not matched.
- **End to end.** After `RELAYED` the two homes run a Noise XX handshake through the relay, on the same sockets: party 0 (the smaller
  identity) initiates, and each side checks that the other's static key is the partner (the initiator before it shows its own). The
  relay sees ephemeral keys and ciphertext only: it can stop or delay the channel, not read or change it. The session then runs on it as
  on a direct one.
- **Limits at the relay:** 8 relays under way at once and 32 homes waiting; 2 relays (waiting or under way) per address block of the
  homes and 2 waits per identity; 32 MiB per relay, both ways together (a session moves a few MB: the auxiliary data of pzsign is
  about 200 KB a message); 4 hours per relay (the channel stays open while the session waits for the confirmation of its first
  transaction). A relay under way is one thread that copies through the receive buffers of the two connections.
- **The channel of a session:** `dht_channel(partner, eid)` gives the daemon one connection with the partner, whichever way it comes.
  Party 0 dials the partner it finds in the table, and so does a side that does not listen (nobody can dial it). A side that listens
  gives the partner's own connection (the one `on_session` got) the first slot. Then the relay of each slot, slot after slot, while the
  partner's connection is still taken if it comes; for 10 minutes. `home.c` asks it for every session.
- The relay carries sessions only: the proofs of an alert go through the mailboxes (7.3), the requests of the table are not relayed.

`tests/peer/t_net.c` splices two connections, runs 6 MB through them end to end, and checks both identities and the limit of bytes.
`tests/peer/t_dht.c` has two homes that do not listen prove their points to each other through their mailboxes and run a stand-in
session of 2 × 2.4 MB through the relay of their slot, then tries the limits of a relay: a wait without a partner, a third party, the
relays at once, the bytes.

### 7.8 What the network layer still lacks for a public network

| Missing | Why it is not done |
|---|---|
| NAT-PMP and PCP, the other two ways a home opens its own port | UPnP is done (7.9) and covers most home routers; these two are for the routers that only speak them |
| IPv6 as a way out of NAT | addresses, listening (`::` takes IPv4 too) and NODES carry IPv6 already, but a home does not learn or announce a public IPv6 address of its own: a responder keeps the address it sees. There is no IPv6 on the test machine |
| Tor, optional (project 2.5) | a SOCKS5 client in `net.c`, and onion addresses in NODES, which has 16 bytes for an address: a new format. A node behind Tor cannot be a custodian, like one that does not listen |
| A defence of the rendezvous key in the BitTorrent DHT | nodes that sit close to the key there can hide the announces (7.9). The rule that ties a node ID of that table to its address (BEP 42) would make it dear; many nodes of that table do not follow it yet |
| Spot checks of the walks (project 4.2) | they need the previous distinguished point of a walk from its owner, so pzsolve and `dp.bin` would have to keep the chain of each walker; a check costs minutes of a core and needs a protocol of its own (ask, walk, revoke a machine) |
| A republish that scales | every day each home sends all its records again: at the end of a campaign with 10 000 GPUs that is about 8·10^7 records a home a day, far above the traffic of project 4.3. A custodian should get only what it lacks (a digest of what it holds, or the republish only to the nodes that became custodians) |
| Open connections instead of polls | a home that does not listen polls its mailboxes every minute (a lookup and 5 connections); the project wants it to keep a few connections open to reachable nodes and get its alerts on them |
| More than one relay at a time | a pair tries one relay per slot; two homes that cannot reach the same node in any slot (a network that blocks part of the Internet) get no channel |
| The cost of an identity | 16 zero bits cost seconds, so thousands of identities are cheap: the limits by block bound what one network can do, not what many networks (a botnet, a cloud) can |
| An index that fits a home | 2^28 records take about 4.5 GB of RAM for the index; a larger store needs its index on disk |

### 7.9 Finding the network with no server of its own

The owner of the network runs no server. Three things make the first contact, used together:

- `peers.txt`: the contacts that answered in an earlier run.
- `--peer HOST:PORT`: an address from someone who is already in.
- **The Mainline DHT of BitTorrent** (`bt.c`, BEP 5), for who has neither. Every node that listens announces its port there under the
  key `SHA256("pznet1/bt" || F)` cut to 20 bytes, and every node looks the key up: a round when it starts, then every half hour (every
  two minutes while it has no contact). A round is a lookup with `get_peers`, eight questions at a time, until the eight closest nodes
  that answered are closer than any node not asked yet (96 questions and 20 seconds at most), then `announce_peer` to those eight.
  pzpeer asks and never answers: it is a guest of that table for some tens of packets every half hour, over UDP and IPv4. A network of
  the main chain starts from the well-known routers of that table (`router.bittorrent.com:6881`, `dht.transmissionbt.com:6881`,
  `router.utorrent.com:6881`, `dht.libtorrent.org:25401`), unless `--no-bt`; `--bt-router HOST:PORT` gives others, and is the only
  way a test network uses it.

Only addresses pass through that table, and none is believed: each is a candidate for the next bootstrap, which dials it with Noise
and takes it only when it is a node of this network with a valid node ID; an address that is not public is dropped on a network of
the main chain. The records, the alerts and the sessions never go there. That table takes an announce only for the address it comes
from (the token of BEP 5), so nobody can announce a third party to make the nodes dial it.

What an attacker can do there: put nodes close to the key, take the announces and give back nothing or only its own nodes. A node
that enters for the first time then finds nobody, or only the attacker, who can hide records from it and delay its alerts (section
7.4 says what the table does against that) and can steal nothing: records are signed, a coincidence is proven by both owners, and
the payout needs both signatures. `peers.txt` and `--peer` do not depend on that table. A packet from that table counts only when it
comes from the address asked, with the number of the question, and is read within its bounds (`t_bt` runs nodes that send random
bytes, answers to other questions, nodes that do not exist, and peers without end).

**The port.** A node listens on TCP port 28140 unless `--listen` says otherwise (`--listen 0`: it only dials out). Behind a home
router it is dialled only when the router lets the port in, and pzpeer asks the router for it unless `--no-upnp` (`upnp.c`,
UPnP IGD): a search on the LAN (SSDP), the description of the router, and `AddPortMapping` for the `--listen` port over TCP, for two
hours, asked again every hour; a router that only takes mappings without end gets one. Nothing the router says is followed out of the
LAN: its description and its control address must be at the address its answer came from, and that address must not be a public
one. It is on by default so that a home takes its part of the custody without its owner doing anything; the mapping lasts two hours
after pzpeer stops. A node that does not listen still
searches, publishes and signs (through the relay, 7.7); it is not a custodian.

`t_bt` and `t_upnp` run against a table and a router made in the tests; `tests/peer/e2e_meet.sh` starts three nodes that are given
no address and find each other through `tests/peer/bt_table.py`. Not tried yet against the real table of BitTorrent nor a real
router: the machine of the tests reaches neither.

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

- A puzzle address with more than about 45 000 coins makes the reply of the scan larger than the 16 MB of the JSON reader: it would cost
  an attacker about 0.3 BTC (the coins of 546 satoshis and their fees), much less than a prize. So the session also takes the prizes of
  the network file (`prize=txid:vout:sats`, at most 16; the txid as explorers show it), each checked with `gettxout` without the
  mempool: unspent, confirmed (mature for a coinbase), of the puzzle's script, with the node's value; `sats` is not used. The largest 48
  coins of the scan, when it works, and the prizes not among them make the coins of the first transaction. Two homes whose network files
  list different prizes may build different transactions when the scan fails: the plan stops them, until their files agree.
- Fee rates are asked for a confirmation within 2 blocks (project 4.5), the higher of the two homes' taken (7.5).

### 8.2 Without a node of one's own

`pzpeer run` without `--rpc` reads the chain from Electrum servers: the `electrum=host:port` lines of the network file and those of
`--electrum HOST:PORT`, 8 at most; a network of the main chain with none of either uses a built-in list of eight public servers of
different operators, and the block 969696 as its checkpoint. The connections stay open from one question to the next (a session asks
every 15 seconds while it waits for a block), and a server that does not answer is left alone for ten minutes. None is trusted: nothing a server says is believed without a proof, and what a lying server can
still do is hide things, which is a delay and never a wrong payment. The connection is plain TCP, one JSON object a line; as every
answer is proven, TLS would add a dependency and no safety. The code is `spv.c` (headers and Merkle branches), `electrum.c` (the
client) and `chain_spv.c`, a second `session_chain` beside `chain_rpc.c`; a node of one's own stays the stronger choice, as it also
checks the rules that the proof of work alone does not (below).

1. **Headers.** They start from a checkpoint, `checkpoint=HEIGHT:HASH` of the network file (the hash as explorers show it; on
   the main chain its height is a multiple of 2016, and a built-in one is used when the file has none; another chain may start from its first block). Every header
   after it is asked of every server and checked: the hash of the one before, its proof of work below its target, on the main chain
   the target of every 2016 blocks, its time above the median of the 11 before and no more than two hours ahead. Of a batch with an
   invalid header the valid ones before it are kept. A branch replaces the kept one only with more work. The chain is kept in
   `headers.bin` of the home's folder, 80 bytes a block, and is the clock of the session: the blocks F waits count on it, and no
   server can make them come faster without the work. A chain whose last block is more than six hours old is not ready (regtest:
   any), and a session waits.
2. **Coins.** `blockchain.scripthash.listunspent` of the puzzle address from each server, and the union of what they list. For each
   coin, the transaction that made it (`blockchain.transaction.get`), whose hash must be its txid, and its Merkle branch
   (`blockchain.transaction.get_merkle`) to the header of its height in the kept chain: then it is confirmed, as deep as that header.
   A "transaction" of 64 bytes is refused, as it could be an inner node of the tree passed off as a leaf (CVE-2017-12842).
3. **Spent** is proven too. A coin a server lists is dropped only when another shows the transaction that spends it, confirmed with
   its branch. For the wait of F and the confirmations of our own transactions, an output is spent when such a transaction is
   confirmed or, while it waits in the mempool, when all its inputs are outputs of the transaction asked about and every signature is
   valid: a server that just stops listing the prize proves nothing, and the partner could run it.
4. **Our transactions** go to every server; the broadcast holds when a server then serves the transaction back.
5. **Fee rate:** the median of `blockchain.estimatefee` of the servers that answer, under the cap of the file.

What it does not check: that the transactions of a block are valid. A coin that does not exist needs a block with real work that
holds an invalid transaction: on the main chain that costs a block's reward for each block, and it only makes the two homes sign a
first transaction the network refuses. On testnet and signet only the proof of work against the header's own target is checked
(their rules of minimum difficulty and of signed blocks are not): good for tests, not for money.

`t_spv` checks the rules on real headers of the main chain; `t_chain_spv` runs the sessions' calls against servers that hide a coin,
list a spent one, give false proofs, add blocks without work, drop a broadcast or do not see the mempool; `PZ_E2E_LIGHT=1
tests/peer/e2e_net.sh` pays a coincidence with three homes that have no node. Not tried yet against a public server.

## 9. Status

Everything below works and is tested; `tests/peer/e2e_net.sh` runs the whole of it on one machine with a bitcoind in regtest: a custodian
node and three homes (`pzpeer run`, each with its pzsolve), a puzzle of 58 bits funded with 1 BTC. With Bitcoin Core 28.1 a coincidence
between two homes ended in the payout: 0.49999366 and 0.49989366 BTC (the difference is F's tiny coin, back to F), nothing left on the
puzzle address. With so few homes a home may find the key on its own first (its own walkers meet too): the script then starts a new try
with a new puzzle. The cases of the session (the payout, and the cancel when a partner keeps the second transaction) also run on the node
in `t_session` with `PZ_REGTEST`.

| Part | State |
|---|---|
| pzsolve in network mode (section 6) | done; `tests/e2e.sh`, `kstat net` (net-06 measured on the curve) |
| pzcore: ChaCha20-Poly1305, the generator of the home seed (section 4.1) | done (`t_aead`: RFC 8439 vectors; `t_seed`); the GPU source of the seed to come |
| `noise.c`, `keys.c`, `record.c`, `dpfeed.c` | done (`t_noise`: byte for byte against the noiseprotocol package of Python; `t_record`; `pzpeer dpcheck` on real dp.bin files) |
| `net.c`, `store.c`, `dht.c` | done (`t_net`, `t_store`, `t_dht`: 33 nodes in one process), with the limits of 7.3 and 7.4 and the relay of 7.7; what a public network still needs is in 7.8 |
| `json.c`, `btc.c`, `btc_rpc.c`, `chain_rpc.c` | done (`t_json`, `t_btc`, `t_btc_rpc`, `t_chain_rpc`, `fuzz_peer`; `t_session` with `PZ_REGTEST` on Bitcoin Core 28.1; the coins also on a pruned node) |
| `bt.c`, `upnp.c` | done (`t_bt`, `t_upnp`, `tests/peer/e2e_meet.sh`); not tried against the real table of BitTorrent nor a real router |
| `sweep.c` | done (`tests/peer/e2e_sweep.sh`: the node would take the transaction, and pzpeer did not send it) |
| `spv.c`, `electrum.c`, `chain_spv.c` | done (`t_spv`, `t_chain_spv`, `PZ_E2E_LIGHT=1 tests/peer/e2e_net.sh`); not tried against a public Electrum server |
| pzsign, `signer.c`, `pieces.c`, `payout.c`, `session.c` | done (`cargo test`; `t_pieces`, `t_payout`, `t_session`: the payout and the cancel, in memory and on regtest) |
| `netfile.c`, `pzpeer netcheck` | done (`t_netfile`) |
| `sys.c`, `sys_posix.c`, `sys_win.c` | done on Linux (`t_sys`). Windows: `make windows winsigner` builds `pzpeer.exe` and `pzsign.exe` with MinGW-w64, and under Wine the unit tests pass (`make wintest`; all but `t_btc_rpc`, `t_chain_rpc` and `t_session`, whose stand-ins are POSIX programs), three nodes find each other (`PZPEER="wine build/win/pzpeer.exe" tests/peer/e2e_meet.sh`) and `t_pieces` signs with `pzsign.exe`; not run on a real Windows, and Wine does not deliver the Ctrl-Break by which pzpeer stops pzsolve there. macOS: written for it, not built or run yet |
| `home.c`, `pzpeer run` | done (`tests/peer/e2e_net.sh`) |
| to come | the restart of the walkers of a merge (now only logged); a session that survives a restart of pzpeer; the spot checks of the walks (project 4.2); pzpeer on a real Windows and on macOS; the public test |

## 10. A test network on one machine

```sh
make && make peer && make signer            # pzsolve, pzpeer, pzsign (Rust: cargo builds GMP the first time)
make peer-primes && make peer-test          # the unit tests, the signing ones with the real pzsign
bitcoind -regtest -daemon -rpcuser=pz -rpcpassword=pz -fallbackfee=0.0002
PZ_REGTEST=127.0.0.1:18443:pz:pz build/peer/t_session    # the session on the node (with PZSIGN and the primes, as make peer-test sets them)
PZ_REGTEST=127.0.0.1:18443:pz:pz tests/peer/e2e_net.sh   # the whole network: a custodian, two homes, a payout
```

By hand, every home runs, with the same network file (section 5),

```sh
pzpeer run --network FILE --payout ADDRESS
```

and has pzsolve hunting, its points published, the records of others kept and its coincidences paid. What it adds on its own: the
folder `pznet-NAME` (`--dir`), pzsolve and pzsign beside pzpeer, port 28140 opened on the router, the other nodes found in the table
of BitTorrent and the chain read from Electrum servers (on the main chain a built-in list and checkpoint; on a test chain
`--electrum`, `--bt-router` or `--peer`, and `--rpc` for a node of one's own). `--debug FILE` writes every message, step and answer
of the chain with its time, and no secret: the file to read when something went wrong. Options after `--` go to pzsolve
(`-- -t 2 --no-gpu`).

With a bitcoind of its own and no `rpcpassword`, `--rpc 127.0.0.1:8332 --rpc-cookie ~/.bitcoin/.cookie` takes its credentials from its
cookie file instead (section 8.1).

The home seed is made on the first run in `HOME/home.seed` (keep a copy: it is the key of every point of the home); pzpeer refuses a seed
file that others may read, or a link, and a home folder that others can open. `HOME/start.key` is the start key pzpeer gives to pzsolve,
written at every start through a new file of mode 0600; `HOME/failed-signings` counts the failed signings (section 7.5); the campaign is
in `HOME/campaigns/net-NAME`, its output in `HOME/pzsolve.out`.

## 11. The shared linear search (under construction)

A puzzle whose public key is not known is searched key by key, and the key is seen whole by whoever tries it: no prize can be split.
What a network can do is keep two people from trying the same keys: a register of the blocks already tried. Tried once, the range
ends for sure, and in half the keys on average; nothing more than that is gained.

A register is worth what its entries are worth, so every entry carries a proof of work.

**Near misses (done, in pzsolve).** Every CPU engine and GPU kernel compares bytes 8 to 11 of the hash160 of a key with the target's
before the rest. A key that passes is a *near miss*: one key in 2^32. The engines report them (`scan_job.on_near`, the hits of the
kernel), the host checks each on the CPU (`run_scan_candidate`, which also takes the key of the campaign from among them), and
`hunt --near` writes them to `near.txt` of the campaign. Nothing else in pzsolve knows of a network.

**The claim of a block (done, `block.c`, `t_block`).** The range is cut into blocks of 2^bits keys, 40 to 48. A *proof key* is a near
miss with byte 12 of the target's too: one key in 2^40, about 16 in a block of 2^44. A claim is

| Field | Bytes | |
|---|---|---|
| index | 8 | the block, big-endian |
| owner | 32 | x-only identity of who scanned it |
| n | 1 | proof keys, 64 at most |
| offsets | 6 each | of the proof keys in the block, increasing |
| signature | 64 | BIP340, by the owner, over `SHA256("pznet1/block" \|\| F \|\| the fields before it)` |

and is valid with a good signature, increasing offsets inside the block, every key a proof key, and at least a quarter of the
average number of them (an honest scan has fewer once in a thousand blocks, and that block is done again). Checking a claim costs
some tens of hashes. A claim shows that most of the block was tried; it cannot show that no key was skipped.

**To come.** The network file of a linear search (an address in place of the public key, the size of the blocks); the claims kept
in the table like the records of the kangaroo; the home that picks a block nobody claimed, runs `pzsolve hunt -r BLOCK --near`,
and publishes its claim; spot checks, where a node scans a random slice of a claimed block and asks the owner for its near misses
of that slice (a near miss the owner cannot show is a proof of fraud that everyone can check); and a second pass over the claimed
blocks when no free one is left, so that a false claim delays the key and does not hide it.

**When the key is found** nothing is sent. pzsolve warns not to spend it through the public mempool, and `pzpeer sweep` makes and
signs the transaction for the finder to give to a mining pool in private (`sweep.c`).
