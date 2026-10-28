#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Checks, without running it, that a release binary will start on the systems it is meant for.
#
#   tools/check_binary.sh linux BINARY      no glibc symbol newer than 2.34 and no __isoc23_* symbol (the code avoids both, so that the
#                                           binary starts on older distributions)
#   tools/check_binary.sh windows BINARY    imports only DLLs that are part of Windows (kernel32, msvcrt, advapi32, bcrypt)
#
# One line on stdout and exit status 0 when the binary passes. When it does not, the reason is printed (on stderr, or as a GitHub
# Actions error annotation when it runs there) and the status is 1. Status 2: the check could not be made (usage, missing file or
# objdump). Environment: OBJDUMP, the objdump to read the binary with (default objdump for linux, x86_64-w64-mingw32-objdump for windows).
# Used by .github/workflows/ci.yml and release.yml, tools/package_release.sh and tools/verify.sh, so that the check exists once.
set -euo pipefail

die() { echo "check_binary: $*" >&2; exit 2; }
fail() { if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::error::$*"; else echo "$*" >&2; fi; exit 1; }

[ $# -eq 2 ] || die "usage: $0 linux|windows BINARY"
kind=$1
bin=$2
[ -f "$bin" ] || die "no such file: $bin"

# the output of objdump is kept in a variable and searched with here-strings: `objdump | grep -q` can end the writer with SIGPIPE,
# which pipefail then reports as a failed pipeline
case "$kind" in
  linux)
    objdump=${OBJDUMP:-objdump}
    command -v "$objdump" >/dev/null 2>&1 || die "$objdump not found"
    syms=$("$objdump" -T "$bin") || die "$objdump -T failed on $bin"
    newest=$(grep -oE 'GLIBC_[0-9.]+' <<<"$syms" | sort -V | uniq | tail -1 || true)
    if grep -q isoc23 <<<"$syms" || [ "$(printf '%s\nGLIBC_2.34\n' "$newest" | sort -V | tail -1)" != GLIBC_2.34 ]; then
      fail "the Linux binary needs $newest (or __isoc23_* symbols): it would not start on older distributions"
    fi
    if [ -n "$newest" ]; then echo "needs at most $newest"; else echo "uses no versioned glibc symbol"; fi
    ;;
  windows)
    objdump=${OBJDUMP:-x86_64-w64-mingw32-objdump}
    command -v "$objdump" >/dev/null 2>&1 || die "$objdump not found"
    imports=$("$objdump" -p "$bin") || die "$objdump -p failed on $bin"
    bad=$(grep 'DLL Name' <<<"$imports" | grep -viE 'kernel32|msvcrt|advapi32|bcrypt' | sed 's/.*DLL Name: *//' | paste -sd, - || true)
    if [ -n "$bad" ]; then
      fail "$(basename "$bin") depends on a DLL that is not part of Windows: ${bad//,/, }"
    fi
    names=$(sed -n 's/.*DLL Name: *//p' <<<"$imports" | paste -sd, -)
    echo "imports only system DLLs${names:+: ${names//,/, }}"
    ;;
  *) die "unknown kind '$kind' (linux or windows)" ;;
esac
