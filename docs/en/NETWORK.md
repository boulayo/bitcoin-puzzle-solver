# The shared kangaroo network: implementation

This is the technical companion of the project document ([PROGETTO_RETE.md](../it/PROGETTO_RETE.md), in Italian; the decisions still
open are in [DA_FARE.md](../it/DA_FARE.md)). It says how the code is organized, what each module does, and the exact formats between
modules. It is written for whoever works on the code; the project document explains the reasons and states the assumptions.

Status: under construction on the branch `rete-condivisa`. Section 9 lists what works and what does not yet.

## 1. What runs where

```
pzsolve (unchanged deps: libc only)          pzpeer (pzcore + lib/secp256k1)                 pzsign (C++, cb-mpc)
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
- **pzsign** is started by pzpeer for a signing session and dies at its end. It never touches the network or the disk: pzpeer relays
  its messages to the partner's pzsign.

## 2. Source layout

| Path | Content |
|---|---|
| `lib/secp256k1/` | libsecp256k1 0.7.1, the library of Bitcoin Core (MIT), with the ecdh, extrakeys and schnorrsig modules. Used for every operation on a secret key: BIP340 signatures, ECDH, ECDSA of the coins of the home |
| `lib/pzcore/src/seed.c` | the generator of the home seed (the OS generator, with the CPU's and the timer mixed in by HKDF) |
| `lib/pzcore/src/aead.c` | ChaCha20-Poly1305 (RFC 8439) |
| `src/peer/` | pzpeer, one file per layer (below) |
| `signer/` | pzsign: `pzsign.cpp`, the copy of cb-mpc it is built on (`vendor/`), the script that builds OpenSSL for it, its tests |
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
| `record.c` | the record of a distinguished point, its checks, the proof of possession |
| `net.c` | TCP: one connection per exchange, the handshake, frames, timeouts |
| `store.c` | the custodian: records by key, on disk, coincidence detection, mailboxes |
| `dht.c` | Kademlia: routing table, lookups, store, alerts, bootstrap |
| `dpfeed.c` | reads the campaign's `dp.bin` and turns its records into points |
| `pieces.c` | the two pieces of the key of a coincidence |
| `signer.c` | starts pzsign and relays its frames |
| `payout.c` | the transactions of the pair: the first, the second, the cancel |
| `session.c` | the session of a coincidence, from the hello to the payout (section 7.5) |
| `bt.c` | the rendezvous in the Mainline DHT of BitTorrent: announce and lookup of the network's key |
| `portmap.c` | the request to the home router to let the listening port in (PCP, NAT-PMP, UPnP) |
| `chain_rpc.c` | the chain of a session on bitcoind |
| `spv.c` | block headers checked by their proof of work, Merkle branches |
| `electrum.c` | a client of the Electrum protocol (plain TCP) |
| `chain_spv.c` | the chain of a session without a node: several Electrum servers, every answer proven |
| `sweep.c` | `pzpeer sweep`: the transaction that moves the coins of a key found by a linear search, signed, and given to a mining pool only on request (`--private`) |
| `block.c` | the claim of a block of a shared linear search and its proof of work (section 11) |
| `claims.c`, `dht_claim.c` | the claims a custodian keeps, and the messages that store and ask them (section 11) |
| `linear.c` | the home of a linear search: a free block, pzsolve on it, its claim (section 11) |
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

| Input, in the order it is read | What is read | |
|---|---|---|
| CPU generator: RDSEED, else RDRAND (x86-64); RNDRRS, else RNDR (arm64) | 4 words (256 bits) | mixed in when the CPU has one |
| timer: the cycle counter of the CPU (the monotonic clock where there is none) | 256 readings, a SHA-256 apart | mixed in |
| OS generator: getrandom, getentropy, BCryptGenRandom | 2 × 32 bytes, which must differ | mandatory |

- The OS generator is mandatory: if it fails, gives nothing or gives the same block twice there is no seed, and the message says why.
  Nothing else refuses a seed.
- The CPU generator and the timer are mixed in as they come: nothing tests them, nothing is counted for them, and a machine without a
  CPU generator, or with a timer that shows no noise, gets its seed all the same.
- The CPU generator is read first, while no other secret is in the process; the OS last, so that its output spends the least time in
  memory.
- Mixing: HKDF-SHA256 (salt `"pzseed2"`, info `"seed"`) over the three inputs, each behind its index and its length, so that the bytes
  of one never pass for another's (the format is in `seed.h`). An input that is not honest and sees the others can only grind: about
  `2^b` tries to fix `b` bits of the seed.
- The inputs are wiped before the function returns. `pz_seed_harden` turns core files off and makes the process non-dumpable; pzpeer
  calls it at its start, pzsolve for a campaign that encrypts its key.

**The guarantee.** The seed is as good as the operating system's generator, which every other secret of the programs comes from too
(the nonces of a session, the random numbers of pzsign, the scalar that encrypts a found key). If that
generator is weak, the seed is no worse than it, and as good as the better of the other two inputs. Nothing here measures entropy, so
nothing more is promised.

It is this small on purpose. An earlier form, never released, had five sources (also a TPM and a source of the caller), health tests
after SP 800-90B on each, a credit in bits, and the rule that 128 of them came from outside the OS and the CPU, or no seed: about a
thousand lines to protect one secret from a failure, a broken OS generator, that gives every other secret away. It refused the homes of
machines with a quiet clock.

`lib/pzcore/tests/t_seed.c`: known answers of the mixing (computed with Python's `hmac`), every bit of every input changing the seed,
no input passing for another, 2000 seeds without a duplicate, and the OS generator failing in each of its ways (a hook of the test
build).

**Limits.**
- A seed copied with a VM image, or generated in a VM that is then cloned, is the same in every copy: the three inputs of two clones
  that start from the same state can be the same. The home seed belongs on a physical computer.
- The CPU generator cannot be checked from outside, and RNDR on Windows is not detected; the timing noise of a machine is not measured.
  That is why neither is promised anything.
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

**The networks written in the program.** Every puzzle of the table of pzsolve whose public key is known has its network in pzpeer, as
a chain's parameters are in a node of that chain: `pzpeer run --puzzle N`, and without `--puzzle` or `--network` the first puzzle whose
public key is known and whose key is not (today 140). Its lines come from the puzzle alone: `net=pN`, its public key, the range
`2^(N-1)` to `2^N - 1`, `jumpseed = SHA256("pznet1/jumps" || public key)`, `schema=net-06`, `dpbits = max(8, (N - 1) / 2 - 44)` and
`chain=main` (`pzpeer netcheck 140` prints them with their fingerprint). The seed comes from the public key, which the creator of the
puzzles fixed in 2015: nobody chose it, so nobody could have picked one that suits them, and everybody computes the same one. A home
that starts the day it gets the program and one that starts a year later are on the same network, and the second meets the walks of the
first. Knowing the seed before others is only being able to walk before them: the same work, which counts the same. The least zero
bits are the delay rule of section 6 for a network of 2^40 walkers (a million GPUs): each home adds up to 8 to them. A puzzle of the
table whose public key is not known has the network of a linear search instead (section 11): `address=` (its P2PKH address, of its
chain) and `blockbits=44` in place of `pubkey=`, `jumpseed=`, `schema=` and `dpbits=`. A network file is for tests (a puzzle of one's
own, another chain) and for a puzzle that is not in the table.

Their fingerprint `F = SHA256("pznet1/file" || canonical lines)` **is** the network: every key, record, message and the rendezvous key
are bound to it, so two nodes are on the same network exactly when their files have the same `F`. Nobody signs a file. Whoever changes
one of those lines is on another network, as a torrent with another hash is another torrent; the file of a public network comes with
the program, as the parameters of a chain come with a node of that chain.

The other lines are settings of the node that reads them, and each is checked where it is used: `entry=host:port` (a node to start
from), `electrum=host:port` and `checkpoint=HEIGHT:HASH` (section 8.2), `prize=txid:vout:sats` (a named coin of the puzzle address,
found even when no scan of the address lists it; its value and script are read from the chain; it counts in what is left at the
address, before the pieces of the key are exchanged, until it is spent, 7.5 step 8). Any number of each, at most 16 `prize=`. The
networks written in the program, of either kind, carry the coins of their address as `prize=` lines (`data/prizes.csv`; `pzpeer
netcheck 140` lists them): every coin that was there, confirmed and worth at least 7400 satoshis (what an input costs at 50 sat/vB,
the lowest fee rate of a sweep; a payout of a pair takes a coin from 8880), when the table was made: the three of the creator at each
address, of 2015, 2017 and 2023, and what strangers sent there. The table is a snapshot. A coin spent since is found spent; a coin that
came later is above the checkpoint of this version, where a scan of the address lists it (a version that moves the checkpoint forward
makes the table again: `tools/fetch_prizes.py`). Naming the old
coins is also what makes a home with a node and a home without one build the same first transaction: a node lists every coin of the
address, servers none below their checkpoint (8.2).

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
- **How many zero bits.** The network file gives the least, `dpbits`; the home picks its own from N, the nodes of the network (the mean
  of the last six lookups of `dht_nodes`, as for the time estimate of 7.5), and K, the walkers of its pzsolve (`walkers` in the
  `status.json` of the campaign). The search takes about `E = 2√W` jumps.
  - The delay: once two walks meet, the walker that came second must still reach a point with the zero bits, `2^dp` of its own jumps,
    while the network's walkers, about N K of them, walk too: the search costs about `N K 2^dp` jumps more, a thirty-second of E when
    `2^dp = E / (32 N K)`.
  - The room: `E / 2^dp` records, each kept by 5 nodes; N nodes that keep S records each (the default store, 2^27) are half full at the
    end when `2^dp = 10 E / (N S)`.
  The home takes the fewer of the two, at least `dpbits` and at most `dpbits + 8` (60 at most); a custodian refuses a record that claims
  more, and a record weighs `2^dp` in the time estimate, so nobody can inflate it more. When the room would want more than the delay
  allows, the home says once that the custodians cannot keep every point (a full custodian still tells the coincidences of what it is given; the answer is
  more room on the nodes). With a GPU's millions of walkers the delay decides: the room it needs is about 160 records a walker of the
  network, whatever the puzzle. The home decides once it has three lookups and pzsolve has written its walkers, at each status line
  (every 10 minutes), and changes only when the value moves by 2 or more (a network or a home 4 times bigger or smaller): pzsolve then
  saves its walkers (6), and resumes its campaign with the new `--dp-bits`. A walk of one home still meets the walks of homes with other zero bits: past the meeting both walk
  the same path, and the first point that has the zero bits of both is in the records of both. `dpbits.txt` in the home folder keeps
  the changes (`index zero-bits`, the first record of `dp.bin` that has them; rewritten whole at each, through a new file synced to the disk),
  so that each record says the zero bits pzsolve had when it wrote it; a change past the end of `dp.bin` (a file made again) is dropped.
  Hostile nodes that lie in the lookups move a home's zero bits within those 8 bits, no further. pzpeer refuses the options after `--`
  that it gives pzsolve itself (`-r`, `-k`, `--dp-bits`, `--jump-seed`, `--start-key`, `-c`, `-d` and the like).
- `dp.bin` is the usual log of the campaign. pzpeer reads it.
- `pzsolve kstat net BITS TRIALS` measures the cost of the walk of the network. On the curve, W = 2^40, 3000 trials each, one thread:
  schema 3 1.061 ± 0.011, net-06 1.182 ± 0.012 steps per √W: net-06 costs 11.4 % more, as the simulator predicted (+11 %), the price
  of the schema that makes a theft pay only above 51 % of the power of the network (project 2.3). The estimates of time take this cost, 1.18
  (`KANG_C_NET` of pzsolve in a campaign of the network, `C_MEAN` of `home.c` for the network line of pzpeer, 7.5).

## 7. pzpeer

### 7.1 Identity and transport

- Node ID = `SHA256("pznet1/id" || xonly identity pubkey)`. It must have 16 leading zero bits: generating an identity costs about 65 000
  key derivations, a few seconds; checking one costs one hash. (S/Kademlia's static puzzle.)
- Transport: TCP, `Noise_XX_secp256k1_ChaChaPoly_SHA256`.
  - DH: libsecp256k1 ECDH, output `SHA256(compressed shared point)`, as in BOLT 8.
  - Static keys are the identity keys (compressed points on the wire, x-only identity = their x).
  - Prologue: `"pznet5" || F`, so peers of another network, or of earlier versions of the protocol (the short record, records named
    without their zero bits, then PROVE, the fee rate after the proofs and F from the point, then a PROVE of several records), fail the
    handshake.
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

Fixed 66 bytes:

| Bytes | Field |
|---|---|
| 1 | signed type t: 0, +1 or −1 (as two's complement byte) |
| 1 | zero bits of x: at least `dpbits` of the network, at most 64 |
| 32 | x of the point; the point is the representative with even y |
| 32 | x-only identity of the owner |

The point is `R = t·Q' + d·G` with `Q' = P − base·G`, `base` the start of the range, `d` the owner's exact distance (an integer of up to
192 bits, kept at home). Checks on arrival: t in range, the zero bits in range and x has them (on `x.n[1]` as pzsolve counts them), x
on the curve, the owner an x-only key. The network's `dpbits` is the least: a home may ask pzsolve for more. Two records are of the same
point when their x is, whatever their zero bits.

A record carries no signature. In a STORE only its first 34 bytes travel, and its owner is the node that sent it, whose identity the
handshake proved (7.1): a custodian takes the records of nobody else. Everything else a record can do (an alert, a session) waits for
its owner's proof of possession, which no copier can make.

**Proof of possession** of a record: a BIP340 signature by the key `d` (the scalar `d mod n`, adjusted so that `d·G = R − t·Q'`), over
`SHA256("pznet1/possess" || F || the name of the record || the identity of the asker || a nonce of the asker)`, the name of a record
being its 66 bytes with the zero bits at 0: two custodians may hold one point of one owner with other zero bits (the first copy each got),
and it is one record, in the hello of a session (7.5), the name of a pair and the id of a session too. It is asked before an alert has any effect.

