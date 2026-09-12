#!/usr/bin/env python3
"""Reproduit le collage depuis Windows : le terminal SSH envoie le texte
encadre par ESC[200~ / ESC[201~ dans le pty du client tmuxv."""
import os, pty, subprocess, sys, time

TM = os.environ.get("TMUXV", "/home/martinien/tmux/build/tmux")
SOCK = "/tmp/pst_sock"
CONF = "/tmp/pstest/conf"

def ctl(*a):
    return subprocess.run([TM, "-S", SOCK] + list(a), capture_output=True,
                          text=True, timeout=6).stdout.strip()

def fmt(f):
    return ctl("display-message", "-p", f)

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.environ["HOME"] = os.path.expanduser("~")
    os.execv(TM, [TM, "-S", SOCK, "-f", CONF, "attach", "-t", "t"])

import fcntl, termios, struct
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 130, 0, 0))

def drain():
    import select
    while select.select([fd], [], [], 0.3)[0]:
        try:
            if not os.read(fd, 65536):
                break
        except OSError:
            break

def w(s):
    os.write(fd, s.encode())
    time.sleep(0.35)
    drain()

time.sleep(2.5)
drain()

dx, dy, dw, dh = map(int, fmt("#{window_desktop_x} #{window_desktop_y} "
                              "#{window_desktop_w} #{window_desktop_h}").split())
print(f"fenetre desktop : x={dx} y={dy} w={dw} h={dh}")

w("printf 'TEXTE-SOURCE-A-SELECTIONNER\\n'\r")
time.sleep(1.0)
drain()

# --- 1) collage sans selection -------------------------------------------
w("\033[200~VENU-DE-WINDOWS-1\033[201~")
time.sleep(0.8)
line1 = ctl("capture-pane", "-p", "-t", "t").strip().split("\n")[-1]
print("1) sans selection      :", repr(line1))
w("\025")  # C-u

# --- 2) selection a la souris (le pane reste en mode copie) ---------------
# ligne 1 du contenu du panneau, en coordonnees terminal 1-based
row = dy + 3
c1, c2 = dx + 2, dx + 14
w(f"\033[<0;{c1};{row}M")
w(f"\033[<32;{c2};{row}M")
w(f"\033[<0;{c2};{row}m")
time.sleep(0.6)
print("2) mode copie apres selection :", fmt("#{pane_in_mode}"),
      "| selection presente :", fmt("#{selection_present}"))

# --- 3) collage ALORS QUE la selection est affichee -----------------------
w("\033[200~VENU-DE-WINDOWS-2\033[201~")
time.sleep(1.0)
print("3) mode copie apres collage   :", fmt("#{pane_in_mode}"), "(0 attendu)")
line2 = ctl("capture-pane", "-p", "-t", "t").strip().split("\n")[-1]
print("3) ligne du shell             :", repr(line2))

ok1 = "VENU-DE-WINDOWS-1" in line1
ok2 = "VENU-DE-WINDOWS-2" in line2
print("\nRESULTAT: sans selection =", "OK" if ok1 else "ECHEC",
      "| avec selection =", "OK" if ok2 else "ECHEC")
os.close(fd)
sys.exit(0 if (ok1 and ok2) else 1)
