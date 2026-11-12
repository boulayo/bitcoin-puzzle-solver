<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Leonardo Boulay -->
# pzsign

The two-party ECDSA signer of the shared kangaroo network ([NETWORK.md](../docs/en/NETWORK.md), section 7.6). When two homes find a
coincidence, each holds an additive piece of the puzzle key, `k = x0 + x1`, and neither may learn the other's. pzpeer starts one
`pzsign session` on each side; the two run the two-party ECDSA of [cb-mpc](vendor/README.md) (Coinbase's library, after Lindell 2017)
through pzpeer, and both get the same standard ECDSA signatures of the transaction digests, with `k` never assembled anywhere.

```
make signer          # build/pzsign: OpenSSL is fetched and built the first time (signer/openssl.sh), then cb-mpc and pzsign
make signer-test     # the selftest and signer/test.py
make winsigner       # build/win/pzsign.exe, with the MinGW-w64 compilers
```

It needs a C++17 compiler, `curl` and `perl` (for OpenSSL). One file, [pzsign.cpp](pzsign.cpp).

## Commands

| Command | What it does |
|---|---|
| `pzsign session` | one signing session, framed on stdin and stdout |
| `pzsign selftest` | both parties in one process, once with each of them first: a random key split in two, three digests, the same verified signatures on both sides |
| `pzsign version` | version and license |

## The session

Frames on stdin and stdout: a 4-byte big-endian length L (1 to 1 MiB), then L bytes, a type byte and its payload. stdout carries only
frames (pzsign keeps a copy of it for them and points stdout itself at stderr, so nothing a library prints can break one); logs go to
stderr.

| Type | Direction | Payload |
|---|---|---|
| 0x01 | pzpeer → pzsign | the job, first frame and only once |
| 0x02 | pzpeer → pzsign | a message of the other party |
| 0x81 | pzsign → pzpeer | a message for the other party |
| 0x82 | pzsign → pzpeer | the signatures: `r` and `s` (32 bytes each, big-endian, `s` low) of each digest, in order; last frame |
| 0x83 | pzsign → pzpeer | the reason it failed, text; last frame |

The job, binary: version (2), `i` (this party, 0 or 1), `first` (the party that gets the signatures first), the session id (32 bytes),
the own piece (32), the public pieces of party 0 and party 1 and the key (33 each, compressed), then 1 to 64 digests of 32 bytes.
Then:

1. **Checks.** The piece is a scalar in `[1, n-1]`, `piece·G = pub[i]` and `pub[0] + pub[1] = key`.
2. **Import of the pieces**: the key generation of the library (`ecdsa2pc::dkg`) with the party's own piece in place of a random one.
   Each party commits to its public piece and proves it knows the piece; the party `first` (P1 in the library) makes a fresh Paillier
   key, encrypts its piece under it and proves the key and the encryption well formed. The pieces must give `key`. The names of the
   two parties carry the session id, so that no proof of one session counts in another.
3. **Signing**, all the digests in one run (`ecdsa2pc::sign_batch`). The digest is the legacy signature hash: it is signed as the number
   it is mod n (ECDSA uses nothing else of it), and the library takes it reduced. P1 gets the signatures, already with a low `s` and
   verified by the library.
4. **The signatures to P2**: P1 sends them, P2 takes them as its own. Each side verifies every signature against `key` before it
   reports it, and refuses a high `s`.

P1 has the signatures before P2, and P2 has them only if P1 sends them: pzpeer makes S, the party that broadcasts the second
transaction, the party `first` (NETWORK.md 7.5, step 6). The library's signing makes P2 prove its last message well formed, so a P2
that cheats gets nothing and learns nothing; there is no presignature and no state kept between sessions.

Anything malformed (a frame, the job, a message, a frame of an unexpected type, the end of stdin) ends the session with 0x83, and so
does a message of the partner larger than its phase allows (64 KiB in the import of the pieces, 16 KiB and 4.5 KiB a digest in the
signing, 64 bytes a digest for the signatures), before the library reads it. pzsign sends the partner nothing when it fails: pzpeer ends
the partner's input instead (S_SIGNED with no signatures). pzsign has no timeout: pzpeer stops it when the session is over. A session
makes no core file and, on Linux, no other process of the user can attach to it or read its memory.

## The envelope

The payload of 0x81 and 0x02 is opaque to pzpeer: three bytes, then the library's message.

| Byte | Meaning |
|---|---|
| version | 2 |
| from | the sender, 0 or 1 |
| phase | 1 import of the pieces, 2 signing, 3 the signatures from P1 |

The receiver checks the three against what it expects: the two parties run the same steps in the same order over an ordered channel,
so any other message is malformed. The library requires its messages to be authenticated and kept secret on the way; the Noise channel
between the homes gives both.

Sizes, for two digests: P1 sends six messages (1.7 KB, 36 KB, 20 B, 36 B, 3.2 KB and the signatures), P2 four (2.4 KB, 20 B, 3.2 KB,
8 KB). The import of the pieces and the signing take well under a second together on one core of a laptop, the Paillier key included.

## Tests

`make signer-test` runs `pzsign selftest` and [test.py](test.py): two `pzsign session` processes with the test as their pzpeer, which
relays the messages of each to the other and changes, drops or replays them. It checks, with the curve of `tools/ref.py` (no code shared
with pzsign), that both sides return the same signatures, valid for `key` and with a low `s`, with either party first; that a message
changed in its middle or its header, or dropped, leaves no signature anywhere but on P1, and on P1 only when it was the last one, the
signatures themselves; that a partner with another digest, in another session, or replaying the messages of an earlier session, gets
nothing; and that a piece that is not its public piece, pieces that do not add up to the key and malformed jobs are refused. The
pzpeer tests `t_pieces` and `t_session` run the real pzsign too, when `make signer` has built it.

## Licenses

pzsign is AGPL-3.0-or-later. It is linked statically with cb-mpc (MIT), with the copy of libsecp256k1 that cb-mpc carries (MIT) and
with OpenSSL 3 (Apache-2.0); all are compatible with the AGPL. [vendor/README.md](vendor/README.md) lists what the copy of cb-mpc holds
and what was changed in it.
