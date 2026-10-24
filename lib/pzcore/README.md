# pzcore

The cryptographic core of pzsolve as a small C library: 256-bit integers, the secp256k1 field and curve, SHA-256 / RIPEMD-160 / HASH160,
Base58Check, bech32 / bech32m and SegWit addresses, a keyed permutation of `[0, M)`, and HASH160 of many public keys at once with the SIMD
code of the CPU. It depends on the C library only (no pthread, no allocation, no I/O) and on GCC or Clang (it uses `unsigned __int128`, GNU
vector types and, on x86-64, inline assembly), so it builds with MinGW-w64 but not with MSVC.

**License:** part of pzsolve, so the same terms: `AGPL-3.0-or-later` (see `../../LICENSE`), with the same paid commercial license
(see the main README). Every file carries the SPDX line.

## Interface

One header per module in `include/pzcore/`; `pzcore/pzcore.h` includes them all. Every function is documented in its header.

| Header | What it gives |
|---|---|
| `defs.h` | fixed-width types, `pz_u128`, attribute macros, `extern "C"` brackets |
| `u256.h` | `pz_u256` (four 64-bit limbs): arithmetic, shifts, compare, hex / decimal text, big-endian bytes, arithmetic modulo the curve order |
| `fe.h` | field GF(p), p = 2^256 - 2^32 - 977: add, sub, mul, sqr (MULX/ADX assembly when built with `-mbmi2 -madx`), inversion, batch inversion, square root |
| `ec.h` | affine and Jacobian points, sums, `k*G` (comb table, built on first use), SEC1 public keys (33 and 65 bytes, strictly validated) |
| `hash.h` | `pz_sha256`, `pz_sha256d`, `pz_ripemd160`, `pz_hash160`, the compression functions, `pz_hash160_pub33` |
| `address.h` | Base58Check, bech32 and bech32m (BIP173, BIP350), SegWit addresses, P2PKH / P2WPKH helpers, WIF |
| `hash160_batch.h` | `pz_hash160_x_batch`: HASH160 of n compressed keys given as x coordinates and parity; run-time choice among avx512, avx2, neon2, neon, portable, scalar; query and override |
| `perm.h` | `pz_perm_init / apply / invert`: a seed-keyed bijection of `[0, M)`, `M <= 2^192`, expected O(1) per call, identical on every platform and version (pinned by `tests/perm_pins.h`) |
| `cpu.h` | `pz_cpu_has_avx2 / avx512 / ifma / sha2` |
| advanced: `fev.h`, `fev52.h`, `impl/` | lane-parallel field arithmetic (AVX2, AVX-512 IFMA) and the lane hash templates that the pzsolve engines include; not part of the stable interface |

## Rules

* **Errors** are return values (`bool`, or a length or `-1`). Nothing prints, exits or allocates. Where a function fails, its header says what
  happens to the output (mostly it is left untouched; after a failed `pz_fe_sqrt` or `pz_gej_to_ge_batch` its contents are unspecified). A function that writes text takes the buffer size and refuses rather than truncates.
* **Hostile input** (text, lengths, public keys) is accepted by every parser and refused cleanly: wrong characters, mixed case, bad
  checksums, over-long strings, coordinates not below p, points off the curve.
* **Threads:** everything is thread-safe. Global state is the curve table (built once, atomically, 61 KB static), the choice of batch
  implementation (an atomic pointer) and a cached CPU probe. No initialisation call is needed.
* **Not constant-time.** Branches and table lookups depend on the data: do not use it with secret scalars where a side channel matters.
  It has no signatures, no random numbers, no wiping of secrets.
* **Aliasing:** outputs may alias inputs unless the header says otherwise. The library is built with strict aliasing on.

## Build and use

```sh
make lib            # build/libpzcore.a
make lib-test       # unit tests (link only the archive)
make lib-examples   # lib/pzcore/examples/*.c, run
make lib-cxx        # the public headers compiled and linked as C++
cc -Ilib/pzcore/include prog.c build/libpzcore.a
```

To vendor it, copy `lib/pzcore` and compile `src/*.c` (`bytes.h` and `h160_impl.h` are private). Per-instruction-set files need their flags:
`h160_avx2.c` `-mavx2 -mbmi2 -madx`, `h160_avx512.c` `-mavx512f -mavx512vl -mavx512bw -mavx512dq -mbmi2 -madx`, `h160_neon.c`, `h160_neon2.c` and
`sha256_arm.c` `-march=armv8-a+crypto`; omit the ones for other architectures, or define `PZ_NO_X86_VARIANTS` / `PZ_NO_ARM_SHA2`. Compile your
own code with `-mbmi2 -madx` on x86-64 to get the assembly field multiplication from `fe.h`; the archive itself is built for the baseline.
Hot loops should include `fe.h`, `fev.h` and the templates and be compiled for their own instruction set, as the pzsolve engines do.

## Examples

`keyaddr.c` (private key to hash160, addresses and WIF), `roundtrip.c` (Base58Check, bech32, SegWit round trips and damaged text),
`permute.c` (visiting order of `0..m-1`, and a position in a domain of 2^100), `batch8.c` (HASH160 of consecutive keys with the SIMD code).

## Tests

`tests/` holds the unit tests and `gen_vectors.py`, which writes `vectors.h` with an independent Python implementation (integers, hashlib) and
checks the BIP173 / BIP350 vectors it embeds. Run clean under ASan, UBSan, TSan, Valgrind, Wine and QEMU (aarch64, with and without the SHA-256
instructions); `tools/verify.sh` runs them all.

## Performance

Speed on one x86-64 machine (AVX-512 IFMA, noisy VM; median of interleaved runs), version 1.8.1 -> pzcore; see `CHANGELOG.md` for the tables:
scalar RIPEMD-160 2.2x, SHA-256 1.3x, HASH160 of one key 1.8x, square root 2.2x. HASH160 of one key takes about 360 ns scalar; in
batches, about 160 ns per key with portable lanes, 90 ns with AVX2 and 40 ns with AVX-512. The search engines of pzsolve run at the same
speed as before.
