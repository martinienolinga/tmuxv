#!/bin/bash
# Two REAL claude agents in the manager, zero prior configuration: they only
# type `claude`. One writes to the other (renamed) through the bus.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/realonly; rm -rf $V; mkdir -p $V; cd $V
BUS=http://localhost:4319; MARK="LIVE-$RANDOM"
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/ro_$$
xterm -geometry 150x44 -fa Monospace -fs 10 -e "cd $V && $TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ timeout 4 $TM -S $S display-message -p "$1" 2>&1; }
pf(){ timeout 4 $TM -S $S display-message -p -t "$1" "$2" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
pane(){ $TM -S $S list-panes -F '#{pane_id}' | sed -n "$1p"; }

$TM -S $S claude-manager -c "$CL"; sleep 1.5
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
TOP=$((1+DY+1)); IH=$((DH-2))
xdotool mousemove $(cx $((DX+3))) $(cy $((TOP+IH-1))) click 1; sleep 2
echo "PATH conv1 prefixe : $(tr '\0' '\n' < /proc/$($TM -S $S list-panes -F '#{pane_pid}' | sed -n 1p)/environ | grep -c "^PATH=$HOME/.tmuxv/bin:")"
echo "PATH conv2 prefixe : $(tr '\0' '\n' < /proc/$($TM -S $S list-panes -F '#{pane_pid}' | sed -n 2p)/environ | grep -c "^PATH=$HOME/.tmuxv/bin:")"
$TM -S $S claude-rename -t "$(pane 2)" "Refonte API"; sleep 1
A1=$(pf "$(pane 1)" '#{pane_agent}'); A2=$(pf "$(pane 2)" '#{pane_agent}')
echo "agents : conv1='$A1'  conv2='$A2' (renommee)"

echo "--- agent 1 ecrit (il tape juste 'claude', aucune option MCP) ---"
$TM -S $S send-keys -t "$(pane 1)" \
  "claude -p \"Avec l'outil send_message, envoie a l'agent '$A2' un message dont le corps est exactement: $MARK (sujet: live). Reponds juste OK.\" --allowedTools 'mcp__tmuxv-bus__send_message' < /dev/null 2>&1 | tail -2" Enter
for i in $(seq 1 50); do sleep 6; curl -s --max-time 4 "$BUS/inbox?agent=$A2&since=0" | grep -q "$MARK" && break; done
if curl -s --max-time 4 "$BUS/inbox?agent=$A2&since=0" | grep -q "$MARK"; then
  echo "OK  : '$MARK' est arrive sur le bus, adresse a $A2"; S1=1
else
  echo "NON : rien sur le bus"; S1=0; $TM -S $S capture-pane -p -t "$(pane 1)" | tail -4
fi

echo "--- agent 2 releve son courrier ---"
$TM -S $S send-keys -t "$(pane 2)" \
  "claude -p \"Releve ton courrier avec check_inbox et reponds UNIQUEMENT avec le corps exact du message le plus recent.\" --allowedTools 'mcp__tmuxv-bus__check_inbox' < /dev/null 2>&1 | tail -2" Enter
for i in $(seq 1 50); do sleep 6; $TM -S $S capture-pane -p -t "$(pane 2)" | grep -q "^$MARK" && break; done
if $TM -S $S capture-pane -p -t "$(pane 2)" | grep -q "^$MARK"; then
  echo "OK  : l'agent 2 a restitue '$MARK'"; S2=1
else
  echo "NON : pas restitue"; S2=0; $TM -S $S capture-pane -p -t "$(pane 2)" | tail -4
fi
import -window root live.png
convert live.png -crop $((DW*8+16))x$((DH*17+8))+$((DX*8))+$(( (1+DY)*17 )) +repage live_crop.png
echo "===== envoi=$S1 reception=$S2 ====="
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; rm -f $S
