#!/usr/bin/env python3
"""Does vyto/terminal put the terminal back when the process is killed?

Driven by tests/run_tests.sh. Needs a pty, which is the whole reason this is a
Python helper rather than a shell loop: the property under test is the state of
a terminal AFTER the process that owned it has died, and only the pty master
can still read that.

Each case forks a pty, waits for the child to enter raw mode, kills it the way
under test, then reads the master's termios. ECHO and ICANON must both be back
on. A failure here means a user who pressed Ctrl+C is left with a blind shell.
"""

import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHIM = os.path.join(ROOT, "lib", "vyto", "terminal", "native", "src", "term_shim.c")
SRC = os.path.join(HERE, "fixtures", "terminal_signal_check.c")


def build(tmpdir):
    exe = os.path.join(tmpdir, "terminal_signal_check")
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-O0", "-o", exe, SRC, SHIM],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL terminal_signal build")
        print(r.stderr.strip()[:2000])
        return None
    return exe


def wait_ready(fd, timeout=5.0):
    """Read until the child says it is in raw mode."""
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        if not select.select([fd], [], [], 0.2)[0]:
            continue
        try:
            c = os.read(fd, 65536)
        except OSError:
            break
        if not c:
            break
        buf += c
        if b"ready" in buf:
            return True
    return False


def drain(fd, seconds=0.4):
    end = time.time() + seconds
    out = b""
    while time.time() < end:
        if not select.select([fd], [], [], 0.1)[0]:
            break
        try:
            c = os.read(fd, 65536)
        except OSError:
            break
        if not c:
            break
        out += c
    return out


def restored(fd):
    """Are ECHO and ICANON both back on, as a cooked terminal has them?"""
    try:
        a = termios.tcgetattr(fd)
    except termios.error:
        return None
    lflag = a[3]
    return bool(lflag & termios.ECHO) and bool(lflag & termios.ICANON)


def case_signal(exe, sig):
    """Raw mode, then die by `sig`. The tty must be restored anyway."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(exe, [exe, "hold"])
    try:
        # A fresh pty is cooked; the child turns that off. If it never got
        # there, the test would trivially "pass", so require the handshake.
        if not wait_ready(fd):
            return "no handshake from child"
        if restored(fd):
            return "child never entered raw mode (tty still cooked)"
        os.kill(pid, sig)
        time.sleep(0.4)
        drain(fd)
        _, status = os.waitpid(pid, 0)
        ok = restored(fd)
        if ok is None:
            return "could not read tty state"
        if not ok:
            return "terminal left in raw mode"
        if not os.WIFSIGNALED(status):
            return "process did not die by the signal (exit %d)" % os.WEXITSTATUS(status)
        if os.WTERMSIG(status) != sig:
            return "died by signal %d, expected %d" % (os.WTERMSIG(status), sig)
        return None
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def case_inline(exe, name):
    """Run a case that reports its own verdict on stdout."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(exe, [exe, name])
    try:
        out = b""
        end = time.time() + 5
        while time.time() < end:
            if not select.select([fd], [], [], 0.2)[0]:
                continue
            try:
                c = os.read(fd, 65536)
            except OSError:
                break
            if not c:
                break
            out += c
        _, status = os.waitpid(pid, 0)
        text = out.decode(errors="replace")
        if os.WIFSIGNALED(status) or os.WEXITSTATUS(status) != 0:
            return text.strip().replace("\r\n", "; ") or "exited non-zero"
        return None
    finally:
        try:
            os.close(fd)
        except OSError:
            pass


def main():
    tmpdir = os.path.join(ROOT, "tests", "tmp")
    os.makedirs(tmpdir, exist_ok=True)
    exe = build(tmpdir)
    if exe is None:
        return 1

    fails = 0
    cases = [
        ("terminal_restore_normal", lambda: case_inline(exe, "normal")),
        ("terminal_signal_guard", lambda: case_inline(exe, "guard")),
        ("terminal_restore_sigint", lambda: case_signal(exe, signal.SIGINT)),
        ("terminal_restore_sigterm", lambda: case_signal(exe, signal.SIGTERM)),
        ("terminal_restore_sighup", lambda: case_signal(exe, signal.SIGHUP)),
    ]
    for name, fn in cases:
        why = fn()
        if why is None:
            print("PASS %s" % name)
        else:
            print("FAIL %s (%s)" % (name, why))
            fails += 1
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
