# Changelog

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows [Semantic Versioning](https://semver.org/).
Each release is tagged `vX.Y.Z` (see [`docs/en/RELEASING.md`](docs/en/RELEASING.md)).

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

Earlier version numbers (1.0.0 to 1.7.9) were development builds that were never released. Their history, in Italian, is in
[`docs/it/CHANGELOG.md`](docs/it/CHANGELOG.md).