### 7.3 Custody and coincidences

- Key of a record: `K = SHA256("pznet1/key" || x)`. A record goes to the 5 nodes whose IDs are closest to K (XOR distance).
- A custodian keeps records on disk and an index in memory by 4 bytes of a keyed hash of x (SipHash-2-4, its key drawn at each start: a
  peer chooses its x within the zero bits and the rule of the custodian, and with a hash it could compute it would put its records in
  one run of the table, which every record of that run would then walk; the owners are found by a keyed hash of their identity too). The file, `store.bin` in the data folder, is
  append-only: a 40-byte header (`PZSTORE2` and F: a file of another network is refused) and 38-byte entries one after the other: the
  type, the zero bits, x and the number of the owner (4 bytes, little-endian). The owners are in `store.bin.owners` (an entry whose owner's line a crash lost names nobody, and its number is never given again): `PZOWNER2`, F, then
  32 bytes an owner, in the order they came. At start the index is rebuilt from the files, and an entry cut by a crash at their end is
  dropped: about 50 s for a full store of 2^27 records (5 GB, read from the cache of the system on a machine of the tests), where it
  took about 2 minutes (a SHA-256 of every x, an owner looked up by its identity for every entry, a table that grew by doubling, two
  places of the memory a slot). What is left is the memory: each record goes to a random place of a table of 2 GB.
- A custodian refuses a record that does not pass the checks of 7.2 or whose owner's node ID has not the zero bits (an owner is a node:
  the alerts find it by its ID), and a second record with the same x **from the same owner** (duplicate: the first is kept).
- What a custodian takes is bounded (a record costs its owner nothing, with any x that has the zero bits: no discrete log is needed, so
  a flood is cheap to make):
  - only records of the node that sends them (an owner stores its own; custodians do not copy), and only records whose key it may be a
    custodian of: it refuses one when its table knows 10 nodes closer to K (a sum over buckets: a contact of bucket j is closer exactly
    when bit j of `ID ⊕ K` is 1). An honest publisher found it among the 5 closest, so it knows at most 4 closer; a flood sprayed at
    random nodes is refused, and one aimed at a node costs about (nodes / 10) tries per record;
  - within a rate per address block of the sender (the table of the limits in 7.4), in which a record of an owner the custodian does not
    know counts as 256 records, and as one when the custodian does not take that owner after all (it is full, or refuses the record);
  - at most `store_max` records in all (`--store-gb`, 5 GB by default, 2^27 records; the index in memory takes 8 bytes a slot at 35 to
    70 % load, 11 to 23 bytes a record, that is 30 to 60 % of the 38 bytes of its entry in the file: for the default 2 GB at most, two
    fifths of the file, and 3 GB for the moment of its last doubling, while the old table is still there), at most 8 owners of one x
    (and as many more whose owners proved their records, below), and owners in all one per 256 records it may hold and 1024 more. An
    identity costs little, and a custodian keeps an owner for ever (the file of owners only grows): this is why a new owner that it
    takes costs its address block 256 record tokens. One block then fills the owners of a custodian no sooner than its records, in
    about a month (before, a new owner cost one token: 7 hours, after which no new home could store there, for ever). A record whose
    owner it does not take costs one token, as any other: a full custodian tells the coincidences of a new home, and answers its
    `PROVE`, at the rate of every home (before, each such record cost 256, at the STORE and again at the PROVE: about 390 records of a
    block at once and then one in five seconds, and the PROVE after a large STORE was refused).
    There is no limit of records of one owner: sixteen identities passed it at once, and on a small network a home is a large share of
    a custodian by right.
  The checks that cost nothing come first, the curve last. A node of a linear search (section 11) takes no record at all, and no
  `ALERT`, `PROVE` or `POSSESS`, and has no `store.bin`: any x on the curve with the zero bits passes the checks of 7.2 with no walk (64
  zero bits too), and no walk of that network makes records. Before, anyone could fill its 5 GB.
- `store.bin` is the custodian's own file, so its records are not checked again when it is opened, except the last 1024: a crash can
  leave an entry whole in length and wrong in content. One that fails there, or names an owner the owners file has not, is not indexed. The records a coincidence is made of are checked again when they are read back, so a record
  damaged in the middle of the file never becomes an alert.
- Same x, different owners: a coincidence. Same type: a merge (both walkers should restart). Different types: useful. Both kinds are
  told the same way; the owners tell them apart by the types. **A custodian is passive**: it alerts nobody and remembers nothing of a
  coincidence. To the owner that stores a record whose x other owners have, it answers `COINCIDENCE` (status 3): for a new record, for
  one it holds already (a republish), and for one it does not keep because it is full. The owner then sends `PROVE` with its proof of
  possession of each record of the STORE that got 3, 8 in a PROVE, **all of them** (the asker is the custodian, the nonce 32 zero
  bytes: a copy of it convinces that custodian alone, of what is true). The custodian checks each proof, takes the record as a STORE
  takes it (only one it may be a custodian of, within the rate of the block) and answers an `MBOX` with the pairs: each record proved
  with every other owner of its x, up to the 8 a custodian keeps, the newest first, read from its store at that moment. The owner
  takes from that answer only the pairs of the records it proved, at most 8 for each (an answer with more pairs than 8 times the records
  proved is refused whole): a custodian cannot make alerts of the owner's other points, nor more than 8 of one of them (before, the
  bound was for the records of a PROVE together: 64 for one record of 8). **The owner tells the other owner itself**: once its own
  proof checks out it sends `ALERT(its record, the other record, proof)` to the other owner when it can dial it and to the other
  owner's mailboxes (the 5 nodes closest to `SHA256("pznet1/mbox" || owner)`), and again every hour for the day of the alert, also
  once it is done: a mailbox may lose it (it keeps its pairs in memory). A post counts once the other owner or one of its mailboxes
  took it (answered `OK`): one that reached nobody is sent again at the next try, and the alert waits for it (before, an alert whose
  proofs had both checked out was done after one post, taken or not, and its proof was never sent again). What follows from this:
  - a copy of another home's point, which its sender cannot prove, alerts nobody: the copier gets `COINCIDENCE` and has nothing to
    send. Nor can a stranger send the alert itself: an `ALERT` without the proof of its first record is refused (before the custodians
    were passive, they sent alerts without a proof, so anyone could: a forged alert with a real record of a home cost it a lookup, a
    dozen connections and a place in its queue);
  - an owner hears of the coincidences of its records from a custodian of its own point, without its mailboxes: those of a home that
    does not listen sit near a public key, where nodes placed there could keep its alerts from it; the next republish to every
    custodian (at most a week, 7.4) brings each coincidence anyway, as every record gets its `COINCIDENCE` again and is proved again;
  - copies planted before the owner's record cannot hide a coincidence. A custodian that learned a point from its owner's STORE can
    store copies of it under other identities at the other custodians before the owner's republish reaches them. A record that its
    owner proves in `PROVE` is kept also past the 8 owners of its x, up to 16 (each one a walk that reached the point: a copier cannot
    prove), and the newest first shows it to the owner that comes next; the owner's proofs and `POSSESS` tell the copies apart. Nor
    can copies planted on other records of the owner keep it from the real one: it proves every record that got 3. (Before, a
    custodian said 3 for 8 records of an address block at once and one more a minute, and the owner proved the first 8 of a STORE:
    copies on 8 records with lower keys took those places, at every republish again.)

  The other owner hears of a coincidence from the owner that proved, not from the custodian. If that owner stops between its PROVE
  and its ALERT, or never sends it, the other owner hears at its own next republish to every custodian, when its record gets
  `COINCIDENCE` and it proves it: a week at most. An owner that proved and keeps silent gains nothing: it cannot use the coincidence
  alone. A custodian that is itself one of the owners hears of the pair as any owner does, with one difference: when its own record
  comes second (it stores it to itself), the coincidence is its own alert at once. When another owner's record comes after its own, it
  answers that owner `COINCIDENCE` as anyone else, and alerts itself only once that owner proves the record (`PROVE`), which a copy
  cannot; an owner that never proves leaves it to the custodian's own next republish to every custodian, a week at most.
- A node that receives an `ALERT` checks both records (7.2, same x, different owners) and the proof of the first one's owner for the
  second's, and refuses it without. It keeps the pair, with the proof, as a mailbox when it may be a mailbox of one of the owners (the
  rule of the custodian above, for the key of the mailbox), in memory: 4096 pairs, at most 64 of one owner. When one owner, or the
  mailbox, has no more room, the oldest of its pairs at the x that has the most of them goes (the oldest, when every x has one).
  Whoever holds a point can pair it with records under any names, with a valid proof of its own, but only at that x: a flood of such
  pairs pushes out its own, and to push out a real pair it needs about a point for each pair kept, each one a walk (before, the oldest
  pair went, and one point was enough to empty the mailboxes of a home, and with identities placed near a mailbox the whole of it).
  Two owners at one point are one pair whatever the zero bits of their records. When one owner is itself it checks the proofs of
  possession: its own (the daemon gives the distance of the point) and the other owner's. Only then the daemon gets the alert.
