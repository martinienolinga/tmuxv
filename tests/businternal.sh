#!/bin/bash
# BUS INTERNE : le serveur tmuxv sert lui-meme l'API du bus d'agents, avec les
# memes regles que l'ancien service. Rejoue en MEMOIRE, puis sur la base de
# TEST (jamais celle de production), puis la bascule base -> memoire -> base.
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/businternal; rm -rf $V; mkdir -p $V/home; cd $V
export HOME=$V/home
TESTDB='mysql:///claude_agent_test?socket=/run/mysqld/mysqld.sock'
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
S=/tmp/bi_$$
$TM -S $S -f /dev/null new-session -d -s t
n=0; until [ "$($TM -S $S display-message -p '#{bus_port}' 2>/dev/null)" != "0" ] || [ $n -ge 30 ]; do sleep 0.2; n=$((n+1)); done
PORT=$($TM -S $S display-message -p '#{bus_port}')
B=http://127.0.0.1:$PORT
echo "  bus du serveur de test : port $PORT, mode $($TM -S $S display-message -p '#{bus_mode}')"
j(){ python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

scenario(){  # $1 = etiquette
  local T=$1
  curl -s $B/health | j 'd["ok"]' | { read r; ok "[$T] sante" "$r" "True"; }
  curl -s -X POST $B/register -d '{"name":"alice","group":"proj","managed":true}' >/dev/null
  curl -s -X POST $B/register -d '{"name":"bob","group":"proj"}' >/dev/null
  curl -s -X POST $B/register -d '{"name":"carol","group":"autre"}' >/dev/null
  ok "[$T] message direct" "$(curl -s -X POST $B/send -d '{"from":"alice","to":"bob","subject":"s","body":"bonjour bob"}' | j 'd["ok"]')" "True"
  ok "[$T] '*' reste dans le projet" "$(curl -s -X POST $B/send -d '{"from":"alice","to":"*","body":"salut le projet"}' | j 'd["recipients"]')" "1"
  ok "[$T] '**' atteint tout le bus" "$(curl -s -X POST $B/send -d '{"from":"alice","to":"@everywhere","body":"salut tout le bus"}' | j 'd["recipients"]')" "2"
  ok "[$T] compteurs (bob, carol)" "$(curl -s $B/agents | j '[a["pending"] for a in d["agents"] if a["name"] in ("bob","carol")]')" "[3, 1]"
  ok "[$T] ordre des cles comme l'ancien service" "$(curl -s $B/agents | python3 -c 'import sys; s=sys.stdin.read(); print(s.find("\"group\"")<s.find("\"last_seen\"")<s.find("\"managed\"")<s.find("\"name\"")<s.find("\"pending\""))')" "True"
  ok "[$T] boite de bob" "$(curl -s "$B/inbox?agent=bob&since=0" | j '[m["body"] for m in d["messages"]]')" "['bonjour bob', 'salut le projet', 'salut tout le bus']"
  C=$(curl -s "$B/inbox?agent=bob&since=0" | j 'd["cursor"]')
  ok "[$T] accuse de reception" "$(curl -s -X POST $B/ack -d "{\"agent\":\"bob\",\"upto\":$C}" | j 'd["acked"]')" "3"
  ok "[$T] premiere releve = le non lu (vide)" "$(curl -s "$B/inbox?agent=bob&since=0&unread=1" | j 'len(d["messages"])')" "0"
  curl -s -X POST $B/join -d '{"agent":"bob","channel":"drive"}' >/dev/null
  curl -s -X POST $B/join -d '{"agent":"carol","channel":"#drive"}' >/dev/null
  ok "[$T] salons" "$(curl -s $B/channels | j 'd["channels"][0]["members"]')" "['bob', 'carol']"
  curl -s -X POST $B/send -d '{"from":"alice","to":"#drive","body":"message de salon"}' >/dev/null
  ok "[$T] carol recoit le salon" "$(curl -s "$B/inbox?agent=carol&since=0" | j '"message de salon" in [m["body"] for m in d["messages"]]')" "True"
  curl -s -X POST $B/leave -d '{"agent":"carol","channel":"drive"}' >/dev/null
  ok "[$T] depart de salon" "$(curl -s $B/channels | j 'd["channels"][0]["members"]')" "['bob']"
  C=$(curl -s "$B/inbox?agent=bob&since=0" | j 'd["cursor"]')
  ( curl -s "$B/inbox?agent=bob&since=$C&wait=20" > $V/lp.json ) &
  sleep 1; T0=$(date +%s%N)
  curl -s -X POST $B/send -d '{"from":"carol","to":"bob","body":"reveille-toi"}' >/dev/null
  wait; T1=$(date +%s%N); MS=$(( (T1-T0)/1000000 ))
  ok "[$T] attente longue reveillee a l'arrivee (< 1 s)" "$([ $MS -lt 1000 ] && echo oui || echo "NON ($MS ms)")" "oui"
  ok "[$T] et elle porte le message" "$(j '[m["body"] for m in d["messages"]]' < $V/lp.json)" "['reveille-toi']"
  curl -s -X POST $B/send -d '{"from":"carol","to":"alice","body":"avant renommage"}' >/dev/null
  ok "[$T] renommage : le non lu suit" "$(curl -s -X POST $B/rename -d '{"from":"alice","to":"alice2"}' | j 'd["messages_moved"]')" "1"
  ok "[$T] l'ancien nom disparait" "$(curl -s "$B/agents?all=1" | j '"alice" in [a["name"] for a in d["agents"]]')" "False"
  ok "[$T] groupe et statut suivent" "$(curl -s "$B/agents?all=1" | j '[(a["group"],a["managed"]) for a in d["agents"] if a["name"]=="alice2"]')" "[('proj', True)]"
  curl -s -X POST $B/send -d '{"from":"alice","to":"bob","body":"vieille session"}' >/dev/null
  ok "[$T] une vieille session ne ressuscite pas l'ancien nom" "$(curl -s "$B/agents?all=1" | j '"alice" in [a["name"] for a in d["agents"]]')" "False"
  ok "[$T] ... et signe du nouveau" "$(curl -s "$B/inbox?agent=bob&since=0" | j '[m["from"] for m in d["messages"] if m["body"]=="vieille session"]')" "['alice2']"
  curl -s -X POST $B/send -d '{"from":"bob","to":"alice","body":"a l ancienne adresse"}' >/dev/null
  ok "[$T] un pair sur l'ancien nom est redirige" "$(curl -s "$B/inbox?agent=alice2&since=0" | j '"a l ancienne adresse" in [m["body"] for m in d["messages"]]')" "True"
  curl -s -X POST $B/register -d '{"name":"alice","group":"neuf","managed":true}' >/dev/null
  curl -s -X POST $B/send -d '{"from":"bob","to":"alice","body":"pour la NOUVELLE"}' >/dev/null
  ok "[$T] un nom repris recoit son propre courrier" "$(curl -s "$B/inbox?agent=alice&since=0" | j '"pour la NOUVELLE" in [m["body"] for m in d["messages"]]')" "True"
  ok "[$T] accents et emoji intacts" "$(curl -s -X POST $B/send -d '{"from":"bob","to":"carol","body":"é à ü ✅ é"}' >/dev/null; curl -s "$B/inbox?agent=carol&since=0" | j '[m["body"] for m in d["messages"]][-1]')" "é à ü ✅ é"
  ok "[$T] erreurs : champ manquant" "$(curl -s -o /dev/null -w '%{http_code}' -X POST $B/send -d '{"to":"bob"}')" "400"
  ok "[$T] erreurs : route inconnue" "$(curl -s -o /dev/null -w '%{http_code}' $B/nimporte)" "404"
}

