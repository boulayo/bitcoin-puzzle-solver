#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The whole network on one machine, on a bitcoind in regtest: a custodian node and three homes (pzpeer run, each with its pzsolve), a
# puzzle of 58 bits funded with 1 BTC, a block a second. It passes when a coincidence between two homes ends in the payout, half each,
# read back from the node. With three equal homes two coincidences in three are between homes; a home that finds the key on its own
# first (its own walkers meet too) starts a new try with a new puzzle, up to 6.
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
    # a puzzle of 58 bits, its key from the independent reference, its P2PKH address on regtest funded with 1 BTC
    K=$(python3 -c "import random; print(hex(random.SystemRandom().randrange(2**57, 2**58)))")
    PUB=$(python3 tools/ref.py "$K" | awk '$1=="pub"{print $2}'); H160=$(python3 tools/ref.py "$K" | awk '$1=="h160"{print $2}')
    PADDR=$(python3 -c "
import hashlib,sys
p=bytes([0x6f])+bytes.fromhex(sys.argv[1]); p+=hashlib.sha256(hashlib.sha256(p).digest()).digest()[:4]
n=int.from_bytes(p,'big'); a='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'; s=''
while n: n,r=divmod(n,58); s=a[r]+s
print('1'*(len(p)-len(p.lstrip(b'\0')))+s)" "$H160")
    rpc wallet sendtoaddress "[\"$PADDR\", 1]" >/dev/null
    PORT=$((30000 + 10 * try))
    printf 'net=e2e%d\npuzzle=0\npubkey=%s\nrange=200000000000000:3ffffffffffffff\njumpseed=%s\nschema=rete-06\ndpbits=14\nchain=regtest\nentry=127.0.0.1:%d\n' \
        "$try" "$PUB" "$(python3 -c 'import os; print(os.urandom(32).hex())')" "$PORT" > "$T/net.txt"
    "$BIN/pzpeer" netsign "$DIR/maint.key" "$T/net.txt" >> "$T/net.txt"
    export PZNET_TRUST=$(tail -1 "$T/net.txt" | cut -c5-68)
    declare -A PAY PID
    for h in a b c; do PAY[$h]=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"'); done
    "$BIN/pzpeer" node --network "$T/net.txt" --seed-hex "$(python3 -c 'import os; print(os.urandom(32).hex())')" --dir "$T/n" \
        --listen $PORT > "$T/n.log" 2>&1 &
    NPID=$!
    sleep 3
    port=$PORT
    for h in a b c; do
        port=$((port + 1))
        timeout 600 "$BIN/pzpeer" run --network "$T/net.txt" --dir "$T/$h" --payout "${PAY[$h]}" --rpc "$PZ_REGTEST" --listen $port \
            --pzsolve "$BIN/pzsolve" --pzfirma "$BIN/pzfirma" --exit-when-done -- -t 1 --no-gpu > "$T/$h.log" 2>&1 &
        PID[$h]=$!
    done
    wait "${PID[a]}" "${PID[b]}" "${PID[c]}"
    kill $NPID 2>/dev/null
    echo "== try $try"; for h in a b c; do grep -h -E "session|pzsolve ended|home stopped" "$T/$h.log" | sed "s/^/   $h /"; done
    PAID=$(grep -l "PAID" "$T"/[abc].log | sed 's|.*/\(.\)\.log|\1|' | tr '\n' ' ')
    if [ "$(echo $PAID | wc -w)" -ge 2 ]; then
        set -- $PAID
        sleep 2
        G1=$(rpc wallet getreceivedbyaddress "[\"${PAY[$1]}\", 1]"); G2=$(rpc wallet getreceivedbyaddress "[\"${PAY[$2]}\", 1]")
        LEFT=$(rpc scantxoutset "[\"start\", [\"addr($PADDR)\"]]" | python3 -c 'import json,sys; print(json.load(sys.stdin)["total_amount"])')
        echo "   received: home $1 $G1 BTC, home $2 $G2 BTC; left on the puzzle address $LEFT BTC"
        python3 -c "import sys; a,b,l=map(float,sys.argv[1:]); sys.exit(0 if 0.4998<a<0.5001 and 0.4998<b<0.5001 and l==0 else 1)" "$G1" "$G2" "$LEFT" &&
            { echo "E2E NET PASSED (try $try)"; exit 0; }
        echo "E2E NET FAILED: the amounts are not half each"; exit 1
    fi
    grep -q "found the key on its own" "$T"/[abc].log && { echo "   a home found the key on its own: a new try"; continue; }
    echo "E2E NET FAILED: no payout and no key (logs in $T)"; trap - EXIT; exit 1
done
echo "E2E NET FAILED: 6 tries, each solved by one home alone"
exit 1
