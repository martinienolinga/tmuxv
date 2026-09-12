#!/bin/bash
# COUT DU SONDAGE DU BUS : au repos, un gestionnaire ne doit pas lancer un curl
# par conversation toutes les 3 s. Et le courrier doit continuer d'arriver.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/buspoll; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
# Faux agents : ils se presentent comme "claude" et affichent une invite.
# Faux agent qui se comporte comme un vrai programme plein ecran : il se
# REDESSINE sur SIGWINCH. Sans cela, un panneau reconstruit resterait blanc -
# et c'est justement ce qu'on veut verifier.
cat > $V/bin/agent.sh <<'EOF'
#!/bin/bash
redraw(){ printf '\033[H\033[2J'; echo "agent pret"; echo "❯ "; }
printf '\033[?1049h\033[?2004h'
redraw
# Un travail lance en arriere-plan par un script recoit /dev/null en entree :
# on lui redonne le terminal explicitement, sinon il voit la fin du fichier et
# le panneau se referme aussitot.
exec 3<&0
stdbuf -o0 cat <&3 >> "$1" &
CAT=$!
trap redraw WINCH
while kill -0 $CAT 2>/dev/null; do wait $CAT; done
EOF
chmod +x $V/bin/agent.sh
# Le bus est celui DU SERVEUR TESTE (interne a tmuxv), lu a chaque appel : son
# port peut changer apres une mise a jour a chaud ou un redemarrage.
bus(){ echo "http://127.0.0.1:$($TM -S $S display-message -p '#{bus_port}' 2>/dev/null)"; }
S=/tmp/bp_$$
xterm -geometry 140x40 -fa Monospace -fs 10 -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
$TM -S $S claude-manager; sleep 2
for i in 1 2 3; do
  $TM -S $S select-layout -t 1 tiled 2>/dev/null
  $TM -S $S split-window -h -t 1 2>/dev/null
  $TM -S $S select-layout -t 1 tiled 2>/dev/null; sleep 0.5
done
# chaque panneau lance un agent
i=0; for p in $($TM -S $S list-panes -t 1 -F '#{pane_id}'); do
  i=$((i+1)); $TM -S $S send-keys -t $p "exec -a claude bash $V/bin/agent.sh $V/recu$i.txt" Enter
done
sleep 8
NB=$($TM -S $S display-message -p '#{window_panes}')
AG=$($TM -S $S list-panes -t 1 -F '#{pane_agent}' | grep -c .)
echo "  $NB conversations, $AG adresses de bus"
echo "=== 30 s au repos : combien de curl lances ? ==="
# On ne compte QUE les curl issus du serveur de test : la machine en fait
# tourner d'autres (le tmuxv reel de l'utilisateur, ses agents), et les
# compter faisait echouer le test sans que le serveur teste y soit pour rien.
SRV=$($TM -S $S display-message -p '#{pid}')
curls_du_serveur(){
  local n=0 p pp k
  for p in $(pgrep -x curl 2>/dev/null); do
    pp=$p
    for k in 1 2 3 4 5; do
      # ppid : le champ APRES la parenthese fermante (le nom peut contenir
      # des espaces, un awk sur $4 tombait alors sur l'etat du processus).
      pp=$(sed 's/.*) //' /proc/$pp/stat 2>/dev/null | awk '{print $2}')
      [ -z "$pp" ] && break
      [ "$pp" = "$SRV" ] && { n=$((n+1)); break; }
      [ "$pp" -le 1 ] && break
    done
  done
  echo $n
}
N=0
for i in $(seq 1 60); do
  N=$((N + $(curls_du_serveur))); sleep 0.5
done
echo "  echantillons (2/s) ou un curl tournait : $N sur 60"
ok "sondage econome au repos (moins de 20 echantillons)" "$([ "$N" -lt 20 ] && echo oui || echo NON)" "oui"
echo "=== mais le courrier arrive-t-il toujours ? ==="
A1=$($TM -S $S list-panes -t 1 -F '#{pane_agent}' | head -1)
curl -s --max-time 3 -X POST $(bus)/send \
  -d "{\"from\":\"testeur\",\"to\":\"$A1\",\"subject\":\"t\",\"body\":\"MESSAGE-DE-CONTROLE\"}" >/dev/null
sleep 12
ok "le message a ete livre a l'agent" "$(grep -lc "MESSAGE-DE-CONTROLE" $V/recu*.txt 2>/dev/null | head -1 | grep -c . )" "1"
# ── APRES UNE MISE A JOUR A CHAUD ────────────────────────────────────────────
# Le guetteur de courrier est arme a la CREATION du gestionnaire : une fenetre
# reconstruite ne passe pas par la, et plus rien n'etait livre (vu en prod).
# Et les programmes plein ecran doivent se redessiner, sinon le panneau reste
# blanc et l'agent n'est jamais vu "a son invite".
# Une conversation RENOMMEE doit rester dans son projet (groupe) et rester
# declaree comme livree par tmuxv, sinon une diffusion `*` ne l'atteint plus.
echo "=== renommage : groupe et livraison conserves ==="
P1=$($TM -S $S list-panes -t 1 -F '#{pane_id}' | head -1)
$TM -S $S claude-rename -t $P1 "essai renomme" 2>/dev/null; sleep 3
curl -s --max-time 3 "$(bus)/agents?all=1" > $V/ag.json
python3 - "$V/ag.json" <<'EOF' 
import json,sys
a={x["name"]:x for x in json.load(open(sys.argv[1]))["agents"]}
x=a.get("essai-renomme")
if x is None: print("  ECHEC le nom renomme n'est pas inscrit au bus")
else:
    print("  OK    groupe conserve (%s)" % (x["group"] or "VIDE") if x["group"] else "  ECHEC groupe vide apres renommage")
    print("  OK    livraison par tmuxv declaree (%s)" % x["managed"] if x.get("managed") else "  ECHEC managed non declare apres renommage")
EOF
echo "=== apres une mise a jour a chaud ==="
$TM -S $S upgrade-server $TM 2>/dev/null; sleep 4
ok "le serveur repond encore" "$(timeout 4 $TM -S $S display-message -p ok 2>/dev/null)" "ok"
VIDES=$($TM -S $S list-panes -t 1 -F '#{pane_id}' 2>/dev/null | while read p; do
          [ "$($TM -S $S capture-pane -p -t $p 2>/dev/null | grep -c .)" = 0 ] && echo x; done | wc -l)
ok "aucun panneau reste blanc" "$VIDES" "0"
A2=$($TM -S $S list-panes -t 1 -F '#{pane_agent}' 2>/dev/null | head -1)
curl -s --max-time 3 -X POST $(bus)/send \
  -d "{\"from\":\"testeur\",\"to\":\"$A2\",\"subject\":\"t\",\"body\":\"APRES-MISE-A-JOUR\"}" >/dev/null
sleep 12
ok "le courrier arrive toujours apres la mise a jour" \
   "$(grep -lc "APRES-MISE-A-JOUR" $V/recu*.txt 2>/dev/null | head -1 | grep -c .)" "1"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 140x40" 2>/dev/null
