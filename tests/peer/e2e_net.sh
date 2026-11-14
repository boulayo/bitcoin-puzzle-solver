#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# The whole network on one machine, on a bitcoind in regtest: a custodian node and three homes (pzpeer run, each with its pzsolve), a
# puzzle of 58 bits funded with 1 BTC, a block a second. It passes when a coincidence between two homes ends in the payout, 45% each and
# 10% to the author, read back from the node, and then in the key of the puzzle in FOUND.txt of both, once they gave each other their
# pieces. With three equal homes two coincidences in three are between homes; a home that finds the key on its own first (its own walkers
# meet too) sends the coins, 90% to itself and 10% to the author, which is checked, and a new try starts with a new puzzle, up to 12. Such a
# home sends them at once; here 90 seconds later (--solo-wait), so that a pair that met meanwhile is paid first: the coincidences between
# homes come later than those within one (in these tries a home found the key on its own in 7 to 60 seconds, the first coincidence of
# two homes after about 40).
# The two homes get the same share, of the prize and of F's tiny coin (the odd satoshi to S). With several coincidences at once more
# than one pair may get its first transaction confirmed, each on the prize the one before sent back to the puzzle address: the last
# pair is paid, and the prize is smaller by the fee and the tiny coin of each first transaction (about 15 000 satoshis at 20 sat/vB).
# With PZ_E2E_LIGHT=1 the homes have no node: they read the chain from two Electrum servers (tests/peer/electrum_bridge.py over the same
# bitcoind) and check every answer themselves (chain_spv.c).
# With PZ_E2E_WIN=1 the three programs are the Windows ones of build/win, run under Wine (the bitcoind stays the one of this machine).
# With PZ_E2E_MEET=1 nobody is given an address, as on the first day of a network: no custodian node and no entry line; the homes find
# each other through a stand-in of the table of BitTorrent (tests/peer/bt_table.py), and the first runs alone for PZ_E2E_ALONE seconds
# (default 30) before the two others start, on a puzzle of 62 bits that it does not solve alone in that time.
# Usage: PZ_REGTEST=host:port:user:password [PZ_E2E_LIGHT=1] [PZ_E2E_WIN=1] [PZ_E2E_MEET=1] tests/peer/e2e_net.sh    (pzsolve, pzpeer and pzsign built)
set -u
IFS=: read -r RH RP RU RW <<< "${PZ_REGTEST:?set PZ_REGTEST=host:port:user:password of a bitcoind in regtest}"
BIN=$(pwd)/build; EXE=; RUN=
if [ -n "${PZ_E2E_WIN:-}" ]; then BIN=$BIN/win; EXE=.exe; RUN=wine; fi   # the Windows programs (make windows winsigner), under Wine
DIR=$(mktemp -d /tmp/pznet.XXXXXX)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DIR"' EXIT
rpc() {   # rpc [wallet] METHOD PARAMS_JSON: the result as JSON text
    local path=/ m=$1 p=$2
    [ "$m" = wallet ] && { path=/wallet/pz; m=$2; p=$3; }
    curl -s --user "$RU:$RW" --data-binary "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"$m\",\"params\":$p}" "http://$RH:$RP$path" |
        python3 -c 'import json,sys; r=json.load(sys.stdin); print(json.dumps(r["result"]) if r["error"] is None else "ERROR " + json.dumps(r["error"]))'
}
rpc createwallet '["pz"]' >/dev/null; rpc loadwallet '["pz"]' >/dev/null   # made by an earlier run: loaded again after a restart of the node
MINE=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"')
case $MINE in ERROR*|"") echo "E2E NET FAILED: the node gives no address: $MINE"; exit 1;; esac
rpc generatetoaddress "[101, \"$MINE\"]" >/dev/null
( while sleep 1; do rpc generatetoaddress "[1, \"$MINE\"]" >/dev/null; done ) &

# pzsolve writes its points to dp.bin at its saves, every 60 seconds: a home publishes nothing before. A home alone solves the puzzle of
# the test in half a minute on a fast machine, so its pzsolve saves every 3 seconds here (the environment reaches it through pzpeer)
export PZ_TEST_CHECKPOINT_INTERVAL=3
CHAIN=(--rpc "$PZ_REGTEST")
MEET=()
if [ -n "${PZ_E2E_MEET:-}" ]; then
    BT=$((${PZ_E2E_PORT:-30000} + 800))
    python3 tests/peer/bt_table.py $BT > "$DIR/table.log" 2>&1 &
    MEET=(--bt-router 127.0.0.1:$BT)
