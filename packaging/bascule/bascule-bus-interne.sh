#!/bin/bash
# BASCULE : le bus d'agents passe du service externe (systemd) au bus interne
# de tmuxv, sur la meme base MariaDB, sans arreter aucune conversation.
#
# Ordre impose :
#  1. @bus-db dans ~/.tmux.conf : c'est l'ANCIEN binaire qui ecrit l'etat de
#     la mise a jour a chaud, et il ne transporte pas les options - le nouveau
#     relit la configuration, c'est donc par elle que passe le reglage.
#  2. arret du service : il libere le port 4319, que le bus interne prend.
#     (Sans cela, le bus interne prendrait 4320 et les agents, reglES sur
#     4319, parleraient toujours au service : deux bus, courrier coupe en deux.)
#  3. mise a jour a chaud du serveur de production.
#  4. verifications ; en cas d'echec, retour arriere automatique.
set -u
T=/usr/bin/tmuxv; S=/tmp/tmux-1000/default
DB='mysql:///claude_agent?socket=/run/mysqld/mysqld.sock'
RETOUR=$HOME/.local/lib/tmuxv/tmuxv-3.4+tmuxv10
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; ECHEC=1; fi; }
ECHEC=0

retour_arriere(){
  echo "=== RETOUR ARRIERE ==="
  $T -S $S set -g @bus off 2>/dev/null; sleep 3          # libere le port
  sudo systemctl start claude-agent-server.service && echo "  service externe relance"
  echo "  (binaire precedent disponible : $RETOUR - tmuxv upgrade-server $RETOUR)"
  exit 1
}

echo "=== 1. configuration ==="
if ! grep -q '^set -g @bus-db' ~/.tmux.conf; then
  cp -p ~/.tmux.conf ~/.tmux.conf.avant-bus-interne
  printf '\n# Bus d agents interne a tmuxv (base partagee : communication entre\n# tmuxv et catalogue des sessions). Vide = messages en memoire.\nset -g @bus-db "%s"\n' "$DB" >> ~/.tmux.conf
  echo "  @bus-db ajoute (sauvegarde : ~/.tmux.conf.avant-bus-interne)"
else
  echo "  @bus-db deja present"
fi

echo "=== 2. arret du service externe ==="
AV=$(curl -s --max-time 3 http://127.0.0.1:4319/health | python3 -c 'import json,sys; print(json.load(sys.stdin)["messages"])' 2>/dev/null)
echo "  messages en base avant : ${AV:-?}"
sudo systemctl disable --now claude-agent-server.service 2>&1 | sed 's/^/  /'
sleep 1
ok "port 4319 libere" "$(ss -ltn 'sport = :4319' | tail -n +2 | wc -l)" "0"

echo "=== 3. mise a jour a chaud ==="
P=$($T -S $S display-message -p '#{pid}')
NB=$($T -S $S list-panes -a -F x | wc -l)
$T -S $S list-panes -a -F '#{pane_pid}' | sort > /tmp/bascule-avant.txt
$T -S $S upgrade-server /usr/bin/tmuxv || retour_arriere
k=0; until [ "$($T -S $S display-message -p '#{bus_mode}' 2>/dev/null)" = "db" ] || [ $k -ge 50 ]; do sleep 0.3; k=$((k+1)); done

echo "=== 4. verifications ==="
ok "meme serveur (pid)" "$($T -S $S display-message -p '#{pid}')" "$P"
ok "memes processus de panneaux" "$(comm -12 /tmp/bascule-avant.txt <($T -S $S list-panes -a -F '#{pane_pid}' | sort) | wc -l)" "$NB"
ok "bus interne sur 4319" "$($T -S $S display-message -p '#{bus_port}')" "4319"
ok "bus sur la base" "$($T -S $S display-message -p '#{bus_mode}')" "db"
AP=$(curl -s --max-time 3 http://127.0.0.1:4319/health | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["messages"], d["version"])' 2>/dev/null)
ok "l'historique est la (meme base)" "$(echo "$AP" | awk '{print ($1 >= '"${AV:-0}"') ? "oui" : "NON"}')" "oui"
ok "c'est bien tmuxv qui repond" "$(echo "$AP" | awk '{print $2}')" "tmuxv-bus-1"
[ "$ECHEC" = 0 ] || retour_arriere
echo "=== bascule reussie ==="
