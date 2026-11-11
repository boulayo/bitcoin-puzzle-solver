#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# Three nodes that are given no address of each other: a network file without entry lines, no --peer. Each announces itself in a table
# like that of BitTorrent (tests/peer/bt_table.py, one node on the loopback) and finds the others there. It passes when every node has
# the two others as contacts.
# Usage: tests/peer/e2e_meet.sh    (build/pzpeer built; PZ_E2E_PORT: another base port; PZPEER: another pzpeer, as a command:
# PZPEER="wine build/win/pzpeer.exe" runs the Windows program under Wine)
set -u
PEER=${PZPEER:-$(pwd)/build/pzpeer}                             # the program under test, as a command
DIR=$(mktemp -d /tmp/pzmeet.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DIR"' EXIT
PORT=${PZ_E2E_PORT:-31000}
python3 tests/peer/bt_table.py $PORT > "$DIR/table.log" 2>&1 &
K=$(python3 -c "import random; print(hex(random.SystemRandom().randrange(2**57, 2**58)))")
PUB=$(python3 tools/ref.py "$K" | awk '$1=="pub"{print $2}')
printf 'net=meet\npuzzle=0\npubkey=%s\nrange=200000000000000:3ffffffffffffff\njumpseed=%s\nschema=net-06\ndpbits=14\nchain=regtest\n' \
    "$PUB" "$(python3 -c 'import os; print(os.urandom(32).hex())')" > "$DIR/net.txt"
sleep 1
for i in 1 2 3; do
    $PEER node --network "$DIR/net.txt" --seed-hex "$(python3 -c 'import os; print(os.urandom(32).hex())')" --dir "$DIR/n$i" \
        --listen $((PORT + i)) --no-upnp --bt-router 127.0.0.1:$PORT --verbose > "$DIR/n$i.log" 2>&1 &
    sleep 3
done
for wait in $(seq 1 150); do                                   # a node alone looks again every two minutes
    OK=0
    for i in 1 2 3; do
        [ "$(grep -o '[0-9]* contacts' "$DIR/n$i.log" | tail -1 | cut -d' ' -f1)" = 2 ] && OK=$((OK + 1))
    done
    [ $OK = 3 ] && { echo "E2E MEET PASSED: three nodes found each other with no address given, in $((9 + wait)) s"; exit 0; }
    sleep 1
done
for i in 1 2 3; do grep -h -E "rendezvous|bootstrap|contacts" "$DIR/n$i.log" | tail -4 | sed "s/^/   n$i /"; done
echo "E2E MEET FAILED (logs in $DIR)"; trap - EXIT; exit 1