fi
if [ -n "${PZ_E2E_LIGHT:-}" ]; then
    EP=$((${PZ_E2E_PORT:-30000} + 900))
    for i in 0 1; do python3 tests/peer/electrum_bridge.py $((EP + i)) "$PZ_REGTEST" > "$DIR/bridge$i.log" 2>&1 & done
    CHAIN=(--electrum 127.0.0.1:$EP --electrum 127.0.0.1:$((EP + 1)))
    for i in 0 1; do                                           # a bridge listens once it has read the chain: minutes on a long one
        timeout 900 bash -c "until (exec 3<>/dev/tcp/127.0.0.1/$((EP + i))) 2>/dev/null; do sleep 1; done" ||
            { echo "E2E NET FAILED: the Electrum bridge did not start (see $DIR/bridge$i.log)"; trap - EXIT; exit 1; }
    done
fi

# the satoshis at a script, from the node's set of unspent outputs; the author's share goes to the script of its address (src/peer/payout.c)
AUTHOR=00144b944a184c8134ef3c6e0241a9ea8bedec57eb5e
at() { rpc scantxoutset "[\"start\", [\"$1\"]]" | python3 -c 'import json,sys; print(json.load(sys.stdin)["total_amount"])'; }
for try in 1 2 3 4 5 6 7 8 9 10 11 12; do
    T="$DIR/try$try"; mkdir -p "$T"
    # a puzzle of 58 bits (62 in PZ_E2E_MEET, where one home runs alone first), its key from the independent reference, its P2PKH
    # address on regtest funded with 1 BTC
    B=$([ -n "${PZ_E2E_MEET:-}" ] && echo 62 || echo 58)
    K=$(python3 -c "import random; print(hex(random.SystemRandom().randrange(2**($B-1), 2**$B)))")
    PUB=$(python3 tools/ref.py "$K" | awk '$1=="pub"{print $2}'); H160=$(python3 tools/ref.py "$K" | awk '$1=="h160"{print $2}')
    PADDR=$(python3 -c "
import hashlib,sys
p=bytes([0x6f])+bytes.fromhex(sys.argv[1]); p+=hashlib.sha256(hashlib.sha256(p).digest()).digest()[:4]
n=int.from_bytes(p,'big'); a='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'; s=''
while n: n,r=divmod(n,58); s=a[r]+s
print('1'*(len(p)-len(p.lstrip(b'\0')))+s)" "$H160")
    rpc wallet sendtoaddress "[\"$PADDR\", 1]" >/dev/null
    PORT=$((${PZ_E2E_PORT:-30000} + 10 * try))      # PZ_E2E_PORT: another base when two runs share the machine
    printf 'net=e2e%d\npuzzle=0\npubkey=%s\nrange=%x:%x\njumpseed=%s\nschema=net-06\ndpbits=14\nchain=regtest\n' \
        "$try" "$PUB" $((1 << (B - 1))) $(((1 << B) - 1)) "$(python3 -c 'import os; print(os.urandom(32).hex())')" > "$T/net.txt"
    [ -z "${PZ_E2E_MEET:-}" ] && echo "entry=127.0.0.1:$PORT" >> "$T/net.txt"
    declare -A PAY PID
    for h in a b c; do PAY[$h]=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"'); done
    A0=$(at "raw($AUTHOR)")
    NPID=
    if [ -z "${PZ_E2E_MEET:-}" ]; then
        $RUN "$BIN/pzpeer$EXE" node --network "$T/net.txt" --dir "$T/n" \
            --listen $PORT --no-portmap > "$T/n.log" 2>&1 &
        NPID=$!
        sleep 3
    fi
    port=$PORT
    for h in a b c; do
        port=$((port + 1))
        timeout 600 $RUN "$BIN/pzpeer$EXE" run --network "$T/net.txt" --dir "$T/$h" --payout "${PAY[$h]}" "${CHAIN[@]}" "${MEET[@]}" --listen $port --no-portmap --accept-terms --solo-wait 90 \
            --pzsolve "$BIN/pzsolve$EXE" --pzsign "$BIN/pzsign$EXE" --debug "$T/$h.debug" --exit-when-done -- -t 1 --no-gpu > "$T/$h.log" 2>&1 &
        PID[$h]=$!
        [ "$h" = a ] && [ -n "${PZ_E2E_MEET:-}" ] && sleep "${PZ_E2E_ALONE:-30}"   # the first home of the network, alone
    done
    wait "${PID[a]}" "${PID[b]}" "${PID[c]}"
    [ -n "$NPID" ] && kill $NPID 2>/dev/null
    echo "== try $try"; for h in a b c; do grep -h -E "rendezvous|session|pzsolve|KEY|home stopped" "$T/$h.log" | head -24 | sed "s/^/   $h /"; done
    PAID=$(grep -l "ended: paid" "$T"/[abc].log | sed 's|.*/\(.\)\.log|\1|' | tr '\n' ' ')
    if [ "$(echo $PAID | wc -w)" -ge 2 ]; then
        set -- $PAID
        sleep 2
        G1=$(rpc wallet getreceivedbyaddress "[\"${PAY[$1]}\", 1]"); G2=$(rpc wallet getreceivedbyaddress "[\"${PAY[$2]}\", 1]")
        LEFT=$(at "addr($PADDR)"); A1=$(at "raw($AUTHOR)")
        echo "   received: home $1 $G1 BTC, home $2 $G2 BTC, the author $(python3 -c "import sys; print(f'{float(sys.argv[2])-float(sys.argv[1]):.8f}')" "$A0" "$A1") BTC; left on the puzzle address $LEFT BTC"
        # 45% each and 10% to the author of what the fees leave: the author gets a ninth of what the two homes got
        python3 -c "import sys; a,b,l,a0,a1=map(float,sys.argv[1:]); sys.exit(0 if min(a,b)>0.449 and abs(a-b)<2e-8 and a+b<0.9 and l==0 and abs(a1-a0-(a+b)/9)<3e-8 else 1)" \
            "$G1" "$G2" "$LEFT" "$A0" "$A1" || { echo "E2E NET FAILED: the amounts are not 45% each and 10% to the author"; exit 1; }
        # after the confirmations of the payout the two homes give each other their pieces: each has the key, in FOUND.txt
        for h in $1 $2; do
            KEY=$(awk '/^private key hex:/{print $4}' "$T/$h/FOUND.txt" 2>/dev/null)
            python3 -c "import sys; sys.exit(0 if sys.argv[1] and int(sys.argv[1], 16) == int(sys.argv[2], 16) else 1)" "${KEY:-}" "$K" ||
                { echo "E2E NET FAILED: home $h has not the key in FOUND.txt (logs in $T)"; trap - EXIT; exit 1; }
            SHOWN=$(sed -n 's/.*THE KEY OF THE PUZZLE: \(0x[0-9a-f]*\).*/\1/p' "$T/$h.log" | head -1)
            python3 -c "import sys; sys.exit(0 if sys.argv[1] and int(sys.argv[1], 16) == int(sys.argv[2], 16) else 1)" "${SHOWN:-}" "$K" ||
                { echo "E2E NET FAILED: home $h did not show the key"; trap - EXIT; exit 1; }
        done
        echo "   the key, in FOUND.txt of both homes: $K"
        echo "E2E NET PASSED (try $try)"; exit 0
    fi
    if grep -q "found the key on its own" "$T"/[abc].log; then
        # it sent the coins of the puzzle at once: 90% to its payout address, 10% to the author
        sleep 2
        R=0; for h in a b c; do R=$(python3 -c "import sys; print(float(sys.argv[1]) + float(sys.argv[2]))" "$R" "$(rpc wallet getreceivedbyaddress "[\"${PAY[$h]}\", 1]")"); done
        LEFT=$(at "addr($PADDR)"); A1=$(at "raw($AUTHOR)")
        echo "   a home found the key on its own and sent the coins: $R BTC to its payout address, the author $A0 -> $A1 BTC, left $LEFT BTC"
        python3 -c "import sys; r,l,a0,a1=map(float,sys.argv[1:]); sys.exit(0 if r>0.899 and l==0 and abs(a1-a0-r/9)<3e-8 else 1)" "$R" "$LEFT" "$A0" "$A1" ||
            { echo "E2E NET FAILED: the home that found the key on its own did not send 90% to itself and 10% to the author (logs in $T)"; trap - EXIT; exit 1; }
        echo "   a new try"; continue
    fi
    echo "E2E NET FAILED: no payout and no key (logs in $T)"; trap - EXIT; exit 1
done
echo "E2E NET FAILED: 12 tries, each solved by one home alone"
exit 1