- The other owner's proof comes one of two ways. In its `ALERT`, which is how an owner hears of a pair from the other one. Or by
  `POSSESS`, with a nonce of the asker, which the owner that heard of the pair from a custodian asks the other owner when it listens.
  The proof of an `ALERT` is over the pair: the asker is the other owner and the nonce is the name of the pair,
  `SHA256("pznet1/pair" || the names of the two records in the order of their owners)`. It binds the asker, so it proves nothing to
  anyone else, and a fixed nonce lets it wait in a mailbox; a copier cannot make it without the distance. A mailbox checks it and keeps
  it with the pair, and `MBOX` carries it. So two homes that do not listen prove their points to each other through their mailboxes.
- An alert that waits is tried again after a minute, then twice as late each time up to an hour, for a day: then it is forgotten, and
  the pair is a new alert when it comes again (the other owner's `ALERT`, a mailbox poll, the next republish to every custodian), so
  proofs that missed each other for a day are not the end of a coincidence (before, the alert was refused, and nothing of that pair
  counted any more until a restart). Only a wrong proof of the other owner refuses an alert. An alert that is done stays, so the
  daemon is told of the pair once, and posts its proof every hour for the rest of its day. A node remembers 1024 alerts: when they
  are all waiting, the oldest goes whose job is not in the queue, so a flood of alerts that will never end cannot shut out the next
  real one, whose job is queued at once and keeps it while it waits behind the jobs of the flood; an alert pushed out comes back with a
  mailbox poll, or the next republish to every custodian. An alert has one job at a time, queued
  or under way. The alerts that came from one address block hold at most 8 places in the queue of the jobs, also when they are tried
  again, and the kinds of jobs take turns (the alerts of the node, the pings). What can still make such a flood: a custodian of the
  home's points that lies in its answers to `PROVE` (at most 8 pairs for each record proved; each costs the home a lookup of the owner
  it names, one of its mailboxes and their ALERTs), and anyone that holds a point and sends it in pairs with a record of that x under
  the home's name, with valid proofs under any number of names (the home does not hold that point: its job finds no distance and
  stops there, at no cost on the network).
- An owner asks its 5 mailboxes for its pairs (`MBOX_GET`): at start, every hour while the others reach it, and every minute when
  they do not (it does not listen, or nothing comes in at its port). A mailbox answers the owner only (the identity of the connection):
  the pairs show the points of its coincidences. The owner takes from a mailbox only the pairs that bring the other owner's proof
  for it: a mailbox cannot make an alert of a pair nobody proved.

**The work of everyone, at any time.** Nothing of the table is sent live only. A custodian keeps every record on its disk (`store.bin`),
without an end, within its limits; every owner sends all its records again every day and as soon as it is back among the others (7.4).
So each point of a home that joins after months is compared with the points of every home since the first day, at the custodians of
that point: nothing is downloaded, and every new point has a better chance than the one before. A home away for a time loses nothing
(it keeps `home.seed` and its campaign): when it is back its records come again, the custodians tell it every coincidence with them
again, and the session of a pair starts again even if its partner had given up on it (7.7). A home that leaves for good takes its half
of every key with it: its distances are its secret, so a coincidence with its points can be paid by nobody. Its records stay in the
table, a walker of another home that meets one is on a path walked already.

### 7.4 Kademlia

k = 20, α = 3, 256 buckets. Messages (inside Noise frames, first byte the type; an answer is its request + 1, but MBOX answers PROVE;
`n` is one byte):

| Type | Message | Answer |
|---|---|---|
| 1 | PING | 2 PONG |
| 3 | FIND_NODE(target 32) | 4 NODES(n, n × (node id 32, xonly 32, ip 16, port 2)), n ≤ 20 |
| 5 | STORE(n, n × the first 34 bytes of a record: the owner is the sender), 1 ≤ n ≤ 255 | 6 STORED(n, n × status byte) |
| 7 | ALERT(record, record, proof 64): the proof of the first record's owner for the second's (7.3) | 8 OK |
| 9 | MBOX_GET(owner 32), from the owner | 10 MBOX(n, n × (record, record, flags, proof 64, proof 64)), n ≤ 64, the newest first; the records in the order of their owners, bit i of flags set when the proof of the owner of record i is there (else zeros) |
| 11 | POSSESS(record, nonce 32) | 12 PROOF(sig 64) |
| 13 | RELAY(name 32, partner 32) (7.7) | 14 RELAYED, when the partner came; then the bytes of the partner's connection |
| 15 | PROVE(n, n × (the first 34 bytes of a record, proof 64)), 1 ≤ n ≤ 8, records a STORED said 3 for: the owner is the sender | 10 MBOX with the pairs of those records, each with every other owner of its x (no proofs in them) |
| 32 | CLAIM(a claim of a block, section 11), from its owner | 33 CLAIMED(status) |
| 34 | CLAIM_GET(index 8) | 35 CLAIMS(n, n × (length 2, claim)), n ≤ 4 |
| 16–31 | session messages (section 7.5) | |

- The lengths are exact. In NODES each contact must have the ID of its identity, with the zero bits, a port, and an address the node may
  dial, or the answer is refused whole. The ip is IPv6, an IPv4 address as `::ffff:a.b.c.d`. What an answer names are the candidates of
  a lookup and no more (a contact enters the table only when it answers), and the lookup has its own limit of queries to one address
  block, so an answer may name an address twice or many contacts of one block: the table of an honest node has two identities at one
  address while a node that left has not yet failed three times. (Before, such answers were refused: a rule that another port passed,
  and that made every answerer filter its own.)
- STORED statuses: 0 stored, 1 duplicate, 2 invalid, 3 a coincidence, of which PROVE tells (the record stored, already there, or not
  kept because the custodian is full), 4 the custodian could not write it,
  5 refused for now (the custodian is full, or the owners of its x, or its address block is over its rate), 6 not
  a custodian of it (it knows 10 nodes closer to its key). A record counts as held for 0, 1 and 3.
- Addresses. On a public network (the chain of the network file is `main`) a node dials and keeps only public addresses: not
  unspecified, loopback, private (RFC 1918, RFC 4193), shared (RFC 6598), link-local, benchmarking, documentation, multicast or reserved.
  A NODES answer that points elsewhere is refused (it would make the node connect into the user's own LAN), and so is a contact that
  asks from such an address (it is answered, not kept). On a test network (`test`, `signet`, `regtest`, or `--allow-private`) they are
  taken, and the limits by block below do not apply to them: the nodes of a test are on one machine.
- The limits by address block: the whole address of a loopback, private or link-local one (a machine or a LAN of its own), else the /24
  of an IPv4 address and the /32 of an IPv6 one, public or not (what one provider gets: with /48, one of them had 65 536 blocks). Before,
  any address that is not public was a block of its own, 6to4 and Teredo too, which carry an IPv4 address: one IPv4 had 2^80 blocks
  behind 6to4, each with its own share. They apply to public addresses everywhere, and to all addresses on a public network.

  | Limit | Value |
  |---|---|
  | contacts of one block in a bucket, in the table | 2, 4 |
  | queries of one lookup, to one block | 64, 4 (a candidate that another node named, past them, counts as failed; a contact of the own table is asked all the same) |
  | connections of one block queued or being answered at a time; new ones | 4; 20 a second, 100 at once (a peer that opens connections and says nothing holds 4 handlers at most, for the 2 seconds the handshake and the request of a connection have together) |
  | records stored or proved from one block | 50 a second, 100 000 at once (a home that was alone sends them all); a record of an owner the custodian does not know counts as 256 if it takes that owner, else as one |
  | alerts of the node from one block waiting in its jobs (also when tried again) | 8 |
  | relays (waiting or under way) of the homes of one block | 2 |

  The two rates, of connections and of records, are all the rates there are. An `ALERT`, a `PROVE` and a `CLAIM` are each the one
  request of their connection, so the rate of the connections bounds them, and with them the signatures a block can make a node check:
  one for an ALERT, 8 for a PROVE (each also a record token), that is 160 a second for a block at most, about a hundredth of a
  processor. (There were two more, of ALERTs and of records with a coincidence: the first bounded what the jobs and the connections
  bound already, the second protected the memory of the coincidences that a custodian no longer has.) A handler takes the handshake and
  the request of a connection within the same 2 seconds: before, a peer that finished the handshake late had 2 more for its request, and
  held a handler twice as long. The rates are buckets of tokens, kept for 4096 blocks (an entry with no connection is reused, the one used longest ago). The place of a
  block in that table comes from a hash keyed with a secret of the node: blocks chosen to land in one place and push each other out,
  each back with a full share of tokens, would have to know it (it was FNV, without a key).
- POSSESS is answered only for a record of the node itself, with its signature; the proof binds the asker's identity, which the
  handshake told. A node that cannot prove closes the connection.
- The routing table: buckets by the number of leading bits an ID shares with the node's own, the least recently seen contact first. A
  contact goes to the tail when it answers. A full bucket pings its weakest contact (the one that failed most, the oldest among equals)
  and keeps it if it answers; the newcomer waits, and takes the place of a dead contact once it has answered a ping of its own. Before,
  a contact that had missed one answer was replaced at once, which an attacker on the path could use to swap good contacts for its own.
  Three failed exchanges in a row at its own address drop a contact: one at an address that an answer named for it does not count
  (before, it did, so names of a known node at dead addresses could drop it). A contact enters the table only when it answers, never
  from a NODES answer, and only within the limits by block: one attacker's nodes, on its few networks, cannot fill a bucket or the
  table (project 4.2). A known contact moves to the address it answered at only when its own failed since it last answered, and the
  new one's block has its share: before, a node that named it at an address of its own, which passed the bytes on to it, took its
  place in the table and could then cut it out (three failures drop a contact).
- A node that asks tells its port itself, so it is not taken at its word: the node asked pings it at that address (3 seconds, one
  ping at a time for a bucket, in turn with the alerts that wait) and takes it when it answers. A node that does not listen (port 0) is never
  put in a table: it dials out, publishes, polls its mailboxes, and is never a custodian. A home whose router lets nothing in listens
  in vain, and must live the same way: a node counts its port open for an hour after a connection of another node came in (and for its
  first two minutes). Without one it tells port 0, takes no place in its own lookups, polls its mailboxes every minute and dials its
  partner or meets it at a relay; for two minutes of every hour it tells its port all the same, and the first node that dials it back
  gives it its place again. Before, such a home was in the table of every node it had asked, was a custodian of records nobody could
  ask it for, and found an alert in its mailboxes up to an hour late.
- Lookups are iterative, with α FIND_NODE always in flight: α workers each take the closest candidate not asked yet, among the `want`
  closest that did not fail, and the next one as soon as its answer comes, so a slow or dead node holds one worker and not the lookup
  (before, each round of α waited for its slowest). A lookup ends when those `want` have all answered, or after 64 queries (answers that
  keep pointing at new nodes cannot make a lookup endless). A lookup asks at most 4 nodes that others named in one address block: answers
  that all point at one network, a victim or the attacker's own, do not make the node dial it more. The contacts of its own table are
  asked whatever their block (the table has 4 of a block at most): before, names that fail, placed in the block of a node the lookup
  knew, used up the 4 queries and kept the lookup from that node. A candidate named again at another address before it answered keeps
  the last such address, and is asked there when its own fails: an answer that names a node first, at an address where it is not,
  cannot hide it from the lookup (before, the first address named was the only one, and `dht_find` failed: no direct `ALERT` or
  `POSSESS`, no dial of a session). The result counts the node itself when it listens.
  `want` is 20 to find nodes, 5 to find the custodians of a record or the mailboxes of an owner.
- Publishing: the custodians of each record, then one STORE per custodian with all its records, 16 custodians at a time, and after it
  the PROVEs of the records it said 3 for, 8 at a time, until one fails. A dead or slow custodian holds one of the 16 for a timeout,
  or two. A custodian whose PROVE failed counts as not holding those records: they go to it again at the next republish (before, they
  counted as held, and it was left out until the weekly one). A lookup finds the 20 nodes closest to
  a target T, and the node keeps it for an hour: the custodians of a key K near T come from it, with no lookup of their own, when it
  shows that they are the 5 closest of all. A node it does not have is at least D from T, D the distance of the farthest it has; with
  `d(T, K) < 2^b`, a XOR with it changes only the bits of a distance below b, so that node is at least D with those bits cleared from
  K. When the 5 closest to K among the 20 are nearer than that, they are the closest of all. Only a whole lookup is kept: one that
  left no node closer than the farthest of its result unasked (it did not stop at its limit of queries); a whole lookup that found
  fewer than 20 knows every node that answers. One cut short serves its own key only. The two lookups around K in the order of their
  targets are tried (the nearest: they share the longest prefix with K), else a lookup of K itself. A custodian that answers "not a
  custodian", or does not answer, makes the node forget the lookup it came from. A home thus makes a lookup per region of the IDs per
  hour, not per record: on a test of 31 nodes 2000 records went out in 0.5 s instead of 1.4.
- `own.bin` keeps the records the node published, in their order: `PZOWN002`, F, then the first 34 bytes of each. A record is written
  once its STORE went out (one cut by a stop is given again after the restart). After a restart the home gives its records again from
  the first (it reads its `dp.bin` from the start): those `own.bin` has, in the same order, are not sent again (whatever their zero
  bits), and the first that differs ends that: `own.bin` is cut there, as the records after it are not the home's any more (another
  `dp.bin`).
