#!/usr/bin/env python3
import os, pty, subprocess, sys, time, fcntl, termios, struct, select

TM = os.environ.get("TMUXV", "/home/martinien/tmux/build/tmux")
SOCK = "/tmp/pst_sock"
CONF = "/tmp/pstest/conf"

def ctl(*a):
    return subprocess.run([TM, "-S", SOCK] + list(a), capture_output=True,
                          text=True, timeout=6).stdout.strip()
def fmt(f): return ctl("display-message", "-p", f)
def last():
    return [l for l in ctl("capture-pane", "-p", "-t", "t").split("\n") if l.strip()][-1]

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(TM, [TM, "-S", SOCK, "-f", CONF, "attach", "-t", "t"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 38, 130, 0, 0))

def drain():
    while select.select([fd], [], [], 0.3)[0]:
        try:
            if not os.read(fd, 65536): break
        except OSError: break
def w(s, d=0.35):
    os.write(fd, s.encode()); time.sleep(d); drain()

time.sleep(2.5); drain()
dx, dy, dw, dh = map(int, fmt("#{window_desktop_x} #{window_desktop_y} "
                              "#{window_desktop_w} #{window_desktop_h}").split())
w("printf 'TEXTE-SOURCE-A-SELECTIONNER\\n'\r", 1.0)

def select_mouse():
    row = dy + 3
    c1, c2 = dx + 2, dx + 16
    w(f"\033[<0;{c1};{row}M"); w(f"\033[<32;{c2};{row}M"); w(f"\033[<0;{c2};{row}m", 0.6)

res = {}

# A. la selection reste visible apres le relachement
select_mouse()
res["A selection visible apres relachement"] = (
    fmt("#{pane_in_mode}") == "1" and fmt("#{selection_present}") == "1")

# B. collage pendant que la selection est affichee
w("\033[200~COLLE-DEPUIS-WINDOWS\033[201~", 1.0)
res["B collage pendant selection"] = "COLLE-DEPUIS-WINDOWS" in last() and fmt("#{pane_in_mode}") == "0"
w("\025")

# C. frappe pendant que la selection est affichee
select_mouse()
w("echo FRAPPE-OK", 1.0)
res["C frappe pendant selection"] = "FRAPPE-OK" in last() and fmt("#{pane_in_mode}") == "0"
w("\r", 1.0)
res["C2 la commande s'est executee"] = "FRAPPE-OK" in ctl("capture-pane", "-p", "-t", "t")
w("\025")

# D. mode copie volontaire : les touches naviguent toujours
w("\033[200~x\033[201~", 0.2)          # rien
ctl("copy-mode", "-t", "t"); time.sleep(0.5)
res["D1 copy-mode volontaire actif"] = fmt("#{pane_in_mode}") == "1"
w("k", 0.6)                              # doit defiler, pas taper
res["D2 'k' ne quitte pas le mode copie"] = fmt("#{pane_in_mode}") == "1"
w("q", 0.6)
res["D3 'q' quitte le mode copie"] = fmt("#{pane_in_mode}") == "0"

# E. le collage marche aussi hors selection
w("\033[200~SANS-SELECTION\033[201~", 0.9)
res["E collage hors selection"] = "SANS-SELECTION" in last()
w("\025")

# F. le prefixe fonctionne encore avec une selection affichee
select_mouse()
w("\002", 0.3); w("c", 1.2)              # C-b c = nouvelle fenetre
res["F prefixe intact (C-b c)"] = int(fmt("#{session_windows}")) >= 2

for k in sorted(res):
    print(("  OK   " if res[k] else "  ECHEC") + "  " + k)
os.close(fd)
sys.exit(0 if all(res.values()) else 1)
