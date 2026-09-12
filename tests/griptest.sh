#!/bin/bash
# Resize grip vs scrollbar: no interaction (TFrame layout).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/grip; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/grip_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
rect(){ read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"; }
$TM -S $S send-keys -t t 'seq 1 400' Enter; sleep 1; rect; R0="${DW}x$DH"
# 1. corner cell (rw-1, rh-1): resize
xdotool mousemove $(cx $((DX+DW-1))) $(cy $((1+DY+DH-1))) mousedown 1 mousemove $(cx $((DX+DW-11))) $(cy $((1+DY+DH-6))) mouseup 1; sleep 0.4; rect
echo "1. corner cell drag: $R0 -> ${DW}x$DH (must shrink), mode=$(fmt '#{pane_in_mode}') pos=$(fmt '#{scroll_position}') (no scroll)"
# 2. cell left of the corner on the bottom row: resize too
R1="${DW}x$DH"; xdotool mousemove $(cx $((DX+DW-2))) $(cy $((1+DY+DH-1))) mousedown 1 mousemove $(cx $((DX+DW+8))) $(cy $((1+DY+DH+4))) mouseup 1; sleep 0.4; rect
echo "2. bottom-row cell (rw-2) drag: $R1 -> ${DW}x$DH (must grow)"
# 3. ▼ cell (rw-1, rh-2) held + dragged like a resize gesture: scroll only, size unchanged
xdotool mousemove $(cx $((DX+DW-1))) $(cy $((1+DY+1))) mousedown 1; sleep 0.9; xdotool mouseup 1; sleep 0.3; P=$(fmt '#{scroll_position}')
R2="${DW}x$DH"; xdotool mousemove $(cx $((DX+DW-1))) $(cy $((1+DY+DH-2))) mousedown 1; sleep 0.6; xdotool mousemove $(cx $((DX+DW+5))) $(cy $((1+DY+DH+3))); sleep 0.3; xdotool mouseup 1; sleep 0.4; rect
echo "3. ▼ held 0.6s then dragged: size $R2 -> ${DW}x$DH (must be unchanged), scroll $P -> $(fmt '#{scroll_position}') (must decrease)"
# 4. last track cell (rw-1, rh-3): thumb grab, no resize
R3="${DW}x$DH"; xdotool mousemove $(cx $((DX+DW-1))) $(cy $((1+DY+DH-3))) mousedown 1; sleep 0.2; xdotool mousemove $(cx $((DX+DW-1))) $(cy $((1+DY+4))); sleep 0.3; xdotool mouseup 1; sleep 0.4; rect
echo "4. last track cell press+drag up: size $R3 -> ${DW}x$DH (unchanged), pos=$(fmt '#{scroll_position}') hist=$(fmt '#{history_size}') (near top)"
echo "5. alive: $(fmt ok)"
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