- The republish, two minutes after the start and then every day: the custodians of each record again, with new lookups, and a record
  goes only to those it did not have at the last republish: the nodes that joined since, or that moved up among the closest of a key as
  others left. It reads `own.bin` 65 536 records at a time and sends them in the order of their keys: a batch of 255 has keys that
  follow each other and goes to a few custodians (in a simulation, 5, 9 and 47 at 100, 1000 and 10 000 nodes), where the records in
  the order they were found have random keys (100, 695 and 1183 custodians: a weekly republish of 10^6 records took about 10 days at
  1000 nodes). Publishing does not hold a lock across an exchange: a publish goes on while a republish waits for a custodian.
  `own.cust` keeps what the last republish saw: the records `own.bin` had, the time of the last republish to every custodian, and its
  clean lookups (target and IDs), those whose custodians all took every record they were given; a record that not
  all its custodians took (one refused, or did not answer, or left its PROVE unanswered) makes unclean every lookup that would show it held, and the records those cover
  go to all their custodians the next time. The records
  published since go to all their custodians too. With no node come or gone it sends nothing (`t_dht`: 486 copies, then 0, then 20
  after a node joined, all of them to it). Once a week every record goes to every custodian: a custodian that lost its store gets it
  back, and a coincidence lost on the way comes again (a record that comes again gets its `COINCIDENCE` again). At once, and to every
  custodian, when the node gets its first contacts after having none, if a record went meanwhile to no other node: the points of the
  first home of a network, or of a home that was cut off, go then. Custodians do not copy records to each other.
- Bootstrap: the `entry=` lines of the network file, addresses given with `--peer`, the saved `peers.txt` (whose lines name the
  identity too, which is checked) up to half of the 64 tried, then the addresses found in the table of BitTorrent. A PING to each, 16 at a time, until 8 answer; then a lookup of the own ID fills the buckets. The
  contacts that answered are saved to `peers.txt` (`address xonly-hex`, one per line) then, every 5 minutes and at stop. A node with an
  empty table tries again every 30 s.
- Every hour each bucket that had no lookup in its range gets one, for a random ID in it (from the first bucket a node ID can be in to
  the one after the deepest that has contacts).
- Threads: one accepts connections, 8 answer them (a connection that finds them all busy and 64 waiting is closed, and so is one whose
  block is over its limits), one runs the jobs that take time (the proofs of an alert, the ping of a full
  bucket: each kind in turn, the oldest first), one does the periodic work above, one copies the bytes of each relay under way (7.7);
  a publish has 16 of its own while it stores a batch, and a lookup 3.

### 7.5 The session of a coincidence

Between the two homes, over a Noise channel between their identities (`src/peer/session.c`, run in a thread with the channel and the
chain given as interfaces). Party 0 is the owner with the smaller identity; `eid = SHA256("pznet1/eid" || F || record_0 || record_1)`;
F, the party whose tiny coin goes into the second transaction, is the record of the smaller type when the last bit of
`SHA256("pznet1/first" || F || x || the two types, the smaller first)` is 0 and the other when it is 1; S is the other party. The point
and the types come from the walks, so neither home can choose its part (it was the last bit of `eid`, and a home could change it with a
new identity, about two tries). Every step is a message both sides
send and then read (types 16 to 22: `S_HELLO`, `S_PROOF`, `S_PLAN`, `S_SIGNER`, `S_SIGNED`, `S_TINYSIG`, `S_ABORT`, and 24 to 26, `S_PIECE`, `S_SECOND` and `S_FEE`; an
`S_ABORT` with its reason can come instead of any of them; type 23, `S_ALIVE`, empty, goes every minute while a side waits for blocks, so
that NATs and relays do not cut the channel as idle, and the reader skips it):

1. `S_HELLO`: my record, your record, a nonce, the script of my tiny coin, the latest version of the terms of the payout I know (1 byte),
   my payout script. Each side
   checks the records, that they are a useful coincidence, that they belong to the two ends of the channel and that the other hello is
   about the same pair. The tiny coin, F's only (the script S sends is not used), grows with the fee rate (step 2): twice the fee of F's cancel and the dust limit,
   10 000 satoshis at least, so F can cancel at any rate (were its coin worth less than that fee, S could hold the second transaction
   back with nothing to fear). It comes from the prize, and the second transaction spends it with the prize: the share of the author
   of what its fee leaves, the rest half each (below); the payout scripts must be of standard addresses. The pair pays by the older of
   the two versions of the terms; one that neither home knows ends the session. The nonce is 32 bytes from the system, new in every session: the partner's
   proof is made over it, and the two nonces make the ids of the signings (step 5), which must never repeat.
2. `S_PROOF`: the proof of possession of my record over your nonce. Then, and not before, each side asks its chain whether it is up to
   date, and sends `S_SECOND`: the second transaction of a payout of this pair it signed in an earlier session (empty when none). A side
   without one takes the partner's, checked as in step 7; when either has one, the session settles that payout (steps 6 to 8) instead of
   making a new one: a home stopped after it signed a second transaction, or one that lost its record of it, still gets paid and its
   key. Then each side asks what fee its chain estimates, and sends `S_FEE`, the fee rate it proposes (8 bytes): a stranger's hello makes
   a home ask its chain nothing (before, the rate came in the hello, and the chain was asked for it). The fee rate is the higher of the two. Each proposes
   twice the estimate of its chain for a confirmation within 2 blocks, at least 20 and at most 1000 sat/vB: these transactions move a
   prize, and no such fee is large beside it. A rate above the home's limit ends the session, and so does a partner's rate more than 4
   times the home's own: every try of the payout pays it from the prize. It shows the distance, hence the piece, so no other proof of the piece
   is needed. The pieces (`src/peer/pieces.c`): with `D = t0 − t1`, `x0 = base − d0/D`, `x1 = d1/D` (mod n); either public piece is
   computed by anyone from the record (`d·G = R − t·Q'`), and `X0 + X1 = P`. The hello and the proof of the partner are awaited 30 s at
   most (it makes them at once); every later message 5 minutes. Until the partner proved its distance a wait counts from its start,
   and `S_ALIVE` does not move it; after, each `S_ALIVE` gives another wait, for 2 hours at most (the partner's chain may be behind).
3. `S_PLAN`: `SHA256("pznet1/plan" || the first transaction)`, which each side built on its own from the confirmed coins of the puzzle
   (section 8); a different hash stops the session. A light chain whose servers show no coin at the puzzle address proves nothing (the
   servers may hide them): the session ends and a later one asks again, while on a node of its own an empty address means the prize was
   taken. Then the two-party signing of its inputs (step 5; only now, when the partner has proved its distance and builds the same
   transaction), the broadcast by both, sent again at every block, and one confirmation. F first sends to its payout address its tiny
   coins of earlier sessions of the pair whose first transaction's prize output is spent in a block (a failure or a restart left them:
   no second transaction can take them any more).
4. `S_PLAN` again for the second transaction, built from the first; then F sends `S_TINYSIG`, the scriptSig of its tiny coin, which S checks.
5. The two-party signing: pzsign on each side, its messages (0x81) relayed as `S_SIGNER(more, bytes)` in pieces of at most 65 000 bytes,
   `more` = 1 on all but the last piece; `S_SIGNED(status)` ends the messages of a side. Each signing has its id
   `SHA256("pznet1/run/" || 'A' or 'B' || eid || nonce_0 || nonce_1)` (the nonces of party 0 and party 1). **S is the party of pzsign
   that gets the signatures first** (section 7.6): its pzsign has them at the end of the protocol, and F's only from S's last message,
   which carries them. So in the second transaction S has the whole signature first, and F, whose tiny coin is in it, holds the cancel;
   a last message of F that is garbage, or missing, leaves S without a signature, and F without one too. The first transaction, which
   only gives the prize back to the puzzle, is signed the same way. pzpeer never relays an error of pzsign: its reason would say which
   check failed (project 4.6); the `S_SIGNED` with no signatures that follows stops the partner. A signing ends when no whole message
   of the partner came for 5 minutes, and after 15 minutes in all, whatever else comes on the channel (one more wait at most);
   signatures that verify are kept whatever the partner sends after them, or however it leaves.
