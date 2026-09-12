#!/bin/bash
# Sur le BUREAU VIDE : rien ne doit agir sur un panneau inexistant.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/emptydesk; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > conf     # bureau + barre : par defaut maintenant
S=/tmp/ed_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.2; }
pfx(){ xdotool mousemove 600 400; xdotool key ctrl+b; sleep 0.15; xdotool key "$1"; sleep 0.8; }
$TM -S $S send-keys -t t "exit" Enter; sleep 2
ok "bureau vide" "$(fmt '#{window_name}')" "Bureau"
import -window root d0.png
echo "--- souris ---"
xdotool mousemove 600 400 click 3; sleep 1; import -window root d1.png
ok "clic droit : rien ne s'affiche" "$(compare -metric AE d0.png d1.png null: 2>&1)" "0"
xdotool mousemove 600 400 click 1; sleep 0.6; import -window root d2.png
ok "clic gauche : rien"             "$(compare -metric AE d0.png d2.png null: 2>&1)" "0"
xdotool mousemove 600 400 click --repeat 2 --delay 120 1; sleep 0.6; import -window root d3.png
ok "double clic : rien"             "$(compare -metric AE d0.png d3.png null: 2>&1)" "0"
echo "--- clavier ---"
# On repart d'un bureau vide avant chaque touche.
vide(){
  while [ "$(fmt '#{window_name}')" != "Bureau" ]; do
    $TM -S $S send-keys -t t "exit" Enter; sleep 1.5
  done
}
# Ces touches n'ont pas de panneau sur quoi agir : rien ne doit bouger.
for k in z x bracketleft o; do
  vide; pfx $k; xdotool key Escape; sleep 0.9
  ok "prefixe+$k : sans effet" "$(fmt '#{window_name}') $(fmt '#{window_panes}')" "Bureau 1"
done
# Un split sur le vide, c'est une demande de shell : on donne une VRAIE fenetre,
# jamais un panneau vivant cache derriere un bureau qui ne dessine rien.
for k in quotedbl percent; do
  vide; sleep 0.6; pfx $k; sleep 1.2
  ok "prefixe+$k : vraie fenetre" "$(fmt '#{window_name}') $(fmt '#{window_panes}')" "bash 1"
  ok "  et elle est utilisable" "$(fmt '#{pane_dead}')" "0"
done
vide
echo "--- la barre de menu reste utilisable ---"
xdotool mousemove 30 8 click 1; sleep 0.8; import -window root d4.png
ok "menu Fichier s'ouvre" "$([ "$(compare -metric AE d0.png d4.png null: 2>&1)" -gt 0 ] && echo oui || echo NON)" "oui"
xdotool key Escape; sleep 0.5
echo "--- nouvelle fenetre : on repart ---"
pfx c; sleep 1.5
ok "vraie fenetre" "$(fmt '#{window_name}')" "bash"
$TM -S $S send-keys -t t "echo OK-SHELL" Enter; sleep 0.8
ok "le shell repond" "$($TM -S $S capture-pane -p | grep -c OK-SHELL)" "2"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
