#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Installs Debian/Ubuntu packages on a CI runner (the workflows use it for MinGW-w64 and QEMU).
#
#   tools/ci_apt_install.sh PACKAGE...
#
# A stalled mirror must not hold the job for the whole of its time limit: each try is limited (120 s for the index, 300 s for the
# install) and a failed try is repeated, three times in all. Exit status 1 when the packages could not be installed.
set -u
export DEBIAN_FRONTEND=noninteractive
[ $# -gt 0 ] || { echo "usage: $0 PACKAGE..." >&2; exit 2; }
for try in 1 2 3; do
  sudo timeout 120 apt-get update -o Acquire::Retries=3 && \
    sudo timeout 300 apt-get install -y --no-install-recommends "$@" && exit 0
  if [ "$try" -lt 3 ]; then echo "apt failed (try $try), retrying"; sleep 5; fi
done
echo "could not install $*" >&2
exit 1