echo "=== 1. EN MEMOIRE (aucune base) ==="
ok "mode memoire" "$($TM -S $S display-message -p '#{bus_mode}')" "local"
scenario memoire

echo "=== 2. SUR LA BASE DE TEST ==="
mysql claude_agent_test -e "drop table if exists messages, memberships, agents, aliases" 2>/dev/null
$TM -S $S kill-server; sleep 1
$TM -S $S -f /dev/null new-session -d -s t
$TM -S $S set -g @bus-db "$TESTDB"
n=0; until [ "$($TM -S $S display-message -p '#{bus_mode}' 2>/dev/null)" = "db" ] || [ $n -ge 50 ]; do sleep 0.2; n=$((n+1)); done
ok "mode base" "$($TM -S $S display-message -p '#{bus_mode}')" "db"
PORT=$($TM -S $S display-message -p '#{bus_port}'); B=http://127.0.0.1:$PORT
scenario base
ok "[base] les messages sont bien EN BASE" "$(mysql claude_agent_test -N -B -e 'select count(*)>=10 from messages' 2>/dev/null)" "1"

echo "=== 3. LA BASE TOMBE, PUIS REVIENT ==="
$TM -S $S set -g @bus-db 'mysql:///claude_agent_test?socket=/tmp/pas-de-base.sock'
n=0; until [ "$($TM -S $S display-message -p '#{bus_mode}')" = "local" ] || [ $n -ge 30 ]; do sleep 0.2; n=$((n+1)); done
ok "base injoignable : le bus passe en memoire" "$($TM -S $S display-message -p '#{bus_mode}')" "local"
AV=$(mysql claude_agent_test -N -B -e 'select count(*) from messages' 2>/dev/null)
ok "le bus repond toujours" "$(curl -s -X POST $B/send -d '{"from":"bob","to":"carol","body":"ecrit PENDANT la panne"}' | j 'd["ok"]')" "True"
ok "la conversation recoit toujours" "$(curl -s "$B/inbox?agent=carol&since=0" | j '"ecrit PENDANT la panne" in [m["body"] for m in d["messages"]]')" "True"
$TM -S $S set -g @bus-db "$TESTDB"
n=0; until [ "$($TM -S $S display-message -p '#{bus_mode}')" = "db" ] || [ $n -ge 100 ]; do sleep 0.2; n=$((n+1)); done
ok "la base revient : le bus y retourne" "$($TM -S $S display-message -p '#{bus_mode}')" "db"
ok "le message ecrit pendant la panne est verse en base" "$(mysql claude_agent_test -N -B -e "select count(*) from messages where body='ecrit PENDANT la panne'" 2>/dev/null)" "1"
ok "... une seule fois" "$(mysql claude_agent_test -N -B -e 'select count(*) from messages' 2>/dev/null)" "$((AV+1))"
ok "le serveur est vivant" "$($TM -S $S display-message -p ok)" "ok"
$TM -S $S kill-server 2>/dev/null
