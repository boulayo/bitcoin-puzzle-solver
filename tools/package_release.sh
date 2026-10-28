#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds and packages the release archives, without GitHub Actions.
#
#   tools/package_release.sh                 build and package Linux x86-64 and Windows x86-64 into dist-release/
#   tools/package_release.sh linux           only the Linux x86-64 package
#   tools/package_release.sh windows         only the Windows x86-64 package (needs MinGW-w64, see WINCC)
#   tools/package_release.sh --pack BINARY OS ARCH
#                                            package a binary that is already built (what the release workflow does on every
#                                            runner); OS is linux, macos or windows, ARCH is x86_64 or arm64
#   tools/package_release.sh --sums          (re)write SHA256SUMS for the archives in dist-release/
#
# Output (OUT, default dist-release/):
#   pzsolve-<version>-linux-x86_64.tar.gz     one top folder per archive, with the binary, README.md, LICENSE (if the
#   pzsolve-<version>-windows-x86_64.zip      repository has one), CHANGELOG.md, docs/ and BUILD-INFO.txt
#   SHA256SUMS
#
# The builds are the ones of the Makefile, from scratch, in build/release/<target>: Linux dynamic against glibc (the code avoids
# the __isoc23_* symbols and anything newer than glibc 2.34, so the binary starts on older distributions; checked below), Windows
# static with MinGW-w64 (only system DLLs; checked below). Release builds never use -march=native, the program picks its SIMD
# engine at run time. The Makefile builds with -g (the build/ folders keep the debug information for gdb, valgrind and the
# sanitizers); the binary that goes into an archive is a copy with the debug information and the symbol table stripped (strip,
# `strip -x` on macOS, the MinGW strip for Windows; a missing strip only warns and the package is then simply bigger).
# Environment: OUT, JOBS, WINCC (default x86_64-w64-mingw32-gcc-posix), SKIP_TESTS=1 (skip the self-test).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=${OUT:-dist-release}
VERSION=$(tr -d '[:space:]' < VERSION)
WINCC=${WINCC:-x86_64-w64-mingw32-gcc-posix}
JOBS=${JOBS:-$( (nproc || sysctl -n hw.ncpu || echo 2) 2>/dev/null | head -1)}
export COPYFILE_DISABLE=1          # macOS tar: no ._* AppleDouble files in the archives

# owner of the archive members: root, whoever built the package (GNU tar and bsdtar spell the options differently)
TAROPTS=()
if tar --version 2>/dev/null | grep -q 'GNU tar'; then TAROPTS=(--owner=0 --group=0 --numeric-owner)
elif tar --version 2>/dev/null | grep -q bsdtar; then TAROPTS=(--uid 0 --gid 0 --numeric-owner); fi

die() { echo "package_release: $*" >&2; exit 1; }
warn() { echo "package_release: warning: $*" >&2; }
say() { echo "== $*"; }

sha256_files() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

STAGES=()
cleanup() { if [ ${#STAGES[@]} -gt 0 ]; then rm -rf "${STAGES[@]}"; fi; }
trap cleanup EXIT

# strip_binary FILE OS: strips the staged copy of a binary in place; it never fails the packaging (it warns and leaves the file as it was)
strip_binary() {
  local f=$1 os=$2 tool="" t before signed=0 flags
  flags=()
  case "$os" in
    macos)   tool=strip; flags=(-x) ;;                                # keeps the global symbols, drops the local ones and the debug map
    windows) for t in "${WINCC%gcc*}strip" x86_64-w64-mingw32-strip; do
               if command -v "$t" >/dev/null 2>&1; then tool=$t; break; fi
             done
             case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) [ -n "$tool" ] || tool=strip ;; esac ;;
    *)       tool=strip ;;
  esac
  if [ -z "$tool" ] || ! command -v "$tool" >/dev/null 2>&1; then warn "no strip tool for $os: the binary keeps its debug information"; return 0; fi
  # a macOS binary that is signed (the release workflow signs it before packaging) loses its signature when it is modified
  if [ "$os" = macos ] && command -v codesign >/dev/null 2>&1 && codesign -v "$f" >/dev/null 2>&1; then signed=1; fi
  before=$(wc -c < "$f" | tr -d ' ')
  cp "$f" "$f.unstripped"
  if ! "$tool" ${flags[@]+"${flags[@]}"} "$f" >/dev/null 2>&1; then
    warn "$tool failed on $(basename "$f"): the binary keeps its debug information"
    mv "$f.unstripped" "$f"; return 0
  fi
  if [ $signed = 1 ] && ! codesign -s - --force "$f" >/dev/null 2>&1; then
    warn "could not sign the stripped binary again: the binary keeps its debug information"
    mv "$f.unstripped" "$f"; return 0
  fi
  # the stripped copy must still start wherever the original starts (it cannot be tried when the binary is for another system)
  if "$f.unstripped" version >/dev/null 2>&1 && ! "$f" version >/dev/null 2>&1; then
    rm -f "$f.unstripped"; die "the stripped $(basename "$f") does not start"
  fi
  rm -f "$f.unstripped"
  echo "  stripped $(basename "$f"): $before -> $(wc -c < "$f" | tr -d ' ') bytes"
}

