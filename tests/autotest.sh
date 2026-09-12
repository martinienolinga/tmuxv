#!/bin/bash
# Automatic bus wiring + rename coherence + two REAL claude agents talking.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/auto; rm -rf $V; mkdir -p $V; cd $V
BUS=http://localhost:4319; MARK="AUTO-$RANDOM"
P=0; F=0
ok(){ P=$((P+1)); echo "  PASS  $1"; }
ko(){ F=$((F+1)); echo "  FAIL  $1 -- $2"; }
chk(){ if eval "$2"; then ok "$1"; else ko "$1" "$3"; fi; }

printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/auto_$$
xterm -geometry 150x44 -fa Monospace -fs 10 -e "cd $V && $TM -S $S -f $V/conf new-session -s t" & XP=$!; sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ timeout 4 $TM -S $S display-message -p "$1" 2>&1; }
pf(){ timeout 4 $TM -S $S display-message -p -t "$1" "$2" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
pane(){ $TM -S $S list-panes -F '#{pane_id}' | sed -n "$1p"; }

echo "== A. cablage automatique =="
$TM -S $S claude-manager -c "$CL"; sleep 1.5
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
TOP=$((1+DY+1)); IH=$((DH-2))
chk "~/.tmuxv/mcp.json ecrit" '[ -f $HOME/.tmuxv/mcp.json ]' "absent"
chk "wrapper ~/.tmuxv/bin/claude ecrit et executable" '[ -x $HOME/.tmuxv/bin/claude ]' "absent"
chk "le serveur MCP declare ne fige PAS AGENT_NAME" '! grep -q AGENT_NAME $HOME/.tmuxv/mcp.json' "$(cat $HOME/.tmuxv/mcp.json 2>/dev/null)"
xdotool mousemove $(cx $((DX+3))) $(cy $((TOP+IH-1))) click 1; sleep 2
chk "PATH de la conversation commence par le wrapper" \
  'tr "\0" "\n" < /proc/$($TM -S $S list-panes -F "#{pane_pid}" | sed -n 2p)/environ | grep -q "^PATH=$HOME/.tmuxv/bin:"' \
  "PATH non prefixe"
A1=$(pf "$(pane 1)" '#{pane_agent}'); A2=$(pf "$(pane 2)" '#{pane_agent}')
echo "     agents: 1='$A1'  2='$A2'"
chk "format #{pane_agent} renvoie l'adresse" '[ -n "$A1" ] && [ -n "$A2" ] && [ "$A1" != "$A2" ]' "$A1/$A2"

echo "== B. renommage coherent =="
$TM -S $S claude-rename -t "$(pane 2)" "Refonte API"; sleep 1.5
NEW=$(pf "$(pane 2)" '#{pane_agent}')
chk "le titre affiche suit" '[ "$(pf "$(pane 2)" "#{pane_title}")" = "Refonte API" ]' "titre=$(pf "$(pane 2)" '#{pane_title}')"
chk "l'adresse de bus suit (assainie)" '[ "$NEW" = "Refonte-API" ]' "adresse=$NEW"
chk "la nouvelle adresse est enregistree sur le bus" 'curl -s --max-time 4 $BUS/agents | grep -q "\"Refonte-API\""' "absente du bus"
chk "le wrapper la lira (pane_agent, pas l'env de naissance)" \
  '[ "$(pf "$(pane 2)" "#{pane_agent}")" != "$A2" ]' "inchangee"

echo "== C. deux VRAIS agents claude, sans configuration prealable =="
$TM -S $S send-keys -t "$(pane 1)" \
  "claude -p \"Envoie a l'agent 'Refonte-API' avec l'outil send_message un message dont le corps est exactement: $MARK . Sujet: auto. Puis reponds juste OK.\" --allowedTools 'mcp__tmuxv-bus__send_message' < /dev/null 2>&1 | tail -2" Enter
echo "     (agent 1 travaille...)"
for i in $(seq 1 48); do sleep 5; curl -s --max-time 4 "$BUS/inbox?agent=Refonte-API&since=0" | grep -q "$MARK" && break; done
chk "l'agent 1 a bien ecrit a l'agent renomme, via le bus" \
  'curl -s --max-time 4 "$BUS/inbox?agent=Refonte-API&since=0" | grep -q "$MARK"' "rien sur le bus"
$TM -S $S send-keys -t "$(pane 2)" \
  "claude -p \"Releve ton courrier avec check_inbox et reponds UNIQUEMENT avec le corps exact du message le plus recent.\" --allowedTools 'mcp__tmuxv-bus__check_inbox' < /dev/null 2>&1 | tail -2" Enter
echo "     (agent 2 travaille...)"
for i in $(seq 1 48); do sleep 5; $TM -S $S capture-pane -p -t "$(pane 2)" | grep -q "^$MARK" && break; done
chk "l'agent 2 a lu le message avec sa nouvelle identite" \
  '$TM -S $S capture-pane -p -t "$(pane 2)" | grep -q "^$MARK"' "pas restitue"
echo "--- fin de la conversation 2 ---"; $TM -S $S capture-pane -p -t "$(pane 2)" | grep -vE '^\s*$' | tail -3

import -window root auto.png
convert auto.png -crop $((DW*8+16))x$((DH*17+8))+$((DX*8))+$(( (1+DY)*17 )) +repage auto_crop.png
chk "serveur vivant" '[ "$(fmt ok)" = ok ]' "mort"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; rm -f $S
echo; echo "===== RESULTAT : $P reussis, $F echoues ====="
