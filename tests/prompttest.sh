#!/bin/bash
# command-prompt in desktop mode -> TVision input box over the live prompt.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/prompt; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf; S=/tmp/pr_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 2.5
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
key(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.15; }
typ(){ xdotool mousemove 30 100; xdotool type --delay 30 "$1"; sleep 0.2; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
sl(){ convert "$1" -crop 1000x20+0+630 +repage "${1%.png}_sl.png"; }
$TM -S $S rename-window base; sleep 0.3; import -window root p0.png; sl p0.png
# 1. rename-window prompt -> box, status line untouched
$TM -S $S command-prompt -b -t "$CL" -I '#W' -p '(rename-window)' 'rename-window -- "%%"'; sleep 0.5; import -window root p1.png; sl p1.png
echo "1. box shown: diff=$(compare -metric AE p0.png p1.png null: 2>&1) (>0); status line unchanged: $(compare -metric AE p0_sl.png p1_sl.png null: 2>&1) (0)"
typ "X"; key Return; sleep 0.6; import -window root p2.png
echo "2. typed X + Enter: window_name=$(fmt '#{window_name}') (baseX); box closed: $(compare -metric AE p0.png p2.png null: 2>&1) (~0 apart from title)"
# 3. Escape cancels
$TM -S $S command-prompt -b -t "$CL" -I '#W' -p '(rename-window)' 'rename-window -- "%%"'; sleep 0.4; typ "ZZ"; key Escape; sleep 0.6
echo "3. Escape: window_name=$(fmt '#{window_name}') (still baseX)"
# 4. mouse: click Annuler then OK (box 56x7 centred: py=15, buttons row 19; bx=17 -> OK cols 51-56, Annuler 61-71)
$TM -S $S command-prompt -b -t "$CL" -I '#W' -p '(rename-window)' 'rename-window -- "%%"'; sleep 0.4; typ "Q"
xdotool mousemove $(cx 66) $(cy 19) click 1; sleep 0.6; echo "4a. click Annuler: window_name=$(fmt '#{window_name}') (baseX)"
$TM -S $S command-prompt -b -t "$CL" -I '#W' -p '(rename-window)' 'rename-window -- "%%"'; sleep 0.4; typ "M"
xdotool mousemove $(cx 53) $(cy 19) mousedown 1; sleep 0.2; import -window root p4.png; xdotool mouseup 1; sleep 0.6
echo "4b. press OK (sunken frame captured) + release: window_name=$(fmt '#{window_name}') (baseXM)"
# 5. multiple prompts (status_prompt_update path)
$TM -S $S set -g @answered none
$TM -S $S command-prompt -b -t "$CL" -p 'un,deux' -I 'a,b' 'set -g @answered "%1+%2"'; sleep 0.4; import -window root p5a.png; key Return; sleep 0.4; import -window root p5b.png; key Return; sleep 0.6
echo "5. two prompts: second box differs from first (title deux): $(compare -metric AE p5a.png p5b.png null: 2>&1) (>0); @answered=$(fmt '#{@answered}') (a+b)"
# 6. ':' command prompt + Tab completion + history
$TM -S $S command-prompt -b -t "$CL"; sleep 0.4; typ "set -g @answered col"; key Tab; sleep 0.2; import -window root p6.png; key Return; sleep 0.6
echo "6. ':' prompt: @answered=$(fmt '#{@answered}') (colon or col: Tab completes options? at least 'col')"
# 7. long text scrolls inside the input line
$TM -S $S command-prompt -b -t "$CL" -p '(long)' 'set -g @answered "%%"'; sleep 0.4; typ "$(printf 'x%.0s' $(seq 80))"; sleep 0.3; import -window root p7.png; key Return; sleep 0.6
echo "7. 80 chars: @answered length=$(fmt '#{@answered}' | wc -c) (81)"
# 8. desktop off -> classic status prompt
$TM -S $S set -g @desktop off; $TM -S $S command-prompt -b -t "$CL" -p '(ligne)' 'set -g @answered "%%"'; sleep 0.4; import -window root p8.png; typ "ok"; key Return; sleep 0.5
echo "8. desktop off: @answered=$(fmt '#{@answered}') (ok) - prompt was in the status line (see p8.png)"; $TM -S $S set -g @desktop on
# 9. box closed by another overlay (menu) cancels the prompt cleanly
$TM -S $S set -g @answered none; $TM -S $S command-prompt -b -t "$CL" -p '(menu)' 'set -g @answered "%%"'; sleep 0.4; xdotool mousemove 40 11 click 1; sleep 0.4; key Escape; sleep 0.5
echo "9. menu over the box: @answered=$(fmt '#{@answered}') (none), alive=$(fmt ok)"
convert p1.png -crop 500x150+260+240 +repage p1_crop.png; convert p4.png -crop 500x150+260+240 +repage p4_crop.png; convert p7.png -crop 500x150+260+240 +repage p7_crop.png
$TM -S $S kill-server; kill $XP 2>/dev/null; rm -f $S
