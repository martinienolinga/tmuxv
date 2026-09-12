#!/bin/bash
# Auto-repeat on held ▲/▼ (TVision evMouseAuto): desktop bar, form, msgbox.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/hold; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/hold_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
W=$(xdotool search --pid $XP | tail -1); CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
$TM -S $S send-keys -t t 'seq 1 400' Enter; sleep 1
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
SBX=$(cx $((DX+DW-1))); UPY=$(cy $((DY+2))); DNY=$(cy $((1+DY+DH-2)))
# 1. hold ▲ 1.2 s
xdotool mousemove $SBX $UPY mousedown 1; sleep 1.2; xdotool mouseup 1; sleep 0.3
P1=$(fmt '#{scroll_position}'); echo "1. hold ▲ 1.2s -> scroll_position=$P1 (expect ~14: 1 + (1200-440)/55)"
# 2. after release nothing more
sleep 0.6; echo "2. 0.6s after release -> $(fmt '#{scroll_position}') (must equal $P1)"
# 3. hold ▲, move off the arrow while holding -> repeat pauses; move back -> resumes
xdotool mousemove $SBX $UPY mousedown 1; sleep 0.7; P3a=$(fmt '#{scroll_position}')
xdotool mousemove $((SBX-40)) $UPY; sleep 0.6; P3b=$(fmt '#{scroll_position}')
xdotool mousemove $SBX $UPY; sleep 0.5; P3c=$(fmt '#{scroll_position}'); xdotool mouseup 1; sleep 0.3
echo "3. hold ▲: on=$P3a, after moving OFF 0.6s=$P3b (must equal $P3a), back ON 0.5s=$P3c (must be > $P3b)"
# 4. hold ▼ until live
xdotool mousemove $SBX $DNY mousedown 1; sleep 2.5; xdotool mouseup 1; sleep 0.3
echo "4. hold ▼ 2.5s -> scroll_position=$(fmt '#{scroll_position}') mode=$(fmt '#{pane_in_mode}') (expect 0 / 0 = back live)"
# 5. thumb drag started on track keeps following outside the column
xdotool mousemove $SBX $(cy $((DY+8))) mousedown 1; sleep 0.2; xdotool mousemove $((SBX-200)) $(cy $((DY+3))); sleep 0.3; xdotool mouseup 1; sleep 0.3
echo "5. thumb drag ending outside the bar column -> scroll_position=$(fmt '#{scroll_position}') history=$(fmt '#{history_size}') (expect near history = top)"
$TM -S $S send-keys -t t -X cancel; sleep 0.2
# 6. form: hold ▼ then ▲
$TM -S $S display-form -c $CL; sleep 0.5; import -window root f0.png
xdotool mousemove 788 521 mousedown 1; sleep 1.2; xdotool mouseup 1; sleep 0.3; import -window root f1.png
xdotool mousemove 788 96 mousedown 1; sleep 1.5; xdotool mouseup 1; sleep 0.3; import -window root f2.png
echo "6. form hold ▼ 1.2s: diff vs open=$(compare -metric AE f0.png f1.png null: 2>&1) (>0 scrolled); hold ▲ 1.5s: diff vs open=$(compare -metric AE f0.png f2.png null: 2>&1) (0 = back at top)"
xdotool mousemove 30 100; xdotool key Escape; sleep 0.8
# 7. msgbox keys: hold ▼
$TM -S $S display-keys -c $CL; sleep 0.5; import -window root k0.png
xdotool mousemove 972 555 mousedown 1; sleep 1.2; xdotool mouseup 1; sleep 0.3; import -window root k1.png
echo "7. keys dialog hold ▼ 1.2s: diff vs open=$(compare -metric AE k0.png k1.png null: 2>&1) (>0 scrolled)"
xdotool mousemove 30 100; xdotool key Escape; sleep 0.8; echo "8. alive: $(fmt ok)"
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
