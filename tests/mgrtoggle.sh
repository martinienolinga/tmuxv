#!/bin/bash
# prefixe+B sur le gestionnaire dans deux etats particuliers :
#   A. fenetre MAXIMISEE (case [^]) - la zone du bureau change de hauteur
#   B. liste PLUS LONGUE que la fenetre (12 conversations)
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgrtoggle; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/mt_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
pfx(){ key ctrl+b; key "$1"; sleep 0.8; }
st(){ echo "   rect=$(fmt '#{window_desktop_x}','#{window_desktop_y}') $(fmt '#{window_desktop_w}')x$(fmt '#{window_desktop_h}') max=$(fmt '#{window_desktop_zoomed}') panneaux=$(fmt '#{window_panes}') zoom=$(fmt '#{window_zoomed_flag}')"; }
newconv(){ $TM -S $S select-layout -t t tiled 2>/dev/null; $TM -S $S split-window -h -t t 2>/dev/null; $TM -S $S select-layout -t t tiled 2>/dev/null; sleep 0.4; }
$TM -S $S claude-manager; sleep 1.5
newconv; newconv; sleep 0.8
echo "=== A. fenetre maximisee ==="
# case [^] : ligne du titre, 5 a 3 colonnes avant le bord droit
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}'); RW=$(fmt '#{window_desktop_w}')
xdotool mousemove $(( (RX+RW-4)*8+4 )) $(( (1+RY)*17+11 )) click 1; sleep 1
echo "0. maximisee"; st; import -window root t0.png
pfx B; echo "1. barre eteinte"; st; import -window root t1.png
pfx B; echo "2. barre rallumee"; st; import -window root t2.png
echo "   diff t0/t2 : $(compare -metric AE t0.png t2.png null: 2>&1)"
# restaurer
xdotool mousemove $(( (RX+RW-4)*8+4 )) $(( (1+RY)*17+11 )) click 1; sleep 0.8
echo "=== B. 12 conversations (liste plus haute que la fenetre) ==="
for i in $(seq 1 9); do newconv; done; sleep 1
echo "3. avant"; st; import -window root t3.png
pfx B; echo "4. barre eteinte"; st; import -window root t4.png
pfx B; echo "5. barre rallumee"; st; import -window root t5.png
echo "   diff t3/t5 : $(compare -metric AE t3.png t5.png null: 2>&1)"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
