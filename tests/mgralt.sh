#!/bin/bash
# prefixe+B (bascule barre de menu) alors que la conversation affichee fait
# tourner une application PLEIN ECRAN (ecran alterne), comme Claude Code.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgralt; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/ma_$$
# fausse application plein ecran : ecran alterne + cadre + curseur fixe
cat > fullscreen.py <<'PY'
import sys, time, signal
w,h=int(sys.argv[1]),int(sys.argv[2])
sys.stdout.write("\033[?1049h")                     # ecran alterne
for i in range(h):
    sys.stdout.write(f"\033[{i+1};1H" + ("#" if i in (0,h-1) else "#"+" "*(w-2)+"#"))
sys.stdout.write(f"\033[{h//2};3HAPPLICATION PLEIN ECRAN")
sys.stdout.flush()
signal.pause()
PY
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
pfx(){ key ctrl+b; key "$1"; sleep 0.8; }
$TM -S $S claude-manager; sleep 1.5
$TM -S $S select-layout -t t tiled 2>/dev/null
$TM -S $S split-window -h -t t 2>/dev/null; sleep 0.6
$TM -S $S select-layout -t t tiled 2>/dev/null; sleep 1
W=$(fmt '#{pane_width}'); H=$(fmt '#{pane_height}')
echo "panneau : ${W}x${H}"
$TM -S $S send-keys -t t "python3 $V/fullscreen.py $W $H" Enter; sleep 2
import -window root a0.png; echo "0. application plein ecran affichee"
pfx B; sleep 0.7; import -window root a1.png; echo "1. apres prefixe+B (barre eteinte)"
pfx B; sleep 0.7; import -window root a2.png; echo "2. apres prefixe+B (barre rallumee)"
echo "diff a0/a2 : $(compare -metric AE a0.png a2.png null: 2>&1)"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