# pack BINARY OS ARCH: stages <name>/ and writes <name>.tar.gz (linux, macos) or <name>.zip (windows) into $OUT
pack() {
  local bin=$1 os=$2 arch=$3
  [ -f "$bin" ] || die "no such binary: $bin"
  case "$os" in linux|macos|windows) ;; *) die "unknown OS '$os' (linux, macos or windows)" ;; esac
  local name="pzsolve-$VERSION-$os-$arch" exe=pzsolve
  [ "$os" = windows ] && exe=pzsolve.exe
  local stage; stage=$(mktemp -d "${TMPDIR:-/tmp}/pzpack.XXXXXX")
  STAGES+=("$stage")
  local d="$stage/$name"
  mkdir -p "$d" "$OUT"
  cp "$bin" "$d/$exe"
  chmod 755 "$d/$exe"
  strip_binary "$d/$exe" "$os"
  cp README.md "$d/"
  for f in LICENSE LICENSE.md LICENSE.txt COPYING CHANGELOG.md; do [ -f "$f" ] && cp "$f" "$d/"; done
  [ -d docs ] && cp -R docs "$d/docs"
  {
    echo "pzsolve $VERSION"
    echo "platform:  $os-$arch"
    echo "commit:    $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "built:     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "builder:   $(uname -sm)"
  } > "$d/BUILD-INFO.txt"
  find "$d" -type d -exec chmod 755 {} +
  find "$d" -type f ! -name "$exe" -exec chmod 644 {} +
  rm -f "$OUT/$name.tar.gz" "$OUT/$name.zip"
  local abs_out; abs_out=$(cd "$OUT" && pwd)
  if [ "$os" = windows ]; then
    command -v zip >/dev/null 2>&1 || die "zip is not installed"
    (cd "$stage" && zip -q -X -r "$abs_out/$name.zip" "$name")
    echo "  $OUT/$name.zip"
  else
    (cd "$stage" && tar ${TAROPTS[@]+"${TAROPTS[@]}"} -czf "$abs_out/$name.tar.gz" "$name")
    echo "  $OUT/$name.tar.gz"
  fi
}

write_sums() {
  [ -d "$OUT" ] || die "no $OUT folder"
  ( cd "$OUT"
    local files=()
    for f in pzsolve-*.tar.gz pzsolve-*.zip; do [ -f "$f" ] && files+=("$f"); done
    [ ${#files[@]} -gt 0 ] || die "no archives in $OUT"
    sha256_files "${files[@]}" > SHA256SUMS
    cat SHA256SUMS )
}

build_linux() {
  if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then die "the Linux x86-64 package is built on a Linux x86-64 machine"; fi
  local dir=build/release/linux
  say "Linux x86-64: build ($(${CC:-cc} --version | head -1))"
  rm -rf "$dir"
  make -s BUILD="$dir" -j"$JOBS"
  if [ -z "${SKIP_TESTS:-}" ]; then say "Linux x86-64: self-test"; "$dir/pzsolve" selftest | tail -2; fi
  if command -v objdump >/dev/null 2>&1; then
    local newest
    newest=$(objdump -T "$dir/pzsolve" | grep -oE 'GLIBC_[0-9.]+' | sort -V | uniq | tail -1)
    if objdump -T "$dir/pzsolve" | grep -q isoc23 || [ "$(printf '%s\nGLIBC_2.34\n' "$newest" | sort -V | tail -1)" != GLIBC_2.34 ]; then
      die "the Linux binary needs $newest (or __isoc23_* symbols): it would not start on older distributions"
    fi
    echo "  glibc: needs at most $newest"
  fi
  say "Linux x86-64: package"
  pack "$dir/pzsolve" linux x86_64
}

build_windows() {
  command -v "$WINCC" >/dev/null 2>&1 || die "$WINCC not found (Debian/Ubuntu: apt install gcc-mingw-w64-x86-64-posix)"
  local dir=build/release/windows
  say "Windows x86-64: build ($($WINCC --version | head -1))"
  rm -rf "$dir"
  make -s CC="$WINCC" BUILD="$dir" -j"$JOBS"
  local objdump=${WINCC%gcc*}objdump
  if command -v "$objdump" >/dev/null 2>&1; then
    if "$objdump" -p "$dir/pzsolve.exe" | grep 'DLL Name' | grep -viqE 'kernel32|msvcrt|advapi32|bcrypt'; then
      die "pzsolve.exe depends on DLLs that are not part of Windows"
    fi
    echo "  pzsolve.exe imports only system DLLs"
  fi
  if [ -z "${SKIP_TESTS:-}" ] && command -v wine >/dev/null 2>&1; then
    say "Windows x86-64: self-test under Wine"
    WINEDEBUG=-all wine "$dir/pzsolve.exe" selftest 2>&1 | tr -d '\r' | tail -2
  fi
  say "Windows x86-64: package"
  pack "$dir/pzsolve.exe" windows x86_64
}

case "${1:-all}" in
  -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
  --pack)    [ $# -ge 4 ] || die "usage: $0 --pack BINARY OS ARCH"; pack "$2" "$3" "$4"; exit 0 ;;
  --sums)    write_sums; exit 0 ;;
  linux)     build_linux ;;
  windows)   build_windows ;;
  all)       build_linux; build_windows ;;
  *)         die "unknown argument '$1' (see --help)" ;;
esac
say "checksums"
write_sums
