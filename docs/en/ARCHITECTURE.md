# Architecture

## 1. Modules (`src/`)

| File | Role |
|------|------|
| `common.h`, `util.c` | types, logging (terminal + `log.txt`), formatting, atomic files, memory with error checking, the readers of numbers (strict `pz_parse_u64` / `pz_parse_i64`, `pz_arg_int` for the command line) and of `key=value` lines (`pz_kv_find`) |
| `envopts.h/.c` | **the only place that reads the environment**: `pz_env_init`, called first thing in `main` and in the test programs, takes the `PZ_*` variables and the standard ones the program looks at into the structure `g_env`; the rest of the code reads its fields (section 5b) |
| `platform.h/.c` | **the only operating-system-dependent point** (POSIX and Windows): binary files with 64-bit offsets, atomic rename, campaign lock, large memory (mmap/THP, VirtualAlloc/large pages), clocks, entropy, console (ANSI sequences, width), Ctrl-C / SIGTERM / SIGHUP / window close, dynamic libraries. The rest of the program is C11 + pthread; a few `#ifdef _WIN32` remain only where the code is specific anyway (`cl_api.c`, `rt_cuda.c`, `sysinfo.c`, `doctor.c`, `util.c`, `kang_selftest.c`, `common.h`) |
| `../lib/pzcore/` | **the reusable crypto core** (own README, with the table of its headers): `u256`, `fe` (field), `ec` (curve), `hash`, `address` (Base58Check, bech32, WIF), `hash160_batch`, `perm` (the visiting order), `cpu`, the lane headers `fev.h` (9 limbs of 29 bits) / `fev52.h` (5 limbs of 52 bits, IFMA; `fev_chain.inc` holds their common parts) and the hash-lane templates `hash_lanes*.inc` in `impl/`, included once per ISA; built into `build/libpzcore.a` with nothing from `src/` |
| `engine_impl.inc` + `eng_base.c` / `eng_avx2.c` / `eng_avx512.c` | CPU engine of the linear search with scalar arithmetic, instantiated per ISA with different flags (`Makefile`) |
| `eng_shani.c` (+ `hash_lanes_shani.inc`, `h160_shani.c`) | the `avx2` engine with the SHA-256 stage on the x86 SHA instructions (`sha256rnds2`, two blocks in flight), RIPEMD-160 on the 8 AVX2 lanes, a transposition between the two; used only when the CPU has the instructions and the autotune measures it faster; built with `-msha -mavx2 -mssse3 -msse4.1`, tested under QEMU by `make test-shani` |
| `eng_neon.c` / `eng_neon2.c` | aarch64: the same engine with the SHA-256 stage on the ARMv8 SHA2 instructions and RIPEMD-160 on NEON vectors, 4 or 8 lanes (`neon2` advances two vectors together); built with `-march=armv8-a+crypto`, used only when the CPU reports SHA2, the autotune decides between them |
| `engine_vec_impl.inc` + `eng_avx2v.c` / `eng_avx512v.c` / `eng_avx512i.c` | CPU engine with curve arithmetic on SIMD lanes (N independent windows per round); `eng_avx512i.c` instantiates it with `fev52.h` |
| `cpu_engine.h`, `cpu_common.c` | engine interface (`scan_job`, `cpu_engine`); shared tables (`i·G`, `cpu_gmultiples` for tables of any size), engine registry, ISA detection, quick check of every engine (known key), autotune |
| `jobq.h/.c` | interval scheduler (work requests, completion, checkpoint, resume) |
| `campaign.h/.c` | immutable campaign configuration, seed generation |
| `puzzles.h/.c`, `puzzles_data.c` | database of the 160 puzzles (generated from `data/puzzles.csv`) plus the addresses of the emptied puzzles 161-256 (`data/puzzles_withdrawn.csv`) and its verification |
| `campaigns.h/.c` | campaign lock (via `platform.h`), `list` command |
| `eta.h/.c`, `ui.h/.c` | time / probability estimation models and the speed estimator (`rate_est`); rendering of the panel and the status line (pure formatting, no I/O) |
| `bench_tg.h/.c`, `profile.h/.c`, `doctor.h/.c` | joint CPU+GPU comparison (`bench --together` and the first-run tuning), machine profile and remembered steady-state speed, `doctor` command |
| `bsgs.h`, `bsgs_priv.h`, `bsgs.c`, `bsgs_isa.inc` (+ `bsgs_base.c`, `bsgs_x86.c`) | symmetric BSGS: hash table of the baby steps, giant steps in groups, save/load |
| `bsgs_vec_impl.inc` + `bsgs_avx2v.c` / `bsgs_avx512v.c` / `bsgs_avx512i.c` | the same windows of 1024 points, 4 or 8 per instruction (`fev.h`/`fev52.h`); verified against the scalar ones before use |
| `kangaroo.h`, `kangaroo_priv.h`, `kangaroo.c` | kangaroo: structures shared by the CPU and GPU workers, setup (jump table, `dp_bits`, size of the table: the first table is as large as the points expected need, within the memory limit), table of distinguished points (growth, thinning in place) and solving of a collision, herds of walkers and their restarts |
| `kang_log.c` | the file of distinguished points (`dp.bin`): creation (a header cut short by a crash is repaired), loading (a torn last record is cut off, a read error is an error, logs of an older or a newer layout are recognized), appending outside the table lock with recovery from write errors, merge of another computer's file |
| `kang_cpu.c` | CPU side of the kangaroo: the list of step variants, comparison of each with the portable one (`kang_round_check`), choice of the fastest, benchmark of the variants, worker thread |
| `kang_selftest.c` | self-tests of the kangaroo: step variants against the portable one, merge of two logs, collision of every pair of kinds of walkers, the limits of the interval, the table of distinguished points (growth, thinning, first size), the log (loading, full disk, writes outside the table lock) |
| `kang_isa.inc`, `kang_base.c`, `kang_x86.c` | scalar walker round, instantiated per ISA |
| `kang_vec_impl.inc` + `kang_avx2v.c` / `kang_avx512v.c` / `kang_avx512i.c` / `kang_avx512i2.c` | kangaroo round on SIMD lanes, bit-identical to the scalar one; walker state in vector form between rounds |
| `kang_spec3.inc` | table of the species and bands of kangaroo schema 3 (coefficient, residue mod 12, zone, share); generated by `tools/research/kspec_table.py` from `tools/research/kang_schema3.txt` |
| `run.h`, `run_common.c` | run context (the state of a run lives in it, so that two runs in one process do not share it), key found (`run_submit_key`; `run_submit_scan_hit` for the linear search), signals, scheduler loading, reporter (panel, report files) |
| `run_linear.c`, `run_kangaroo.c`, `run_bsgs.c` | orchestration of the three modes, kangaroo self-test and benchmark |
| `gpu.h`, `gpu_priv.h`, `gpu.c` | GPU: listing and choice of devices, start-up of the devices (one thread each, at the same time; the start-up tests are repeated up to three times when they stop on an error of the API), sessions, linear-search and kangaroo workers, recovery after a device error (the launches that follow a failure are a quarter of the size; the failures are forgotten after ten minutes of good launches), `doctor`. Written against `gpu_rt.h`, so identical for CUDA and OpenCL |
| `gpu_tune.c` | variants of the GPU kernels, choice of the fastest for each device by one procedure for both kernels (measured once, saved in `gpu_tune.txt`; two devices of one type measure once), size of the kangaroo herd, GPU benchmark |
| `gpu_selftest.c` | the GPU kernels against the CPU, bit by bit: field arithmetic, hash160, curve arithmetic, every variant of the linear (at every position where it branches) and of the kangaroo kernel; the rules that need no device (recovery, tuning with invented rates, the log of simultaneous start-ups) |
| `gpu_rt.h`, `rt.c`, `rt_priv.h` | minimal GPU runtime interface (context, modules, kernels, buffers, launch), the combined device list (an NVIDIA card seen by both runtimes is recognized by PCI address and used once) and `rt_logseq`, which keeps the log lines of devices that start at the same time in the order of the devices |
| `rt_cuda.c` | CUDA implementation: *driver API* loaded at run time (`nvcuda.dll` / `libcuda.so.1`), embedded PTX modules (compressed, unpacked when one is loaded), context with passive waiting |
| `rt_cl.c`, `cl_api.h/.c` | OpenCL implementation: dynamic loader, kernels compiled from embedded sources |
| `lz.h`, `lz.c` | decoder of the compressed PTX modules (a small LZ77 of our own with an optional dictionary, libc only, every access bounds-checked, checksummed) and `pz_blob_text`, which unpacks an embedded module; the encoder is in `tools/embed.c` (see below) |
| `kernels/*.cl`, `kernels/pz_cuda.h`, `kernels/pz_kang.h`, `kernels/ptx/*.ptx`, `kernels/ptx/modules.txt` | **a single kernel source**: OpenCL C, compiled by the OpenCL driver or, through `pz_cuda.h`, by NVRTC into PTX (`tools/gen_ptx.py`, committed files). `pz_kang.h` holds the numbers that the kangaroo kernel and the host share (preprocessor lines only: it is put in front of the kernel text wherever that is compiled). `modules.txt` is the one list of the PTX modules, read by the `Makefile` (through `tools/ptx_table.awk`), by `tools/gen_ptx.py`, by the CUDA runtime (`build/ptx_mods.c`) and by the simulated driver |
| `sysinfo.h/.c` | CPU/RAM/cache, thread affinity |
| `selftest.h`, `selftest.c`, `outfmt.h`, `main.c` | self-test, benchmark, command-line interface; `outfmt.h` is the look of the check lists and tables |

