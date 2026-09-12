#!/bin/bash
# Real end-to-end: two ACTUAL `claude` agents, each in its own conversation of
# the manager window, talking to each other through the agent bus via MCP.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/real; rm -rf $V; mkdir -p $V; cd $V
BUS=http://localhost:4319
MARK="PIVOT-$RANDOM"

# MCP config WITHOUT a pinned AGENT_NAME: the identity is inherited from the
# conversation's environment, which is what tmuxv sets per conversation.
cat > mcp.json <<'EOF'
{"mcpServers":{"agent-bus":{"command":"/home/martinien/claude-agent-server/target/release/claude-agent-mcp","env":{"AGENT_BUS_URL":"http://localhost:4319"}}}}
EOF

printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/real_$$
xterm -geometry 150x44 -fa Monospace -fs 10 -e "cd $V && $TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ timeout 4 $TM -S $S display-message -p "$1" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
pane(){ $TM -S $S list-panes -F '#{pane_id}' | sed -n "$1p"; }
agent(){ tr '\0' '\n' < /proc/$($TM -S $S list-panes -F '#{pane_pid}' | sed -n "$1p")/environ 2>/dev/null | sed -n 's/^AGENT_NAME=//p'; }

$TM -S $S claude-manager -c "$CL"; sleep 1.5
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
TOP=$((1+DY+1)); IH=$((DH-2))
xdotool mousemove $(cx $((DX+3))) $(cy $((TOP+IH-1))) click 1; sleep 2   # 2e conversation
A1=$(agent 1); A2=$(agent 2)
echo "conversation 1 = agent '$A1'   |   conversation 2 = agent '$A2'"
[ -z "$A1" ] || [ -z "$A2" ] && { echo "ECHEC: identites manquantes"; kill $XP; exit 1; }

TOOLS="mcp__agent-bus__send_message,mcp__agent-bus__check_inbox,mcp__agent-bus__list_agents"

echo
echo "== 1. l'agent de la conversation 1 ecrit a celui de la conversation 2 =="
$TM -S $S send-keys -t "$(pane 1)" \
  "claude --mcp-config $V/mcp.json --allowedTools '$TOOLS' -p \"Tu es un agent sur le bus. Envoie a l'agent '$A2', avec l'outil send_message, un message dont le corps est exactement: $MARK . Sujet: pivot. Puis reponds uniquement ENVOYE.\" 2>&1 < /dev/null 2>&1 | tail -3" Enter
echo "   (l'agent travaille...)"
for i in $(seq 1 40); do
  sleep 5
  $TM -S $S capture-pane -p -t "$(pane 1)" | grep -qE "ENVOYE|Error|error" && break
done
echo "--- sortie de la conversation 1 ---"
$TM -S $S capture-pane -p -t "$(pane 1)" | grep -vE '^\s*$' | tail -6

echo
echo "== 2. le message est-il sur le bus, adresse a $A2 ? =="
if curl -s --max-time 5 "$BUS/inbox?agent=$A2&since=0" | grep -q "$MARK"; then
  echo "   OUI: '$MARK' est dans la boite de $A2"; BUSOK=1
else
  echo "   NON: rien trouve"; BUSOK=0
fi

echo
echo "== 3. l'agent de la conversation 2 le lit avec son propre outil =="
$TM -S $S send-keys -t "$(pane 2)" \
  "claude --mcp-config $V/mcp.json --allowedTools '$TOOLS' -p \"Tu es un agent sur le bus. Utilise check_inbox pour relever ton courrier, puis reponds UNIQUEMENT avec le corps exact du message recu le plus recent, sans rien ajouter.\" 2>&1 < /dev/null 2>&1 | tail -3" Enter
for i in $(seq 1 40); do
  sleep 5
  $TM -S $S capture-pane -p -t "$(pane 2)" | grep -qE "$MARK|Error|error" && break
done
echo "--- sortie de la conversation 2 ---"
$TM -S $S capture-pane -p -t "$(pane 2)" | grep -vE '^\s*$' | tail -6
if $TM -S $S capture-pane -p -t "$(pane 2)" | grep -q "$MARK"; then
  echo "   RECU: l'agent 2 a bien restitue '$MARK'"; RECVOK=1
else
  echo "   NON RECU"; RECVOK=0
fi

import -window root real.png
convert real.png -crop $((DW*8+16))x$((DH*17+8))+$((DX*8))+$(( (1+DY)*17 )) +repage real_crop.png
echo
echo "===== BILAN: envoi sur le bus=$BUSOK, lecture par l'agent destinataire=$RECVOK ====="
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; rm -f $S
