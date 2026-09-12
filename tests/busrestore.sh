#!/bin/bash
# RESTAURER DEPUIS LA BASE : le serveur A tient une session avec deux
# conversations Claude lancees avec un identifiant EXACT (--session-id). Son
# instantane part dans le catalogue de la base (de TEST). A s'arrete. Le
# serveur B, qui n'a jamais connu cette session, la restaure : les deux
# conversations doivent reprendre sur LEURS identifiants, une fois chacune.
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/busrestore; rm -rf $V; mkdir -p $V/home $V/work $V/bin; cd $V/work
export HOME=$V/home
TESTDB='mysql:///claude_agent_test?socket=/run/mysqld/mysqld.sock'
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
# Faux claude : il note ses arguments et reste en vie. Vu par tmuxv comme
# "claude" (argv[0]) et sa ligne de commande garde --session-id / --resume.
cat > $V/bin/claude <<'EOF'
#!/bin/bash
BASE=$(cd "$(dirname "$0")/.." && pwd)
echo "ARGS $*" >> "$BASE/trace.txt"
printf '\033[H\033[2J'; r=$(stty size 2>/dev/null | cut -d' ' -f1); printf '\033[%s;1H❯ ' "${r:-24}"
while :; do sleep 5; done
EOF
chmod +x $V/bin/claude
mysql claude_agent_test -e "drop table if exists messages, memberships, agents, aliases, tmuxv_snapshots" 2>/dev/null
printf 'set -g @bus-db "%s"\nset -g @restore-interval 2\n' "$TESTDB" > $V/conf
U1=11111111-aaaa-4aaa-8aaa-111111111111
U2=22222222-bbbb-4bbb-8bbb-222222222222
SA=/tmp/br_A_$$; SB=/tmp/br_B_$$
echo "--- serveur A : une session, deux conversations a identifiant exact ---"
PATH=$V/bin:$PATH $TM -S $SA -f $V/conf new-session -d -s projet -c $V/work -x 120 -y 30
$TM -S $SA split-window -d -t projet -c $V/work
k=0; until [ "$($TM -S $SA display-message -p '#{bus_mode}')" = "db" ] || [ $k -ge 50 ]; do sleep 0.2; k=$((k+1)); done
ok "A parle a la base" "$($TM -S $SA display-message -p '#{bus_mode}')" "db"
P=($($TM -S $SA list-panes -t projet -F '#{pane_id}'))
$TM -S $SA send-keys -t ${P[0]} "exec -a claude bash $V/bin/claude --session-id $U1" Enter
$TM -S $SA send-keys -t ${P[1]} "exec -a claude bash $V/bin/claude --session-id $U2" Enter
sleep 6                                        # deux instantanes au moins
PA=$($TM -S $SA display-message -p '#{bus_port}')
NODE=$(curl -s http://127.0.0.1:$PA/sessions | python3 -c 'import json,sys; s=[x for x in json.load(sys.stdin)["sessions"] if x["session"]=="projet"]; print(s[0]["node"] if s else "")')
ok "la session est au catalogue" "$([ -n "$NODE" ] && echo oui || echo NON)" "oui"
ok "avec ses deux conversations" "$(curl -s http://127.0.0.1:$PA/sessions | python3 -c 'import json,sys; print([x["conversations"] for x in json.load(sys.stdin)["sessions"] if x["session"]=="projet"])')" "[2]"
ok "les identifiants EXACTS sont dans l'instantane" "$(curl -s "http://127.0.0.1:$PA/snapshot?node=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$NODE")&session=projet" | python3 -c "import json,sys; t=json.load(sys.stdin)['snapshot']; print(('$U1' in t) and ('$U2' in t))")" "True"
echo "--- A s'arrete proprement ---"
$TM -S $SA kill-server; sleep 1
rm -f $V/trace.txt
echo "--- serveur B, qui n'a jamais connu cette session ---"
PATH=$V/bin:$PATH $TM -S $SB -f $V/conf new-session -d -s projet -c $V/work
k=0; until [ "$($TM -S $SB display-message -p '#{bus_mode}')" = "db" ] || [ $k -ge 50 ]; do sleep 0.2; k=$((k+1)); done
ok "B voit la session de A au catalogue" "$($TM -S $SB restore-session -l 2>&1 | grep -c "^projet.$NODE")" "1"
ok "sans -f, B refuse (A vient de l'ecrire)" "$($TM -S $SB restore-session -n "$NODE" -s projet 2>&1 | grep -c 'tourne sans doute')" "1"
$TM -S $SB restore-session -f -n "$NODE" -s projet 2>&1 | sed 's/^/  /'
sleep 3
ok "une session du meme nom existait : restauree sous projet-2" "$($TM -S $SB list-sessions -F '#{session_name}' | grep -c '^projet-2$')" "1"
ok "avec ses deux panneaux" "$($TM -S $SB list-panes -t projet-2 2>/dev/null | wc -l)" "2"
ok "conversation 1 reprise sur SON identifiant" "$(grep -c -- "--resume $U1" $V/trace.txt 2>/dev/null)" "1"
ok "conversation 2 reprise sur SON identifiant" "$(grep -c -- "--resume $U2" $V/trace.txt 2>/dev/null)" "1"
ok "aucune reprise en trop" "$(grep -c -- '--resume' $V/trace.txt 2>/dev/null)" "2"
echo "--- restaurer une seconde fois : les conversations deja ouvertes ne sont pas dedoublees ---"
rm -f $V/trace.txt
$TM -S $SB restore-session -f -n "$NODE" -s projet 2>&1 | sed 's/^/  /'
sleep 3
ok "session projet-3 creee" "$($TM -S $SB list-sessions -F '#{session_name}' | grep -c '^projet-3$')" "1"
# Aucune reprise = le faux claude n'a jamais ecrit sa trace : fichier absent.
ok "mais aucune conversation rouverte en double" "$(cat $V/trace.txt 2>/dev/null | grep -c -- '--resume')" "0"
ok "le serveur B est vivant" "$($TM -S $SB display-message -p ok)" "ok"
$TM -S $SB kill-server 2>/dev/null