**Embedded kernels.** The kernels are compiled into the executable by `tools/embed.c`, which the `Makefile` builds and runs on the build
machine. The OpenCL sources (53 KB) become plain arrays (`build/kernels_gen.c`). The 19 PTX modules (12 MB of text in `src/kernels/ptx/`,
committed) are embedded compressed (`build/ptx_gen.c`, `embed -z`) in the format described at the top of `src/lz.c`; the encoder is in
`embed.c`, which checks each stream it writes with the real decoder, so the build stops if a stream does not unpack to its file. The
modules of a family differ in few lines (`linear128` and `linear256` in 148 of 38,000), so each module is stored against another one
(the `base` column of `src/kernels/ptx/modules.txt`): the text of the base is the dictionary of the stream, and `pz_blob_text` unpacks the base first. Together
the modules take about 1 MB instead of 12 MB, and the stripped Linux executable is under 3 MB instead of 14 MB. `rtcu_build` unpacks the
module it is about to load (a few milliseconds, once per module load; no hot path touches this), hands the NUL-terminated text to the
driver and frees it. `tools/gen_ptx.py` and the `.ptx` files are unchanged. The simulated CUDA driver embeds the same files plain
(`embed` without `-z`) and compares the text it is given with them, so it checks the unpacked bytes against the files themselves;
`tests/t_kernels.c` does the same for every module without a driver.

