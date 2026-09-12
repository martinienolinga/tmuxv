#!/bin/bash
# En mode fenetre : fermer la DERNIERE fenetre laisse un bureau VIDE.
# Ni sortie du mode, ni arret de tmux. Une nouvelle fenetre repart normalement.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/lastwin; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/lw_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
alive(){ timeout 3 $TM -S $S display-message -p ok 2>/dev/null | grep -q ok && echo oui || echo NON; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.2; }

echo "--- 1. fermer la derniere fenetre par 'exit' ---"
ok "une seule fenetre au depart" "$(fmt '#{session_windows}')" "1"
$TM -S $S send-keys -t t "exit" Enter; sleep 2
ok "serveur toujours vivant" "$(alive)" "oui"
ok "mode bureau toujours actif" "$(fmt '#{@desktop}')" "on"
ok "fenetre = bureau vide" "$(fmt '#{window_name}')" "Bureau"
ok "toujours une fenetre (le bureau)" "$(fmt '#{session_windows}')" "1"
import -window root w1.png

echo "--- 2. une nouvelle fenetre repart normalement ---"
key ctrl+b; key c; sleep 2
ok "serveur vivant" "$(alive)" "oui"
ok "fenetre reelle" "$(fmt '#{window_name}')" "bash"
ok "le bureau vide a disparu" "$(fmt '#{session_windows}')" "1"
import -window root w2.png
$TM -S $S send-keys -t t "echo VIVANT" Enter; sleep 0.8
ok "le shell repond" "$($TM -S $S capture-pane -p | grep -c VIVANT)" "2"

echo "--- 3. fermer la derniere fenetre par la case [X] ---"
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
xdotool mousemove $(( (RX+3)*8+4 )) $(( (MB+RY)*17+11 )) click 1; sleep 2
ok "serveur toujours vivant" "$(alive)" "oui"
ok "fenetre = bureau vide" "$(fmt '#{window_name}')" "Bureau"
import -window root w3.png

echo "--- 4. plusieurs fenetres : la fermeture reste normale ---"
key ctrl+b; key c; sleep 1.5; key ctrl+b; key c; sleep 1.5
ok "deux fenetres" "$(fmt '#{session_windows}')" "2"
key ctrl+b; key ampersand; sleep 0.5; key y; sleep 1.5
ok "une fenetre apres fermeture" "$(fmt '#{session_windows}')" "1"
ok "et c'est une vraie fenetre" "$(fmt '#{window_name}')" "bash"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
