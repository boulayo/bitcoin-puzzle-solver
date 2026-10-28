# Tools

| File | What it does |
|------|--------------|
| `ref.py` | **Independent** reference in pure Python (secp256k1, hash160, addresses, WIF): generates the test vectors and the keys for the end-to-end tests. `python3 tools/ref.py 0x1a838b13505b26867` |
| `embed.c` | Embeds the OpenCL kernels (`src/kernels/*.cl`) and the PTX modules (`src/kernels/ptx/*.ptx`) in the executable at build time |
| `gen_ptx.py` | Regenerates the CUDA modules (PTX) from the OpenCL kernels with NVRTC (`pip download nvidia-cuda-nvrtc-cu12==12.0.140`): `python3 -I tools/gen_ptx.py --nvrtc .../libnvrtc.so.12 [--ptxas .../ptxas] [--check]`. With `--ptxas` it prints registers and spills for sm_86. The NVRTC version fixes the PTX version and therefore the minimum driver (12.0 means driver 525) |
| `ifma_emul.h` | Emulates `vpmadd52luq/huq` with AVX-512F to test the IFMA paths on CPUs without IFMA: `make BUILD=build/emu OPT="-O2 -include tools/ifma_emul.h" build/emu/pzsolve && PZ_ASSUME_IFMA=1 build/emu/pzsolve selftest`. Correctness only, not speed; `verify.sh` does this by itself |
| `gen_puzzles.py` | Verifies `data/puzzles.csv` (with `ref.py`) and `data/puzzles_withdrawn.csv` (the withdrawn puzzles 161-256) and generates `src/puzzles_data.c`; `--check` verifies that it is up to date |
| `verify.sh` | Full battery: database, tests, glibc compatibility, the CUDA path with the simulated driver, end-to-end, ASan/UBSan, TSan, Valgrind, the Windows exe under Wine (if MinGW-w64 and Wine are installed). `PZ_NVRTC=...` adds the check that the PTX modules are up to date, `PZ_SDE=/path/to/sde64` tests the IFMA engine under Intel SDE in Tiger Lake mode |
| `pty_run.py` | Runs a command in a pseudo-terminal for a few seconds and prints what it drew (used to test the multi-line panel in `verify.sh`) |
| `release.sh` | Versioned release: checks, `VERSION` bump, `verify.sh`, annotated tag; prints the push command (see `docs/en/RELEASING.md`) |
| `package_release.sh` | Builds the Linux x86-64 and Windows x86-64 packages on this machine into `dist-release/` (archives and `SHA256SUMS`), with the same checks as `verify.sh` (glibc ceiling, only system DLLs); `--pack BINARY OS ARCH` packages an existing binary (used by the release workflow on every runner). Needs MinGW-w64 and `zip`; Wine optional |

The simulators, the layout files of the kangaroo and the scripts behind the measurements in `docs/en/ALGORITHMS.md` are in [`research/`](research/README.md): they are not needed to build or use the program.
