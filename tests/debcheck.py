import os, pty, subprocess, time, fcntl, termios, struct, select, sys
TM="/usr/bin/tmuxv"; S="/tmp/deb_sock"; C="/tmp/debtest/conf"
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"]="xterm-256color"
    os.execv(TM,[TM,"-S",S,"-f",C,"attach","-t","d"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH",34,120,0,0))
buf=b""; t=time.time()
while time.time()-t<3:
    if select.select([fd],[],[],0.3)[0]:
        try: buf+=os.read(fd,65536)
        except OSError: break
def ctl(*a): return subprocess.run([TM,"-S",S]+list(a),capture_output=True,text=True,timeout=6).stdout.strip()
print("geometrie fenetre :", ctl("display-message","-p","#{window_desktop_x},#{window_desktop_y} #{window_desktop_w}x#{window_desktop_h}"))
txt=buf.decode("utf-8","replace")
print("barre de menu      :", "OUI" if "Fichier" in txt else "NON")
print("cadre de fenetre   :", "OUI" if ("│" in txt or "┌" in txt or "─" in txt) else "NON")
os.close(fd)
