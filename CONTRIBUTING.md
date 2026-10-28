# Contributing to pzsolve

Bug reports, benchmark results from your hardware, documentation fixes and patches are welcome. For anything larger than a small fix,
open an issue first so we can agree on the approach.

* `pzsolve selftest` and `make test` must pass, and new behavior needs a test.
* `tools/verify.sh` runs the full battery: AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer, Valgrind, a re-check of the puzzle
  database, and the Windows exe under Wine. The GPU kernels are tested bit by bit against the CPU on pocl (OpenCL on the CPU) and, for
  CUDA, with a simulated driver (`tests/mock_cuda`).
* One topic per pull request, in the style of the surrounding code (C11, no external dependencies).
* Do not copy code you cannot license under the terms below, nor code from other puzzle tools unless its license allows it.

## License of contributions

pzsolve is released under `AGPL-3.0-or-later` and is also offered under a paid commercial license (see the README). So that both
are possible, by opening a pull request or sending a patch you confirm that you wrote the contribution, or have the right to submit
it, and you agree that:

1. it is licensed under `AGPL-3.0-or-later`, like the rest of the project; and
2. the maintainer may also license it under other terms, including proprietary ones.
