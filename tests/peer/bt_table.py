#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""bt_table.py - one node of a table like that of BitTorrent (BEP 5), for the tests of the rendezvous of pzpeer (bt.c).

usage: bt_table.py PORT

It answers get_peers with a token and the peers announced under the key, and takes announce_peer with that token.
"""
import os, socket, sys

ME, SECRET, PEERS = os.urandom(20), os.urandom(4), {}


def decode(b, i=0):
    c = b[i:i + 1]
    if c == b"i":
        e = b.index(b"e", i)
        return int(b[i + 1:e]), e + 1
    if c in (b"l", b"d"):
        out, i = [], i + 1
        while b[i:i + 1] != b"e":
            v, i = decode(b, i)
            out.append(v)
        return (out if c == b"l" else dict(zip(out[::2], out[1::2]))), i + 1
    colon = b.index(b":", i)
    n = int(b[i:colon])
    return b[colon + 1:colon + 1 + n], colon + 1 + n


def encode(v):
    if isinstance(v, int):
        return b"i%de" % v
    if isinstance(v, bytes):
        return b"%d:%s" % (len(v), v)
    if isinstance(v, list):
        return b"l" + b"".join(map(encode, v)) + b"e"
    return b"d" + b"".join(encode(k) + encode(v[k]) for k in sorted(v)) + b"e"


s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", int(sys.argv[1])))
while True:
    msg, who = s.recvfrom(2048)
    try:
        q, _ = decode(msg)
        a, token = q[b"a"], bytes(x ^ y for x, y in zip(socket.inet_aton(who[0]), SECRET))
        r = {b"id": ME}
        if q[b"q"] == b"get_peers":
            r[b"token"] = token
            if PEERS.get(a[b"info_hash"]):
                r[b"values"] = sorted(PEERS[a[b"info_hash"]])
        elif q[b"q"] == b"announce_peer" and a[b"token"] == token and 0 < a[b"port"] < 65536:
            PEERS.setdefault(a[b"info_hash"], set()).add(socket.inet_aton(who[0]) + a[b"port"].to_bytes(2, "big"))
        else:
            continue
        s.sendto(encode({b"r": r, b"t": q[b"t"], b"y": b"r"}), who)
    except (KeyError, ValueError, IndexError, TypeError):
        pass
