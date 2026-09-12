#!/bin/bash
# La souris du gestionnaire vise-t-elle la bonne ligne quand la barre de menu
# est eteinte (prefixe+B) ? Le bureau se decale d'une ligne : les clics sur la
# liste des conversations doivent suivre.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgrmouse; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/mm_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
pfx(){ key ctrl+b; key "$1"; sleep 0.7; }
cx(){ echo $(( $1*8+4 )); }
cy(){ echo $(( $1*17+11 )); }
$TM -S $S claude-manager; sleep 1.5
for i in 1 2; do
  $TM -S $S select-layout -t t tiled 2>/dev/null
  $TM -S $S split-window -h -t t 2>/dev/null; sleep 0.6
  $TM -S $S select-layout -t t tiled 2>/dev/null; sleep 0.8
done
echo "conversations : $(fmt '#{window_panes}')"
# ligne N de la liste = rect_y + 1 (cadre) + 1 (en-tete) + N   (0-based -> +1)
# Ligne ecran = barre de menu + (statut en haut ?) + rect_y + cadre + entete + N
clickrow(){   # $1 = numero de conversation (1..n)
  local x y dx dy mb
  dx=$(fmt '#{window_desktop_x}'); dy=$(fmt '#{window_desktop_y}')
  mb=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && mb=1
  x=$(cx $(( dx + 4 )))
  y=$(cy $(( mb + dy + 1 + $1 )))
  xdotool mousemove $x $y click 1; sleep 0.6
}
report(){ echo "   $1 : conversation active = $(fmt '#{pane_index}') (attendu $2)  [rect $(fmt '#{window_desktop_x}','#{window_desktop_y}')]"; }
echo "--- barre de menu ALLUMEE ---"
clickrow 1; report "clic ligne 1" 0
clickrow 3; report "clic ligne 3" 2
pfx B; echo "--- barre de menu ETEINTE (prefixe+B) ---"
clickrow 1; report "clic ligne 1" 0
clickrow 3; report "clic ligne 3" 2
import -window root mm.png
pfx B
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
