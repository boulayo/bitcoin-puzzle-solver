#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""test.py - the tests of `pzsign session` (signer/README.md): two pzsign processes with this script as their pzpeer, which relays the
messages of each to the other and changes, drops or replays them. Signatures are checked with the curve of tools/ref.py, which shares
no code with pzsign.

usage: python3 -I signer/test.py PZSIGN...      (PZSIGN may be a command, as in: wine build/win/pzsign.exe)
"""
import io, os, secrets, struct, subprocess, sys, threading

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import ref  # noqa: E402

CMD = sys.argv[1:]
JOB, FROM_PARTNER, TO_PARTNER, RESULT, ERROR, UNPAID = 0x01, 0x02, 0x81, 0x82, 0x83, 0x84
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print('  FAIL', what)


def frame(kind, payload):
    return struct.pack('>IB', len(payload) + 1, kind) + payload


def read_frame(f):
    head = f.read(5)
    if len(head) < 5:
        return None
    n, kind = struct.unpack('>IB', head)
    return kind, f.read(n - 1)


def job(i, first, eid, share, pubs, key, digests, version=2):
    return bytes([version, i, first]) + eid + share.to_bytes(32, 'big') + b''.join(pubs) + key + b''.join(digests)


def setup(n=3, first=0):
    """Two jobs for a random key k = x0 + x1, n digests (the last one 0xff..ff, above the group order) and the party that is first."""
    x = [secrets.randbelow(ref.N - 1) + 1 for _ in range(2)]
    k = (x[0] + x[1]) % ref.N
    eid = secrets.token_bytes(32)
    digests = [secrets.token_bytes(32) for _ in range(n - 1)] + [b'\xff' * 32]
    pubs = [ref.pub33(v) for v in x]
    jobs = [job(i, first, eid, x[i], pubs, ref.pub33(k), digests) for i in range(2)]
    return jobs, k, digests


def verifies(k, digest, rs):
    """ECDSA verification with the reference curve, on the digest as it is (reduced mod n as ECDSA does), and s low."""
    r, s = int.from_bytes(rs[:32], 'big'), int.from_bytes(rs[32:], 'big')
    if not (0 < r < ref.N and 0 < s <= ref.N // 2):
        return False
    e, w = int.from_bytes(digest, 'big') % ref.N, ref.inv(s, ref.N)
    pt = ref.add(ref.mul(e * w % ref.N), ref.mul(r * w % ref.N, ref.mul(k)))
    return pt is not None and pt[0] % ref.N == r


def session(jobs, change=None, sides=(0, 1), script=None):
    """Runs the parties of `sides` as pzsign processes; `script` stands for the other one: the list of envelopes it sends, one after each
    message it gets. change(side, index, envelope) returns the envelope to deliver; None drops it and ends the partner's input, as pzpeer
    does when the messages of a partner stop. Returns the answers [(kind, payload)]
    and the envelopes each side sent."""
    procs = {s: subprocess.Popen(CMD + ['session'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) for s in sides}
    answers, sent, lock = [None, None], [[], []], threading.Lock()

    def deliver(to, env):
        with lock:
            if to in procs and not procs[to].stdin.closed:
                try:
                    procs[to].stdin.write(frame(FROM_PARTNER, env))
                    procs[to].stdin.flush()
                except OSError:                        # it has ended
                    pass

    def end(to):                                       # what pzpeer does when the partner's messages end
        with lock:
            if to in procs and not procs[to].stdin.closed:
                try:
                    procs[to].stdin.close()
                except OSError:
                    pass

    def pump(s):
        while True:
            f = read_frame(procs[s].stdout)
            if f is None or f[0] != TO_PARTNER:
                answers[s] = f
                end(1 - s)
                return
            env = f[1] if change is None else change(s, len(sent[s]), f[1])
            sent[s].append(f[1])
            if script is not None:
                if script:
                    deliver(s, script.pop(0))
            elif env is not None:
                deliver(1 - s, env)
            else:
                end(1 - s)

    for s in sides:
        procs[s].stdin.write(frame(JOB, jobs[s]))
        procs[s].stdin.flush()
    threads = [threading.Thread(target=pump, args=(s,)) for s in sides]
    for t in threads:
        t.start()
    for t in threads:
        t.join(120)
    for p in procs.values():
        p.kill() if p.poll() is None else None
        p.wait()
    return answers, sent


def signed(answers, k, digests):
    """The sides that answered with valid signatures; any other answer must be an error frame."""
    out = []
    for s, a in enumerate(answers):
        if a is not None and a[0] == RESULT:
            ok = len(a[1]) == 64 * len(digests) and all(verifies(k, d, a[1][64 * i:64 * i + 64]) for i, d in enumerate(digests))
            check(ok, f'side {s} answered with signatures that do not verify')
            out.append(s)
        else:
            check(a is not None and a[0] in (ERROR, UNPAID), f'side {s} ended without an error frame: {a}')
    return out


def main():
    print('both parties get the same valid signatures, with either of them first')
    for first in (1, 0):                                  # the last one, with P1 on side 0, is the model of the tests below
        jobs, k, digests = setup(first=first)
        answers, sent = session(jobs)
        check(signed(answers, k, digests) == [0, 1] and answers[0][1] == answers[1][1], f'party {first} first: {answers}')
    counts = [len(sent[0]), len(sent[1])]
    print(f'  messages of a session: {counts[0]} from P1, {counts[1]} from P2')

    print('every message counts: changed in its middle, damaged in its header, or dropped, it leaves no signature but on P1')
    for side in (0, 1):
        for index in range(counts[side]):
            for how in ('middle', 'header', 'drop'):
                def change(s, i, env, how=how, side=side, index=index):
                    if (s, i) != (side, index):
                        return env
                    if how == 'drop':
                        return None
                    at = len(env) // 2 if how == 'middle' else secrets.randbelow(3)
                    return env[:at] + bytes([env[at] ^ 0x01]) + env[at + 1:]
                jobs, k, digests = setup()
                answers, _ = session(jobs, change)
                got = signed(answers, k, digests)
                last = side == 0 and index == counts[0] - 1     # the signatures themselves, which P1 already has
                check(got == ([0] if last else []), f'message {index} of side {side}, {how}: signatures on {got}')
                # P2 says it only lacked the signatures (0x84, not counted by pzpeer) once it gave all its messages, and only then
                done = last or (side == 1 and index == counts[1] - 1)
                end = answers[1][0] if answers[1] else None
                check(end == (UNPAID if done else ERROR), f'message {index} of side {side}, {how}: P2 ended with {end}')

    print('a partner with another digest, or in another session, gets nothing')
    jobs, k, digests = setup()
    other, _, _ = setup()
    answers, _ = session([jobs[0], jobs[1][:-32] + secrets.token_bytes(32)])
    check(signed(answers, k, digests) == [], 'another digest')
    answers, _ = session([jobs[0], jobs[1][:3] + secrets.token_bytes(32) + jobs[1][35:]])
    check(signed(answers, k, digests) == [], 'another session id')

    print('a replayed session gets nothing: the messages of P2 from a session, given again to P1 in the same job')
    jobs, k, digests = setup()
    answers, sent = session(jobs)
    answers, _ = session(jobs, sides=(0,), script=list(sent[1]))
    check(signed([answers[0], (ERROR, b'')], k, digests) == [], 'replay')

    print('a message far larger than its phase allows is refused before the library reads it')
    jobs, k, digests = setup()
    answers, _ = session(jobs, lambda s, i, env: env[:3] + bytes((1 << 20) - 16) if (s, i) == (0, 0) else env)
    check(signed(answers, k, digests) == [] and answers[1] is not None and b'bytes from the partner' in answers[1][1], f'large message: {answers[1]}')

    print('a share that is not its piece, pieces that do not add up to the key, malformed jobs: refused')
    jobs, k, digests = setup()
    j = jobs[0]
    bad = [j[:35] + ((int.from_bytes(j[35:67], 'big') + 1) % ref.N).to_bytes(32, 'big') + j[67:],   # share + 1
           j[:100] + ref.pub33(5) + j[133:],                                                       # pub[1] another point
           j[:166], j + b'\x00', j[:166] + b'\x00' * 32 * 65, bytes([1]) + j[1:], j[:1] + b'\x02' + j[2:],
           j[:35] + b'\x00' * 32 + j[67:], j[:35] + ref.N.to_bytes(32, 'big') + j[67:], j[:67] + b'\x05' + j[68:], b'']
    for n, b in enumerate(bad):
        p = subprocess.run(CMD + ['session'], input=frame(JOB, b), capture_output=True, timeout=60)
        f = read_frame(io.BytesIO(p.stdout))
        check(p.returncode == 1 and f is not None and f[0] == ERROR, f'bad job {n}: exit {p.returncode}, {f}')
    p = subprocess.run(CMD + ['session'], input=frame(FROM_PARTNER, b'x'), capture_output=True, timeout=60)
    check(p.returncode == 1 and read_frame(io.BytesIO(p.stdout))[0] == ERROR, 'a first frame that is not a job')

    print('test.py: all OK' if not failures else f'test.py: {failures} FAILED')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
