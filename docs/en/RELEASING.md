# Versioning and releases

## Rules

* Version numbers are **MAJOR.MINOR.PATCH** ([Semantic Versioning](https://semver.org/)). The single source is the `VERSION` file.
  * **PATCH**: bug fixes, optimizations, documentation.
  * **MINOR**: new features (options, engines, commands, file formats). Before 2.0 a MINOR may also remove options and file formats.
  * **MAJOR**: incompatible changes to the campaign files that existing campaigns cannot survive.
* Whatever a release removes or stops reading (an option, an environment variable, a file format, a way of resuming) is listed in the
  "Removed" section of its `CHANGELOG.md` entry, with an upgrade note that says what to do instead. 1.9.1, a PATCH, is the one release
  that did so outside a MINOR (see the table at the end).
* Every release has an annotated git tag `vX.Y.Z` and an entry in `CHANGELOG.md`.
* The format of the campaign files has its own number (`format=` in `campaign.conf`, `PZSCHED n`, `PZDPn`, `PZBSGSnn`): the program refuses
  formats it does not know with a clear message instead of misreading them. `campaign.conf` also records the version that created
  the campaign (`created_with`).
* `pzsolve version` prints the version and the source commit the binary was built from.

## Changelog discipline

`CHANGELOG.md` follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

* Each release has a section `## [X.Y.Z] — YYYY-MM-DD`, newest first. The section must exist before the release script is run, and the
  script refuses to release without it.
* Group the changes under the headings Added, Changed, Removed and Fixed.
* Anything that affects existing users goes in the entry: renamed or removed options, changes to the log lines or the output files, any
  change to the campaign file formats (and whether old campaigns still resume).
* Say what was measured and what was not. Performance claims that rest on a model, an estimate or a simulated device are marked as
  such, with the machine and the numbers when they were measured.

## Where things are

```
VERSION                current version
CHANGELOG.md           history of the changes
tools/release.sh       release script: checks, bumps VERSION, verifies, tags
tools/package_release.sh  builds the Linux and Windows packages locally (the workflow packs the binary of every platform with it)
tools/verify.sh        the full verification run by the release script
tools/check_binary.sh  the two checks of a binary that is about to be shipped: the glibc ceiling of the Linux one, the DLLs of the Windows one
tools/ci_apt_install.sh  installs the packages of the workflows (MinGW-w64, QEMU) with a time limit per try and retries
.github/workflows/     ci.yml (every push and pull request) and release.yml (the release)
GitHub Releases        the binaries of every published version, with SHA256SUMS
```

The repository does not keep binaries in git. The binaries of each version are release assets on GitHub.

The Makefile always builds with `-g`, so that the binaries in `build/` can be used with gdb, Valgrind and the sanitizers. The binaries
in the release archives are stripped copies: `tools/package_release.sh` (both when it builds, and for `--pack BINARY OS ARCH`, which the
release workflow runs on every runner) strips the staged file with `strip` on Linux, `strip -x` on macOS (a signed binary is signed
ad hoc again afterwards, since stripping invalidates the signature) and the MinGW `strip` for the Windows exe. If no strip tool is
found, or it fails, the script only prints a warning and packages the binary as it is. After stripping it runs `pzsolve version` on the
copy (when the binary can run on the packaging machine) and stops if the original starts and the stripped one does not.

## How to release

1. Develop and commit the changes.
2. Decide the new number according to the rules above and write the `## [X.Y.Z] — date` section in `CHANGELOG.md`. Commit it.
3. Run `tools/release.sh X.Y.Z`. The script:
   * checks that the version is of the form `X.Y.Z`, that it is greater than the current one, that the git tree is clean, that the section
     exists in `CHANGELOG.md` and that the tag does not exist yet;
   * updates `VERSION` and commits;
   * runs `tools/verify.sh` (rebuild from scratch, `make test`, `tests/e2e.sh`, sanitizers, Valgrind; see below) and stops at the first error;
   * creates the annotated tag `vX.Y.Z` on the resulting commit.
   It never builds binaries and never pushes: it prints the push command at the end.
4. Publish the commit and the tag: `git push origin <branch> --follow-tags`.
5. Pushing the tag starts the GitHub Actions workflow. It builds the binaries for every supported platform (Linux x86-64 and arm64, macOS
   Apple Silicon and Intel, Windows x86-64), runs the self-test of each (the Windows exe is run on a Windows machine), checks the Linux binary
   (no glibc symbol newer than 2.34) and the Windows one (system DLLs only) with `tools/check_binary.sh`, and publishes the five archives, together
   with `SHA256SUMS`, as a GitHub Release for the tag. When it has finished, open the release page and check that every archive and the
   checksum file are there.

The workflow can also be run by hand (Actions tab, "Run workflow"). Without "publish" it only builds the archives and keeps them as artifacts of the
run. With "publish" ticked it creates the tag on the selected commit and a **draft** release (untick "draft" to publish at once).
Releases of one version are made one at a time, and a run refuses to create or replace the files of a release whose tag points to
another commit than the one it built (the log says so). To replace the files of a release on purpose, for example after a failed
upload, run the workflow by hand from the tagged commit; the input "replace" is needed only when the selected commit is another one, and
then the tag and the release notes stay as they are.

Without arguments (`tools/release.sh`) the script releases the version already written in `VERSION`, as for a first release.
The optional environment variable `COMMIT_TRAILERS` adds lines (for example `Co-Authored-By: …`) to the message of the commit created by the script.
`VERIFY_MODE=full` runs the complete end-to-end test instead of the quick one.

## Before every release

`tools/verify.sh` (also used by `tools/release.sh`) runs in sequence:

0. a re-check of the puzzle database with the independent implementation (`tools/gen_puzzles.py --check`);
1. a build from scratch and `make test` (unit tests + built-in self-test), the examples and tests of the library, also unoptimized (`-O0`) and as C++;
2. the binary and the paths that need a particular machine: `tools/check_binary.sh` on the Linux binary (no glibc symbol newer than 2.34); the AVX-512 IFMA
   engine (executed by the CPU if it can, under Intel SDE when `PZ_SDE` is set, else with the emulation of `tools/ifma_emul.h`); the CUDA path with a
   simulated driver (self-test, bench, machine profile, `--prio-gpu`, two devices, resume after an error injected into a launch, the number of modules
   loaded), `doctor`, suspend (frozen process), several computers (kangaroo `merge` and linear-search `--part`);
3. `tests/e2e.sh` (real campaigns on small puzzles; `E2E_QUICK=1`, or `verify.sh quick`, uses the short version);
4. a build with **AddressSanitizer + UndefinedBehaviorSanitizer** (with `float-cast-overflow`, which neither GCC nor clang include in
   `undefined`): self-test, **all unit tests** (including the robustness ones on corrupt files, `t_fuzz`, on corrupt compressed kernel
   streams, `t_lz`, and the reading of the environment, `t_env`) and linear, BSGS (also with the table in a file), kangaroo,
   kangaroo-with-a-tiny-table and puzzle-140-with-`--dp-bits 2` campaigns;
5. a build with **ThreadSanitizer**: linear, BSGS and kangaroo campaigns and interruption with Ctrl-C;
6. **Valgrind** (memcheck with full leak checking) on a linear, a BSGS and a kangaroo campaign, on the portable path
   (Valgrind does not know AVX-512/ADX, which are covered by the previous points);
7. the **aarch64** build, cross-compiled and tried under QEMU user-mode emulation (the NEON and SHA-256 code of the library and of the engines);
8. the **Windows** exe (static MinGW-w64): `check_binary.sh windows`, the unit tests, self-test and campaigns under Wine, with the simulated driver as `nvcuda.dll`.

(The steps whose tools are not installed, such as Valgrind, QEMU or Wine, are skipped with a message.) The release stops at the first error. If the machine has a GPU (CUDA or OpenCL), also run `pzsolve selftest` and `pzsolve bench`
and check that the GPU self-test passes.

The check that the committed PTX modules (`src/kernels/ptx/`) are what the kernels (`src/kernels/*.cl`) generate is made by `verify.sh`
only when `PZ_NVRTC` is set to the path of `libnvrtc.so.12` (NVRTC 12.0 from pip, `pip download nvidia-cuda-nvrtc-cu12==12.0.140`; it
runs `python3 -I tools/gen_ptx.py --nvrtc "$PZ_NVRTC" --check` and takes about a minute). **Neither workflow runs it**, and
`tools/release.sh` does not set it: for a release that changed a kernel, run `PZ_NVRTC=/path/to/libnvrtc.so.12 tools/release.sh`, or
`verify.sh` by itself, so that a stale module cannot ship. What the CI workflow (`ci.yml`) does run on every push, besides the build
and the self-test: the unit tests, `tools/gen_puzzles.py --check`, the self-test through the simulated CUDA driver and, as an
informational job, `make test-shani` (the SHA-extension code under QEMU).

## Compatibility of the files between versions

The public versions start at 1.8.0 (the version numbers before it were development builds that were never released).

| From → to | What changes |
|-----------|--------------|
| 1.8.x–1.9.0 → 1.9.1 and later | campaigns resume, except kangaroo campaigns with fewer than 256 kangaroos (small ranges): their `dp.bin` (schema 1) cannot be used, `hunt` keeps it as `dp.bin.v2` and starts a new one, `merge` refuses it. `--no-progress` is gone (use `--ui log`) |
| 1.11.0 and later → earlier | BSGS tables saved by 1.11.0 to 1.13.0 (`PZBSGS02`) cannot be read by earlier versions; the `PZBSGS01` tables of earlier versions are still read |
| 1.12.0 and later → earlier | a `dp.bin` created by 1.12.0 and later (`PZDP5`, with the jump table id in its header) cannot be read by earlier versions; a `PZDP4` file of 1.9.1 to 1.11.0 is read and continued as it is |
| 1.14.0 → 1.13.0 and earlier | BSGS tables saved by 1.14.0 and later (`PZBSGS03`, 4-byte cells) cannot be read by 1.13.0 and earlier; the `PZBSGS01` and `PZBSGS02` tables are still read and used as they are |
| 1.11.0 | the Italian spellings of the options (`--parte`, `--nodo`, `--limite`, ...) are gone |
| 1.12.0 | `--limit 100` is refused (it meant no limit: leave `--limit` out) |

To continue a campaign with a specific version, use the binary of that version from the GitHub Releases page, or build the tag `vX.Y.Z` from source.
