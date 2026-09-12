#!/bin/bash
# Isolated test of the keyboard press-flash. NEVER touches default/-L menubar.
set -u
export DISPLAY=:99
SOCK=/tmp/flashtest_$$
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
CONF="$(dirname "$0")/desktop.conf"
OUT=${TMUXV_WORK:-/tmp/tmuxv-tests}

cleanup() {
  "$TM" -S "$SOCK" kill-server 2>/dev/null
  [ -n "${XTERM_PID:-}" ] && kill "$XTERM_PID" 2>/dev/null
  rm -f "$SOCK"
}
trap cleanup EXIT

# launch xterm running tmuxv on the isolated socket
xterm -geometry 125x38 -fa 'Monospace' -fs 10 \
  -e "$TM -S $SOCK -f $CONF new-session -s t 'sleep 600'" &
XTERM_PID=$!
sleep 2.5

CLIENT=$("$TM" -S "$SOCK" list-clients -F '#{client_name}' 2>/dev/null | head -1)
echo "client=$CLIENT"

# open the settings form on that client
"$TM" -S "$SOCK" display-form -c "$CLIENT" 2>/dev/null
sleep 0.8
import -window root "$OUT/flash_form.png" 2>/dev/null

# press Enter, then burst-capture to catch the ~90ms pressed frame
xdotool key --window "$(xdotool search --name xterm | head -1)" Return &
for i in 1 2 3 4 5 6; do
  import -window root "$OUT/flash_burst_$i.png" 2>/dev/null
done
sleep 0.5
import -window root "$OUT/flash_after.png" 2>/dev/null
echo "done"
