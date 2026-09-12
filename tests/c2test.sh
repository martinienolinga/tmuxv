#!/bin/bash
# C2 instrumentation: are rapid ▲ clicks lost upstream (tmux mouse parsing) or in
# my scrollbar code?  Count tmux's own "down/double/triple-click" log lines vs the
# resulting scroll position. Then verify the new dialog resize callbacks.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; E=$D/torture/c2; rm -rf "$E"; mkdir -p "$E"; cd "$E"
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/c2_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -vv -f $E/conf new-session -s t" & XP=$!; sleep 2.5
W=$(xdotool search --pid $XP | tail -1); CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1"; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
$TM -S $S send-keys -t t 'seq 1 400' Enter; sleep 1
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
SBX=$(cx $((DX+DW-1))); UPY=$(cy $((1+DY+1)))
LOG=$(ls tmux-server-*.log | head -1)
for gap in 0.06 0.15 0.4; do
  $TM -S $S send-keys -t t -X cancel 2>/dev/null; sleep 0.3
  echo "MARK-$gap" >> "$LOG"
  for i in $(seq 1 11); do xdotool mousemove $SBX $UPY click 1; sleep $gap; done; sleep 0.5
  P=$(fmt '#{scroll_position}')
  N=$(sed -n "/MARK-$gap/,\$p" "$LOG" | grep -cE "down at|double-click at|triple-click at")
  echo "gap=${gap}s: tmux saw $N presses, scroll_position=$P (expected 11)"
done
echo "=== resize callbacks: form stays open and re-centres ==="
$TM -S $S send-keys -t t -X cancel 2>/dev/null
import -window root r0.png; $TM -S $S display-form -c $CL; sleep 0.5; import -window root r1.png
xdotool windowsize $W $((100*8+4)) $((30*17+4)); sleep 0.9; import -window root r2.png
echo "client now: $(fmt '#{client_width}x#{client_height}')"
xdotool windowsize $W 1004 650; sleep 0.9; import -window root r3.png
echo "diff form-open vs after-shrink (must be >0 = still visible but moved): $(compare -metric AE r1.png r2.png null: 2>&1)"
echo "diff form-open vs after-restore (should be ~0 = same place again):     $(compare -metric AE r1.png r3.png null: 2>&1)"
xdotool mousemove 30 100; xdotool key Escape; sleep 0.8
$TM -S $S display-keys -c $CL; sleep 0.5; xdotool windowsize $W $((60*8+4)) $((20*17+4)); sleep 0.9; import -window root r4.png
echo "keys dialog after shrink to 60x20 alive: $(fmt ok)"
xdotool windowsize $W $((30*8+4)) $((8*17+4)); sleep 0.9
echo "keys dialog after shrink to 30x8 (must close, alive): $(fmt ok)"
xdotool windowsize $W 1004 650; sleep 0.9
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S; echo done