The x86 SIMD engines are optional. When the `Makefile` builds for a target other than x86-64 it defines `PZ_NO_X86_VARIANTS`; the engine
registry (`cpu_common.c`), BSGS (`bsgs.c`) and the kangaroo (`kang_cpu.c`) then use only the scalar code.

## 2. Flow of a linear campaign

```
main ─ cmd_hunt ─ creates/loads campaign.conf ─ run_linear
                        │ sched_new (perm + intervals) ─ sched_load (state.dat / .bak)
                        ├─ CPU thread i:  loop { sched_claim → engine.scan → sched_complete }
                        ├─ GPU thread j:  loop { sched_claim → kernel pz_search → hit → sched_complete }
                        └─ main thread: reporter (1 Hz) → terminal, report.txt, status.json, stats.csv,
                                        checkpoint every 60 s
Every hit (CPU or GPU) → run_submit_scan_hit: ignored if the key lies beyond the end of the range (the engines scan whole lanes, so the last
block can reach a few keys past it) → run_submit_key: recomputes k·G, compares hash160/pubkey → FOUND.txt → global stop
```

The scheduler hands out contiguous portions of a block (≈ 3 s of work for the CPUs, ≈ 0.35 s per launch on the GPUs, adjusted by the
measured speed). An interrupted worker returns the completed prefix; the rest remains a "hole" in the checkpoint and is taken up first in
the next session.

