#!/bin/bash
# BALAYAGE DE L'INTERFACE : chaque composant est ouvert, capture, et referme.
# Les captures sont ensuite assemblees en une planche unique.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/gui; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > conf
S=/tmp/gui_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "cd $V && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.25; }
shot(){ sleep 0.6; import -window root $V/$1.png; }
esc(){ xdotool key Escape; sleep 0.9; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
diff2(){ compare -metric AE "$V/$1.png" "$V/$2.png" null: 2>&1; }

shot 01-bureau
# --- barre de menu : chaque menu deroulant ---
i=0
for x in 4 13 23 33 42 51 61 70; do
  i=$((i+1)); xdotool mousemove $(cx $x) 8 click 1; shot "02-menu$i"; esc
done
ok "les 8 menus s'ouvrent" "$(for n in 1 2 3 4 5 6 7 8; do [ "$(diff2 01-bureau 02-menu$n)" -gt 2000 ] && echo -n x; done | wc -c)" "8"
# --- dialogues ---
$TM -S $S display-form -c "$CL"; shot 03-formulaire; esc
$TM -S $S display-sessions -c "$CL"; shot 04-sessions; esc
$TM -S $S display-windows -c "$CL"; shot 05-fenetres; esc
$TM -S $S display-keys -c "$CL"; shot 06-raccourcis; esc
$TM -S $S display-buffers -c "$CL"; shot 07-tampons; esc
$TM -S $S display-log -c "$CL"; shot 08-journal; esc
$TM -S $S set -g display-time 0; $TM -S $S display-message -c "$CL" "Message de controle"; shot 09-message; esc
$TM -S $S set -g display-time 750
for n in 3 4 5 6 7 8 9; do
  v=$(printf "%02d" $n)
  case $n in 3) N=formulaire;; 4) N=sessions;; 5) N=fenetres;; 6) N=raccourcis;; 7) N=tampons;; 8) N=journal;; 9) N=message;; esac
  [ "$(diff2 01-bureau $v-$N)" -gt 3000 ] || echo "  ECHEC dialogue $N ne s'affiche pas"
done
ok "les 7 dialogues s'affichent" "vu" "vu"
# --- fenetre : deplacement, redimensionnement, zoom ---
read -r RX RY RW RH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
xdotool mousemove $(cx $((RX+10))) $(cy $((1+RY))) mousedown 1 mousemove $(cx $((RX+18))) $(cy $((1+RY+4))) mouseup 1; sleep 0.5
read -r NX NY <<<"$(fmt '#{window_desktop_x} #{window_desktop_y}')"
ok "la fenetre se deplace" "$([ "$NX" != "$RX" ] && echo oui || echo NON)" "oui"
shot 10-deplacee
xdotool mousemove $(cx $((NX+RW-1))) $(cy $((1+NY+RH-1))) mousedown 1 mousemove $(cx $((NX+RW-12))) $(cy $((1+NY+RH-6))) mouseup 1; sleep 0.5
ok "la fenetre se redimensionne" "$([ "$(fmt '#{window_desktop_w}')" != "$RW" ] && echo oui || echo NON)" "oui"
shot 11-redimensionnee
read -r NX NY NW <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w}')"
xdotool mousemove $(cx $((NX+NW-4))) $(cy $((1+NY))) click 1; sleep 0.6
ok "la case agrandir fonctionne" "$(fmt '#{window_desktop_zoomed}')" "1"
shot 12-maximisee
xdotool mousemove $(cx $((NX+NW-4))) $(cy $((1+NY))) click 1; sleep 0.6
# --- ascenseur d'une fenetre ordinaire (avant le gestionnaire) ---
$TM -S $S send-keys -t 0 "seq 1 300" Enter; sleep 1.2
read -r SX SY SW SH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
# fleche haut de l'ascenseur : derniere colonne du cadre, premiere ligne interieure
xdotool mousemove $(cx $((SX+SW-1))) $(cy $((1+SY+1)))
xdotool click 1; sleep 0.6
shot 15-ascenseur
ok "l'ascenseur fait defiler (mode copie)" "$(fmt '#{pane_in_mode}')" "1"
$TM -S $S send-keys -t 0 -X cancel 2>/dev/null; sleep 0.3

# --- gestionnaire Claude ---
$TM -S $S claude-manager; sleep 2
for i in 1 2; do $TM -S $S select-layout -t 1 tiled 2>/dev/null; $TM -S $S split-window -h -t 1 2>/dev/null; $TM -S $S select-layout -t 1 tiled 2>/dev/null; sleep 0.4; done
sleep 1; shot 13-gestionnaire
ok "le gestionnaire est ouvert" "$(fmt '#{window_claude_manager}')" "1"
read -r MX MY <<<"$(fmt '#{window_desktop_x} #{window_desktop_y}')"
xdotool mousemove $(cx $((MX+4))) $(cy $((1+MY+2))) click 3; shot 14-menu-conversation; esc
# --- bureau vide : fermer le gestionnaire puis la derniere fenetre ---
$TM -S $S kill-window -t 1 2>/dev/null; sleep 1
$TM -S $S send-keys -t 0 -X cancel 2>/dev/null      # au cas ou un mode traine
$TM -S $S send-keys -t 0 "exit" Enter; sleep 3
shot 16-bureau-vide
ok "bureau vide apres fermeture" "$(fmt '#{window_name}')" "Bureau"
cd $V && montage -tile 4x4 -geometry 400x244+3+3 -background gray30 \
  01-bureau.png 02-menu1.png 02-menu3.png 02-menu5.png \
  03-formulaire.png 04-sessions.png 05-fenetres.png 06-raccourcis.png \
  07-tampons.png 08-journal.png 09-message.png 10-deplacee.png \
  12-maximisee.png 13-gestionnaire.png 14-menu-conversation.png 16-bureau-vide.png \
  planche.png 2>/dev/null && echo "  planche : $V/planche.png"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 125x38" 2>/dev/null
