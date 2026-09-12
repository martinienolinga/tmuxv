#!/bin/bash
# APRES UN REDEMARRAGE (restauration a froid), le gestionnaire revient mais
# n'est PAS a l'ecran : le courrier doit quand meme etre livre. C'est ce qui
# s'est passe en production le 11/09 - le guetteur n'etait arme que par la
# commande claude-manager, une mise a jour a chaud, ou l'affichage de la liste.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/buscold; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\nset -g @restore-interval 2\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
# Le bus est celui DU SERVEUR TESTE (interne a tmuxv), lu a chaque appel : son
# port peut changer apres une mise a jour a chaud ou un redemarrage.
bus(){ echo "http://127.0.0.1:$($TM -S $S display-message -p '#{bus_port}' 2>/dev/null)"; }
cat > $V/bin/agent.sh <<'EOF'
#!/bin/bash
# L'invite en BAS de l'ecran, comme un vrai Claude Code : tmuxv la cherche
# dans les 12 dernieres lignes (une invite en haut d'un grand panneau n'est
# jamais vue, et rien n'est livre - premiere version de ce test).
redraw(){ printf '\033[H\033[2J'; echo "agent pret"
          r=$(stty size 2>/dev/null | cut -d' ' -f1); printf '\033[%s;1H❯ ' "${r:-24}"; }
printf '\033[?1049h\033[?2004h'; redraw
exec 3<&0; stdbuf -o0 cat <&3 >> "$1" & CAT=$!
trap redraw WINCH
while kill -0 $CAT 2>/dev/null; do wait $CAT; done
EOF
chmod +x $V/bin/agent.sh
srvpid(){ local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done; }
S=/tmp/bc_$$
xterm -geometry 140x40 -fa Monospace -fs 10 -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
XP=$!; sleep 3
$TM -S $S claude-manager; sleep 2
MGR=$($TM -S $S list-windows -F '#{window_index} #{window_claude_manager}' | awk '$2==1{print $1}')
ok "un gestionnaire existe" "$([ -n "$MGR" ] && echo oui || echo NON)" "oui"
sleep 4                                   # laisser l'instantane s'ecrire
echo "--- SIGKILL puis redemarrage : restauration a froid, SANS client ---"
kill -9 $(srvpid) 2>/dev/null; sleep 1; kill $XP 2>/dev/null; sleep 1
$TM -S $S -f $V/conf start-server 2>&1 | head -2; sleep 4
MGR=$($TM -S $S list-windows -F '#{window_index} #{window_claude_manager}' 2>/dev/null | awk '$2==1{print $1}')
ok "le gestionnaire est revenu" "$([ -n "$MGR" ] && echo oui || echo NON)" "oui"
ok "aucun client : sa liste n'est dessinee nulle part" "$($TM -S $S list-clients 2>/dev/null | wc -l)" "0"
P=$($TM -S $S list-panes -t :$MGR -F '#{pane_id}' | head -1)
$TM -S $S send-keys -t $P "exec -a claude bash $V/bin/agent.sh $V/recu.txt" Enter; sleep 3
NOM=essai-froid-$$
# Du courrier ATTEND deja ce nom avant qu'une conversation le porte : c'est le
# cas de tous les agents apres un redemarrage. Il doit etre livre, pas saute.
curl -s --max-time 3 -X POST $(bus)/send \
  -d "{\"from\":\"testeur\",\"to\":\"$NOM\",\"subject\":\"t\",\"body\":\"EN-ATTENTE-AVANT\"}" >/dev/null
$TM -S $S claude-rename -t $P "$NOM" 2>/dev/null; sleep 5
curl -s --max-time 3 -X POST $(bus)/send \
  -d "{\"from\":\"testeur\",\"to\":\"$NOM\",\"subject\":\"t\",\"body\":\"APRES-REDEMARRAGE\"}" >/dev/null
n=0; until grep -q APRES-REDEMARRAGE $V/recu.txt 2>/dev/null || [ $n -ge 20 ]; do sleep 1; n=$((n+1)); done
ok "le courrier est livre sans que le gestionnaire soit affiche" "$(grep -c APRES-REDEMARRAGE $V/recu.txt 2>/dev/null)" "1"
ok "le courrier qui attendait AVANT est livre aussi" "$(grep -c EN-ATTENTE-AVANT $V/recu.txt 2>/dev/null)" "1"
ok "et une seule fois" "$(grep -o 'EN-ATTENTE-AVANT\|APRES-REDEMARRAGE' $V/recu.txt 2>/dev/null | wc -l)" "2"
$TM -S $S kill-server 2>/dev/null
curl -s --max-time 3 -X POST $(bus)/rename -d "{\"from\":\"$NOM\",\"to\":\"$NOM-fini\"}" >/dev/null
