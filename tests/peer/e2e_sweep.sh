#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# pzpeer sweep on a bitcoind in regtest: a key with two coins, the transaction that moves them, which the node would take
# (testmempoolaccept) and which pzpeer did not send (the mempool does not have it).
# Usage: PZ_REGTEST=host:port:user:password tests/peer/e2e_sweep.sh    (build/pzpeer built)
set -u
IFS=: read -r RH RP RU RW <<< "${PZ_REGTEST:?set PZ_REGTEST=host:port:user:password of a bitcoind in regtest}"
rpc() {   # rpc [wallet] METHOD PARAMS_JSON: the result as JSON text
    local path=/ m=$1 p=$2
    [ "$m" = wallet ] && { path=/wallet/pz; m=$2; p=$3; }
    curl -s --user "$RU:$RW" --data-binary "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"$m\",\"params\":$p}" "http://$RH:$RP$path" |
        python3 -c 'import json,sys; r=json.load(sys.stdin); print(json.dumps(r["result"]) if r["error"] is None else "ERROR " + json.dumps(r["error"]))'
}
rpc createwallet '["pz"]' >/dev/null; rpc loadwallet '["pz"]' >/dev/null
MINE=$(rpc wallet getnewaddress '["", "legacy"]' | tr -d '"')
newkey() { python3 -c "import random; print('%064x' % random.SystemRandom().randrange(2**70, 2**71))"; }
addr_of() {   # the regtest address of a key
    python3 -c "
import hashlib,sys
p=bytes([0x6f])+bytes.fromhex(sys.argv[1]); p+=hashlib.sha256(hashlib.sha256(p).digest()).digest()[:4]
n=int.from_bytes(p,'big'); a='123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz'; s=''
while n: n,r=divmod(n,58); s=a[r]+s
print('1'*(len(p)-len(p.lstrip(b'\0')))+s)" "$(python3 tools/ref.py "0x$1" | awk '$1=="h160"{print $2}')"
}
K=$(newkey); ADDR=$(addr_of "$K")
rpc generatetoaddress "[101, \"$MINE\"]" >/dev/null
rpc wallet sendtoaddress "[\"$ADDR\", 0.7]" >/dev/null; rpc wallet sendtoaddress "[\"$ADDR\", 0.3]" >/dev/null
rpc generatetoaddress "[1, \"$MINE\"]" >/dev/null
OUT=$(echo "$K" | build/pzpeer sweep --chain regtest --rpc "$PZ_REGTEST" --to "$MINE" --feerate 80) || { echo "E2E SWEEP FAILED: $OUT"; exit 1; }
HEX=$(echo "$OUT" | grep -E '^[0-9a-f]{100,}$'); TXID=$(echo "$OUT" | awk '$1=="txid:"{print $2}')
echo "$OUT" | sed -n 1,2p
ACC=$(rpc testmempoolaccept "[[\"$HEX\"]]")
echo "$ACC" | python3 -c "
import json,sys
r=json.load(sys.stdin)[0]
ok = r['allowed'] and r['txid']==sys.argv[1] and abs(r['fees']['base']*1e8/r['vsize']-80) < 1
print('   the node would take it: %s, fee %.0f sat/vB' % (r['allowed'], r['fees']['base']*1e8/r['vsize'])); sys.exit(0 if ok else 1)" "$TXID" || { echo "E2E SWEEP FAILED: $ACC"; exit 1; }
rpc getrawmempool '[]' | grep -q "$TXID" && { echo "E2E SWEEP FAILED: the transaction was sent"; exit 1; }
echo "$OUT" | grep -q "NOT SENT" && echo "0000" | build/pzpeer sweep --chain regtest --rpc "$PZ_REGTEST" --to "$MINE" 2>&1 | grep -q "64 hex digits" || { echo "E2E SWEEP FAILED: the messages"; exit 1; }
# an address with more coins than a transaction holds, two of them dust (a puzzle address collects dozens): the 16 largest go, the
# node would take the transaction, and the rest is named (before: "the coins of this key do not pay the fee")
K=$(newkey); ADDR=$(addr_of "$K")
for i in $(seq 1 18); do rpc wallet sendtoaddress "[\"$ADDR\", 0.0$((10 + i))]" >/dev/null; done
rpc wallet sendtoaddress "[\"$ADDR\", 0.00000600]" >/dev/null; rpc wallet sendtoaddress "[\"$ADDR\", 0.00000700]" >/dev/null
rpc generatetoaddress "[1, \"$MINE\"]" >/dev/null
OUT=$(echo "$K" | build/pzpeer sweep --chain regtest --rpc "$PZ_REGTEST" --to "$MINE" --feerate 80) || { echo "E2E SWEEP FAILED (20 coins): $OUT"; exit 1; }
HEX=$(echo "$OUT" | grep -E '^[0-9a-f]{100,}$')
echo "$OUT" | sed -n 1p
echo "$OUT" | grep -q "^coins: 16, 32800000 satoshis" && echo "$OUT" | grep -q "^4 smaller coins are left" && rpc testmempoolaccept "[[\"$HEX\"]]" | grep -q '"allowed": true' \
    || { echo "E2E SWEEP FAILED: 20 coins: $(echo "$OUT" | grep -E '^coins|left')"; exit 1; }
echo "E2E SWEEP PASSED"
