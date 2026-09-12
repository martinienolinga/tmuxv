#!/bin/bash
# Reproduction : bascules globales (prefixe B = barre de menu, prefixe F =
# bureau) declenchees ALORS QUE la fenetre active est le gestionnaire Claude.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgrkeys; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/mk_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
pfx(){ key ctrl+b; key "$1"; sleep 0.7; }
st(){
  local menu desk panes zoom rect
  menu=$(fmt '#{@menu-bar}'); desk=$(fmt '#{@desktop}')
  panes=$(fmt '#{window_panes}'); zoom=$(fmt '#{window_zoomed_flag}')
  rect=$(fmt '#{window_desktop_x},#{window_desktop_y} #{window_desktop_w}x#{window_desktop_h}')
  echo "   menu=$menu desk=$desk panneaux=$panes zoom=$zoom rect=$rect"
}
$TM -S $S claude-manager; sleep 1.5
$TM -S $S select-layout -t t tiled 2>/dev/null
$TM -S $S split-window -h -t t 2>/dev/null; sleep 0.8
$TM -S $S select-layout -t t tiled 2>/dev/null; sleep 1.2
import -window root m0.png; echo "0. gestionnaire ouvert"; st
pfx B; import -window root m1.png; echo "1. apres prefixe+B"; st
pfx B; import -window root m2.png; echo "2. prefixe+B retour"; st
pfx F; import -window root m3.png; echo "3. apres prefixe+F"; st
pfx F; import -window root m4.png; echo "4. prefixe+F retour"; st
echo "diff m0/m2 barre revenue : $(compare -metric AE m0.png m2.png null: 2>&1)"
echo "diff m0/m4 bureau revenu : $(compare -metric AE m0.png m4.png null: 2>&1)"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
