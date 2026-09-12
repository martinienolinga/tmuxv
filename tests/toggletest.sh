#!/bin/bash
# Isolated test of prefix+B (menu bar) / prefix+F (desktop) toggles.
set -u
export DISPLAY=:99
SOCK=/tmp/toggle_$$
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
xterm -geometry 125x38 -fa 'Monospace' -fs 10 -e "$TM -S $SOCK -f $(dirname "$0")/desktop.conf new-session -s t" &
XP=$!; sleep 2.5
W=$(xdotool search --name xterm | head -1)
q() { "$TM" -S "$SOCK" display-message -p '#{@menu-bar} #{@desktop}'; }
echo "init: $(q)"; import -window root "$D/t0.png"
xdotool key --window "$W" ctrl+b; sleep 0.2; xdotool key --window "$W" B; sleep 0.6
echo "after B: $(q)"; import -window root "$D/t1.png"
xdotool key --window "$W" ctrl+b; sleep 0.2; xdotool key --window "$W" F; sleep 0.6
echo "after F: $(q)"; import -window root "$D/t2.png"
xdotool key --window "$W" ctrl+b; sleep 0.2; xdotool key --window "$W" B; sleep 0.3
xdotool key --window "$W" ctrl+b; sleep 0.2; xdotool key --window "$W" F; sleep 0.6
echo "after B,F again: $(q)"; import -window root "$D/t3.png"
"$TM" -S "$SOCK" kill-server; kill $XP 2>/dev/null; rm -f "$SOCK"
convert "$D/t0.png" "$D/t1.png" "$D/t2.png" "$D/t3.png" -resize 50% +append "$D/toggle_cmp.png"
echo done
