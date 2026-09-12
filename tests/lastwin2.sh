#!/bin/bash
# Cas limites du bureau vide : sortie du mode fenetre, Quitter, detach/reattach.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/lastwin2; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/l2_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
alive(){ timeout 3 $TM -S $S display-message -p ok 2>/dev/null | grep -q ok && echo oui || echo NON; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.2; }
$TM -S $S send-keys -t t "exit" Enter; sleep 2
ok "bureau vide en place" "$(fmt '#{window_name}')" "Bureau"
echo "--- sortie du mode fenetre (prefixe+F) avec un bureau vide ---"
key ctrl+b; key F; sleep 1.5
ok "serveur vivant" "$(alive)" "oui"
echo "  bureau=$(fmt '#{@desktop}') fenetre=$(fmt '#{window_name}') panneaux=$(fmt '#{window_panes}')"
import -window root e1.png
echo "--- retour en mode fenetre ---"
key ctrl+b; key F; sleep 1.2
ok "serveur vivant" "$(alive)" "oui"; import -window root e2.png
echo "--- detach puis reattach ---"
key ctrl+b; key d; sleep 1.5
ok "serveur survit au detach" "$(alive)" "oui"
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf attach -t t" & XP2=$!
sleep 2.5; import -window root e3.png
# La fenetre a ete RESSUSCITEE a l'etape "sortie du mode" : c'est une vraie
# fenetre desormais, son nom doit etre celui du shell.
ok "reattache sur une vraie fenetre" "$(fmt '#{window_name}')" "bash"
echo "--- detach/reattach AVEC un bureau vide ---"
$TM -S $S send-keys -t t "exit" Enter; sleep 2
ok "bureau vide" "$(fmt '#{window_name}')" "Bureau"
key ctrl+b; key d; sleep 1.5
ok "serveur vivant detache" "$(alive)" "oui"
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf attach -t t" & XP3=$!
sleep 2.5; import -window root e4.png
ok "bureau vide preserve" "$(fmt '#{window_name}')" "Bureau"
ok "et toujours en mode bureau" "$(fmt '#{@desktop}')" "on"
echo "--- Fichier > Quitter doit toujours arreter tmux ---"
$TM -S $S kill-server 2>/dev/null; sleep 1.5
ok "serveur arrete" "$(alive)" "NON"
echo "captures dans $V"
kill $XP $XP2 $XP3 2>/dev/null