6. S keeps the second transaction (`HOME/paid`) and sends it, again at every block until the prize moves (and every 10 minutes while
   the pair has no session, from the home itself); it stops waiting when F's tiny coin is spent alone in a block, the cancel, which voids
   it. F waits 3 blocks for the prize to move; if it does not, F sends the cancel,
   its tiny coin to its payout address, and the transaction S may be holding is worth nothing; it sends the cancel again at every block
   until it is in a block, or the prize moves. A spend counts only once it is in a block: one in the mempool can still leave it, and S
   hold the second transaction again. F also sends the second transaction itself when it got the whole signature, and then does not
   cancel. A wait asks the chain once a block (and again every minute while it does not answer: Electrum servers answer so for a
   transaction none of them has, the cancel while the second waits in the mempools); a wait for n blocks counts them from the first height
   the node gives, and gives up after (n + 6) × 20 minutes if the chain stops (a node that stops answering keeps its last height): the
   session ends instead of waiting for ever, and F, then, tries its cancel. A wait also gives up two blocks after the coins it waits on
   went another way in a block (an old copy of the first transaction, another pair's).
7. The payout counts only when the spend of output 0 of the first transaction is this pair's second transaction: another pair's first
   transaction can spend it too, the prize going back to the puzzle address. With the whole signature a side checks that its second
   transaction is in a block; F without it checks that the prize did not come back to the address. The second transaction F gets in the
   `S_SECOND` of a later session is checked: it spends outputs 0 and 1 of one first transaction (the one F saw paid, if it saw it), and
   both its inputs are validly signed, which only the two homes together can do for the first; that signature covers every output, so
   they are the ones the home signed for then (they are not compared with the payout addresses of the later session: a home may have
   changed its own since).
8. `S_PIECE`, once the payout is settled: the secret piece of the side, 32 bytes. Settled means 6 confirmations of the second transaction,
   proven by a block that holds it (by its outputs, the author's among them, which is seldom spent soon; when all three are spent already
   and its depth cannot be read, the state must have held for 6 blocks in a row, also in a later session, as the home saw it in a
   block), output 0 of the first spent in a block, no spend of the puzzle address waiting in the mempool (a light chain does not show a
   coin whose spend waits there; what servers show there counts only with an input signed by the key of the puzzle), and less than
   100 000 satoshis left at the address that a payout could take: the key protects nothing more. What is left is what a scan of the
   address shows and, one by one, the named coins (the `prize=` lines of the network file, which the networks written in the program
   have) that the chain of the home does not show spent: on a node, what the node says; on a light chain, a confirmed spend with its
   proof, which no server can make up. Only the coins worth an input of a payout at the lowest fee rate count (8880 satoshis at
   20 sat/vB): the rest no payout would ever take, and counting it would let anyone stop the exchange for ever with twelve coins of
   dust (it is left to whoever is first with the key). When the chain does not answer about the address or about a named coin, nothing
   is settled yet. With 100 000 satoshis or more left (coins that came after the payout, or more of them than one payout takes) they
   are paid out first, by a new payout of the pair, whose second transaction, once signed, is then the one each home keeps, and the
   pieces follow; also when every output of the second transaction is spent already. A light chain shows an address as servers list
   it, and servers that hide a coin would show it empty: the partner, with both pieces, would then take that coin alone. A named coin
   they cannot hide: it is there until its spend is proven. So a light home of a network that names no coin of its address never gives
   its piece (the shares are paid all the same; only the key is lost, and a start with `--rpc` exchanges the pieces). Each side first
   says whether it gives its piece (an empty `S_PIECE`; one byte: never, on its chain) and reads the partner's word, and the pieces follow only when both give them: a side that keeps its own takes none, and a piece
   that comes before its word is wiped. After a "never" neither side tries the pair again, until one of them starts with `--rpc`. Each
   side checks the piece it gets against the public piece of the partner's record, adds the two, and checks that the sum is
   the key of the puzzle. Someone gives first, and a partner can keep its
   own piece: all it wins is what the key still opens, the coins of the address on the chains that split from Bitcoin in 2017 and after
   (the prizes of 2015 and of July 2017 are there too: 1.40 BCH and 1.40 BSV at #140, about 300 euros in November 2026, against 14 BTC)
   and whatever comes to the address later. Six confirmations: a reorganization of more blocks has not happened since 2013, and the
   partner would have to make one to take the prize back (a reorganization that takes the second transaction out starts the count again);
   the exchange waits (6 + 6) blocks of 20 minutes at most. A second transaction of an earlier session that is in no block yet is sent
   again at every block meanwhile; one whose coins went another way (F's cancel, another pair) is forgotten, and a later session pays anew.

**The limit of failed signings** (project 4.6). A bug of the signer could leak a bit of a piece through each signing that fails (a selective
failure), so the home counts them in `HOME/failed-signings`, lines `<hex of the network fingerprint, of SHA256("pznet1/fail" ||
fingerprint || x of an own point), or of SHA256("pznet1/ever" || fingerprint || identity of the home)> <failures>`, and on the line of a network the time of its first failure (a line at 0 is left out): every run of pzsign counts as failed before it starts (written
through a new file, synced, that replaces the old one) and is taken off when it gives valid signatures, so a crash or a kill in the middle
counts; a pzsign that could not start counts nothing, and neither does P2 (F) when it gave all its messages and only the signatures of
P1 did not come or do not verify (pzsign ends with 0x84): nothing it answered depends on that, and a partner that keeps them could
otherwise use up the home's count. At 10 failures on a network in 30 days, with any partner, 3 on one own point for ever, with any
partner (the piece at stake is that of the point, and whoever knows the distance of the other point can name it again under new
identities, each a new eid), or 64 of the home on the network for ever, the home signs no more there, and says so
before the plan of the first transaction, so that the partner's pzsign does not start (it would count a failure of its own). Runs are one
at a time; a session that does not get its turn within 2 minutes gives up without a run. The count of a network starts again 30 days
after its first failure: a partner that spoils its messages, with the points of a merger, could otherwise stop an honest home on the
network for ever. The count of a point does not bound what leaks of a walk: once a tame and a wild walk of two homes have met, both
find every later distinguished point of that path, each a new coincidence of the same pair, and the pieces of the home there differ
from one another by sums of jumps that anyone computes from the public seed. A leak about one is a leak about all, and each new
point would give the partner three more failures. What bounds it is the count of the network, ten every 30 days, and, since that one
starts again, the count of the home: at 64 failures in all (kept under the identity of the home, which comes from its seed) the home
signs nothing more on the network, and the reason it gives says what to do: move `home.seed` away and start again. A new seed is new
walks and new pieces, and what may have leaked of the old ones is worth nothing; the points of the old seed are lost to the search.

**The limit of payouts.** Each first transaction pays a fee and F's tiny coin from the prize, so a payout counts, in the same file under
`SHA256("pznet1/paid" || fingerprint || x of the own point)`, from the signing of its first transaction until the prize is paid. At 5 on
one own point the home tries no more there. Without it a partner could make the pair try again and again: F spending its tiny coin
before the second transaction, S keeping either signature back, an old copy of the first transaction sent before the new one; each try
cost the prize a fee and a tiny coin. Not done yet: the count published in the network
and countersigned by the partner, so that it survives a restore of the seed; the file is next to the seed and goes with it in a copy.

**The share of the author** (`src/peer/payout.c`). The second transaction has a third output: 10% of what its fee leaves goes to the
author of pzpeer, at `bc1qfw2y5xzvsy6w70rwqfq6n65tahk906678c7tvz`, and each home gets 45%. The share and the address are in the code,
by version of the terms (`payout_author`), not in the network file nor in an option: nobody sets them, and they are outside the fingerprint
of the network, so a later version may change the address or lower the share without a new network and without losing a point. A pair
pays by the older version of its two homes, which a newer one knows: from this version on, an older home and a newer one always pay each
other. The second
transaction is signed by both homes together, and a home checks its outputs before it signs (the plan; one it gets later from the
partner, `second_ok`, carries that signature over all of them): a home that runs a
program changed to leave the share out pays it all the same when its partner runs this one, and the session fails otherwise. A home whose
pzsolve finds the key on its own sends the coins of the puzzle at once, 90% to its payout address and 10% to the author (`solo_payout`,
`sweep_tx`), but only once its own sessions are over and no second transaction it signed waits for its block: one of them may be paying
it with a partner, whose share it would otherwise take.
The public key of the puzzle is known, so that transaction shows nothing and goes to the public mempool; it is tried again every minute
for an hour while the chain does not take it, and the key goes on the terminal once the coins are sent (or at the last try). The coins
are those of a session, the scan of the address and its named coins; a home without a node sends them all or none (8.2: with a named
coin that no server gives, or with none named, it does not send a part of them, and says why). A key
pzsolve encrypted is not read, and its line says so. The terms are shown
at the first start of a home, which asks to accept them (or `--accept-terms`), and in one line at every start.

**After the payout** (`src/peer/home.c`). The pair, the first and the second transaction and the height go into `HOME/paid`, a line
each, as soon as the second transaction is signed (height 0 until it is seen in a block), so that the payout is settled and the pieces
still exchanged after a restart: a session of a pair in that file only does that (hello, proofs, `S_SECOND`, `S_PIECE`). It is tried
again every 10 minutes for a day, until the key is known, but not after a side said it never gives its piece (a light home of a network
that names no coin of its prize, step 8): then a start of either home with `--rpc` tries again. A light home of such a network says so
when it starts. The key goes into `HOME/FOUND.txt` and on the terminal, as pzsolve does; when the
campaign of pzsolve encrypts its key (`--encrypt-pubkey` or `--encrypt-password` after `--`, the public key in its `campaign.conf`), the
file and the terminal carry only the encrypted line, which `pzsolve decrypt-key` opens. With the prize paid pzsolve has nothing more to
search: the home stops it, and does not start it again at the next start; it stays on, a custodian, and answers a partner that still
lacks this home's piece.

**What the terminal shows.** At the start the terms and the payout address. A home that finds points faster than it publishes them
(a few hundred a second on a test machine: at dpbits 30, tens of graphics cards in one home) says so with the status line, with the
number of points that wait in its dp.bin. Then one line with the time for each step that matters: the
coincidence and its partner, the proofs, each transaction sent (its txid) and confirmed, the payout with the amounts (to each home and to
the author), F's wait and cancel, the exchange of the pieces and the key, and the
end of the session with its reason (`session ID: ended: paid; the key of the puzzle is known`, `... ended: failed: REASON`). A
coincidence that comes after the prize was taken is not signed (one line says so), and pzsolve stops: a home that paid stays on only to
give the partner its piece; one whose session finds the puzzle address empty stops searching too. When pzsolve ends on its own the line
says whether it found the key (and where it is) or why it stopped; one that found it sends the coins, and a line gives the amounts and
the txid. Every 10 minutes a line of the network: its nodes, the points in all, its jumps a second and the share of this home, with what
it gets of the prize on average (its share of the work times the 90% the terms leave to the homes: two homes split a prize, one alone
takes it whole, so either way the share of the work), and the time within which the key comes with half the chance and with 9 in 10. The network is estimated from what this home
keeps as a custodian, so only a home that listens shows it: a record goes to the 5 nodes closest to its key, so the network holds about
`held × nodes / 5` records (`held` those this home keeps), each worth `2^dp` jumps on average with dp its zero bits, and its rate is the
growth of their worth over the last hour. The nodes come from the distances of the 20 closest to a random point (`dht_nodes`: the j-th of N nodes is at about
j / (N + 1) of the space of the IDs), a fifth off for one lookup, the mean of the last six used. Then the model of pzsolve: the key comes
after n jumps with `P = 1 − exp(−(n/c)^2)`, `c = 1.18 / 0.8862 × √W` (1.18 is the cost of the walk of `net-06`,
section 6), and given the work done (`done`) half the chance is within
`√(done² + c² ln 2) − done` more jumps, 9 in 10 within `√(done² + c² ln 10) − done`. `t_dht` checks the estimate on 30 nodes and 2000
records (about 32 and 2120). It saturates when the custodians are full (`--store-gb`), and it trusts nothing a node says about itself: a
point costs the work of a point, real or not.

**The daemon** (`src/peer/home.c`). A session starts on the alert of the table (both proofs checked there) or on a hello that names the
pair; anyone can copy the x of a published record into a record of its own, so a session started by a hello alone costs little before
the proofs (pzsign starts after them, step 3), at most 16 of them run at a time, one per partner, and their slots are given back when
they end unless the partner proved its distance: at least 48 of the 64 slots are always there for the pairs the table alerted. Once a
partner proved its distance on an own point, the point has its pair: no other pair on it gets a slot, a session of another pair on it
ends at its own proof, and the slots other pairs hold on it are given back when their sessions end (the records of one partner named
under other identities, which a custodian alerts together, and copies that nobody can prove); while a session runs on a point a hello
for another pair on it is refused. The rule lives in memory: after a restart another identity of the same partner may be the pair of
the point, and it finds the same counts of failed signings and of payouts, which are by the own point, not by the pair. A pair
that the table alerted, or whose partner proved its distance, and whose session did not pay starts again 10 minutes after it ends, up to
144 times (a session may wait for blocks for a day); then its slot is given back, and an alert that comes again starts it afresh, within
the limits above.

`tests/peer/t_session.c` runs both homes against each other with the real pzsign and a chain in memory that checks every input: the
payout (10% to the author, the rest half each of the prize and of F's tiny coin; with a chain that loses its first answer about the height); a partner that keeps the second transaction and its last message, which ends with F's cancel and the prize still
under the joint key, also when it keeps the channel alive with `S_ALIVE` for ever; a partner F that stops, or sends garbage, in place of
its last message, which gets nothing and cancels, or sends garbage after it, which leaves S its signatures and pays; the same at a
high fee rate, where the tiny coin is larger and F still cancels; a partner's rate far above the home's, refused; the limits of failed signings and of
payouts, checked in every case, refused before anything is signed; a hello whose version of the terms no home knows; a partner that copied the x of a
record and cannot prove it, for which no pzsign runs; a pzsign that stops reading while the other floods it; a scan of the puzzle
address that fails, where the prize of the network file is paid; a chain that stops after the first transaction, where the wait ends;
scans of F slower than the partner's wait; a coin that comes to the address after the payout, whose own payout S holds back; every
output of the second transaction spent before the pieces; the named coins of the address, spent as a node shows them, one shown unspent by a
light chain (it counts in what is left: no piece, and the partner gives none), none known to a light chain (never a piece, and no more tries); the
limit of the home on a network for ever; a second transaction given again to a home that changed its payout address; dust that no payout takes at the address after the payout,
which does not stop the pieces. The counts of the limit are checked in every case, and every channel carries `S_ALIVE` before the messages that are read.

Not yet handled: a first transaction whose signature encoding a third party changes before it is mined (its txid changes: the session waits
for its own txid and gives up); the state of a session is in memory only (after a restart the pair starts again from the coins as they are),
but for a payout (`HOME/paid`);
two sessions of one puzzle at once build their first transactions from the same coins, and the one that loses waits its 144 blocks;
"paid" means that the prize output was spent in a block, by the second transaction or by anything else (another session of the same
puzzle); on a node of one's own, a reorganization that takes the first transaction back to the mempool while the second waits there too
looks like a spend in a block (neither output is in the node's set).

### 7.6 pzsign

pzsign runs the two-party ECDSA of cb-mpc, Coinbase's library (after Lindell 2017; MIT; a copy in `signer/vendor` with two changes
for Windows). [signer/README.md](../../signer/README.md) has its frames, its job and its envelope byte by byte; in short:

| Type | Direction | Payload |
|---|---|---|
| 0x01 | pzpeer → pzsign | the job: party `i`, the party `first` that gets the signatures first, the signing id, the own piece, the two public pieces, the key, the digests |
| 0x02 | pzpeer → pzsign | a message of the other party, opaque |
| 0x81 | pzsign → pzpeer | a message for the other party, opaque |
| 0x82 | pzsign → pzpeer | `r` and `s` of each digest, `s` low, verified against the key; last frame |
| 0x83 | pzsign → pzpeer | the reason it failed; last frame |
| 0x84 | pzsign → pzpeer | the same, from P2 once it gave all its messages: the signatures of P1 did not come or do not verify (not counted, 7.5) |

The own piece of party 0 includes `base`; pzpeer computes both public pieces from the records (section 7.5). A session imports the two
pieces through the key generation of the library (with each party's piece in place of a random one: the commitments, the proofs of
knowledge of the pieces, a fresh Paillier key of `first` with the proofs that it and the encryption of its piece are well formed),
the key must come out as the puzzle key, and then signs all the digests in one run. `first` (P1 in the library) gets the signatures and
sends them to the other; each side verifies them before it gives them. The name of P1 carries the signing id, and every proof is bound
to a session id that both sides make random together, so that no proof counts in another session. No presignature, no state between
sessions, nothing on disk.

- pzsign has no timeouts: pzpeer ends a process that does not answer, and kills it once its last frame came or did not come in time
  (a pzsign that stopped reading would otherwise keep the relay of the partner's messages waiting for ever). Its pipes are not
  inherited by any other program pzpeer starts, nor are dp.bin and the sockets.
- The last message, the signatures, can be lost after `first` already has them: one side ends with 0x82 and the other with 0x83. This
  is the case of the cancel lever of step 6 of section 7.5.

**The review of how pzsign uses the library** (by the author of pzsign, not an outside audit). What was read: `pzsign.cpp`, and in
cb-mpc the two-party key generation and signing (`protocol/ecdsa_2p.cpp`, `protocol/ec_dkg.cpp`), the proofs they use about Paillier
(`zk/zk_paillier.cpp`) and the commitments (`zk/zk_pedersen.cpp`, `protocol/int_commitment.cpp`, `zk/zk_unknown_order.cpp`).

- The commitments of the key generation are in a fixed group: its modulus is a constant of the library and its second generator comes
  from a hash, so neither party chooses them. The proofs that P1's Paillier key and the encryption of its piece are well formed take
  their challenges from the other party after P1 committed, so they cannot be retried offline. Their margin is about 2^-50 a run, not
  2^-128: a P1 that cheated in them and was lucky once could learn the other piece in that run. Every failed run counts toward the
  limit of failed signings (7.5), 3 on an own point, so the chance stays near 3·2^-50.
- A message of the partner inside the size limit of its phase can make the library reserve about 400 times its size (about 120 MB for a
  run of 64 digests) before it is refused: memory only, one run at a time.
- The signing uses the default protocol of the library, which makes P2 prove its last message well formed: the variant without that
  proof, whose failures can leak a bit of P1's piece, is not used. That proof uses commitments modulo a number whose factors Coinbase
  generated once (its `SECURE_USAGE.md`); with the published proof that the parameters are related as they must be, the one trust
  left is that nobody kept the factors: whoever did could cheat in that proof as F, which is what the limit of failed signings
  (section 7.5) bounds.
- The pieces are checked before anything else: each `x_i·G` equals its public piece, and the two add up to the puzzle key.
- A signature leaves pzsign only after it verifies against the puzzle key, and pzpeer verifies each again with libsecp256k1 in the
  transaction (section 8).
- The digests are given to the library reduced mod n: ECDSA uses nothing else of them, and the library's proofs and its own check need
  them so (a digest above the order failed there before; the tests sign one).
- `signer/test.py` plays the partner that cheats: one byte changed in the middle or in the header of each message, in turn, or the
  message dropped; the messages of an earlier session given again; another digest; another signing id; a piece that is not its public
  piece; pieces that are not the key; malformed jobs. In every case no side gets a signature, but `first` when its own last message,
  the signatures themselves, is the one damaged.

What this does not give: a proof that the library has no flaw. Its protocols have published specifications and proofs, and the library
had two audits in 2024; an implementation can still differ from its proof, and finding that kind of flaw is the work of an outside
audit of how this network uses it.

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
- **Limits at the relay:** 32 relays under way at once and 256 homes waiting (a waiting home is an open connection, without a thread; with 2 per block, 16 and 128 blocks to take them all, where 8 and 32 took 4 and 16;
  pzpeer raises the files it may have open to 4096 where the system lets it, as macOS gives a program 256); 2 relays (waiting or under
  way) per address block of the homes, counted for the home that waits and for the one that joins it (no limit per identity: an identity costs less than an address); 32 MiB per relay, both ways together
  (a session moves about 100 KB, the largest message of pzsign 36 KB); 4 hours per relay (the channel stays open while the session
  waits for the confirmation of its first transaction), and 3 minutes without a byte (a session says it is alive every minute): pairs
  that join and say nothing do not hold the relays. A relay under way is one thread that copies through the receive buffers of the two connections.
  A node of a linear search (section 11) relays nothing: that search has no sessions, and its relays would serve strangers only.
- **The channel of a session:** `dht_channel(partner, eid)` gives the daemon one connection with the partner, whichever way it comes.
  Party 0 dials the partner it finds in the table, and so does a side nobody reaches (nobody can dial it). A side the others reach
  gives the partner's own connection (the one `on_session` got) the first slot, then dials the partner too: the partner may not reach
  it, or may wait for nothing (its session gave up on the pair long ago, and a home back after an absence starts it again). Then the
  relay of each slot, slot after slot, while the partner's connection is still taken if it comes, also during the wait at a relay; for
  10 minutes. `home.c` asks it for every session once its chain is ready and its fee rate known: the partner closes a connection whose
  hello does not come within 2 seconds of the connection, handshake included, and a chain without a node can take longer to answer.
- The relay carries sessions only: the proofs of an alert go through the mailboxes (7.3), the requests of the table are not relayed.

`tests/peer/t_net.c` splices two connections, runs 6 MB through them end to end, and checks both identities and the limit of bytes.
`tests/peer/t_dht.c` has two homes that do not listen prove their points to each other through their mailboxes and run a stand-in
session of 2 × 2.4 MB through the relay of their slot, then tries the limits of a relay: a wait without a partner, a third party, the
relays at once, the bytes.

### 7.8 What the network layer still lacks for a public network

| Missing | Why it is not done |
|---|---|
| IPv6 as a way out of NAT | addresses, listening (`::` takes IPv4 too) and NODES carry IPv6 already, but a home does not learn or announce a public IPv6 address of its own: a responder keeps the address it sees. There is no IPv6 on the test machine |
| Tor, optional (project 2.5) | a SOCKS5 client in `net.c`, and onion addresses in NODES, which has 16 bytes for an address: a new format. A node behind Tor cannot be a custodian, like one that does not listen |
| A defence of the rendezvous key in the BitTorrent DHT | nodes that sit close to the key there can hide the announces (7.9). The rule that ties a node ID of that table to its address (BEP 42) would make it dear; many nodes of that table do not follow it yet |
| Spot checks of the walks (project 4.2) | they need the previous distinguished point of a walk from its owner, so pzsolve and `dp.bin` would have to keep the chain of each walker; a check costs minutes of a core and needs a protocol of its own (ask, walk, revoke a machine) |
| Open connections instead of polls | a home that does not listen polls its mailboxes every minute (a lookup and 5 connections); the project wants it to keep a few connections open to reachable nodes and get its alerts on them |
| More than one relay at a time | a pair tries one relay per slot; two homes that cannot reach the same node in any slot (a network that blocks part of the Internet) get no channel |
| The cost of an identity | 16 zero bits cost seconds, so thousands of identities are cheap: the limits by block bound what one network can do, not what many networks (a botnet, a cloud) can |
| An index that fits a home | the index takes 8 bytes a slot, 11 to 23 bytes a record (7.3): 2 GB for the 2^27 records of the default store (5 GB of disk), 4 GB for 2^28 (10 GB); a larger store needs its index on disk |

### 7.9 Finding the network with no server of its own

The owner of the network runs no server. Three things make the first contact, used together:

- `peers.txt`: the contacts that answered in an earlier run.
- `--peer HOST:PORT`: an address from someone who is already in.
- **The Mainline DHT of BitTorrent** (`bt.c`, BEP 5), for who has neither. Every node that listens announces its port there under the
  key `SHA256("pznet1/bt" || F)` cut to 20 bytes, and every node looks the key up: a round when it starts, then every quarter of an
  hour, since the nodes of that table forget an announce after about half an hour (every two minutes while it has no contact). A round
  is a lookup with `get_peers`, eight questions at a time, until the eight closest nodes that answered are closer than any node not
  asked yet (96 questions and 20 seconds at most), then `announce_peer` to those eight. pzpeer asks and never answers: it is a guest of
  that table for some tens of packets every quarter of an hour, over UDP and IPv4. A network of
  the main chain starts from the well-known routers of that table (`router.bittorrent.com:6881`, `dht.transmissionbt.com:6881`,
  `router.utorrent.com:6881`, `dht.libtorrent.org:25401`), unless `--no-bt`; `--bt-router HOST:PORT` gives others, and is the only
  way a test network uses it.

Only addresses pass through that table, and none is believed: each is a candidate for the next bootstrap, which dials it with Noise
and takes it only when it is a node of this network with a valid node ID; an address that is not public is dropped on a network of
the main chain. The records, the alerts and the sessions never go there. That table takes an announce only for the address it comes
from (the token of BEP 5), so nobody can announce a third party to make the nodes dial it.

What an attacker can do there: put nodes close to the key, take the announces and give back nothing or only its own nodes. A node
that enters for the first time then finds nobody, or only the attacker, who can hide records from it and delay its alerts (section
7.4 says what the table does against that) and can steal nothing: a node stores only its own records, a coincidence is proven by both owners, and
the payout needs both signatures. `peers.txt` and `--peer` do not depend on that table. A packet from that table counts only when it
comes from the address asked, with the number of the question, and is read within its bounds (`t_bt` runs nodes that send random
bytes, answers to other questions, nodes that do not exist, and peers without end).

**The port.** A node listens on TCP port 28140 unless `--listen` says otherwise (`--listen 0`: it only dials out). Behind a home
router it is dialled only when the router lets the port in, and pzpeer asks the router for it unless `--no-portmap`
(`portmap.c`), as Bitcoin Core does since its version 30: the `--listen` port over TCP, the same port outside, for two hours, asked
again before half of the time the router gives passes. First by PCP (RFC 6887), one datagram to port 5351 of the router of the default
route and its answer, which counts only from there; a router that only speaks NAT-PMP (RFC 6886) answers PCP with version 0 and is
asked in that way. A router that speaks neither is asked by UPnP IGD: a search on the LAN (SSDP), the description of the router, and
`AddPortMapping`; a router that only takes mappings without end gets one. Nothing the router says there is followed out of the LAN:
its description and its control address must be at the address its answer came from, and that address must not be a public one.
The line `router:` of the log says which way worked, or why none did. It is on by default so that a home takes its part of the custody
without its owner doing anything; pzpeer deletes the mapping when it stops (one left by a crash lasts its two hours). A node that does
not listen still searches, publishes and signs (through the relay, 7.7); it is not a custodian.

`t_bt` and `t_portmap` run against a table and a router made in the tests (one that speaks PCP, one that only speaks NAT-PMP, the UPnP ones); `tests/peer/e2e_meet.sh` starts three nodes that are given
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
- The rules of the project document (section 4.5): at most 8 inputs, coins worth less than 3 times their fee are left; 10% to the author,
  the rest half each.
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
- An output spent only in the node's mempool still counts as unspent (`gettxout` without the mempool, which reads the set of the
  chain; when the output is not there, `getmempoolentry` tells a transaction that waits there from a spend in a block): a spend counts
  once it is in a block (7.5, step 6).

- A puzzle address with more than about 45 000 coins makes the reply of the scan larger than the 16 MB of the JSON reader: it would cost
  an attacker about 0.3 BTC (the coins of 546 satoshis and their fees), much less than a prize. So the coins of a puzzle address are
  always the scan and the named coins (`session_coins`, one function for the session, the solo payout of a home and `pzpeer sweep`):
  the `prize=txid:vout:sats` of the network file (at most 16; the txid as explorers show it), each checked with `gettxout` without the
  mempool: unspent, confirmed (mature for a coinbase), of the puzzle's script, with the node's value; `sats` is not used. The largest 48
  coins of the scan, when it works, and the named ones not among them make the coins of the first transaction. Two homes whose network
  files name different coins may build different transactions when the scan fails: the plan stops them, until their files agree.
- A sweep of a key found alone (`pzpeer sweep`, and the solo payout of a home, 7.5) takes what the node lists, the largest 16 coins
  worth their input, and says how many it leaves; the fee is twice the estimate, from 50 sat/vB (a pair starts at 20: it can pay
  again at a higher rate, while this transaction goes once, raced by whoever reads the key in it).
- Fee rates are asked for a confirmation within 2 blocks (project 4.5), the higher of the two homes' taken (7.5).

### 8.2 Without a node of one's own

`pzpeer run` without `--rpc` reads the chain from Electrum servers: the `electrum=host:port` lines of the network file and those of
`--electrum HOST:PORT`, 8 at most; a network of the main chain with none of either uses a built-in list of eight public servers of
different operators, and the block 969696 as its checkpoint. The connections stay open from one question to the next (a session asks
once a block while it waits for one, and the height every 15 seconds, which the servers are asked every 10 seconds at most for all the
sessions); the servers are asked one after another, each with a minute for its calls of an operation, its headers included, and one that
does not answer, or announces blocks it cannot show, is left alone for ten minutes. None is trusted: nothing a server says is
believed without a proof, and what a lying server can still do is hide things, which is a delay and never a wrong payment: servers that
show no coin at the puzzle address do not make the home take the prize for gone (7.5, step 3), and an address they show empty makes it
give its piece of the key only when every named coin of the address is proven spent (7.5, step 8; with none named, never: a coin that
came to the address later and that all the servers hide would still go to the partner alone). The named coins, of the network file or
of the program, are looked up one by one (`blockchain.transaction.get` of the txid the file or the program names, its bytes hashed to
it, and no server that proves the output spent): the coins of a puzzle address are older than the built-in checkpoint, where no kept
header proves their block, so no scan of the address lists them; a server that hides their spend only makes the first transaction
spend a coin that is gone, which the network refuses. The table of the program (`data/prizes.csv`) names every coin that was at the
address of an open puzzle when it was made, so a home without a node builds the same first transaction as one with a node, which lists
them all. It is a snapshot and needs no renewal with the checkpoint: a coin that came after it is above every later checkpoint,
where the scan lists it, and one spent since is found spent.

A sweep of a key found alone, by `pzpeer sweep` or by the solo payout of a home (7.5), works without a node for the puzzles the
program knows, or with the `prize=` lines of a network file: it takes the named coins and what the scan lists. It never sweeps a part
of the coins unknowingly, because the transaction shows the public key and whoever is fastest takes the rest. So through servers it
refuses, and says why, when no server gives the transaction of a named coin or none answers about the address (a server hid it: try
again, or use `--rpc`), and when no coin of the address is named (its older coins cannot be seen without a node). A spend the servers
hide makes the transaction invalid, never partial. What it leaves on purpose it says: the coins worth less than their input, and those
beyond the 16 inputs of one transaction.

The connection is plain TCP, one JSON object a line; as every
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
   its branch. For the wait of F and the confirmations of our own transactions, an output is spent only when such a transaction is
   confirmed: a server that just stops listing the prize proves nothing, nor does one that shows a spend in its mempool (the partner
   could run it, and keep the transaction to itself). Every confirmed transaction of the history of the script from the block of the
   coin on is looked into, oldest first, as far as the server's minute goes, every input of it however many it has, and what each
   spends is kept for the next look: dust sent to the address cannot hide a spend, nor can the shape of the transaction that spends.
4. **Our transactions** go to every server; the broadcast holds when a server then serves the transaction back.
5. **Fee rate:** the lower median of `blockchain.estimatefee` of the servers that answer, under the cap of the file: servers that raise
   it win only as a majority.

What it does not check: that the transactions of a block are valid. A coin that does not exist needs a block with real work that
holds an invalid transaction: on the main chain that costs a block's reward for each block, and it only makes the two homes sign a
first transaction the network refuses. On testnet and signet only the proof of work against the header's own target is checked
(their rules of minimum difficulty and of signed blocks are not): good for tests, not for money.

`t_spv` checks the rules on real headers of the main chain; `t_chain_spv` runs the sessions' calls against servers that hide a coin,
list a spent one, give false proofs, add blocks without work, drop a broadcast, do not see the mempool, list there a transaction they do
not have or answer slowly, a spend behind 260 transactions of dust, a spend by a transaction of 17 inputs, a coin of the prize below the
checkpoint, and the sweep of a key (a named coin below the checkpoint taken, a named coin no server gives and an address with no named
coin refused); `PZ_E2E_LIGHT=1
tests/peer/e2e_net.sh` pays a coincidence with three homes that have no node. Not tried yet against a public server.

## 9. Status

Everything below works and is tested; `tests/peer/e2e_net.sh` runs the whole of it on one machine with a bitcoind in regtest: a custodian
node and three homes (`pzpeer run`, each with its pzsolve), a puzzle of 58 bits funded with 1 BTC. With Bitcoin Core 28.1 a coincidence
between two homes ended in the payout: 45% each of the prize less two fees and 10% to the author, nothing left on the puzzle address.
With so few homes a home may find the key on its own first (its own walkers meet too): it sends the coins at once, 90% to itself and 10%
to the author, which the script checks, and then starts a new try with a new puzzle. The cases of the session (the payout, and the cancel when a partner keeps the second transaction) also run on the node
in `t_session` with `PZ_REGTEST`.

| Part | State |
|---|---|
| pzsolve in network mode (section 6) | done; `tests/e2e.sh`, `kstat net` (net-06 measured on the curve) |
| pzcore: ChaCha20-Poly1305, the generator of the home seed (section 4.1) | done (`t_aead`: RFC 8439 vectors; `t_seed`) |
| `noise.c`, `keys.c`, `record.c`, `dpfeed.c` | done (`t_noise`: byte for byte against the noiseprotocol package of Python; `t_record`; `pzpeer dpcheck` on real dp.bin files) |
| `net.c`, `store.c`, `dht.c` | done (`t_net`, `t_store`, `t_dht`: 33 nodes in one process), with the limits of 7.3 and 7.4 and the relay of 7.7; what a public network still needs is in 7.8 |
| `json.c`, `btc.c`, `btc_rpc.c`, `chain_rpc.c` | done (`t_json`, `t_btc`, `t_btc_rpc`, `t_chain_rpc`, `fuzz_peer`; `t_session` with `PZ_REGTEST` on Bitcoin Core 28.1; the coins also on a pruned node) |
| `bt.c`, `portmap.c` | done (`t_bt`, `t_portmap`, `tests/peer/e2e_meet.sh`); not tried against the real table of BitTorrent nor a real router |
| `sweep.c` | done (`tests/peer/e2e_sweep.sh`: the node would take the transaction, and pzpeer did not send it); `--private` tried against a stand-in of the pool, not the real one |
| `block.c`, `claims.c`, `dht_claim.c`, `linear.c` | done (`t_block`, `t_netfile`, `t_dht`; `tests/peer/e2e_linear.sh`: a node and two homes, two blocks of 2^34 keys, the key found and the claims held); the spot checks to come |
| `spv.c`, `electrum.c`, `chain_spv.c` | done (`t_spv`, `t_chain_spv`, `PZ_E2E_LIGHT=1 tests/peer/e2e_net.sh`); not tried against a public Electrum server |
| pzsign, `signer.c`, `pieces.c`, `payout.c`, `session.c` | done (`make signer-test`; `t_pieces`, `t_payout`, `t_session`: the payout and the cancel, in memory and on regtest) |
| `netfile.c`, `pzpeer netcheck` | done (`t_netfile`) |
| `sys.c`, `sys_posix.c`, `sys_win.c` | done on Linux (`t_sys`). Windows: `make windows winsigner` builds `pzpeer.exe` and `pzsign.exe` with MinGW-w64, and under Wine the unit tests pass (`make wintest`; all but `t_btc_rpc`, `t_chain_rpc` and `t_session`, whose stand-ins are POSIX programs), three nodes find each other (`PZPEER="wine build/win/pzpeer.exe" tests/peer/e2e_meet.sh`) and `t_pieces` signs with `pzsign.exe`; on the Windows machine of the CI the same unit tests pass, `t_sys` with the Ctrl-Break by which pzpeer stops pzsolve (Wine does not deliver it) and `t_pieces` signing with `pzsign.exe`. macOS: the CI builds pzpeer and pzsign and passes their tests, the signing included, on both architectures (x86_64 under Rosetta). A whole network (pzsolve, the session, a payout) has not been run on a real Windows or on a Mac |
| `home.c`, `pzpeer run` | done (`tests/peer/e2e_net.sh`) |
| to come | the restart of the walkers of a merge (now only logged); a session that survives a restart of pzpeer; the spot checks of the walks (project 4.2); a whole network on a real Windows and on a Mac; the public test |

## 10. A test network on one machine

```sh
make && make peer && make signer            # pzsolve, pzpeer, pzsign (OpenSSL is fetched and built the first time)
make peer-test                              # the unit tests, the signing ones with the real pzsign
bitcoind -regtest -daemon -rpcuser=pz -rpcpassword=pz -fallbackfee=0.0002
PZ_REGTEST=127.0.0.1:18443:pz:pz PZSIGN=build/pzsign build/peer/t_session    # the session on the node
PZ_REGTEST=127.0.0.1:18443:pz:pz tests/peer/e2e_net.sh   # the whole network: a custodian node, three homes, a payout, the key
```

By hand, every home runs, with the same network file (section 5),

```sh
pzpeer run --network FILE
```

The first start shows the terms of the payout and asks to accept them, then asks the payout address (any standard address); both are
kept in the folder (`HOME/terms`, `HOME/payout`), so the next start needs neither. A service, which has no terminal, takes
`--payout ADDRESS --accept-terms` once. The home has pzsolve hunting, its points published, the records of others kept and its coincidences paid. What it adds on its own: the
folder `pznet-NAME` (`--dir`), pzsolve and pzsign beside pzpeer, port 28140 opened on the router, the other nodes found in the table
of BitTorrent and the chain read from Electrum servers (on the main chain a built-in list and checkpoint; on a test chain
`--electrum`, `--bt-router` or `--peer`, and `--rpc` for a node of one's own). `--debug FILE` writes every message, step and answer
of the chain with its time, and no secret: the file to read when something went wrong. Options after `--` go to pzsolve
(`-- -t 2 --no-gpu`).

**A home that starts with the computer** is a task of the system: start it once by hand (the terms and the payout address), then add,
with the full paths and `--dir`:
- Linux and macOS: `crontab -e`, a line `@reboot /PATH/pzpeer run --network /PATH/net.txt --dir /PATH/home >> /PATH/home.log 2>&1`;
- Windows: `schtasks /create /tn pzpeer /sc onlogon /tr "C:\PATH\pzpeer.exe run --network C:\PATH\net.txt --dir C:\PATH\home"`.

With a bitcoind of its own and no `rpcpassword`, `--rpc 127.0.0.1:8332 --rpc-cookie ~/.bitcoin/.cookie` takes its credentials from its
cookie file instead (section 8.1).

The home seed is made on the first run in `HOME/home.seed` (keep a copy: it is the key of every point of the home); pzpeer refuses a seed
file that others may read, or a link, and a home folder that others can open (on Windows it gives the folder the rights of this user and
of the system alone, also when the folder was made from an elevated shell). A folder is for one pzpeer at a time (`HOME/lock`,
held while it runs: a second one on it stops at once; two would write over each other's records, journals and sessions).
`HOME/start.key` is the start key pzpeer gives to pzsolve, written at every start through a new file of mode 0600; `HOME/terms` and `HOME/payout` keep the terms accepted and the payout address;
`HOME/failed-signings` counts the failed signings (section 7.5); the campaign is
in `HOME/campaigns/net-NAME`, its output in `HOME/pzsolve.out`, and what pzsign says in `HOME/pzsign.out`. After a payout `HOME/paid`
keeps the pair until the pieces are exchanged, and the key goes into `HOME/FOUND.txt`; the node of the home keeps its contacts and the
records it holds for others in `HOME/node/`.

### A node that stays on

What a Bitcoin node does is the model.
- **Where.** A machine with no personal data on it (a small rented server, a small computer of its own) rather than one that holds them,
  such as a NAS: the port is open to everyone, and a fault of pzpeer would give whoever used it what pzpeer can reach.
- **How.** `pzpeer node --network FILE`: no pzsolve, a custodian and a relay only; its identity is `DIR/node.seed`, made at the first
  start (as a Bitcoin node keeps its keys in its data folder, never on the command line). As its own user that sees only its folder:
  [`tools/pzpeer-node.service`](../../tools/pzpeer-node.service) is the systemd unit, with the protections of the unit of Bitcoin Core
  (`systemd-analyze security` rates it 1.3, where 10 is a service with none).
- **The port.** Only TCP 28140: a rented server has it open already; at home it is opened by hand on the router toward that machine,
  so the request to the router is off (`--no-portmap`). The node shows the others its address and port, as every node of the table
  does, and nothing else.
- **Disk and memory.** `--store-gb N` (5 by default): once the store is full its index takes 30 to 60 % of N GB in RAM, depending
  on N (2 GB at the default, two fifths; section 7.3).
- **A VPN such as Tailscale** is for reaching the machine, not for the network: the other nodes reach a custodian only at a public
  address. Bitcoin Core also takes connections through a Tor onion service, with no open port and no address shown; in a table such as
  this one every node must reach every custodian, so a custodian behind Tor would serve only the nodes that have Tor too: not now.

## 11. The shared linear search

A puzzle whose public key is not known is searched key by key, and the key is seen whole by whoever tries it: no prize can be split.
What a network can do is keep two people from trying the same keys: a register of the blocks already tried. Tried once, the range
ends for sure, and in half the keys on average; nothing more than that is gained.

A register is worth what its entries are worth, so every entry carries a proof of work.

**Near misses (pzsolve).** Every CPU engine and GPU kernel compares bytes 8 to 11 of the hash160 of a key with the target's
before the rest. A key that passes is a *near miss*: one key in 2^32. The engines report them (`scan_job.on_near`, the hits of the
kernel), the host checks each on the CPU (`run_scan_candidate`, which also takes the key of the campaign from among them), and
`hunt --near` writes them to `near.txt` of the campaign. Nothing else in pzsolve knows of a network.

**The network.** A network file with `address=` (the P2PKH address of the puzzle, of its chain) and `blockbits=` in place of `pubkey=`,
`jumpseed=`, `schema=` and `dpbits=` is a linear search; the network of every puzzle of the table without a known public key is in the
program, with blocks of 2^44 keys (`pzpeer run --puzzle 71`, `pzpeer netcheck 71`). The range is cut into blocks of 2^blockbits keys
from its start, the last one maybe shorter (`block_range`, `block_count`): puzzle 71 has 2^26 of them.

**The claim of a block (`block.c`, `t_block`).** A *proof key* of a block is a near miss (blocks of 34 to 37 bits: bytes 8 to 11 of the
target's) or a near miss with byte 12 of the target's too (42 to 45 bits): 16 in a block of 2^36 or 2^44 on average, 4 in one of 2^34
(for tests). A claim is

| Field | Bytes | |
|---|---|---|
| index | 8 | the block, big-endian |
| owner | 32 | x-only identity of who scanned it |
| n | 1 | proof keys, 64 at most |
| offsets | 6 each | of the proof keys in the block, increasing |
| signature | 64 | BIP340, by the owner, over `SHA256("pznet1/block" \|\| F \|\| the fields before it)` |

and is valid with a good signature, increasing offsets inside the block, every key a proof key, and at least half the average number
of them (`block_min_proofs`: 8 at 36 and 44 bits), so that a false claim costs half a block of work on average. An honest scan has
fewer in about one block in a hundred at 36 and 44 bits (one in ten at 34 and 42, one in a thousand at 37 and 45): its claim is kept
by its home, which does not scan that block again, but not taken by the custodians, and other homes may scan it again. Those blocks
cost little of the time to the key, which they do not hide (whoever scans one finds the key in it). Checking a claim costs some tens of
hashes. A claim shows that at least half of the block was tried, on average; it cannot show that no key was skipped.

**The register (`claims.c`, `dht_claim.c`).** A claim goes to the 5 nodes closest to `SHA256("pznet1/blockkey" || F || index)`, with
`CLAIM(claim)` (type 32, answered `CLAIMED(status)`: 0 stored, 1 already held from this owner, 2 nothing new, 3 refused (not a
custodian of that key, 4 owners of the block already, the store full), 4 not valid or not written), from its owner only (the
owner in the claim is the identity of the connection), checked before it is kept (a claim is the one request of its connection: the
rate of the connections of an address block bounds them). The first three leave the claim held. A custodian keeps at most 4 owners of a block, and another owner only with a proof key
that the claims it holds of the block lack: the proof keys of a block are the same for whoever scans it, so a claim copied under a new
identity, which costs nothing, brings none ("nothing new"), while the scan of a block that a false claim covered brings the keys the
false claim left out. 2^21 claims in all, in `claims.bin` (slots of 491 bytes, about 1 GB); a slot that cannot be read (a write cut by
a crash) ends the file there at the next start. In memory a table of the block of each slot, by the index of the block, at most half
full: 16 bytes a slot, 64 MB at 2^21 claims, and a few steps to find the claims of a block or add one (before, a sorted array moved
half of itself for each claim added: 4 ms a claim near the end, against 1 µs). At start the table is made again from the file: 1.5 s
for 2^21 claims read from the cache of the system. `CLAIM_GET(index)` (34) is answered `CLAIMS(n, n × (length 2, claim))` (35), and whoever asks checks each claim itself.

**The home (`linear.c`).** `pzpeer run` on a linear network: no payout, no terms, no chain. The home picks a block at random that it
did not scan itself, asks its custodians whether anyone claimed it, and tries another one while it is claimed. After 64 claimed ones,
the second pass: of those 64, the one whose claims show the fewest proof keys is scanned again, so that a false claim, which shows few,
delays the key and does not hide it. With many homes two seldom pick the same block. Then `pzsolve hunt -r BLOCK -a ADDRESS --near
--stop-with-input` scans it (the campaign `campaigns/b<index>`, which a restart resumes: the block under way is in `block`); at its end
the home makes the claim from `near.txt`, keeps it in `own_claims.bin` (a file of claims as above: the blocks this home scanned),
publishes it when it has enough proof keys, and deletes the campaign. Its claims go to their custodians again from two minutes after a
start, one a turn of the loop, and then every day. A key found stays in `FOUND.txt` of its campaign, and the home stops. Options of
pzsolve after `--` are refused when pzpeer sets them itself, `--part` among them (half a block would be claimed).

**The time.** Every 10 minutes a line: the blocks this home scanned, and the claims the network holds, estimated as those this node
keeps × nodes / 5 (a home that does not listen keeps none, and shows no estimate). A block has one claim, seldom more.

**Not done.** Spot checks, where a node scans a random slice of a claimed block and asks the owner for its near misses of that slice (a
near miss the owner cannot show would be a proof of fraud that everyone can check): they need a protocol of their own. Without them a
false claim costs the network the time to its second pass, which takes the blocks with the fewest proof keys first.

**When the key is found** nothing is sent. pzsolve and the home warn not to spend it through the public mempool: the transaction shows
the public key, and with a key that short anyone finds the private key in seconds and sends the coins elsewhere first. `pzpeer sweep`
makes and signs the transaction (`sweep.c`) and prints it; with `--private` it gives it to MARA's Slipstream, a pool that mines
transactions without showing them (free and open to all since August 2026), and to nothing else: a `POST` of `{"tx_hex": ...}` to
`https://slipstream.mara.com/api/transactions` through the `curl` of the system (Linux, macOS, and Windows since 10 have it), so that
pzpeer needs no TLS of its own. That interface is not documented by MARA: it is the one its page and the programs of others use, and
`--pool URL` gives another address. The coins are those of the key's address: with `--rpc`, what the node lists; without a node, the
coins the program names for the puzzle (`data/prizes.csv`, every open puzzle of the table) and what the servers list above their
checkpoint, all of them or none (8.2): for a key of an address the program does not know, or when a server hides a named coin, it makes
no transaction and says so.
