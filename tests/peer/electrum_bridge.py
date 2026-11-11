#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""electrum_bridge.py - a small Electrum server over a bitcoind, for the tests of the light chain of pzpeer (chain_spv.c).

usage: electrum_bridge.py LISTEN_PORT HOST:PORT:USER:PASSWORD

It answers the calls pzpeer makes (headers, transactions, Merkle proofs, the coins and the history of a script, broadcast, fee estimate) from
the blocks of the node, which it reads whole: good for a regtest chain of a few hundred blocks, not for a real one.
"""
import base64, hashlib, json, socketserver, sys, threading, urllib.request

HOST, PORT, USER, PW = sys.argv[2].split(":", 3)
LOCK = threading.Lock()
BLOCKS = []          # by height: (hash, header hex, [txid])
TXS = {}             # txid -> (hex, height, vin [(txid, vout)], vout [(scripthash, sats)])


def rpc(method, *params):
    req = urllib.request.Request("http://%s:%s/" % (HOST, PORT), json.dumps({"id": 1, "method": method, "params": params}).encode())
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (USER, PW)).encode()).decode())
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r)["result"]
    except urllib.error.HTTPError as e:
        raise ValueError(json.load(e)["error"]["message"])


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def entry(tx, height):
    vin = [(i["txid"], i["vout"]) for i in tx["vin"] if "txid" in i]
    vout = [(hashlib.sha256(bytes.fromhex(o["scriptPubKey"]["hex"])).digest()[::-1].hex(), round(o["value"] * 1e8)) for o in tx["vout"]]
    return (tx["hex"], height, vin, vout)


def view():
    """The confirmed transactions (kept between calls) and those of the mempool (read again every time), as one map."""
    tip = rpc("getblockcount")
    while BLOCKS and (len(BLOCKS) - 1 > tip or rpc("getblockhash", len(BLOCKS) - 1) != BLOCKS[-1][0]):   # a reorganization
        for t in BLOCKS.pop()[2]:
            TXS.pop(t, None)
    while len(BLOCKS) <= tip:
        h = rpc("getblockhash", len(BLOCKS))
        b = rpc("getblock", h, 2)
        for tx in b["tx"]:
            TXS[tx["txid"]] = entry(tx, len(BLOCKS))
        BLOCKS.append((h, rpc("getblockheader", h, False), [tx["txid"] for tx in b["tx"]]))
    txs = dict(TXS)
    for t in rpc("getrawmempool"):
        try:
            txs[t] = entry(rpc("getrawtransaction", t, 1), 0)
        except ValueError:
            pass                                               # confirmed meanwhile
    return txs


def branch(ids, pos):
    level, out = [bytes.fromhex(i)[::-1] for i in ids], []
    while len(level) > 1:
        if len(level) & 1:
            level.append(level[-1])
        out.append(level[pos ^ 1][::-1].hex())
        level = [dsha(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
        pos >>= 1
    return out


def answer(method, p):
    if method == "server.version":
        return ["bridge", "1.4"]
    if method == "blockchain.transaction.broadcast":
        return rpc("sendrawtransaction", p[0])
    if method == "blockchain.estimatefee":
        return rpc("estimatesmartfee", p[0]).get("feerate", -1)
    txs = view()
    if method == "blockchain.headers.subscribe":
        return {"height": len(BLOCKS) - 1, "hex": BLOCKS[-1][1]}
    if method == "blockchain.block.headers":
        hs = BLOCKS[p[0]:p[0] + min(p[1], 2016)]
        return {"hex": "".join(h[1] for h in hs), "count": len(hs), "max": 2016}
    if method == "blockchain.transaction.get":
        return txs[p[0]][0]
    if method == "blockchain.transaction.get_merkle":
        ids = BLOCKS[p[1]][2]
        return {"block_height": p[1], "pos": ids.index(p[0]), "merkle": branch(ids, ids.index(p[0]))}
    if method == "blockchain.scripthash.listunspent":
        spent = {i for t in txs.values() for i in t[2]}
        return [{"tx_hash": t, "tx_pos": n, "height": v[1], "value": o[1]}
                for t, v in txs.items() for n, o in enumerate(v[3]) if o[0] == p[0] and (t, n) not in spent]
    if method == "blockchain.scripthash.get_history":
        mine = {t for t, v in txs.items() if any(o[0] == p[0] for o in v[3])}
        mine |= {t for t, v in txs.items() if any(i[0] in mine and txs[i[0]][3][i[1]][0] == p[0] for i in v[2])}
        return sorted(({"tx_hash": t, "height": txs[t][1]} for t in mine), key=lambda e: (e["height"] == 0, e["height"]))
    raise ValueError("unknown method")


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            try:
                q = json.loads(line)
            except ValueError:
                return
            out = {"jsonrpc": "2.0", "id": q.get("id")}
            try:
                with LOCK:
                    out["result"] = answer(q.get("method"), q.get("params") or [])
            except Exception as e:                             # an unknown transaction, a height out of the chain, the node's refusal
                out["error"] = {"code": 1, "message": str(e)}
            self.wfile.write((json.dumps(out) + "\n").encode())


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


view()                                                         # the whole chain is read before the first client comes: it takes a while
Server(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
