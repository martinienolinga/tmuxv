#!/bin/bash
# Remaining "conversations" windowed on the desktop: messages, windows list,
# buffers list, messages log, keys, sessions via their default prefix keys.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/conv; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/cv_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
pfx(){ key ctrl+b; key "$1"; sleep 0.5; }
cy(){ echo $(( $1*17+11 )); }
d(){ compare -metric AE "$1" "$2" null: 2>&1; }
import -window root c0.png
# 1. display-message -> box, auto-closes after display-time (750 ms default)
$TM -S $S display-message -c "$CL" "Bonjour le bureau"; sleep 0.3; import -window root c1.png; sleep 1.2; import -window root c2.png
echo "1. message box shown: $(d c0.png c1.png) (>0); gone after display-time: $(d c0.png c2.png) (~0)"
# 2. display-time 0 -> stays until Escape
$TM -S $S set -g display-time 0; $TM -S $S display-message -c "$CL" "Reste"; sleep 1.2; import -window root c3.png; key Escape; sleep 0.5; import -window root c4.png
echo "2. display-time 0: still there after 1.2s: $(d c0.png c3.png) (>0); Escape closes: $(d c0.png c4.png) (~0)"; $TM -S $S set -g display-time 750
# 3. prefix i (display-message window info) -> box
pfx i; import -window root c5.png; echo "3. prefix+i info box: $(d c0.png c5.png) (>0)"; sleep 1.2
# 4. prefix w -> windows list; double-click 2nd row selects window 1
$TM -S $S new-window -n deux; $TM -S $S select-window -t 0; sleep 0.3; import -window root c6.png
pfx w; import -window root c7.png; echo "4a. prefix+w windows box: $(d c6.png c7.png) (>0), window_index=$(fmt '#{window_index}') (0)"
xdotool mousemove 400 $(cy 17) click --repeat 2 --delay 120 1; sleep 0.6
echo "4b. double-click row 2: window_index=$(fmt '#{window_index}') (1), window_name=$(fmt '#{window_name}') (deux)"
# 5. prefix ~ -> messages log box (contains our earlier message)
pfx "asciitilde"; import -window root c8.png; echo "5. prefix+~ messages box: $(d c6.png c8.png) (>0)"; key Escape; sleep 0.5
# 6. prefix # -> buffers box; double-click pastes into the pane
$TM -S $S set-buffer "hello-paste-xyz"; pfx numbersign; import -window root c9.png; echo "6a. prefix+# buffers box: $(d c6.png c9.png) (>0)"
xdotool mousemove 400 $(cy 16) click --repeat 2 --delay 120 1; sleep 0.8
echo "6b. double-click buffer: pane has it: $($TM -S $S capture-pane -p | grep -c hello-paste-xyz) (1)"
$TM -S $S send-keys -t t C-u
# 7. prefix ? -> keys box ; prefix s -> sessions box ; = -> buffers
pfx question; import -window root c10.png; echo "7. prefix+? keys box: $(d c6.png c10.png) (>0), mode=$(fmt '#{pane_in_mode}') (0 = not the pane view)"; key Escape; sleep 0.5
pfx s; import -window root c11.png; echo "8. prefix+s sessions box: $(d c6.png c11.png) (>0), mode=$(fmt '#{pane_in_mode}') (0)"; key Escape; sleep 0.5
pfx equal; sleep 0.2; echo "9. prefix+= buffers box, mode=$(fmt '#{pane_in_mode}') (0)"; key Escape; sleep 0.5
# 10. desktop off -> classic behaviours
$TM -S $S set -g @desktop off; pfx w; echo "10. desktop off, prefix+w: pane_in_mode=$(fmt '#{pane_in_mode}') (1 = choose-tree in the pane)"; key q; sleep 0.3
$TM -S $S display-message -c "$CL" "ligne"; sleep 0.3; import -window root c12.png; convert c12.png -crop 400x20+0+630 +repage c12_sl.png
echo "11. desktop off message in the status line (see c12_sl.png), alive: $(fmt ok)"
convert c1.png -crop 500x140+250+240 +repage c1_crop.png; convert c7.png -crop 620x150+190+240 +repage c7_crop.png; convert c9.png -crop 700x150+150+240 +repage c9_crop.png
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
