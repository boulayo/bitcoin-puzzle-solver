<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
<!-- SPDX-FileCopyrightText: 2026 Leonardo Boulay -->
# signer/vendor

## cb-mpc

[cb-mpc](https://github.com/coinbase/cb-mpc), Coinbase's library of multi-party computation for keys of digital assets, MIT
([cb-mpc/LICENSE.md](cb-mpc/LICENSE.md)), at commit `0b71670` (2026-10-05). Coinbase publishes a specification and a theory paper for
each protocol, and the library had two audits by Cure53 in 2024 (in its repository, `docs/`). pzsign uses its two-party ECDSA
(`ecdsa2pc`).

The copy holds only what pzsign builds: `include/`, `include-internal/`, the sources of `src/cbmpc/{core,crypto,zk,protocol}` (the
public C++ and C interfaces, the tests, the benchmarks and the documents are left out), and the files of libsecp256k1 that
`base_ecc_secp256k1.cpp` includes, from Coinbase's own copy of it (`vendors/secp256k1`, MIT, [COPYING](cb-mpc/vendors/secp256k1/COPYING)).
The `CMakeLists.txt` files are left out too: the Makefile builds the sources (`src/peer/peer.mk`).

Upstream supports Linux and macOS only. Two changes make it build and run on Windows with MinGW-w64; each is marked `pzsign patch`:

| File | Change |
|---|---|
| `include/cbmpc/core/precompiled.h` | on Windows, the Windows headers in place of the Unix ones (sockets, `mmap`, `termios` and the like, which the code that pzsign builds does not use) |
| `src/cbmpc/crypto/base_bn256.cpp` | `support_x64_mulx()` is false on Windows: the four `mulx` routines in assembly that it switches on (`mul256` and `barrett_reduce256` here, the field product and square of `ec25519_core.cpp`) take their arguments in the registers of the System V calling convention, not those of Windows, and would compute garbage there (seen: three tests of cb-mpc fail, then a crash) |

With these, all 1454 unit tests of cb-mpc pass under Wine as on Linux (1453 passed, 1 skipped on both: a file of test vectors that the
repository keeps outside git).

OpenSSL is not in the copy: [../openssl.sh](../openssl.sh) fetches the release that cb-mpc names (3.6.4), checks its hash and builds it
with the one change cb-mpc asks for.
