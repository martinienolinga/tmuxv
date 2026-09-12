#!/bin/bash
# Sessions dialog as a TListViewer: click selects, double-click/Enter switches
# and closes, OK/Escape close, clicks elsewhere inside do nothing.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/list; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/ls_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s alpha" & XP=$!; sleep 2.5
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
$TM -S $S new-session -d -s beta; $TM -S $S new-session -d -s gamma; $TM -S $S switch-client -t alpha; sleep 0.3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
import -window root l0.png
# box geometry: nlines=3 -> h=8, w=longest+4 (>= FORM_MINW); centred: py=(38-8)/2=15; rows: alpha=16 beta=17 gamma=18; OK row = py+h-3 = 20
$TM -S $S display-sessions -c "$CL"; sleep 0.5; import -window root l1.png
read -r W H <<<"$(echo "$(fmt '#{client_width}') 38")"; echo "1. box open: diff=$(compare -metric AE l0.png l1.png null: 2>&1) (>0), session=$(fmt '#{client_session}') (alpha)"
xdotool mousemove 500 $(cy 17) click 1; sleep 0.4; import -window root l2.png
echo "2. click on 'beta' row: still open + selection drawn: diff vs open=$(compare -metric AE l1.png l2.png null: 2>&1) (>0, small), session=$(fmt '#{client_session}') (alpha)"
xdotool mousemove 300 $(cy 19) click 1; sleep 0.4; import -window root l3.png
echo "3. click on empty area inside: still open: diff vs l2=$(compare -metric AE l2.png l3.png null: 2>&1) (0)"
xdotool mousemove 500 $(cy 18) click --repeat 2 --delay 120 1; sleep 0.6; import -window root l4.png
echo "4. double-click 'gamma': session=$(fmt '#{client_session}') (gamma), closed: diff vs l0=$(compare -metric AE l0.png l4.png null: 2>&1) (~0 or small)"
$TM -S $S display-sessions -c "$CL"; sleep 0.4; key Down; key Down; import -window root l5.png; key Return; sleep 0.6
echo "5. Down Down Enter: session=$(fmt '#{client_session}') (gamma -> after 2 downs from top... expect gamma)"
$TM -S $S switch-client -t alpha; sleep 0.3
$TM -S $S display-sessions -c "$CL"; sleep 0.4; key Down; key Return; sleep 0.6
echo "6. Down Enter from alpha: session=$(fmt '#{client_session}') (beta)"
$TM -S $S display-sessions -c "$CL"; sleep 0.4; key x; key a; sleep 0.3; import -window root l7.png
echo "7. random keys x,a: still open: diff vs l0=$(compare -metric AE l0.png l7.png null: 2>&1) (>0)"
key Escape; sleep 0.6; import -window root l8.png; echo "8. Escape closes: diff vs l0=$(compare -metric AE l0.png l8.png null: 2>&1) (small: title changed only)"
$TM -S $S display-sessions -c "$CL"; sleep 0.4; import -window root l9a.png
OKX=$(( (125 - $(fmt '#{client_width}') ) )); xdotool mousemove 500 $(cy 20) click 1; sleep 0.6; import -window root l9.png
echo "9. click OK (row 20, centre): closed: diff vs l9a=$(compare -metric AE l9a.png l9.png null: 2>&1) (>0)"
$TM -S $S display-about -c "$CL"; sleep 0.4; import -window root la.png; xdotool mousemove 300 300 click 1; sleep 0.4; import -window root lb.png; key q; sleep 0.6
echo "10. about: click inside keeps it open ($(compare -metric AE la.png lb.png null: 2>&1) = 0), 'q' closes: $(compare -metric AE l0.png "$V/../list/lb.png" null: 2>&1 >/dev/null; import -window root lc.png; compare -metric AE la.png lc.png null: 2>&1) (>0)"
echo "11. alive: $(fmt ok)"
convert l2.png -crop 760x200+120+230 +repage l2_crop.png
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
