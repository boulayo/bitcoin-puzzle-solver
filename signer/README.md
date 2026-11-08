<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Leonardo Boulay -->
# pzfirma

The two-party ECDSA signer of the shared kangaroo network ([NETWORK.md](../docs/en/NETWORK.md), section 7.6). When two homes find a
coincidence, each holds an additive piece of the puzzle key, `k = x0 + x1`, and neither may learn the other's. pzpeer starts one
`pzfirma session` on each side; the two run the CGGMP21 protocol (the `cggmp21` crate) through pzpeer and both get the same
standard ECDSA signatures of the transaction digests, with `k` never assembled anywhere.

```
make signer          # build/pzfirma (cargo build --release --locked; GMP is built from source the first time)
make signer-test     # the tests below
```

## Commands

| Command | What it does |
|---|---|
| `pzfirma session` | one signing session, framed on stdin and stdout |
| `pzfirma primes FILE` | pregenerates the two safe primes (1536 bits) of a session into FILE, mode 0600, through `FILE.tmp` |
| `pzfirma selftest [P0 P1]` | both parties in one process: a random key split in two, the auxiliary data, three digests, the same verified signatures on both sides; with primes from P0 and P1, or fresh ones |
| `pzfirma version` | version and license |

The primes are the party's Paillier secret key. A file is for one session: pzfirma only reads it, pzpeer removes it once used.

## The session

Frames on stdin and stdout: a 4-byte big-endian length L (1 to 1 MiB), then L bytes, a type byte and its payload. stdout carries only
frames; logs go to stderr.

| Type | Direction | Payload |
|---|---|---|
| 0x01 | pzpeer → pzfirma | the job, JSON, first frame and only once |
| 0x02 | pzpeer → pzfirma | a message of the other party |
| 0x81 | pzfirma → pzpeer | a message for the other party |
| 0x82 | pzfirma → pzpeer | `{"sigs": [{"r": hex32, "s": hex32}, ...]}`, last frame |
| 0x83 | pzfirma → pzpeer | `{"error": "...", "blame": [i...]}`, last frame |

Job: `{"op": "sign", "i": 0|1, "eid": hex32, "share": hex32, "pub": [hex33, hex33], "key": hex33, "primes": path, "digests": [hex32, ...]}`
(unknown fields are refused, 1 to 64 digests). Then:

1. **Key share.** pzfirma checks that `share·G = pub[i]` and `pub[0] + pub[1] = key`, and builds the library's key share from the pieces:
   `DirtyCoreKeyShare { i, key_info: { shared_public_key: key, public_shares: pub, vss_setup: None }, x: share }`. Without a VSS setup
   the library validates it as an additive 2-of-2 sharing. There is no dealer and no key generation.
2. **Auxiliary data**, `aux_info_gen` with the primes of the file and the execution id `SHA256("pznet1/aux" || eid)`. It gives the
   Paillier keys and ring-Pedersen parameters of both parties and completes the key share.
3. **One signing per digest**, in order, with the execution id `SHA256("pznet1/eid" || eid || u32_be(index))`. The digest is the
   legacy signature hash: it is signed as the number it is (reduced mod n, as ECDSA does), not hashed again. `s` is made low and
   each signature is verified against `key` before it is reported. No presignatures.

Any malformed input (a frame, the job, a message, a frame of an unexpected type, the end of stdin) ends the session with 0x83.
Before it, pzfirma sends the partner an abort message (0x81), so that the other side stops at once. `blame` lists the parties that
the library names as culprits, or the partner when its message does not decode; it is empty otherwise. pzfirma has no timeout:
pzpeer kills it when the session is over.

The last message of each run reaches one side before the other: if it is lost or damaged, one side may report its signatures (0x82)
while the other reports an error. That is the asymmetry that the project's cancel lever is for (NETWORK.md 7.5, step 6).

## The envelope

The payload of 0x81 and 0x02 is opaque to pzpeer. It is a header and the library's message, both in postcard (serde, varints, no
field names):

| Field | Encoding | Meaning |
|---|---|---|
| version | byte | 1 |
| from | varint | the sender, 0 or 1 |
| phase | varint 0, or 1 and a varint index, or 2 | auxiliary data, signing of the digest `index`, abort |
| broadcast | byte 0 or 1 | how the library sent it |
| message | postcard | the library's message for that phase; for an abort, the reason as a string |

The receiver checks each field against what it expects (version, sender, the phase it is running) and refuses trailing bytes: the
two parties run the same runs in the same order over an ordered channel, so any other message is malformed. The library requires
its messages to be authenticated and the point-to-point ones encrypted; the Noise channel between the homes gives both.

Sizes: the second and third rounds of the auxiliary data are about 200 KB each (the library writes its big numbers as hex strings);
a signing is about 45 KB per party, the largest message 31 KB. pzpeer must split them across its Noise frames of at most 64 KB.

## Tests

`cargo test --release` (or `make signer-test`) runs `tests/session.rs`: two `pzfirma session` processes with the test as their
pzpeer, which relays the 0x81 frames of each to the other as 0x02. It checks that both sides return the same signatures, valid for
`key` (verified there with curve operations only) and with a low `s`; that a damaged envelope, a corrupted proof and a corrupted
broadcast make both sides abort, with the blame on the sender where the library names it; that a share that does not match its
piece, or pieces that do not add up to the key, are refused; and that malformed jobs are refused. The first run makes the two primes
files the tests share, in `target/tmp`.

On a 4-core machine with both parties on it: a primes file in 4 to 40 s (it is luck), the auxiliary data in about 6 s, a signature
in 0.7 s.

## Licenses

pzfirma is AGPL-3.0-or-later. Its main dependencies: `cggmp21`, `key-share`, `paillier-zk`, `fast-paillier`, `generic-ec`,
`round-based` (the protocol, MIT OR Apache-2.0); `rug` and `gmp-mpfr-sys` (LGPL-3.0-or-later), which bring GMP (LGPL-3.0-or-later or
GPL-2.0-or-later), linked statically; `postcard`, `serde`, `serde_json`, `sha2`, `hex`, `zeroize`, `rand_core` (MIT OR Apache-2.0).
The rest of the tree is MIT, Apache-2.0, BSD-3-Clause or similar permissive licenses. All are compatible with the AGPL; a binary
distributed under other terms must still let its user relink GMP, as the LGPL asks.
