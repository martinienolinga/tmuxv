#!/usr/bin/env python3
"""Banc de rendu headless pour tmux-custom.

Imbrique une session tmux-custom dans un tmux "extérieur", attache un client
sur un PTY de taille fixe pour forcer le rendu, puis capture la grille visible
(barre de menu en haut, panneaux, barre de statut en bas) et l'imprime.
"""
import os, sys, pty, struct, fcntl, termios, subprocess, time, signal

TMUX = os.path.expanduser("~/.local/bin/tmux-custom")
COLS, ROWS = 120, 40

def sh(*args):
    return subprocess.run([TMUX, *args], capture_output=True, text=True)

def main():
    conf = sys.argv[1]
    inner_cmd = sys.argv[2] if len(sys.argv) > 2 else None

    # Nettoyage
    sh("-L", "outer", "kill-server")
    sh("-L", "inner", "kill-server")
    time.sleep(0.3)

    # Session intérieure lancée DANS le pane de l'extérieur, à la bonne taille.
    inner = f"{TMUX} -L inner -f {conf} new-session"
    outer_new = [TMUX, "-L", "outer", "new-session", "-d",
                 "-x", str(COLS), "-y", str(ROWS), inner]
    subprocess.run(outer_new, capture_output=True, text=True)
    time.sleep(0.3)

    # Attache un client sur un PTY de taille fixe pour déclencher le rendu.
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(TMUX, [TMUX, "-L", "outer", "attach"])
        os._exit(1)
    # règle la taille du PTY
    winsize = struct.pack("HHHH", ROWS, COLS, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)

    # laisse le temps de rendre, en vidant la sortie du PTY
    deadline = time.time() + 1.5
    while time.time() < deadline:
        try:
            os.read(fd, 65536)
        except OSError:
            break

    if inner_cmd:
        for part in inner_cmd.split(";"):
            sh("-L", "inner", *part.split())
        time.sleep(0.5)
        try:
            os.read(fd, 65536)
        except OSError:
            pass

    # Capture la grille rendue par l'extérieur (= ce que l'intérieur a dessiné)
    cap = sh("-L", "outer", "capture-pane", "-p")
    print("===== RENDU (%dx%d) =====" % (COLS, ROWS))
    print(cap.stdout.rstrip("\n"))
    print("===== FIN =====")

    # Nettoyage
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    sh("-L", "outer", "kill-server")
    sh("-L", "inner", "kill-server")

main()
