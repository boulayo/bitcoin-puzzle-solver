#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The whole network on one machine, on a bitcoind in regtest: a custodian node and two homes (pzpeer run, each with its pzsolve), a puzzle
# of 48 bits funded with 1 BTC, a block a second. It passes when a coincidence between the two homes ends in the payout, half each, read
# back from the node. A home that finds the key on its own first (about one try in two: its own walkers meet too) starts a new try with a
# new puzzle, up to 6.
# Usage: PZ_REGTEST=host:port:user:password tests/peer/e2e_net.sh    (build/pzsolve, build/pzpeer and build/pzfirma built)
set -u
IFS=: read -r RH RP RU RW <<< "${PZ_REGTEST:?set PZ_REGTEST=host:port:user:password of a bitcoind in regtest}"
BIN=$(pwd)/build
DIR=$(mktemp -d /tmp/pznet.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DIR"' EXIT
rpc() {   # rpc [wallet] METHOD PARAMS_JSON: the result as JSON text
    local path=/ m=$1 p=$2
    [ "$m" = wallet ] && { path=/wallet/pz; m=$2; p=$3; }
    curl -s --user "$RU:$RW" --data-binary "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"$m\",\"params\":$p}" "http://$RH:$RP$path" |
        python3 -c 'import json,sys; r=json.load(sys.stdin); print(json.dumps(r["result"]) if r["error"] is None else "ERROR " + json.dumps(r["error"]))'
}
rpc createwallet '["pz"]' >/dev/null
MINE=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"')
rpc generatetoaddress "[101, \"$MINE\"]" >/dev/null
( while sleep 1; do rpc generatetoaddress "[1, \"$MINE\"]" >/dev/null; done ) &

python3 -c "import os; print(os.urandom(32).hex())" > "$DIR/maint.key"
for try in 1 2 3 4 5 6; do
    T="$DIR/try$try"; mkdir -p "$T"
    # a puzzle of 48 bits, its key from the independent reference, its P2PKH address on regtest funded with 1 BTC
    K=$(python3 -c "import random; print(hex(random.SystemRandom().randrange(2**47, 2**48)))")
    PUB=$(python3 tools/ref.py "$K" | awk '$1=="pub"{print $2}'); H160=$(python3 tools/ref.py "$K" | awk '$1=="h160"{print $2}')
    PADDR=$(python3 -c "
import hashlib,sys
p=bytes([0x6f])+bytes.fromhex(sys.argv[1]); p+=hashlib.sha256(hashlib.sha256(p).digest()).digest()[:4]
n=int.from_bytes(p,'big'); a='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'; s=''
while n: n,r=divmod(n,58); s=a[r]+s
print('1'*(len(p)-len(p.lstrip(b'\0')))+s)" "$H160")
    rpc wallet sendtoaddress "[\"$PADDR\", 1]" >/dev/null
    PORT=$((30000 + 10 * try))
    printf 'net=e2e%d\npuzzle=0\npubkey=%s\nrange=800000000000:ffffffffffff\njumpseed=%s\nschema=rete-06\ndpbits=10\nchain=regtest\nentry=127.0.0.1:%d\n' \
        "$try" "$PUB" "$(python3 -c 'import os; print(os.urandom(32).hex())')" "$PORT" > "$T/net.txt"
    "$BIN/pzpeer" netsign "$DIR/maint.key" "$T/net.txt" >> "$T/net.txt"
    export PZNET_TRUST=$(tail -1 "$T/net.txt" | cut -c5-68)
    PAY_A=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"'); PAY_B=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"')
    "$BIN/pzpeer" node --network "$T/net.txt" --seed-hex "$(python3 -c 'import os; print(os.urandom(32).hex())')" --dir "$T/c" \
        --listen $PORT > "$T/c.log" 2>&1 &
    CPID=$!
    sleep 3
    for h in a b; do
        pay=$PAY_A port=$((PORT + 1)); [ $h = b ] && pay=$PAY_B port=$((PORT + 2))
        timeout 600 "$BIN/pzpeer" run --network "$T/net.txt" --dir "$T/$h" --payout "$pay" --rpc "$PZ_REGTEST" --listen $port \
            --pzsolve "$BIN/pzsolve" --pzfirma "$BIN/pzfirma" --exit-when-done -- -t 1 --no-gpu > "$T/$h.log" 2>&1 &
        eval "PID_$h=\$!"
    done 2>/dev/null
    wait "$PID_a" "$PID_b"
    kill $CPID 2>/dev/null
    echo "== try $try"; grep -h -E "session|pzsolve ended|home stopped" "$T/a.log" "$T/b.log" | sed 's/^/   /'
    if grep -q "PAID" "$T/a.log" && grep -q "PAID" "$T/b.log"; then
        sleep 2
        GA=$(rpc wallet getreceivedbyaddress "[\"$PAY_A\", 1]"); GB=$(rpc wallet getreceivedbyaddress "[\"$PAY_B\", 1]")
        LEFT=$(rpc scantxoutset "[\"start\", [\"addr($PADDR)\"]]" | python3 -c 'import json,sys; print(json.load(sys.stdin)["total_amount"])')
        echo "   received: home a $GA BTC, home b $GB BTC; left on the puzzle address $LEFT BTC"
        python3 -c "import sys; a,b,l=map(float,sys.argv[1:]); sys.exit(0 if 0.4998<a<0.5001 and 0.4998<b<0.5001 and l==0 else 1)" "$GA" "$GB" "$LEFT" &&
            { echo "E2E NET PASSED (try $try)"; exit 0; }
        echo "E2E NET FAILED: the amounts are not half each"; exit 1
    fi
    grep -q "found the key on its own" "$T/a.log" "$T/b.log" && { echo "   a home found the key on its own: a new try"; continue; }
    echo "E2E NET FAILED: no payout and no key (logs in $T)"; trap - EXIT; exit 1
done
echo "E2E NET FAILED: 6 tries, each solved by one home alone"
exit 1
