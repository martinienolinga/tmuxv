#!/bin/bash
# PLUSIEURS TMUXV : deux serveurs distincts, chacun avec son bus interne sur son
# propre port, partagent la meme base (celle de TEST). Un agent de l'un ecrit a
# un agent de l'autre : le message doit etre DELIVRE dans la conversation de
# l'autre serveur. C'est ce qui permet a plusieurs tmuxv - et a des agents
# d'autres machines - de communiquer.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/busmulti; rm -rf $V; mkdir -p $V/home $V/work; cd $V/work
export HOME=$V/home
TESTDB='mysql:///claude_agent_test?socket=/run/mysqld/mysqld.sock'
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cat > $V/agent.sh <<'EOF'
#!/bin/bash
redraw(){ printf '\033[H\033[2J'; echo "agent pret"
          r=$(stty size 2>/dev/null | cut -d' ' -f1); printf '\033[%s;1H❯ ' "${r:-24}"; }
printf '\033[?1049h\033[?2004h'; redraw
exec 3<&0; stdbuf -o0 cat <&3 >> "$1" & CAT=$!
trap redraw WINCH
while kill -0 $CAT 2>/dev/null; do wait $CAT; done
EOF
chmod +x $V/agent.sh
mysql claude_agent_test -e "drop table if exists messages, memberships, agents, aliases" 2>/dev/null
printf 'set -g @bus-db "%s"\n' "$TESTDB" > $V/conf
# Deux serveurs, deux gestionnaires, un agent chacun.
declare -A SOCK PORT
for n in A B; do
  SOCK[$n]=/tmp/bm_${n}_$$
  xterm -geometry 120x36 -fa Monospace -fs 10 -e "cd $V/work && $TM -S ${SOCK[$n]} -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
done
sleep 3
for n in A B; do
  S=${SOCK[$n]}
  $TM -S $S claude-manager; sleep 2
  k=0; until [ "$($TM -S $S display-message -p '#{bus_mode}' 2>/dev/null)" = "db" ] || [ $k -ge 50 ]; do sleep 0.2; k=$((k+1)); done
  PORT[$n]=$($TM -S $S display-message -p '#{bus_port}')
  MGR=$($TM -S $S list-windows -F '#{window_index} #{window_claude_manager}' | awk '$2==1{print $1}')
  P=$($TM -S $S list-panes -t :$MGR -F '#{pane_id}' | head -1)
  $TM -S $S send-keys -t $P "exec -a claude bash $V/agent.sh $V/recu_$n.txt" Enter; sleep 2
  $TM -S $S claude-rename -t $P "agent-$n-$$"
done
echo "  serveur A : port ${PORT[A]} ($($TM -S ${SOCK[A]} display-message -p '#{bus_mode}'))"
echo "  serveur B : port ${PORT[B]} ($($TM -S ${SOCK[B]} display-message -p '#{bus_mode}'))"
ok "deux bus sur deux ports differents" "$([ "${PORT[A]}" != "${PORT[B]}" ] && [ "${PORT[A]}" != 0 ] && echo oui || echo NON)" "oui"
sleep 5
echo "--- A ecrit a B, par le port de A ---"
curl -s --max-time 3 -X POST http://127.0.0.1:${PORT[A]}/send \
  -d "{\"from\":\"agent-A-$$\",\"to\":\"agent-B-$$\",\"body\":\"DE-A-VERS-B\"}" >/dev/null
k=0; until grep -q DE-A-VERS-B $V/recu_B.txt 2>/dev/null || [ $k -ge 20 ]; do sleep 1; k=$((k+1)); done
ok "livre dans la conversation de l'AUTRE serveur" "$(grep -c DE-A-VERS-B $V/recu_B.txt 2>/dev/null)" "1"
ok "et pas chez l'expediteur" "$(grep -c DE-A-VERS-B $V/recu_A.txt 2>/dev/null)" "0"
echo "--- un agent exterieur (une autre machine) ecrit a A, par le port de B ---"
curl -s --max-time 3 -X POST http://127.0.0.1:${PORT[B]}/send \
  -d "{\"from\":\"poste-distant\",\"to\":\"agent-A-$$\",\"body\":\"DEPUIS-AILLEURS\"}" >/dev/null
k=0; until grep -q DEPUIS-AILLEURS $V/recu_A.txt 2>/dev/null || [ $k -ge 20 ]; do sleep 1; k=$((k+1)); done
ok "livre a A quel que soit le port utilise" "$(grep -c DEPUIS-AILLEURS $V/recu_A.txt 2>/dev/null)" "1"
ok "les deux serveurs voient les memes agents" \
   "$(curl -s http://127.0.0.1:${PORT[A]}/agents | python3 -c 'import json,sys; print(sorted(a["name"] for a in json.load(sys.stdin)["agents"]))')" \
   "$(curl -s http://127.0.0.1:${PORT[B]}/agents | python3 -c 'import json,sys; print(sorted(a["name"] for a in json.load(sys.stdin)["agents"]))')"
for n in A B; do $TM -S ${SOCK[$n]} kill-server 2>/dev/null; done
