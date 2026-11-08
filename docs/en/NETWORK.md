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
- Amounts are verified from the previous transactions (their hash is their id): `btc_prevout` takes the raw bytes of the previous
  transaction, in either serialization, and gives the value and the script of the output only when the hash of those bytes is the txid.
- The vectors of `tests/peer/t_btc.c` come from transactions of the main chain, pinned by their txids, and from
  `tests/peer/gen_btc_vectors.py`, an independent implementation in Python of the serialization, the legacy signature hash, RFC 6979 and
  DER (`python3 tests/peer/gen_btc_vectors.py > tests/peer/btc_vectors.h`).
- The rules of the project document (section 4.5): at most 8 inputs, coins worth less than 3 times their fee are left, half each.

## 9. Status

| Part | State |
|---|---|
| build of pzpeer, libsecp256k1 | done |
| `json.c`, `btc.c`, `btc_rpc.c` | done, with unit tests (`tests/peer/t_json.c`, `t_btc.c`, `t_btc_rpc.c`); the test against a real node on regtest is to come |
| generator of the home seed (`lib/pzcore/src/seed.c`, section 4.1) | done; the GPU source of pzsolve and pzpeer to come |
| everything else | in progress |
