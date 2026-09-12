#!/bin/bash
# CONSOLES : un texte selectionne a la souris est duplique par un clic du
# milieu (comme sous X11). La selection laisse le pane en mode copie : le clic
# du milieu doit en sortir et coller. Et la selection a la souris va jusqu'a la
# case sous le pointeur (relacher sur la derniere lettre la garde).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/midpaste; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar off\nset -g @desktop off\nset -g status off\n' > conf
S=/tmp/mp_$$
xterm -geometry 100x30 -fa Monospace -fs 10 \
  -e "$TM -S $S -f $V/conf new-session -s t env PS1='$ ' bash --norc -i" \
  >/dev/null 2>&1 </dev/null & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : [$2] au lieu de [$3]"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
drag(){ # $1 x debut, $2 x fin, $3 ligne
  xdotool mousemove $(cx $1) $(cy $3); sleep 0.2
  xdotool mousedown 1; sleep 0.2
  xdotool mousemove $(cx $(( ($1+$2)/2 ))) $(cy $3); sleep 0.2
  xdotool mousemove $(cx $2) $(cy $3); sleep 0.2
  xdotool mouseup 1; sleep 0.6; }
middle(){ xdotool mousemove $(cx 30) $(cy 10) click 2; sleep 0.8; }
prompt(){ $TM -S $S capture-pane -p | grep '^\$ ' | tail -1; }
reset(){ $TM -S $S send-keys C-u; $TM -S $S send-keys "clear; echo 'xx MOTUNIQUE123 yy'" Enter; sleep 0.6; }

reset
L=$($TM -S $S capture-pane -p | grep -n '^xx MOTUNIQUE123' | cut -d: -f1); L=$((L-1))
echo "texte en ligne $L, mot en colonnes 3..14"

echo "--- glisser vers l'avant, relache sur la derniere lettre ---"
drag 3 14 $L
ok "le buffer contient le mot entier" "$($TM -S $S show-buffer 2>&1)" "MOTUNIQUE123"
ok "le pane reste en mode copie (selection visible)" "$(fmt '#{pane_in_mode}')" "1"
middle
ok "le clic du milieu sort du mode copie" "$(fmt '#{pane_in_mode}')" "0"
ok "et colle la selection au prompt" "$(prompt)" "\$ MOTUNIQUE123"

echo "--- glisser vers l'arriere ---"
reset
drag 14 3 $L
ok "le buffer contient le mot entier" "$($TM -S $S show-buffer 2>&1)" "MOTUNIQUE123"
middle
ok "colle au prompt" "$(prompt)" "\$ MOTUNIQUE123"

echo "--- clic du milieu hors mode copie : colle le dernier buffer ---"
reset
$TM -S $S set-buffer "autre texte"
ok "pas en mode copie" "$(fmt '#{pane_in_mode}')" "0"
middle
ok "colle au prompt" "$(prompt)" "\$ autre texte"

echo "--- clavier en mode emacs : inchange (le curseur est exclu) ---"
reset
$TM -S $S copy-mode
$TM -S $S send -X top-line
$TM -S $S send -X start-of-line
for i in 1 2 3; do $TM -S $S send -X cursor-right; done
$TM -S $S send -X begin-selection
for i in 1 2 3; do $TM -S $S send -X cursor-right; done
$TM -S $S send -X copy-selection-and-cancel; sleep 0.3
ok "3 deplacements = 3 caracteres" "$($TM -S $S show-buffer 2>&1)" "MOT"
ok "le serveur est vivant" "$(fmt ok)" "ok"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; true
