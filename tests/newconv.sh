#!/bin/bash
# Une nouvelle conversation doit montrer un PROMPT BASH propre : pas de ligne
# curl, pas de mode visualisation ("[0/0]") laisse par run-shell.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/newconv; rm -rf $V; mkdir -p $V/work; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $V/conf
S=/tmp/nc_$$
xterm -geometry 130x34 -fa Monospace -fs 10 \
  -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
$TM -S $S claude-manager; sleep 2
echo "--- conversation creee avec la fenetre ---"
ok "pas en mode visualisation" "$(fmt '#{pane_in_mode}')" "0"
ok "aucune trace de curl" "$($TM -S $S capture-pane -p -t t | grep -c curl)" "0"
echo "--- [+ Nouvelle] ---"
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}'); RH=$(fmt '#{window_desktop_h}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+RH-2 ))) click 1; sleep 2.5
ok "une conversation de plus" "$(fmt '#{window_panes}')" "2"
ok "pas en mode visualisation" "$(fmt '#{pane_in_mode}')" "0"
ok "aucune trace de curl" "$($TM -S $S capture-pane -p | grep -c curl)" "0"
ok "un prompt bash est la" "$($TM -S $S capture-pane -p | grep -c '\$')" "1"
import -window root n0.png
echo "capture : $V/work/n0.png"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 130x34" 2>/dev/null
