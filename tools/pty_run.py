#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Runs a command in a pseudo-terminal for SECS seconds, then sends it SIGINT, and prints what it drew (escape sequences removed).
Usage: tools/pty_run.py SECS COMMAND [ARGS...]   (for trying the multi-line panel, which only appears on a terminal)"""
import os, pty, re, select, signal, sys, time

def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    secs = float(sys.argv[1])
    args = sys.argv[2:]
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(args[0], args)
    t0 = time.time()
    buf = b""
    sent = False
    while True:
        ready, _, _ = select.select([fd], [], [], 0.2)
        if ready:
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            buf += data
        if not sent and time.time() - t0 > secs:
            os.kill(pid, signal.SIGINT)
            sent = True
        if time.time() - t0 > secs + 30:
            os.kill(pid, signal.SIGKILL)
            break
    os.waitpid(pid, 0)
    sys.stdout.write(re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", buf.decode("utf8", "replace")))

main()