## 2b. Flow of BSGS

```
run_bsgs ─ bsgs_params_init (c, T, S, A, Ng) ─ run_load_sched (giants 1..Ng, same scheduler as the linear search)
         ├─ "driver" thread: table (bsgs_table_load from --table, or bsgs_table_build with N threads and save)
         │                   → N search threads: loop { sched_claim → bsgs_scan → sched_complete }
         └─ main thread: reporter (phase 1/2 during the table, then giants)
bsgs_scan: 1, 4 or 8 groups of 1024 giants (batch inversion, on SIMD lanes if available) → x coordinates → prefetch of the bucket → table probe (a bucket is one cache line, 1.4 per lookup on average; its 8 tags are compared at once with SSE2) → candidate → check k·G = P → run_submit_key
```

## 3. Flow of the kangaroo

```
run_kangaroo ─ (GPU: opens the devices, NK tuning, self-test) ─ kang_create (interval, base, Q', zones)
             ─ kang_configure (K, dp_bits, table, jumps from seed) ─ kang_load_log (dp.bin)
             ─ CPU thread: kang_cpu_worker_main { round (fastest variant among scalar and lanes, verified) → DP → table }
             ─ GPU thread: gpu_kang_thread { kernel pz_kang_step (NK walkers per thread, up to 64 rounds per launch, jump history in
                                              a buffer of 16 bits per step; d and the DPs are rebuilt by replaying it) → DP/restarts → table }
             ─ reporter: probability of success, DPs in the table, restarts
kang_insert_dps (under the table mutex): inserts or compares with the existing entry → solve_pair → run_submit_key; a full block of the log is written after the mutex is released
```

## 4. On-disk formats

* `campaign.conf`: text `key=value` (`format=1`, `created_with`, range, target, `mode`, `seed`, `chunk_bits`, `dp_bits`, `bsgs_entries`, `part`); `format` changes only with incompatible changes.
* `state.dat`: text; header `PZSCHED 1`, `cbits`, `seed_id` (the seed id of the campaign: a state of another seed is refused; files written
  before it was recorded have no such line and are accepted), `v_limit`, `keys_done`, lines `iv start end` (hex), final line
  `crc=` (first 8 bytes of the SHA-256 of the content). Atomic write (a temporary file of its own next to it → fsync → rename); the previous file, if valid,
  becomes `state.dat.bak`.
* `dp.bin`: `"PZDP5\n"` + SHA-256 fingerprint (start, W, `"PZDPLOG4"`, pubkey) + the first 4 bytes of the SHA-256 of the jump table (the
  jump table id): a header of 42 bytes; then 32-byte records `{tagk, d[3]}` little-endian (`tagk` = 61-bit abscissa ∥ 3-bit type: 0 tame,
  1/2 wild on `Q` with sign, 3/4 wild on `3Q` with sign; `d` = 192-bit signed exponent). `PZDP4` (versions 1.9.1 to 1.11.0) is the same
  without the jump table id (a header of 38 bytes): it is loaded, appended to and merged as it is. A file whose jump table id differs from the
  campaign's is refused by `hunt` and `merge`. The magics `PZDP1` to `PZDP3` are older layouts (versions up to 1.9.0, campaigns with fewer than
  256 kangaroos): `hunt` keeps such a file as `dp.bin.vN` and starts a new log. Any other `PZDP` magic is a later version's: `hunt` and `merge`
  say so and leave the file as it is. A file shorter than the header that is the beginning of this campaign's header (the program stopped while
  creating it) holds no point and is replaced.
