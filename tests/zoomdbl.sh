#!/bin/bash
# BUREAU : un DOUBLE CLIC sur la barre de titre agrandit la fenetre, un second
# la remet a sa taille, exactement comme sous Windows. Les cases [#] et [^] de
# la barre gardent leur comportement au clic simple et ne doivent PAS basculer
# une fois de plus quand on double-clique dessus.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/zoomdbl; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > conf
S=/tmp/zd_$$
xterm -geometry 150x40 -fa Monospace -fs 10 -e "cd $V && $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
rect(){ fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}'; }
# On part d'une fenetre qui n'occupe PAS tout le bureau, sinon agrandir ne se voit pas.
$TM -S $S resize-window -x 90 -y 20 2>/dev/null
xdotool mousemove 40 300; sleep 0.5
R0=$(rect); read -r RX RY RW RH <<<"$R0"
echo "rect de depart : $R0 (menu-bar=$MB)"
ok "au depart, non agrandie" "$(fmt '#{window_desktop_zoomed}')" "0"
import -window root z0.png
# Milieu de la barre de titre : ni la case fermer, ni la case agrandir.
TX=$(( RX + RW/2 )); TY=$(( MB + RY ))
echo "--- double clic au milieu de la barre de titre ---"
xdotool mousemove $(cx $TX) $(cy $TY) click --repeat 2 --delay 120 1; sleep 1.2
ok "la fenetre est agrandie" "$(fmt '#{window_desktop_zoomed}')" "1"
read -r AW AH <<<"$(fmt '#{client_width} #{client_height}')"
ok "elle occupe toute la largeur du bureau" "$(fmt '#{window_desktop_w}')" "$AW"
ok "elle est collee en haut a gauche" "$(fmt '#{window_desktop_x} #{window_desktop_y}')" "0 0"
import -window root z1.png
ok "l'ecran a change" "$([ "$(compare -metric AE z0.png z1.png null: 2>&1)" -gt 5000 ] && echo oui || echo NON)" "oui"
echo "--- second double clic : retour a la taille d'avant ---"
xdotool mousemove $(cx 40) $(cy $MB) click --repeat 2 --delay 120 1; sleep 1.2
ok "la fenetre n'est plus agrandie" "$(fmt '#{window_desktop_zoomed}')" "0"
ok "le rectangle d'avant est restitue" "$(rect)" "$R0"
import -window root z2.png
echo "--- double clic sur la case agrandir ---"
# La case part avec la fenetre des le premier clic (comme sous Windows) : le
# second clic tombe dans le corps de la fenetre agrandie. Un seul basculement,
# pas trois : le double clic differe ne doit PAS en rajouter un.
xdotool mousemove $(cx $(( RX + RW - 4 ))) $(cy $TY) click --repeat 2 --delay 120 1; sleep 1.2
ok "la case agrandit une fois, pas trois" "$(fmt '#{window_desktop_zoomed}')" "1"
xdotool mousemove $(cx 40) $(cy $MB) click --repeat 2 --delay 120 1; sleep 1.2
ok "le double clic la remet a sa taille" "$(rect)" "$R0"
echo "--- double clic AILLEURS que sur la barre de titre : sans effet ---"
xdotool mousemove $(cx $(( RX + RW/2 ))) $(cy $(( MB + RY + RH/2 ))) click --repeat 2 --delay 120 1; sleep 1.2
ok "l'interieur de la fenetre n'agrandit pas" "$(fmt '#{window_desktop_zoomed}')" "0"
echo "--- clic droit double sur la barre : reserve au bouton gauche ---"
xdotool mousemove $(cx $TX) $(cy $TY) click --repeat 2 --delay 120 3; sleep 1.2
ok "le bouton droit n'agrandit pas" "$(fmt '#{window_desktop_zoomed}')" "0"
xdotool key Escape; sleep 0.5
echo "--- un simple glissement de la barre deplace toujours ---"
xdotool mousemove $(cx $TX) $(cy $TY) mousedown 1 mousemove $(cx $(( TX + 6 ))) $(cy $(( TY + 3 ))) mouseup 1; sleep 0.8
ok "la fenetre s'est deplacee" "$([ "$(fmt '#{window_desktop_x}')" != "$RX" ] && echo oui || echo NON)" "oui"
ok "toujours pas agrandie" "$(fmt '#{window_desktop_zoomed}')" "0"
import -window root z3.png
echo "--- double clic sur la barre d'une fenetre deplacee ---"
read -r NX NY NW NH <<<"$(rect)"
xdotool mousemove $(cx $(( NX + NW/2 ))) $(cy $(( MB + NY ))) click --repeat 2 --delay 120 1; sleep 1.2
ok "agrandie depuis sa nouvelle position" "$(fmt '#{window_desktop_zoomed}')" "1"
xdotool mousemove $(cx 40) $(cy $MB) click --repeat 2 --delay 120 1; sleep 1.2
ok "et rendue a sa nouvelle position" "$(rect)" "$NX $NY $NW $NH"
ok "le serveur est vivant" "$(fmt ok)" "ok"
import -window root z4.png
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 150x40 -fa Monospace -fs 10 -e cd $V" 2>/dev/null
