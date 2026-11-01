#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
"""The hidden password prompt on a real (pseudo) terminal, for tests/e2e.sh: the echo is off while the password is typed and on again afterwards (also
after a Ctrl-C at the prompt), what is typed never appears in the output, two different passwords are refused, `hunt --encrypt-password` makes the
public key that `encrypt-keygen` makes for the same password and does not ask again when the campaign is resumed.
usage: tests/pty_seal.py PZSOLVE WORKDIR"""
import os, pty, re, select, signal, sys, termios, time

BIN, WORK = sys.argv[1], sys.argv[2]
ENV = dict(os.environ, PZ_TEST_SEAL_LOG2N="4", TERM="xterm")
fails = 0

def ok(msg):
    print("  OK   " + msg)
    return True
def bad(msg):
    global fails
    fails += 1
    print("  FAIL " + msg)

class Run:
    def __init__(self, argv):
        self.buf = ""
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvpe(argv[0], argv, ENV)
        self.status = None

    def read(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    return False
                if not d:
                    return False
                self.buf += d.decode("utf8", "replace")
        return True

    def expect(self, text, timeout=60):
        end = time.time() + timeout
        start = len(self.buf)
        while time.time() < end:
            if text in self.buf[max(0, start - len(text)):] or text in self.buf:
                return True
            if not self.read(0.2):
                return text in self.buf
        return False

    def echo_on(self):
        return bool(termios.tcgetattr(self.fd)[3] & termios.ECHO)

    def send(self, s):
        os.write(self.fd, s.encode())

    def finish(self, timeout=60):
        self.read_until_eof(timeout)
        _, st = os.waitpid(self.pid, 0)
        self.status = os.waitstatus_to_exitcode(st)
        return self.status

    def read_until_eof(self, timeout):
        end = time.time() + timeout
        while time.time() < end and self.read(0.2):
            pass

    def kill(self, sig=signal.SIGINT):
        try:
            os.kill(self.pid, sig)
        except ProcessLookupError:
            pass

def clean(s): return re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", s)

PW = "tr0ub4dor&3 zebra staple 9"

# 1. encrypt-keygen: the echo is off at the prompt, the password is not echoed, the terminal is as it was
r = Run([BIN, "encrypt-keygen"])
if not r.expect("Password: "): bad("keygen: no prompt")
else:
    time.sleep(0.2)
    off = not r.echo_on()
    r.send(PW + "\n")
    if not r.expect("Repeat the password: "): bad("keygen: no second prompt")
    r.send(PW + "\n")
    st = r.finish()
    out = clean(r.buf)
    m = re.search(r"public key:\s+([0-9a-f]{66})", out)
    off and ok("keygen: the echo is off while the password is typed") or bad("keygen: the echo was on at the prompt")
    PW not in out and ok("keygen: the typed password does not appear in the output") or bad("keygen: the password appears in the output")
    st == 0 and m and ok("keygen: public key printed, exit status 0") or bad("keygen: status %s output %r" % (st, out[-200:]))
    KEYGEN_Q = m.group(1) if m else ""
    try:
        r.echo_on() and ok("keygen: the terminal is as it was") or bad("keygen: the echo stayed off")
    except termios.error:
        ok("keygen: the terminal is gone with the process (state checked below)")

# 2. two different passwords are refused
r = Run([BIN, "encrypt-keygen"])
r.expect("Password: ")
r.send("first password one\n")
r.expect("Repeat the password: ")
r.send("another password two\n")
st = r.finish()
out = clean(r.buf)
st == 2 and "differ" in out and "public key:" not in out and ok("keygen: two different passwords are refused (exit status 2, no key)") \
    or bad("keygen mismatch: status %s output %r" % (st, out[-200:]))
"first password one" not in out and "another password two" not in out and ok("keygen: neither password appears in the output") or bad("keygen mismatch: password echoed")

# 3. Ctrl-C at the prompt: the terminal is restored. The shell around the program ignores the signal and shows the state of the terminal afterwards.
r = Run(["sh", "-c", "trap '' INT; %s encrypt-keygen; echo STATE; stty -a | tr ' ' '\\n' | grep -x -e echo -e -echo" % BIN])
if not r.expect("Password: "): bad("Ctrl-C: no prompt")
else:
    time.sleep(0.3)
    r.send("partial passw")
    time.sleep(0.2)
    r.send("\x03")
    r.finish()
    out = clean(r.buf)
    tail = out.split("STATE", 1)[1] if "STATE" in out else ""
    re.search(r"^echo\s*$", tail, re.M) and ok("Ctrl-C at the prompt: the echo is on again") or bad("Ctrl-C at the prompt: state %r" % tail[-80:])
    "partial passw" not in out and ok("Ctrl-C at the prompt: the typed text does not appear") or bad("Ctrl-C: typed text echoed")

# 4. SIGTERM at the prompt too
r = Run(["sh", "-c", "trap '' TERM; %s encrypt-keygen; echo STATE; stty -a | tr ' ' '\\n' | grep -x -e echo -e -echo" % BIN])
if r.expect("Password: "):
    time.sleep(0.3)
    # the program is the child of sh: find it by its parent
    pids = [p for p in os.listdir("/proc") if p.isdigit() and open("/proc/%s/stat" % p).read().split(")")[-1].split()[1] == str(r.pid)]
    for p in pids:
        os.kill(int(p), signal.SIGTERM)
    r.finish()
    out = clean(r.buf)
    tail = out.split("STATE", 1)[1] if "STATE" in out else ""
    re.search(r"^echo\s*$", tail, re.M) and ok("SIGTERM at the prompt: the echo is on again") or bad("SIGTERM at the prompt: state %r" % tail[-80:])
else:
    bad("SIGTERM: no prompt")

# 5. hunt --encrypt-password: the public key is that of encrypt-keygen, the password is typed hidden, nothing keeps it
CAMP = os.path.join(WORK, "ptyhunt")
args = [BIN, "hunt", "-p", "71", "-m", "linear", "--no-gpu", "-t", "1", "-c", "pw", "-d", CAMP, "--ui", "log", "--encrypt-password"]
r = Run(args)
if not r.expect("Password: "): bad("hunt: no prompt")
else:
    time.sleep(0.2)
    off = not r.echo_on()
    r.send(PW + "\n")
    r.expect("Repeat the password: ")
    r.send(PW + "\n")
    got = r.expect("Public key made from the password", 60)
    r.expect("compare", 5) if False else None
    r.read(2.5)
    r.kill()
    st = r.finish(30)
    out = clean(r.buf)
    off and ok("hunt --encrypt-password: the echo is off at the prompt") or bad("hunt: echo on at the prompt")
    m = re.search(r"Public key made from the password \(key id ([0-9a-f]{8})\):\s+([0-9a-f]{66})", out)
    m and m.group(2) == KEYGEN_Q and ok("hunt --encrypt-password: the public key equals the one of encrypt-keygen for the same password") \
        or bad("hunt: public key %r against keygen %r" % (m.group(2) if m else None, KEYGEN_Q))
    "trust" in out and "capture the password" in out and ok("hunt --encrypt-password: the notice about the trusted machine is shown") or bad("hunt: no notice")
    PW not in out and ok("hunt --encrypt-password: the password does not appear in the output") or bad("hunt: the password is in the output")
    st == 130 and ok("hunt: interrupted, exit status 130") or bad("hunt: status %s" % st)
    bad_files = []
    for root, _, files in os.walk(CAMP):
        for f in files:
            if PW.encode() in open(os.path.join(root, f), "rb").read():
                bad_files.append(f)
    not bad_files and ok("hunt --encrypt-password: the password is in no file of the campaign") or bad("the password is in %s" % bad_files)
    conf = open(os.path.join(CAMP, "pw", "campaign.conf")).read()
    ("encrypt_pubkey=" + KEYGEN_Q) in conf and ok("hunt --encrypt-password: only the public key is stored in campaign.conf") or bad("campaign.conf: %r" % conf[-120:])

# 6. the resumed campaign has its key: no prompt, no password; a different key is refused
r = Run(args)
asked = r.expect("Password: ", 6)
r.read(2.5)
r.kill()
r.finish(30)
out = clean(r.buf)
not asked and "no password is asked" in out and ok("resume: a campaign that has its key does not ask for the password") or bad("resume: prompt %s output %r" % (asked, out[-300:]))
r = Run([BIN, "hunt", "-p", "71", "-m", "linear", "--no-gpu", "-t", "1", "-c", "pw", "-d", CAMP, "--ui", "log", "--encrypt-pubkey", "03" + "11" * 32])
bad_key = "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5"
r.finish(30)
r = Run([BIN, "hunt", "-p", "71", "-m", "linear", "--no-gpu", "-t", "1", "-c", "pw", "-d", CAMP, "--ui", "log", "--encrypt-pubkey", bad_key])
st = r.finish(30)
out = clean(r.buf)
st == 2 and "cannot be switched" in out and ok("resume with another public key: refused") or bad("resume with another key: status %s %r" % (st, out[-300:]))

print("PTY TESTS PASSED" if not fails else "PTY TESTS FAILED: %d" % fails)
sys.exit(1 if fails else 0)