* BSGS table (`--table`): 4 KiB header (magic, SHA-256, `T`, number of cells, abscissa of `T·G`, checksum) + little-endian 8-byte cells
  (24-bit tag from the second word of the abscissa ∥ 40-bit baby index, 0 = empty). `PZBSGS02` (written now): buckets of 8 cells, one cache
  line, 70 % of the cells in use, the bucket of an abscissa is `⌊x.n[0]·buckets/2^64⌋`, a full bucket spills into the next one, and the
  cells of a bucket are taken in order (a lookup stops at the first bucket whose last cell is free; `load` checks that property).
  `PZBSGS01` (up to 1.10.0): one array, linear probing, load 0.65; it is still loaded and used as it is (`bsgs_table.linear`, 12.3 bytes
  per baby step) and written back in that format, because it cannot be turned into the new one without computing the baby steps again.
* `session.txt`, `stats.csv`, `status.json`, `report.txt`: see [`USAGE.md`](USAGE.md).

## 5. Concurrency and memory

* One mutex for the scheduler and, in the kangaroo, two more: `mu` guards the table of distinguished points and `wmu` the file `dp.bin`. Accesses are
  rare compared with the work (blocks of seconds, a DP every `2^dp_bits` steps). The log of the points is written in blocks: a full buffer is
  exchanged for the empty second one under `mu` (a pointer swap), and the thread that did it writes the block after it let go of `mu`
  (`kang_table_unlock`), so a slow disk does not hold up the other threads; the locks are taken in the order `mu`, `wmu`, never the other way.
* The progress line does not take `mu`. It reads relaxed atomic copies of the number of points, the size of the table and the thinning level
  (`count_mirror`, `cap_mirror`, `thin_mirror`, written under `mu`): the rebuild of a table of several GiB takes longer than the 15 s after which the
  reporter takes a gap in its own loop for a suspension of the system.
* When the table has reached its RAM limit it is thinned in place (one sweep over the array, no second table; the density of the stored points is
  halved), and it grows, with the old and the new table side by side for a while, only if both fit in the limit.
* The devices start at the same time, one thread each; the saved tuning (`gpu_tune.txt`) is updated in one step under a lock, and a device whose
  type is being measured by another waits for the result. Progress counters are atomic *relaxed*; the stop flag is atomic.
* Each thread allocates its own buffers (no false sharing); threads are pinned to cores with `sched_setaffinity` (Windows: "ideal" processor, a hint to the scheduler).
* The only shared hot writes are the counters; ThreadSanitizer reports nothing on CPU, GPU and mixed campaigns.
* Memory: linear search ≈ a few MiB; kangaroo: `32 bytes × entries` (limit `--mem`; the table never exceeds it, also while it is being rebuilt) plus ≈ 0.4 KiB per walker.

## 5b. The environment

No file but `src/envopts.c` calls `getenv` or reads the environment list (`doctor` shows the `PZ_*` entries through `pz_env_each_pz`), and `lib/pzcore/src` never does. `pz_env_init` runs first thing in `main` and in the test programs; it reads
every variable once into `struct pz_env` (`g_env`, declared in `src/envopts.h` with the meaning of each field), and from then on the code reads the
fields. A program that does not call it sees the values of a clean environment. The rules are the same for all the variables: unset and empty are the
same thing; a flag is on with any value except `""` and `"0"`; a value that cannot be used (a number that is not a whole decimal number in its range, a
name that is not one of the allowed ones, a folder name that does not fit) is reported once, as a warning `NAME=TEXT ignored (...)`, and the default
stays. The variables of the users are described in [`USAGE.md`](USAGE.md) (section 7.10); those for tests and diagnostics only here:

