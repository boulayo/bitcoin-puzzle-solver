#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The shared linear search on one machine (docs/en/NETWORK.md 11): a custodian node and two homes (pzpeer run on a network with an address,
# each with its pzsolve) over two blocks of 2^34 keys, the key of the test in block 1. It passes when the key is found, in FOUND.txt of the
# home that scanned block 1, and the claim of block 0, with its proof keys, is held by the node. The two homes start together, when no
# block is claimed yet, so they may scan the same block first; a block is about 5 minutes of 2 threads, the test 10 at most. No bitcoind:
# a linear search pays nothing.
# Usage: tests/peer/e2e_linear.sh    (pzsolve and pzpeer built)
set -u
BIN=$(pwd)/build
DIR=$(mktemp -d /tmp/pzlin.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DIR"' EXIT
PORT=${PZ_E2E_PORT:-31000}
START=$((1 << 40))
K=$(python3 -c "import random; print(hex($START + (1 << 34) + random.SystemRandom().randrange(1 << 34)))")   # in block 1
H160=$(python3 tools/ref.py "$K" | awk '$1=="h160"{print $2}')
ADDR=$(python3 -c "
import hashlib,sys
p=bytes([0x6f])+bytes.fromhex(sys.argv[1]); p+=hashlib.sha256(hashlib.sha256(p).digest()).digest()[:4]
n=int.from_bytes(p,'big'); a='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'; s=''
while n: n,r=divmod(n,58); s=a[r]+s
print('1'*(len(p)-len(p.lstrip(b'\0')))+s)" "$H160")
printf 'net=lin%d\npuzzle=0\naddress=%s\nrange=%x:%x\nblockbits=34\nchain=regtest\nentry=127.0.0.1:%d\n' "$$" "$ADDR" $START \
    $((START + 2 * (1 << 34) - 1)) $PORT > "$DIR/net.txt"
"$BIN/pzpeer" node --network "$DIR/net.txt" --dir "$DIR/n" --listen $PORT --no-portmap > "$DIR/n.log" 2>&1 &
sleep 2
port=$PORT
for h in a b; do
    port=$((port + 1))
    timeout 900 "$BIN/pzpeer" run --network "$DIR/net.txt" --dir "$DIR/$h" --listen $port --no-portmap \
        --pzsolve "$BIN/pzsolve" --debug "$DIR/$h.debug" -- -t 2 --no-gpu > "$DIR/$h.log" 2>&1 &
    eval "P_$h=\$!"
    sleep 1
done
found=
for t in $(seq 1 900); do
    for h in a b; do ls "$DIR/$h"/campaigns/b*/FOUND.txt >/dev/null 2>&1 && found=$h; done
    [ -n "$found" ] && break
    sleep 1
done
sleep 3
kill $P_a $P_b 2>/dev/null; wait $P_a $P_b 2>/dev/null
for h in a b; do echo "--- home $h"; sed 's/^/   /' "$DIR/$h.log" | grep -v "^   *$" | tail -8; done
claimed=$(grep -h "block 0 scanned and claimed" "$DIR/a.log" "$DIR/b.log" | head -1)
held=$(( $(stat -c %s "$DIR/n/claims.bin" 2>/dev/null || echo 40) > 40 ))
if [ -n "$found" ] && grep -qi "$(printf '%x' "$K" | sed 's/^0*//')" "$DIR/$found"/campaigns/b1/FOUND.txt && [ -n "$claimed" ] && [ "$held" = 1 ]; then
    echo "   the key found by home $found; $claimed; the node holds the claims"
    echo "E2E LINEAR PASSED"
    exit 0
fi
echo "E2E LINEAR FAILED (found: ${found:-none}; block 0 claimed: ${claimed:-no}; claims at the node: $held; files in $DIR)"
trap - EXIT
exit 1
