#!/usr/bin/env python3
"""Reproduce the OSC-reply leak: a terminal answers OSC 11 in two chunks,
slower than escape-time, and tmux flushes the bytes into the pane as keys."""
import os, pty, subprocess, sys, time

BIN = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("TMUXV", "/home/martinien/tmux/build/tmux")
SOCK = "/tmp/osc_leak_%d" % os.getpid()
CONF = "/tmp/osc_leak_%d.conf" % os.getpid()
open(CONF, "w").write("set -s escape-time 10\nset -g @menu-bar on\nset -g @desktop on\nset -g mouse on\n")

master, slave = pty.openpty()
os.set_blocking(master, False)
env = dict(os.environ, TERM="xterm-256color")
p = subprocess.Popen([BIN, "-S", SOCK, "-f", CONF, "new-session", "-s", "t"],
                     stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True)
os.close(slave)
time.sleep(2.5)
# drain whatever tmux has written so far
try:
    while os.read(master, 65536):
        pass
except BlockingIOError:
    pass

# The terminal replies to tmux's OSC 11 query, split across two writes with a
# gap larger than escape-time (10 ms).
os.write(master, b"\033]11;rgb:1e1e")
time.sleep(0.12)
os.write(master, b"/1e1e/1e1e\033\\")
time.sleep(1.2)

out = subprocess.run([BIN, "-S", SOCK, "capture-pane", "-p", "-t", "t"],
                     capture_output=True, text=True).stdout
leak = [l for l in out.splitlines() if "rgb:" in l or "11;" in l]
print("LEAK" if leak else "CLEAN", "->", leak[:2])
subprocess.run([BIN, "-S", SOCK, "kill-server"], capture_output=True)
p.wait(timeout=5)
os.close(master); os.unlink(CONF)