| Variable | Field of `g_env` | Used in |
|----------|------------------|---------|
| `PZ_NO_CUDA`, `PZ_NO_OPENCL` | `no_cuda`, `no_opencl` | `rt.c`: which runtimes are loaded |
| `PZ_GPU_API`, `PZ_OPENCL_ALL` | `gpu_api`, `opencl_all` | `gpu.c`: choice of the devices (`--gpu-api` comes first) |
| `PZ_GPU_HALF`, `PZ_GPU_NK`, `PZ_GPU_TUNE` | `gpu_half`, `gpu_nk` (+ `gpu_nk_cfg`, `gpu_nk_l2`, `gpu_nk_text`), `gpu_tune` | `gpu_tune.c`: forced variants, tuning on or off |
| `PZ_NO_PROFILE` | `no_profile` | `main.c`, `profile.c` |
| `PZ_NO_LARGE_PAGES` | `no_large_pages` | `platform.c`: `pz_map_zeroed` on Windows |
| `PZ_BENCH_SCALE` | `bench_scale` | `bench_tg.c`: duration of the measurements |
| `XDG_CACHE_HOME`, `HOME`, `LOCALAPPDATA` | `xdg_cache_home`, `home`, `local_app_data` | `util.c`: `pz_cache_file` |
| `TMPDIR` | `tmpdir` | `platform.c`: `pz_make_temp_dir` (POSIX) |
| `TERM`, `NO_COLOR` | `term_dumb`, `no_color` | `platform.c` (`pz_stderr_is_terminal`), `run_common.c` (the panel) |
| `PZ_KANG_ROUND`, `PZ_KANG_VERBOSE`, `PZ_KANG_MIX` | `kang_round`, `kang_verbose`, `kang_mix` (-1 when unset: measured) | `kang_cpu.c`: choice of the CPU step of the kangaroo; `PZ_KANG_ROUND=base\|x86\|avx2v\|avx512v\|avx512i\|avx512i2` imposes one when the CPU can run it (`avx512i2`: IFMA on pairs of vectors), `PZ_KANG_VERBOSE=1` prints the speed of every variant measured |
| `PZ_BSGS_SCALAR` | `bsgs_scalar` | `bsgs.c`: scalar windows only, no lane-based ones |
| `PZ_ASSUME_IFMA` | `assume_ifma` | `cpu_common.c`: `cpu_has_ifma` is true when the CPU has AVX-512 (for the emulator of `tools/ifma_emul.h` and hosts whose virtual CPUID hides IFMA) |
| `PZ_TEST_KTHREAD`, `PZ_TEST_JUMP`, `PZ_TEST_DP_CAP` | `test_kthread`, `test_jump`, `test_dp_cap` | `kangaroo.c` (`kang_create`, `kang_configure`): walkers per CPU thread (16 to 1048576); the mean jump in units of √W (1 or more: long walks on a small range); the first size of the table of distinguished points (at least 64: a tiny table exercises its growth and thinning) |
| `PZ_EXPECT_SHANI` | `expect_shani` | `tests/t_engine.c`: the tests fail if the SHA-extension code cannot run (`make test-shani`) |

None of them is read in a loop that runs per key or per step: they are read at start-up, when a device or a campaign is set up, or in a one-time choice.
The tests that need another value set the field and put it back (`xdg_cache_home` in the self-tests, `test_dp_cap` for the first size of a table);
`tests/t_env.c` tests the reading itself by setting the variables and calling `pz_env_init` again. Two things are outside: the simulated CUDA driver
(`tests/mock_cuda/mock_cuda.c`, section 6) is a library of its own and reads its four variables in one function, `mock_env`, at its first
`cuInit`: `PZ_MOCK_DEVICES=n` (n devices, 1 to 8), `PZ_MOCK_SAME_NAME=1` (all with one name, like the cards of one model), `PZ_MOCK_COUNT=1`
(at exit it reports how many modules were loaded and kernels launched), `PZ_MOCK_FAIL_FILE=path` (it fails one launch while that file
exists, and removes the file: a device reset after sleep); and the tests of the library (`lib/pzcore/tests/t_batch.c`, `t_fev52.c`) read
`PZ_EXPECT_SHANI` and `PZ_ASSUME_IFMA` in `main`, because they link nothing of `src/`.

