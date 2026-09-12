#!/bin/bash
# confirm-before in desktop mode -> TVision [Oui] [Non] box; callback contract.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/confirm; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/cf_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
$TM -S $S set -g @answered none
import -window root c0.png
# 1. box appears (non-blocking -b), 'n' => command NOT run, box closes
$TM -S $S confirm-before -b -t "$CL" -p "Quitter tmux ? (y/n)" 'set -g @answered YES'; sleep 0.5; import -window root c1.png
echo "1. box shown: diff=$(compare -metric AE c0.png c1.png null: 2>&1) (>0)"
key n; sleep 0.5; import -window root c2.png
echo "2. after 'n': @answered=$(fmt '#{@answered}') (none), closed: diff=$(compare -metric AE c0.png c2.png null: 2>&1) (~0)"
# 3. 'y' runs the command
$TM -S $S confirm-before -b -t "$CL" -p "Quitter tmux ?" 'set -g @answered YES'; sleep 0.4; key y; sleep 0.5
echo "3. after 'y': @answered=$(fmt '#{@answered}') (YES)"
# 4. Enter = default button Oui ; Tab+Enter = Non
$TM -S $S set -g @answered none; $TM -S $S confirm-before -b -t "$CL" -p "Encore ?" 'set -g @answered ENTER'; sleep 0.4; key Return; sleep 0.5
echo "4. Enter (default Oui): @answered=$(fmt '#{@answered}') (ENTER)"
$TM -S $S set -g @answered none; $TM -S $S confirm-before -b -t "$CL" -p "Encore ?" 'set -g @answered TAB'; sleep 0.4; key Tab; key Return; sleep 0.5
echo "5. Tab+Enter (Non): @answered=$(fmt '#{@answered}') (none)"
# 6. mouse: click Oui (box centred: find button row from geometry: h = 7 -> py=(38-7)/2=15, button row = py+4 = 19)
$TM -S $S set -g @answered none; $TM -S $S confirm-before -b -t "$CL" -p "Souris ?" 'set -g @answered MOUSE'; sleep 0.4; import -window root c6.png
W=$(( 3+6 > 24 ? 3+6 : 24 )); PX=$(( (125-24)/2 )); BX=$(( PX + (24-18)/2 ))
xdotool mousemove $((BX*8+4+3*8)) $((19*17+11)) mousedown 1; sleep 0.2; import -window root c6p.png; xdotool mouseup 1; sleep 0.5
echo "6. click Oui: @answered=$(fmt '#{@answered}') (MOUSE); pressed-frame differs from open: $(compare -metric AE c6.png c6p.png null: 2>&1) (>0)"
# 7. WAITING confirm-before (no -b) + Escape must not hang the command client
$TM -S $S set -g @answered none
( timeout 5 $TM -S $S confirm-before -t "$CL" -p "Attente ?" 'set -g @answered WAIT'; echo "   waiting client returned rc=$?" ) & BG=$!; sleep 0.6; key Escape; sleep 0.8; wait $BG
echo "7. Escape on a waiting confirm: @answered=$(fmt '#{@answered}') (none), client returned (rc=1 = declined, not 124 timeout)"
# 8. custom -c key
$TM -S $S set -g @answered none; $TM -S $S confirm-before -b -t "$CL" -p "Touche k ?" -c k 'set -g @answered K'; sleep 0.4; key k; sleep 0.5
echo "8. -c k then 'k': @answered=$(fmt '#{@answered}') (K)"
echo "9. status-line fallback when desktop off: "; $TM -S $S set -g @desktop off; $TM -S $S confirm-before -b -t "$CL" -p "Ligne ?" 'set -g @answered LINE'; sleep 0.4; import -window root c9.png; key y; sleep 0.4
echo "   @answered=$(fmt '#{@answered}') (LINE)"; $TM -S $S set -g @desktop on
echo "10. alive: $(fmt ok)"
convert c1.png -crop 420x180+290+230 +repage c1_crop.png; convert c9.png -crop 500x40+0+622 +repage c9_crop.png
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
