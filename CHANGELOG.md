# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows [Semantic Versioning](https://semver.org/).
Each release is tagged `vX.Y.Z` (see [`docs/en/RELEASING.md`](docs/en/RELEASING.md)).

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
  265 MiB at `--mem 0.25`), and the table grows only if the old and the new one fit in the limit together.
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
