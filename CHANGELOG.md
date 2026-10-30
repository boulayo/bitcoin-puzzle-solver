# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows [Semantic Versioning](https://semver.org/).
Each release is tagged `vX.Y.Z` (see [`docs/en/RELEASING.md`](https://github.com/boulayo/bitcoin-puzzle-solver/blob/main/docs/en/RELEASING.md)).

## [Unreleased]

### Changed
- CUDA linear search: the first denominator and the centre of a group are handled after the loop over the pairs of keys, so the compiler
  no longer copies the inverse and the centre ahead of the tests on the index (24 moves per pair on sm_86): 3155 to 3141 instructions per
  key, the same arithmetic. Four more variants for the tuner to measure: windows 256 and 512 capped at 96 registers (5 blocks of 128 per
  SM, 20 warps; the spills are outside the loops over the keys) and a window of 1024 with either cap. The arithmetic form is chosen per card
  too: the additions on the FMA pipe (1.18.0) pay on Ampere and Ada, whose SM partitions have separate integer pipes, while a card whose
  lanes do both multiplies and additions (unified datapaths) is bound by the total count, where the form of 1.17.0 has 12 % fewer
  instructions; both forms are embedded for windows 256, 512 and 1024 with either register cap (six one-pipe modules), and after the ten
  two-pipe variants the tuner measures the one-pipe twins of the two fastest and keeps the fastest, bit-exact check included. The
  stripped executable grows by 0.26 MB of compressed PTX (2.43 to 2.69 MB) and the first-run tuning takes about half as long again (twelve measurements instead
  of six). RESEARCH.md section 5 reads the scheduling control words of the SASS (`tools/research/sass_sched.py`):
  by the latencies `ptxas` itself schedules with, the kernel of 1.18.0 is not latency-bound with four warps per partition, and the
  modelled time per key is 3760 cycles per 32 keys against the 5485 the A2000 measured, a gap that is the same (1.3 to 1.45) for 1.17.0 and
  1.18.0 and that only a clock below the probes' 1.89 GHz under sustained load explains; the section lists the changes measured and dropped
  (the SHA state sums back on the ALU pipe, the hash inlined in the pair loop, 80 registers) and what the owner can check on the card.
- CPU linear search, lane engines: the hash filter has fewer operations on the two vector ports. The majority function of SHA-256 is one
  `vpternlogd` with AVX-512 (GCC made two of it) and four operations instead of five elsewhere; the RIPEMD-160 round constants, folded with
  the padding words, are read as broadcast operands from a table instead of being moved to a general register and broadcast on port 5 (57
  times per batch). Per batch of 16 keys 3009 to 2904 instructions; measured on one thread avx512i +2.0 % (275 to 269 instructions per
  key), avx512v +1.2 %; the AVX2 and SSE2 engines change by less than 0.1 % in instructions. Same digests, same candidates.
- Documentation: RESEARCH.md 10.7 records what was measured and dropped: the step of the next points interleaved with the hash of the last
  batch at the instruction level (no gain: the hash is bound by the vector ports and the decoder alike, and the merge costs spills), the
  compiler variants for the engine object, and why the second thread of a core adds 9 to 12 % on the laptop of 10.5, with the test that
  would show otherwise.

## [1.18.0] — 2026-10-30

### Changed
- CUDA linear search: the additions of SHA-256 and RIPEMD-160, the carries out of the field addition chains and the normalization of x
  are `IMAD` with a multiplier of 1 read from the constant bank, so they run on the FMA pipe, which was idle three quarters of the time,
  and the ALU pipe that bounds the kernel keeps only the rotations, the logic and the carry chains: per key on sm_86 2249 to 1743
  ALU-pipe instructions and 515 to 1385 FMA-pipe ones (3155 in all, was 2791), modelled time per key -20 to -23 % (RESEARCH.md section 5
  has the model, checked against the 793 M keys/s of the A2000, the budget per phase, and why the borrow of a subtraction cannot be
  taken that way: `ptxas` carries the inverse of the PTX borrow, which the first test build read wrongly on the card). Counted with
  ptxas; measured on an RTX A2000 Laptop: 793 to 882 M keys/s in `bench` (+11 %), self-tests passing. The register-capped variants no
  longer spill; the kangaroo kernels are unchanged; the OpenCL kernels compile to the same code as before.

## [1.17.0] — 2026-10-30

### Changed
- AVX-512 IFMA lanes (linear search and kangaroo): the subtractions that follow a product (`x3 = λ² − x − x'`, `y3 = λ·t − y`) go into the
  product as a term added before its reduction, so their carry pass, a stage of the dependency chain of every point, is gone. Measured on
  one thread: linear search avx512i +2 % (the curve part alone +7 %), kangaroo avx512i +3 to +4 %, avx512i2 unchanged; 280 to 275
  instructions per key (kangaroo lane step 143.2 to 141.1). The 29-bit engines are unchanged.
- Kangaroo: where RAM does not set `dp-bits`, the automatic value is one bit lower (the overhead term goes from 0.01 to 0.005 of `√W`):
  the delay of the last distinguished point, about `0.95·K·2^dp-bits` steps (measured in the walk model and on the real curve, RESEARCH.md
  8.13), falls from 0.7 % to 0.35 % of the expected steps on average, for twice the table and twice the `dp.bin`. CPU herds and GPU herds
  up to 2^20 walkers on machines with 32 GB and more are affected; with 2^22 walkers memory sets `dp-bits` below 128 GB and nothing changes.
  The jumps, the walks and the format of `dp.bin` are unchanged: campaigns resume as they are, and the points already recorded stay valid.
- `pzsolve kstat` prints the standard error of the mean.
- The build refuses an incompatible pointer type (`-Werror=incompatible-pointer-types`): GCC only warns about it by default, and a call
  with the address of a pointer instead of the pointer reads the wrong memory.
- Documentation: RESEARCH.md 10.6 has the cycle model of the lane engines (ports, the AVX-512 clock, where the cycles of a key go) and the
  changes that were measured and dropped; 8.13 records the last percent of the kangaroo constant: finer start bands gave 0.8 ± 0.3 % on the
  real curve and 0.23 % in the exact model of the puzzles, below the bar for a change of the walk, and the finding that a resume draws new
  starts, so the bands and shares are not part of what a resume needs (`tools/research/kang_schema3f.txt` keeps the layout).

## [1.16.0] — 2026-10-30

### Changed
- The first-run measurement of CPU threads and GPUs also tries one CPU thread fewer per CUDA device, and leaves out a GPU whose loss is not
  measurable (a configuration without it within 1 % of the best, confirmed in alternating pairs like a faster one): on a laptop such a
  device costs more shared power than it returns. Measured on an i7-11850H with an RTX A2000 and an Intel UHD: the integrated GPU gave
  16 M keys/s and cost the other GPU 90 M; without it the linear search went from 865 to 948 M keys/s. `--retune` redoes the choice.
- Documentation: RESEARCH.md 10.5 models the linear search on that laptop against the owner's measurements: port floors of Tiger Lake
  (112 cycles per key against 158 measured), why SHA-NI does not pay there, the issue ceiling of the A2000 (the kernel at 73 % of its
  ALU pipe), power sharing between CPU and GPU; what the options do.

## [1.15.1] — 2026-10-29

### Fixed
- OpenCL on GPUs other than AMD (Intel, NVIDIA through OpenCL): the add-with-carry and subtract-with-borrow helpers of the field
  arithmetic went through a 64-bit sum, which the Intel Xe-LP compiler got wrong in a chain of eight: the start-up self-test reported
  "operation 3 differs from the CPU" and the device was excluded. They use 32-bit operations only now. Reported on an Intel UHD (Tiger Lake).

## [1.15.0] — 2026-10-29

### Changed
- CPU kangaroo: x and y of a step are made canonical by the fast reduction (the full one only for a vector with a lane that needs it); the
  jump coordinates are stored as `K1 − c`, so the subtractions of the step become additions; on AVX2 the jump entries are read as rows and
  transposed. Measured on one thread against 1.14.0: avx512i2 +7.2 %, avx2v +5.3 %, avx512i +4.0 %, avx512v +2.7 %. The jumps, the walk
  and `dp.bin` are unchanged.
- BSGS with AVX-512 IFMA: the x of a giant step is made canonical by its top limb, about 5 % faster.
- Kangaroo walker starts about twice as fast: herd starts are made affine 64 at a time, and every `k·G` uses signed 8-bit comb windows (at
  most 32 additions instead of 64; table 264 KB, was 61 KB). A GPU herd of 2^22 walkers needs about 26 s of CPU time to start instead of 52.
- GPU kernels: a subtraction from a normalized value skips its second correction (CUDA kangaroo step 1167 to 1132 instructions, linear
  search -1 % per key on sm_86; one spill of `kang128b` is gone).
- OpenCL on AMD GPUs (gfx9 to gfx12): the field product in inline assembly on `v_mad_u64_u32` with its carry out (multiplication -22 %,
  kangaroo step loop -11 %, linear loop -17 % in instructions). A build or a start-up self-test that fails with it falls back to the C form.
- GPU figures are instruction counts (ptxas, clang for AMD); not yet measured on a card.
- Documentation: RESEARCH.md 10.4 and section 5 log what was measured, including the tables that do not pay (the jump table in L1 or in
  shared memory, a larger linear window, precomputed hash words, per-jump constants).

## [1.14.0] — 2026-10-29

### Changed
- **Upgrade note.** BSGS tables are saved as `PZBSGS03`, which 1.13.0 and earlier cannot read. `PZBSGS02` and `PZBSGS01` tables are still
  read and used as they are.
- BSGS: the baby-step table keeps a 32-bit tag in 4-byte cells (16 per cache line, 70 % in use): 5.7 bytes per baby step instead of 11.4, so
  twice the baby steps fit in the same RAM. When a tag matches, the baby index is found again by a small search, a few milliseconds about once
  per 3·10^8 giant steps. Measured: 0.78 of the time per giant step at equal table size, and about half the time per key when RAM limits the
  table (2^51 keys with `--mem 0.05`: 3.5 s to 1.2 s). With 128 GB BSGS now pays off up to about 2^71 keys instead of 2^69.
- Faster field inversion on the CPU: divsteps in variable time instead of an addition chain (about 4 times faster), and the SIMD lanes are
  inverted together through one scalar inversion. Herd start about twice as fast; a walker start needs one inversion instead of two.
- Faster lane arithmetic: the canonical reduction needs two carry passes instead of four; subtractions that only feed a product skip their
  carry pass; on AVX2 the column sums of a product are no longer reordered and spilled by the compiler.
- Faster lane kangaroo: x3 and x − x3 come from the denominator with lazy carries, and restarted walkers are found a vector at a time.
  Measured on one thread against 1.13.0 (interleaved runs): about +38 % on AVX2 lanes, +18 % on AVX-512 lanes, +10 % with IFMA.
- Faster CPU linear search: x and the parity of y are made canonical by a fast path; the filter compares hash160 word 2, which needs four
  fewer RIPEMD-160 steps; the lane engines hash eight batches at a time; with IFMA the batch inversion runs as two chains of products; on
  AVX-512 the field products are inlined; on AVX2 the byte swap and the rotations by 8 are one byte shuffle; the scalar engines form
  x(C)+x(i·G) once per pair. Measured on one thread against 1.13.0: avx2v about +25 %, avx512v about +20 %, avx512i +12 %, avx2 +10 %,
  avx512 +9 %. BSGS giant steps on lanes: +4 to +13 % from the same arithmetic.
- GPU (CUDA): one `IMAD.WIDE` per limb product in the field multiplication and square (multiplication 235 to 121 instructions); the modular
  inversion does its divsteps in variable time (about 14,600 instead of 22,000 instructions per warp); the jump table is read with 16-byte
  loads; RIPEMD-160 byte swaps are one `PRMT`; x3 on one pair of borrow chains. Instructions per kangaroo step on sm_86: 1868 to 1167
  (-38 %); per key of the linear search: about -18 %. Counted with ptxas, not yet measured on a card; the tuner picks the fastest variant.
- GPU linear kernel: filter on hash160 word 2, and the keys that pass are hashed again in full on the device, so a hit matches all 160 bits.
- OpenCL on AMD GPUs (and pocl): field arithmetic on add-with-carry chains, 40 to 49 % fewer instructions per kangaroo step (static counts
  for gfx906, gfx1030, gfx1100). NVIDIA through OpenCL and Intel keep the portable code.
- `bench`: kangaroo ceilings computed from about 1300 instructions per GPU step instead of 2200.
- Documentation: the expected cost of the kangaroo at the scale of the open puzzles is about 1.04·√W (was 1.02, from a run of 300 trials);
  RESEARCH.md 8.12 splits the cost from the counting bound 0.67 to the measured 1.06 and lists the ideas measured and dropped (longer jumps,
  other jump distributions, finer bands, other species); 8.11, 10.2 and 10.3 and section 5 log every measurement of this release.
- Research tools: `tools/research/kwalk.c` (the walk of schema 3 with the split of its cost) and `tools/research/kcover.py`.

## [1.13.0] — 2026-10-29

### Changed
- **Upgrade note.** Every command refuses the options it does not use (for example `list -t 7`) instead of ignoring them; `--mem` must be
  at least 0.001 (GiB); `merge` exits with 2, not 1, when a file cannot be merged. Scripts that relied on the old behaviour must be adjusted.
- The kangaroo table of distinguished points uses 16 bytes per entry instead of 32: it keeps the index of the point in `dp.bin` and reads the
  distance back when two kinds of walkers meet. Where memory sets `dp_bits` it is one bit lower, which halves the delay before a collision is
  seen. Modelled with 2^22 kangaroos on puzzle 140: 1.10 to 1.08 times the square root of the range with 32 GB, 1.15 to 1.10 with 16 GB,
  1.23 to 1.15 with 8 GB; 1 % or less with 2^20 kangaroos or 64 GB and more. Measured on 2^40 and 2^44 keys with a small `--mem`: 8 to 9 %
  fewer steps. `dp.bin` is unchanged; points that are in no log keep a full copy, so a solve is never lost.
- CPU threads go to distinct physical cores first for every thread count (`-t N`, the machine profile, `bench`), not only with `--limit`.
  This matters where the two threads of a core are numbered next to each other (Windows, Intel hybrid CPUs).
- Kangaroo on CPUs with SMT: the mixed layout (vector step on the first thread of each core, BMI2/ADX step on the other) is measured at
  start-up and used when it is at least 3 % faster (+14 % on the CI runner). `PZ_KANG_MIX=0` turns it off, `=1` forces it.
- BSGS: the table is built in blocks sized to the table and the threads, so small tables use every thread (`bench`: about 3 times faster on 4
  cores). The table is the same.
- GPU kangaroo: the tuner also tries wider herds (4 and 8 times the work-items per compute unit with OpenCL, at most 128 per unit on Intel
  GPUs; 512 threads per SM on CUDA cards with more than about 80 SMs); a launch runs up to 256 rounds when the device memory allows and reads
  its two counters in one transfer; the replay at the end of a launch loads the jump history 8 entries at a time. Correctness checked on the
  simulated CUDA driver and on pocl; the gain on real cards is not measured yet.
- CUDA linear search: the register-capped variants get a grid of 4 blocks per SM, the occupancy they were built for.
- Terminal output:
  - the kangaroo start-up is one line with grouped digits (the internals with `-v`);
  - GPU tuning prints one line per GPU and kernel, with the rates of every variant in `log.txt`;
  - the jump table id is shown with any `--node`, 0 included;
  - a completed campaign is shown as completed, and resuming it prints one line and exits with 1 instead of running a session;
  - usage errors show the usage line of the command;
  - the main help has one line per command, and `hunt --help` documents every option and the complete rule of `-m auto`;
  - `info` and finished campaigns show no rows of zeros; chance windows stop at the first 100 %; a range size is written `2^k` only when
    it is exact;
  - tables of `bench --together` are never wrapped;
  - a GPU runtime switched off by `PZ_NO_CUDA` or `PZ_NO_OPENCL` is reported as disabled.
- The thinning of the table of distinguished points is an information, not a warning: it is expected in about one run in five where memory
  sets `dp_bits`.
- Documentation: shorter guide (the option list is `pzsolve hunt --help`), a glossary, the GPU memory of the kangaroo (40 MiB plus 256 bytes
  per kangaroo), `--mem` in GiB; release archives ship only the user documents.

### Fixed
- A failed opening of `dp.bin` for a reason other than a missing file (permissions, disk error) recreated it empty.
- A thread count in `machine_profile.txt` beyond the worker limit stopped the kangaroo with "too many workers".
- `doctor` on an empty campaigns folder, and `--mem` below 32 bytes, did undefined operations.
- `log.txt` got two "final state" lines in log mode and none with the panel; log records written to a file or a pipe were split; `--ui line`
  and `info` ignored the terminal width; the panel stopped at 110 columns instead of 128.
- The first-run measurement wrote its kangaroo set-up into the log of the new campaign.
- `split` and `puzzle --rate` accepted speeds with blanks, a sign or hexadecimal digits.
- Messages: a foreign `dp.bin` is "left as it is", a log with another jump table means "another seed", GPUs are always named `GPU[n]`;
  `FOUND.txt` writes the key with `0x`.

## [1.12.0] — 2026-10-29

### Removed
- `--limit 100` is refused (no `--limit` means no limit, as before). Scripts that passed 100 must drop the option.

### Changed
- **Upgrade note.** The kangaroo log `dp.bin` now starts with `PZDP5` and records the jump table: versions before 1.12.0 cannot read a log written
  by 1.12.0. Logs written by 1.9.1 to 1.11.0 (`PZDP4`) are still read, appended to and merged.
- Faster hash160 stage: the filter on the first word of the hash no longer computes the unused part of RIPEMD-160, the SHA-256 round constants
  are read as broadcasts from memory and the byte swap is one shuffle on AVX-512. Instructions per key: -7.4 % (AVX-512), -3.6 % (AVX2); time
  measured in interleaved runs: about -4 % on both.
- Faster lane kangaroo: the second pass of a step reads only the parts of the jump it needs. Time per step: -4.5 % (AVX2 lanes), -3.8 %
  (AVX-512 lanes); about -2 % of instructions on the IFMA variants.
- The kangaroo picks its distinguished-point density with a margin of 1.3 instead of 4 when memory decides it, now that the table is thinned in
  place: the expected cost on machines where memory sets `dp_bits` drops (modelled, puzzle 140 with 2^22 kangaroos: 1.19 to 1.10 times
  the square root of the range at 32 GB, 1.33 to 1.15 at 16 GB), and `dp.bin` grows 2 to 4 times.
- `merge` refuses a log written with another jump table (another seed or another setup) instead of merging points that can never meet.
- Terminal output reviewed: fewer lines at start-up (one line per GPU, the machine profile only when it changes something, the jump table id
  only with `--node`) and at the end (the path of `FOUND.txt` once, no time left after the end); one format for numbers, percentages,
  durations and labels; output wraps at the width of the terminal (at most 128 columns); every command has a short help
  (`pzsolve help CMD`, `CMD --help`); errors say what to do next; `where` says whether the key was already scanned; `info` and `list` show
  the time left at the last speed for a stopped campaign and hide it for a finished one. Labels of `report.txt` changed slightly (the keys of
  `status.json` did not; its ETAs are null when beyond the model).
- The documentation describes what the program does now; history and discarded variants are in `docs/en/RESEARCH.md`; the Italian guide has
  the same sections as the English one.

### Fixed
- Windows: a `dp.bin` larger than 2 GiB could not be cut back after a torn write (resume failed) or, above 4 GiB, was cut to the wrong size.
- The kangaroo now warns when it has done 5 times the expected effort without a collision (wrong public key or range) instead of showing
  "100 %" and "<0.1 s" forever.
- BSGS: the speed shown after the table was built was the speed of the baby steps, and so were the time left and `list`.
- Custom targets were shown as "puzzle #0"; the BSGS notice about GPUs was printed without any GPU; the panel cut words at 80 columns;
  `help CMD` printed the main help; `split` refused decimal speeds such as `1.5G`.
- A big batch of points from a GPU could write the kangaroo log while holding the table lock.
- A fatal error could leave a torn line at the end of `log.txt` when another thread was logging at the same moment.
- The CI check of the puzzle database failed where Python's hashlib lacks RIPEMD-160 (`tools/ref.py` has its own now).

## [1.11.0] — 2026-10-28

### Added
- Exit status of `hunt`: 0 key found, 1 search finished without the key, 130 interrupted (Ctrl-C, SIGTERM, SIGHUP), 2 error. A failed final
  state save is now 2.
- A fatal error is also written to the `log.txt` of the campaign, so after a crash the log says why the run stopped.
- `tests/t_env.c` and the sanitizer flag `float-cast-overflow` in the sanitizer builds.

### Changed
- BSGS: the baby-step table is made of buckets of 8 cells (one cache line) at 70 % load: 11.4 bytes per baby step instead of 12.3, and a
  lookup compares the 8 tags of a cache line at once instead of walking a run of cells. The search is 20 to 30 % faster at equal RAM
  (measured on tables from 256 MiB to 3.5 GiB; on builds without SSE2 only the 8 % more baby steps remain). Tables are saved as `PZBSGS02`;
  those of earlier versions (`PZBSGS01`) are still read and used as they are (they need 12.3 bytes per baby step), but earlier versions
  cannot read the new files.
- The kangaroo thins its table of distinguished points in place: the peak memory drops from twice the limit to the limit (measured 521 to
  256 MiB at `--mem 0.25`), and the table grows only if the old and the new one fit in the limit together.
- The dashboard no longer waits for the table of the kangaroo while it is rebuilt (the pause was up to several seconds and could be taken for
  a system suspension), and the log of distinguished points is written outside the table lock, under a lock of its own: a slow or remote
  disk no longer stalls the search threads.
- GPU start-up: the devices are tested and tuned at the same time.
- Every environment variable is read once at start-up into a single structure (`src/envopts.c`); the names and values documented are the same.
  The flag variables follow one rule (on for any value but empty and "0"), a malformed number is reported once and ignored, and the warnings
  print at the start of every command.
- `bench -t` and the parameters of `kstat` are read strictly (`12abc` is refused, `bench -t 0` too). The Italian spellings of options
  (`--parte`, `--nodo`, `--limite`, `--ritara`, `--sveglio`, ...) are gone; the Italian state words in old `status.json` files are still read.
- `doctor` counts the suspensions in `log.txt` without reading the whole file, and `doctor --full` runs the GPU tests once.
- The state of a run (BSGS, linear workers, benchmark totals) lives in the run, so several runs in one process cannot interfere.
- One list of CUDA modules, one header for the numbers the kernel and the host share, one procedure for choosing the variant of either GPU
  kernel, one rule for the per-ISA objects of the Makefile. The code of the search loops is unchanged.
- The linear search ignores a hit beyond the end of `--range` (the last block is scanned in whole lanes): `-r 1:fff` no longer reports key
  0x1000.
- Documentation brought up to date (architecture notes, usage guides, tools table, release notes).

### Fixed
- A very small `--dp-bits` on a huge range (for example `hunt -p 140 --dp-bits 2`) overflowed a floating-point to integer conversion
  (undefined behaviour); the table size is now taken from the memory limit.

## [1.10.0] — 2026-10-28

### Added
- `shani`: on x86-64 CPUs with the SHA extensions (AMD Zen 2 and later, Intel Ice Lake and later) a hash160 stage that runs SHA-256 on
  `sha256rnds2`/`sha256msg1`/`sha256msg2` and RIPEMD-160 on the AVX2 lanes, as the pzcore batch implementation `"shani"` (selected by name)
  and as the CPU engine `--isa shani`, which `--isa auto` uses only if it measures faster. Without the extensions nothing changes. The
  start-up check of every engine now also compares each lane of its hash with the scalar code. `make test-shani` runs the SHA-extension code
  under QEMU.
- The CI prints the speed of the CPU engines on every Linux runner, runs the self-test against the simulated CUDA driver, checks the puzzle
  database, and has a job for the SHA-extension code under QEMU.

### Changed
- Release binaries are shipped without debug information and the CUDA modules are embedded compressed (a small LZ decoder, a dictionary per
  family of modules): the Linux binary goes from 22.7 MB to 2.8 MB, the release archives from 5 MB to 1.5 MB. Crash backtraces of release
  builds show no function names.
- `kangaroo.c` and `gpu.c` are split by concern (`kang_log.c`, `kang_cpu.c`, `kang_selftest.c`, `gpu_tune.c`, `gpu_selftest.c`); the code of
  the search loops is byte for byte the same.
- The algorithms document is split: `docs/en/ALGORITHMS.md` describes what the program does, `docs/en/RESEARCH.md` is the log of what was
  tried and measured. The time tables of the kangaroo and of the BSGS comparison are recomputed with the constant the program uses (1.06).
- `report.txt` and `status.json` are written without waiting for the disk (fewer syncs); `status.json` lists every worker.
- `hunt` checks everything that can make a campaign unusable before it creates the folder, and a command that created a campaign can resume it
  when an option was cut down to fit the range.
- The kangaroo accepts intervals up to 2^188 keys (puzzles up to 189), not 2^190: above 2^188 the 192-bit exponents of part of the walkers wrapped
  and a third of the herd was wasted.

### Fixed
- Two simultaneous starts of the same new campaign could leave the seed of the loser in `campaign.conf`, so that a later resume scanned other blocks
  than the ones recorded as done. The folder is now locked before the file is written, and `state.dat` records an identifier of the seed
  (a `state.dat` of another seed is refused; files without it still load).
- `where` on a BSGS campaign always printed the last step.
- Two processes writing the shared caches (`machine_profile.txt`, `gpu_tune.txt`) could corrupt them or lose an update; the temporary file of an
  atomic write is now unique and the profile is merged under a lock.
- The first-run measurement saved a result even when nothing had been measured, and spent two minutes at every start when the cache folder was not
  writable; Ctrl-C during it now ends it at once and removes its scratch folder.
- Options: a rejected option is named correctly (`hunt -xq`), empty or oversized values of `-d`, `-r`, `-f` are refused instead of being cut,
  `--mem` is capped; campaign names that Windows cannot have (`con`, `nul`, ...) are refused; log files are written in binary mode on Windows.
- Kangaroo log: a read error was taken for the end of the file and the log was truncated there; a file shorter than its header blocked the
  campaign; a log of a newer version was moved away as if older. The progress line showed the wrong counter for the points lost.
- GPU: the start-up check of the linear kernel missed the centre key and the edges of each group, so a defect there would have passed; after a
  failed launch the launch size is reduced; the recovery counter decays after ten good minutes (a laptop that sleeps every night no longer loses
  its GPU); the self-tests use a random generator per device; one transient API error at start-up no longer excludes the GPU; Ctrl-C interrupts
  a recovery wait; the kangaroo kernel is built once at start-up instead of eight times.
- Build: the objects that print the version are rebuilt when `VERSION` changes; an interrupted generation of an embedded file no longer breaks the
  next build; Apple clang is recognized from the first line of `--version` only; line endings are fixed for Windows checkouts. The release
  workflow runs one release per version at a time and refuses to replace the assets of a tag that points to another commit unless asked.

## [1.9.1] — 2026-10-28

### Removed
- The older kangaroo layouts (schemas 1 and 2) of the development builds, their log formats (`PZDP1` to `PZDP3`), the conversion of old logs and the
  variable `PZ_KANG_SCHEMA`. Every campaign now uses schema 3, which versions 1.8.0 to 1.9.0 already used for every campaign with at least 256
  kangaroos (all the open puzzles); campaigns started with them in that case resume unchanged (same jump table, same `dp.bin`).
- **Upgrade note.** Versions 1.8.0 to 1.9.0 used schema 1 for a new kangaroo campaign with fewer than 256 kangaroos, that is on small intervals
  (below about 2^28 to 2^32 keys, depending on the number of threads) or with `-m kangaroo` on a small range. Such a campaign has a `dp.bin` starting
  with `PZDP2`, whose points 1.9.1 cannot use: `hunt` keeps the file as `dp.bin.v2`, says so and goes on with a new log (the keys already scanned
  by that campaign are scanned again), and `merge` refuses the file with an explanation.
- Cost on small intervals: schema 3 takes more steps than schema 1 did when there are few kangaroos (`kstat`, 1 thread: W = 2^24, 1.54 instead of 1.40
  times √W; at 2^28 the means are about equal, 1.2, with a longer tail), and the same from 2^32 on.
- The option `--no-progress`: use `--ui log`.

### Changed
- The Italian documentation is reduced to the README and the usage guide (the translations of the algorithms, architecture, data, release and tools
  documents were behind the English ones); they point to the English documents.
- The simulators and measurement scripts behind `docs/en/ALGORITHMS.md` moved to `tools/research/`; `tools/export_public.sh` is no longer in the repository.

## [1.9.0] — 2026-10-24

### Added
- `lib/pzcore`: the crypto core (256-bit integers, secp256k1 field and curve, SHA-256 / RIPEMD-160 / HASH160, Base58Check, bech32 / bech32m,
  SegWit addresses, the keyed permutation, and HASH160 of many keys with the best SIMD code of the CPU) as a reusable C library with its own
  README, tests and examples. `make lib`, `lib-test`, `lib-examples`, `lib-cxx`. pzsolve links the same archive.

### Changed
- Every exported name of the core has the `pz_` prefix; functions return errors instead of exiting; the campaign permutation is unchanged
  (380 values produced by 1.8.1 are pinned by a test).
- Faster scalar code (median of interleaved runs, ns): RIPEMD-160 353 -> 160, SHA-256 of 33 bytes 313 -> 239, HASH160 of one key 640 -> 360,
  square root 8.7 us -> 3.9 us. The search engines run at the same speed as before (within the noise, checked by an independent review).
- A plain `make` builds `build/pzsolve` again after the library rules were added; the field code compiles with `-O0` and `-mbmi2 -madx`.

### Fixed
- Base58Check encoding overflowed a stack buffer for payloads above 124 bytes and cut texts that did not fit.
- 65-byte public keys with a coordinate written as x + p were accepted.
- The permutation could loop forever for a number outside its domain; the point at infinity of a Jacobian sum kept stale coordinates;
  a bit index of 256 or more read out of bounds; a valid empty Base58Check payload was refused.

## [1.8.1] — 2026-10-24

### Fixed
- The table of distinguished points of the kangaroo could be up to twice as large as the memory limit (`--mem` or the automatic limit):
  its size is a power of two and was rounded up past the limit. It is now rounded down.
- `doctor --full` no longer mixes the two output streams, and `doctor` reports a wrong option like every other command.

### Changed
- No line printed to the terminal is longer than 128 columns: the help, the log lines, `doctor` and `devices` are wrapped at a space
  with a hanging indent. The log file `log.txt` keeps whole lines.
- `selftest` no longer prints the log lines of the small campaigns it runs (temporary paths, "KEY FOUND"), and its results are one
  aligned list with one line per group.
- `bench`, `devices`, `puzzle`, `list`, `info`, `merge` and `kstat` print aligned tables and property lists, with shorter texts;
  `bench --together` is a table with one column per device. In `report.txt` some labels changed slightly: scripts should read
  `status.json`, whose keys are unchanged.
- `puzzle N`: no control characters when the output is not a terminal, digit groups in the key count, thousands separators in the
  years, a final line with the command that starts the campaign, and `--rate` accepts the suffixes k, M, G and T (`--rate 800M`).
- An unknown command or option gets a one-line message that points to `pzsolve help`, instead of the whole help text. All errors start
  with `ERROR:`.
- Long lines are broken at natural places: after a sentence, a semicolon, a colon or a comma, never between a number and its unit. The
  live panel drops whole fields instead of cutting one in half.

### Added
- arm64 (Linux and Apple silicon): the engines `neon` and `neon2` hash with NEON and the hardware SHA-256 of the CPU, chosen by the
  start-up benchmark (`--isa neon|neon2`); the linear scan is about 55-60% faster than the portable engine on Linux arm64 and faster on
  Apple silicon (measured on CI machines). CPUs without the SHA-256 instructions keep the portable engine.
- Native SegWit addresses (`bc1q...`, P2WPKH) are accepted as targets, besides `1...` addresses.
- `pzsolve help hunt` and `pzsolve help bench` show the options of a command, grouped by topic.
- The README shows what sets the program apart; the CI badge; a bug report form for issues.

## [1.8.0] — 2026-10-23

### First public release
- Linear scan, BSGS and kangaroo on the CPU (SIMD) and on GPUs (CUDA, OpenCL) for the 160 puzzles, with resumable campaigns, several
  computers on the same search, self-tuning and a live panel. The documentation is in English; the Italian one is under `docs/it/`.
- Builds for Linux x86-64, Linux arm64, macOS (Apple silicon and Intel) and Windows x86-64, published on GitHub Releases with
  `SHA256SUMS`. The macOS and Linux arm64 builds run the portable scalar engine on the CPU only.
- The jump table of schema 3 is computed with integer arithmetic only, so computers of any architecture working on the same campaign
  agree. The startup log prints `jump table id XXXXXXXX`, and it must be the same on every computer. Campaigns of schemas 1 and 2
  use `long double` and only work consistently on x86-64 (the program warns on other architectures).
- License: `AGPL-3.0-or-later`, with a commercial license available (see the README).

Earlier version numbers (1.0.0 to 1.7.9) were development builds that were never released.