## 6. Tests and verification

| Level | Where |
|-------|-------|
| Vectors from an independent reference (`tools/ref.py`, pure Python) | `tests/t_core.c`, `src/selftest.c` |
| Field/assembly | `lib/pzcore/tests/t_fe.c` (two builds: C and MULX/ADX); `lib/pzcore/tests/t_fev.c` (AVX2 and AVX-512: limb bounds, extreme values, verified mutations) |
| BSGS, ETA, database | `tests/t_bsgs.c` (both table formats, buckets, the file of an earlier version), `t_eta.c`, `t_puzzles.c` |
| Platform layer, environment | `tests/t_core.c`: files written by several processes at once, `pz_kv_find`, `pz_file_count`, the log and `pz_die` from several threads; `tests/t_env.c`: the reading of every variable into `g_env` (the flag rule, numbers, texts, values that are refused and reported once) |
| Robustness on damaged input | `tests/t_fuzz.c`: corrupted and hostile files and strings (campaign files, checkpoint, BSGS tables, addresses) from a fixed-seed generator: nothing may crash, hang or read out of bounds (run under ASan/UBSan by `tools/verify.sh`); the state that `list` and `info` show |
| GPU kernel arithmetic | `tests/t_gpufe.c`: the field of `kernels/pz_common.cl` compiled for the host, portable variant and PTX carry-chain variant (with the carry flag emulated according to the PTX ISA), against the CPU on 5 million combinations with all the edge cases |
| Permutation, scheduler, SIMD engines | `lib/pzcore/tests/t_perm.c`, `tests/t_jobq.c`, `t_engine.c` |
| Complete campaigns | `tests/e2e.sh` |
| Compressed kernels | `tests/t_lz.c`: the decoder and the encoder (round trips of the real PTX modules and of random and adversarial texts, streams built by hand, truncated, extended and corrupted streams must be refused and never touch memory outside the buffers; run under ASan/UBSan by `tools/verify.sh`); `tests/t_kernels.c`: every module embedded in the program unpacks to exactly the bytes of its `.ptx` file |
| CUDA path without a GPU | `tests/mock_cuda/`: a **simulated CUDA driver** (`libcuda.so.1` / `nvcuda.dll`) with the same APIs, which recognizes the PTX modules (by comparing the text it is given with the files, byte for byte) and runs the same kernel sources on the CPU, checking every call (pointers, copies outside the buffers, launch shape, arguments). It can show several devices (`PZ_MOCK_DEVICES`, `PZ_MOCK_SAME_NAME`), serves several threads, counts the modules loaded and the launches (`PZ_MOCK_COUNT`) and fails one launch on request (`PZ_MOCK_FAIL_FILE`). It verifies all the CUDA host code; the PTX-to-GPU compilation is left to the real driver and is covered by the self-test at startup |
| Windows | `make windows` / `make wintest`: static MinGW-w64 exe, unit tests, self-test and campaigns run under Wine (`tools/verify.sh`) |
| Sanitizer / Valgrind | `make asan`, `make tsan`, `valgrind` (see `docs/en/RELEASING.md`, section "Before every release") |
| Kangaroo parameters | `tools/research/ksim.c` (abstract model) and `pzsolve kstat` (real curve); the results are in `docs/en/ALGORITHMS.md` and `docs/en/RESEARCH.md` |

## 7. Roadmap

* OpenCL kernels with 64-bit limbs for AMD (they need hardware to be tuned). Done: CUDA kernel in PTX with carry chains, lane arithmetic with AVX-512 IFMA, BSGS and kangaroo on SIMD lanes, the SHA extensions for SHA-256 (x86 `shani`, aarch64 `neon`).
* Multi-machine distribution with a coordinator. Today: linear search split into fixed shares (`--part`, `split`), kangaroo with `--node` and `merge`; only dynamic balancing between machines is missing.
* Support for uncompressed addresses.
