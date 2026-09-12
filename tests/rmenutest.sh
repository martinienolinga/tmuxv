#!/bin/bash
# Right-click pane menu on the desktop: TVision colours + stays open on release.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/rmenu; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/rm_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
d(){ compare -metric AE "$1" "$2" null: 2>&1; }
import -window root r0.png
# right-click inside the pane: press, capture, release, capture
xdotool mousemove 400 300 mousedown 3; sleep 0.5; import -window root r1.png
xdotool mouseup 3; sleep 0.5; import -window root r2.png
echo "1. menu opened on right press: $(d r0.png r1.png) (>0)"
echo "2. still open after releasing the right button: $(d r1.png r2.png) (0 = unchanged)"
convert r2.png -crop 320x300+390+290 +repage r2_crop.png
# colours: sample the menu body (a few cells inside it)
echo "3. menu body colour: $(convert r2.png -crop 1x1+420+330 +repage -format '%[pixel:p{0,0}]' info:) (expect grey c0c0c0)"
# hover highlights, click selects (Zoom item is last); Escape closes
xdotool mousemove 420 330; sleep 0.4; import -window root r3.png; echo "4. hover highlights: $(d r2.png r3.png) (>0)"
xdotool mousemove 30 100; xdotool key Escape; sleep 0.5; import -window root r4.png; echo "5. Escape closes: $(d r0.png r4.png) (~0)"
# a click on an item still works: right-click then left-click 'Zoom'
xdotool mousemove 400 300 mousedown 3; sleep 0.3; xdotool mouseup 3; sleep 0.4
Y=$(( 300 + 17*10 ))   # roughly the 'Zoom' row
xdotool mousemove 420 $Y click 1; sleep 0.6
echo "6. click an item: zoomed=$(fmt '#{window_zoomed_flag}') (1 if Zoom was hit), alive=$(fmt ok)"
$TM -S $S resize-pane -Z 2>/dev/null
# desktop off -> tmux default menu colours again
$TM -S $S set -g @desktop off; sleep 0.3; xdotool mousemove 400 300 mousedown 3; sleep 0.4; xdotool mouseup 3; sleep 0.3; import -window root r5.png
echo "7. desktop off, menu body colour: $(convert r5.png -crop 1x1+420+330 +repage -format '%[pixel:p{0,0}]' info:) (tmux default, not grey)"
xdotool mousemove 30 100; xdotool key Escape; sleep 0.3
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
