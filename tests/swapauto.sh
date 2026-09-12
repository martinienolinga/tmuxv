#!/bin/bash
# AUTOMATIQUE : lance depuis un terminal ordinaire (portee de session NON
# deleguee), tmuxv doit se replacer tout seul dans une portee deleguee et la
# mise en swap doit marcher sans que l'utilisateur ait rien fait de special.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/swapauto; rm -rf $V; mkdir -p $V/work; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cat > $V/gonfle.py <<'EOF'
import sys, time
b = bytearray(250*1024*1024)
for i in range(0,len(b),4096): b[i]=1
sys.stdout.write("pret\n"); sys.stdout.flush(); time.sleep(600)
EOF
echo "  cgroup de ce shell : $(cat /proc/self/cgroup | cut -d: -f3)"
S=/tmp/swa_$$
# Lancement ORDINAIRE, exactement comme l'utilisateur le ferait.
xterm -geometry 100x30 -fa Monospace -fs 10 -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 4
srvpid(){ local p c
  for p in $(ls /proc|grep -E '^[0-9]+$'); do c=$(cat /proc/$p/comm 2>/dev/null)
    case "$c" in bash|sh|dash|grep|xterm) continue;; esac; case "$c" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done; }
SRV=$(srvpid)
CG=$(cat /proc/$SRV/cgroup 2>/dev/null | cut -d: -f3)
echo "  cgroup du serveur  : $CG"
ok "le serveur s'est replace dans une portee deleguee" "$(echo $CG | grep -c 'tmuxv-server')" "1"
$TM -S $S send-keys -t t:0 "python3 $V/gonfle.py" Enter
$TM -S $S new-window -t t: -d
$TM -S $S send-keys -t t:1 "python3 $V/gonfle.py" Enter
$TM -S $S select-window -t t:1
sleep 8
P0=$($TM -S $S list-panes -t t:0 -F '#{pane_pid}'); P1=$($TM -S $S list-panes -t t:1 -F '#{pane_pid}')
enfant(){ head -1 /proc/$1/task/$1/children 2>/dev/null | awk '{print $1}'; }
C0=$(enfant $P0); C1=$(enfant $P1)
swp(){ awk '/VmSwap/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
rss(){ awk '/VmRSS/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
ok "chaque panneau a son cgroup" "$(cat /proc/$P0/cgroup | grep -c 'pane-')" "1"
$TM -S $S set -g @memory-warn 100000; sleep 12
echo "  cachee : RSS $(rss $C0) Mo swap $(swp $C0) Mo | affichee : RSS $(rss $C1) Mo swap $(swp $C1) Mo"
ok "la cachee est partie en swap" "$([ "$(swp $C0)" -gt 100 ] && echo oui || echo NON)" "oui"
ok "l'affichee est epargnee"      "$([ "$(swp $C1)" -lt 20 ] && echo oui || echo NON)" "oui"
echo "=== et si on refuse le replacement (TMUXV_NO_SCOPE=1) ? ==="
S2=/tmp/swa2_$$
TMUXV_NO_SCOPE=1 $TM -S $S2 -f $V/conf new-session -d -s u; sleep 2
ok "le serveur demarre quand meme" "$(timeout 4 $TM -S $S2 display-message -p ok 2>/dev/null)" "ok"
$TM -S $S kill-server 2>/dev/null; $TM -S $S2 kill-server 2>/dev/null
pkill -f "^xterm -geometr[y] 100x30" 2>/dev/null
