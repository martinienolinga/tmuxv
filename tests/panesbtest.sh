#!/bin/bash
# One scrollbar per pane (vertical and horizontal splits), each driving its own
# pane; a sideways drag on a separator-column bar still resizes the panes.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/panesb; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/psb_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
pf(){ $TM -S $S display-message -p -t "$1" "$2" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
rect(){ read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"; }
geo(){ read -r PL PT PW PH <<<"$(pf "$1" '#{pane_left} #{pane_top} #{pane_width} #{pane_height}')"; }
$TM -S $S split-window -h; sleep 0.4; $TM -S $S send-keys -t :.0 'seq 1 300' Enter; $TM -S $S send-keys -t :.1 'seq 1 200' Enter; sleep 1
rect; geo :.0; BX0=$(cx $((DX+1+PL+PW))); BY0=$(cy $((1+DY+1+PT))); geo :.1; BX1=$(cx $((DX+DW-1))); BY1=$(cy $((1+DY+1+PT)))
import -window root s1.png; convert s1.png -crop $((DW*8+16))x$((DH*17+8))+$(( (DX)*8 ))+$(( (1+DY)*17 )) +repage s1_crop.png
echo "1. vertical split: left bar col=$((DX+1+PL+PW)) (separator), right bar col=$((DX+DW-1)) (frame)  -> see s1_crop.png"
xdotool mousemove $BX0 $BY0 click 1; sleep 0.4
echo "2. ▲ on LEFT bar: pane0 pos=$(pf :.0 '#{scroll_position}') (1), pane1 mode=$(pf :.1 '#{pane_in_mode}') (0)"
xdotool mousemove $BX1 $BY1 click 1; sleep 0.4
echo "3. ▲ on RIGHT bar: pane1 pos=$(pf :.1 '#{scroll_position}') (1), pane0 pos=$(pf :.0 '#{scroll_position}') (still 1)"
$TM -S $S send-keys -t :.0 -X cancel; $TM -S $S send-keys -t :.1 -X cancel; sleep 0.3; geo :.0; W0=$PW
TY=$(cy $((1+DY+1+PT+5)))
xdotool mousemove $BX0 $TY mousedown 1; sleep 0.15; xdotool mousemove $((BX0-40)) $TY; sleep 0.15; xdotool mousemove $((BX0-80)) $TY; sleep 0.2; xdotool mouseup 1; sleep 0.5; geo :.0
echo "4. sideways drag on LEFT bar track: pane0 width $W0 -> $PW (smaller = border resize kept), pane0 mode=$(pf :.0 '#{pane_in_mode}') (0)"
rect; geo :.0; BX0=$(cx $((DX+1+PL+PW))); TY=$(cy $((1+DY+1+PT+PH-4))); W0=$PW
xdotool mousemove $BX0 $TY mousedown 1; sleep 0.15; xdotool mousemove $BX0 $((TY-60)) ; sleep 0.15; xdotool mousemove $BX0 $(cy $((1+DY+1+PT+1))); sleep 0.2; xdotool mouseup 1; sleep 0.5; geo :.0
echo "5. vertical drag on LEFT bar track: pane0 pos=$(pf :.0 '#{scroll_position}') hist=$(pf :.0 '#{history_size}') (near top), width $W0 -> $PW (unchanged)"
$TM -S $S send-keys -t :.0 -X cancel; $TM -S $S kill-pane -t :.1; sleep 0.3
$TM -S $S split-window -v; sleep 0.4; $TM -S $S send-keys -t :.1 'seq 1 150' Enter; sleep 0.8
rect; geo :.0; TBY=$(cy $((1+DY+1+PT))); geo :.1; BBY=$(cy $((1+DY+1+PT))); BXF=$(cx $((DX+DW-1)))
import -window root s6.png; convert s6.png -crop $((DW*8+16))x$((DH*17+8))+$(( (DX)*8 ))+$(( (1+DY)*17 )) +repage s6_crop.png
xdotool mousemove $BXF $BBY click 1; sleep 0.4
echo "6. horizontal split, ▲ on BOTTOM pane bar (frame col rows of pane1): pane1 pos=$(pf :.1 '#{scroll_position}') (1), pane0 mode=$(pf :.0 '#{pane_in_mode}') (0)"
xdotool mousemove $BXF $TBY click 1; sleep 0.4
echo "7. ▲ on TOP pane bar: pane0 pos=$(pf :.0 '#{scroll_position}') (1)"
echo "8. alive: $(fmt ok)"
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
