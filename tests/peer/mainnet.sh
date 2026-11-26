#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The main chain without a node of one's own, on the public Electrum servers of the program: the block headers since block 338688 fetched
# and kept (pzchain/headers.bin), the coins of the creator at two open puzzles (140, 71) proven, and a sweep built, nothing sent. It costs nothing
# and moves nothing: the check of a machine that will run a home without a node (docs/en/NETWORK.md 8.2).
# Usage: tests/peer/mainnet.sh [PZPEER]    (default build/pzpeer; in a folder where pzchain may be made)
set -u
P=${1:-build/pzpeer}
t0=$(date +%s)
"$P" netcheck 140 --chain || { echo "MAINNET FAILED: puzzle 140"; exit 1; }   # a public key: the three coins of the creator, unspent
t1=$(date +%s)
SECOND=$("$P" netcheck 71 --chain) || { echo "$SECOND"; echo "MAINNET FAILED: puzzle 71"; exit 1; }   # a linear search, dozens of coins
echo "$SECOND"
t2=$(date +%s)
echo "$SECOND" | grep -q "are fetched" && { echo "MAINNET FAILED: the second check fetched the headers again"; exit 1; }
# a key nobody funds (SHA-256 of a sentence): the scan of its address answers, and the sweep says there is nothing to move
OUT=$(echo be5435bc3c485ca532767e7a982fcf463ea921b12599d2a78ae84bf20bee7447 | "$P" sweep --to 1PWo3JeB9jrGwfHDNpdGK54CRas7fsVzXU 2>&1)
echo "$OUT"
echo "$OUT" | grep -q "no transaction: the address has no confirmed coin" || { echo "MAINNET FAILED: the sweep"; exit 1; }
echo "MAINNET PASSED: the first check in $((t1 - t0)) s, the second in $((t2 - t1)) s"
