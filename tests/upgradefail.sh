#!/bin/bash
# FILET DE REPLI : le nouveau binaire meurt PENDANT la reconstruction.
# L'ancien doit reprendre la main avec le meme etat - les processus des
# panneaux ne doivent RIEN remarquer. La mort est provoquee par la variable
# TMUXV_UPGRADE_CRASH, lue dans upgrade_load().
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/upgradefail; rm -rf $V; mkdir -p $V/work; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $V/conf
cp "$TM" $V/tmuxv-neuf; chmod +x $V/tmuxv-neuf
S=/tmp/ugf_$$
# Le serveur herite de TMUXV_UPGRADE_CRASH : le binaire exec'e se tuera.
TMUXV_UPGRADE_CRASH=1 xterm -geometry 140x36 -fa Monospace -fs 10 \
  -e "cd $V/work && TMUXV_UPGRADE_CRASH=1 $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
srvpid(){
  local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done
}
$TM -S $S split-window -t t; sleep 1
$TM -S $S send-keys -t t.0 "python3 -c 'import time;time.sleep(600)'" Enter; sleep 1.5
SRV=$(srvpid); PIDS=$($TM -S $S list-panes -a -F '#{pane_pid}' | sort -n | tr '\n' ' ')
echo "  serveur $SRV ; panneaux : $PIDS"

echo "--- mise a jour vers un binaire qui va mourir en chemin ---"
timeout 20 $TM -S $S upgrade-server $V/tmuxv-neuf 2>&1 | head -2
sleep 5
ok "le serveur est toujours la"      "$(timeout 5 $TM -S $S display-message -p ok 2>/dev/null)" "ok"
ok "meme pid (repli par exec, pas relance)" "$(srvpid)" "$SRV"
ok "les processus des panneaux ont survecu" "$($TM -S $S list-panes -a -F '#{pane_pid}' 2>/dev/null | sort -n | tr '\n' ' ')" "$PIDS"
ok "il tourne sur le binaire de REPLI"      "$(readlink /proc/$SRV/exe 2>/dev/null | xargs basename 2>/dev/null | cut -c1-14)" "tmuxv-fallback"
ok "les panneaux sont tous la"              "$(fmt '#{window_panes}')" "2"
$TM -S $S send-keys -t t.1 "echo APRES-REPLI" Enter; sleep 1.5
ok "un shell repond encore"                 "$($TM -S $S capture-pane -p -t t.1 | grep -c APRES-REPLI)" "2"
import -window root $V/work/f1.png
echo "capture : $V/work/f1.png"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 140x36" 2>/dev/null
rm -f /tmp/tmuxv-fallback-* 2>/dev/null
