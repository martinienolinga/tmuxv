#!/bin/bash
# GESTIONNAIRE CLAUDE : les conversations cachees gardent la taille de celle
# qui est affichee (en mosaique, trente conversations n'avaient que 24x5 et
# Claude Code n'y dessinait pas son invite : le courrier ne partait plus).
# Changer de conversation ne fait redessiner personne d'autre. Un agent qui
# montre un menu d'autorisation ne recoit rien tant que le menu est la. Et un
# agent en veille depuis plus d'une heure recoit encore son courrier.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
TESTDB='mysql:///claude_agent_test?socket=/run/mysqld/mysqld.sock'
V=$D/torture/mgrhidden; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\nset -g @bus-port 4380\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
S=/tmp/mh_$$
fmt(){ $TM -S $S display-message -p "$@" 2>/dev/null; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }

# Faux agent : invite EN BAS (comme Claude Code), redessinee a chaque
# changement de taille (compte dans $2) ; $3 dit s'il montre son invite ou un
# menu d'autorisation ; USR1 le redessine apres changement de mode.
cat > $V/bin/agent.sh <<'EOF'
#!/bin/bash
R=$1; W=$2; M=$3
draw(){
  L=$(tput lines 2>/dev/null || echo 24)
  printf '\033[H\033[2J'
  for ((i = 1; i < L - 2; i++)); do echo; done
  if [ "$(cat $M 2>/dev/null)" = menu ]; then
    echo " Do you want to proceed?"; printf ' \342\235\257 1. Yes'
  else
    echo "agent pret"; printf '\342\235\257 '
  fi
}
printf '\033[?1049h\033[?2004h'
draw
exec 3<&0
stdbuf -o0 cat <&3 >> "$R" &
CAT=$!
trap 'echo x >> "$W"; draw' WINCH
trap draw USR1
while kill -0 $CAT 2>/dev/null; do wait $CAT; done
EOF
chmod +x $V/bin/agent.sh

xterm -geometry 150x44 -fa Monospace -fs 10 -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null & XP=$!
sleep 3
$TM -S $S claude-manager; sleep 2
MW=$(fmt '#{window_index}')
for n in 1 2 3 4 5 6 7 8; do
  echo prompt > $V/mode$n
  $TM -S $S select-layout -t $MW tiled 2>/dev/null
  $TM -S $S split-window -d -t $MW -e AGENT_NAME=mh-$n \
    "exec -a claude bash $V/bin/agent.sh $V/recu$n.txt $V/winch$n.txt $V/mode$n"
  sleep 0.4
done
$TM -S $S select-layout -t $MW tiled; sleep 3
echo "fenetre $(fmt -t :$MW '#{window_width}x#{window_height}') ; $(fmt -t :$MW '#{window_panes}') conversations"

echo "--- les conversations cachees ont la taille de celle affichee ---"
ACT=$(fmt -t :$MW '#{pane_width}x#{pane_height}')
echo "  affichee : $ACT"
ok "toutes les conversations ont cette taille" \
   "$($TM -S $S list-panes -t :$MW -F '#{pane_width}x#{pane_height}' | sort -u | tr '\n' ' ')" "$ACT "
ok "la fenetre reste zoomee" "$(fmt -t :$MW '#{window_zoomed_flag}')" "1"

echo "--- changer de conversation ne fait redessiner personne ---"
sleep 2
w0=$(cat $V/winch*.txt 2>/dev/null | wc -l)
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
for r in 3 5 2 6; do
  xdotool mousemove $(cx $((RX+4))) $(cy $((MB + RY + 1 + r))) click 1; sleep 0.8
done
sleep 1.5
ok "4 clics : la conversation affichee a change" "$(fmt -t :$MW '#{pane_index}')" "5"
ok "aucune conversation redessinee" "$(( $(cat $V/winch*.txt 2>/dev/null | wc -l) - w0 ))" "0"
ok "toujours une seule taille" \
   "$($TM -S $S list-panes -t :$MW -F '#{pane_width}x#{pane_height}' | sort -u | wc -l | tr -d ' ')" "1"

echo "--- un agent cache recoit son courrier ---"
B=http://127.0.0.1:$(fmt '#{bus_port}')
curl -s -X POST $B/send -d '{"from":"mh-test","to":"mh-2","body":"COURRIER-CACHE"}' >/dev/null
k=0; until grep -q COURRIER-CACHE $V/recu2.txt 2>/dev/null || [ $k -ge 30 ]; do sleep 0.5; k=$((k+1)); done
ok "livre a mh-2 (cache)" "$(grep -c COURRIER-CACHE $V/recu2.txt 2>/dev/null)" "1"

echo "--- un menu d'autorisation n'est jamais repondu a la place de l'utilisateur ---"
echo menu > $V/mode3; P3=$(pgrep -f "agent.sh $V/recu3.txt" | head -1); kill -USR1 $P3; sleep 1
curl -s -X POST $B/send -d '{"from":"mh-test","to":"mh-3","body":"PENDANT-LE-MENU"}' >/dev/null
sleep 8
ok "rien n'est tape tant que le menu est la" "$(cat $V/recu3.txt 2>/dev/null | grep -c PENDANT-LE-MENU)" "0"
echo prompt > $V/mode3; kill -USR1 $P3
k=0; until grep -q PENDANT-LE-MENU $V/recu3.txt 2>/dev/null || [ $k -ge 30 ]; do sleep 0.5; k=$((k+1)); done
ok "revenu a son invite, il le recoit" "$(grep -c PENDANT-LE-MENU $V/recu3.txt 2>/dev/null)" "1"

echo "--- un agent en veille depuis deux heures recoit encore son courrier ---"
mysql claude_agent_test -e "drop table if exists messages, memberships, agents, aliases, pieces" 2>/dev/null
$TM -S $S set -g @bus-db "$TESTDB"
k=0; until [ "$(fmt '#{bus_mode}')" = "db" ] || [ $k -ge 30 ]; do sleep 0.3; k=$((k+1)); done
ok "bus en base" "$(fmt '#{bus_mode}')" "db"
sleep 4
mysql claude_agent_test -e "update agents set last_seen = unix_timestamp() - 7200 where name = 'mh-4'"
ok "mh-4 n'apparait plus dans /agents" "$(curl -s $B/agents | grep -c '"mh-4"')" "0"
curl -s -X POST $B/send -d '{"from":"mh-test","to":"mh-4","body":"REVEIL-APRES-VEILLE"}' >/dev/null
k=0; until grep -q REVEIL-APRES-VEILLE $V/recu4.txt 2>/dev/null || [ $k -ge 30 ]; do sleep 0.5; k=$((k+1)); done
ok "livre malgre deux heures de veille" "$(grep -c REVEIL-APRES-VEILLE $V/recu4.txt 2>/dev/null)" "1"
ok "le serveur est vivant" "$(fmt ok)" "ok"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; true
