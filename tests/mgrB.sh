#!/bin/bash
# Sur le gestionnaire Claude : Ctrl+B puis Shift+B, puis Ctrl+B puis b.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgrB; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/mb_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
st(){ echo "   menu=$(fmt '#{@menu-bar}') desk=$(fmt '#{@desktop}') mgr=$(fmt '#{window_claude_manager}') panneaux=$(fmt '#{window_panes}') zoom=$(fmt '#{window_zoomed_flag}') fenetre=$(fmt '#{window_index}:#{window_name}')"; }
pane(){ echo "   derniere ligne du panneau : [$($TM -S $S capture-pane -p -t t | grep -vE '^\s*$' | tail -1)]"; }
$TM -S $S claude-manager; sleep 1.5
for i in 1 2; do
  $TM -S $S select-layout -t t tiled 2>/dev/null; $TM -S $S split-window -h -t t 2>/dev/null
  $TM -S $S select-layout -t t tiled 2>/dev/null; sleep 0.5
done
sleep 1; import -window root b0.png
echo "0. depart"; st; pane
echo "=== Ctrl+B puis Shift+B ==="
xdotool mousemove 400 300; xdotool key ctrl+b; sleep 0.2; xdotool key shift+b; sleep 1
import -window root b1.png; st; pane
echo "   diff avec le depart : $(compare -metric AE b0.png b1.png null: 2>&1)"
echo "=== Ctrl+B puis b (minuscule) ==="
xdotool key ctrl+b; sleep 0.2; xdotool key b; sleep 1
import -window root b2.png; st; pane
echo "   diff avec l'etat precedent : $(compare -metric AE b1.png b2.png null: 2>&1)"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
