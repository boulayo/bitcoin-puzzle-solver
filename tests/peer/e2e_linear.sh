#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The shared linear search on one machine (docs/en/NETWORK.md 11): a custodian node and two homes (pzpeer run on a network with an address,
# each with its pzsolve) over two blocks of 2^34 keys, the key of the test in block 1. Home a starts on block 0 and home b on block 1 (the
# file "block" of a home is the block it resumes). It passes when home b finds the key, in FOUND.txt, and home a ends block 0: claimed,
# with its claim held by the node, or, about one time in ten at this size, with fewer proof keys than a claim needs (then not claimed,
# and said so). A block is 5 to 20 minutes of 2 threads, by the machine (each home has 40 minutes). No bitcoind: a linear search pays
# nothing.
# Usage: tests/peer/e2e_linear.sh    (pzsolve and pzpeer built)
set -u
BIN=$(pwd)/build
DIR=$(mktemp -d /tmp/pzlin.XXXXXX)
stop() { kill $(jobs -p) 2>/dev/null; wait 2>/dev/null; }
trap 'stop; rm -rf "$DIR"' EXIT
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
block=0
for h in a b; do
    port=$((port + 1))
    mkdir -m 700 "$DIR/$h" && echo $block > "$DIR/$h/block"
    timeout 2400 "$BIN/pzpeer" run --network "$DIR/net.txt" --dir "$DIR/$h" --listen $port --no-portmap \
        --pzsolve "$BIN/pzsolve" --debug "$DIR/$h.debug" -- -t 2 --no-gpu > "$DIR/$h.log" 2>&1 &
    block=$((block + 1))
    sleep 1
done
found= ended=
for t in $(seq 1 2400); do
    [ -f "$DIR/b/campaigns/b1/FOUND.txt" ] && found=1
    ended=$(grep -h "block 0 scanned" "$DIR/a.log" | head -1)
    [ -n "$found" ] && [ -n "$ended" ] && break
    sleep 1
done
sleep 3
stop
for h in a b; do echo "--- home $h"; sed 's/^/   /' "$DIR/$h.log" | grep -v "^   *$" | tail -8; done
held=$(( $(wc -c < "$DIR/n/claims.bin" 2>/dev/null || echo 40) > 40 ))
case "$ended" in
*"and claimed"*) claimed=$held ;;
*"not claimed"*) claimed=1; echo "   block 0 had too few proof keys to be claimed (about one time in ten with blocks of 2^34 keys)" ;;
*) claimed=0 ;;
esac
if [ -n "$found" ] && grep -qi "$(printf '%x' "$K" | sed 's/^0*//')" "$DIR/b/campaigns/b1/FOUND.txt" && [ "$claimed" = 1 ]; then
    echo "   the key found by home b; $ended; the node holds the claims: $held"
    echo "E2E LINEAR PASSED"
    exit 0
fi
echo "E2E LINEAR FAILED (key found: ${found:-no}; block 0: ${ended:-not ended}; claims at the node: $held; files in $DIR)"
trap - EXIT
exit 1
