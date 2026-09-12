#!/bin/bash
# MISE A JOUR A CHAUD : le binaire est remplace, les processus survivent.
# On verifie ce qui compte vraiment : les PID sont INCHANGES (donc rien n'a ete
# relance), les programmes repondent encore, la disposition est la, et le
# serveur tourne bien le NOUVEAU binaire.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/upgrade; rm -rf $V; mkdir -p $V/work; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
# La configuration porte une option utilisateur, une valeur non par defaut et
# un ajout (-ga) : tout doit survivre a l'echange de binaire, sans doublon.
printf 'set -g mouse on\nset -g @marqueur-conf oui\nset -g history-limit 4242\nset -ga terminal-overrides ",essai:RGB"\n' > $V/conf
# Un `run` dans la configuration (comme tpm) : relu apres l'echange, avec un
# client attache - la combinaison qui faisait planter le nouveau serveur.
echo "run 'touch $V/run-fait'" >> $V/conf
# Copie du binaire = la "nouvelle version" ; empreinte differente pour le prouver.
cp "$TM" $V/tmuxv-neuf
printf '\n# marqueur de version %s\n' "$(date +%s)" >> $V/tmuxv-neuf 2>/dev/null || true
chmod +x $V/tmuxv-neuf
S=/tmp/upg_$$
xterm -geometry 140x36 -fa Monospace -fs 10 \
  -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }

echo "--- mise en place : 3 panneaux, dont un programme au long cours ---"
$TM -S $S split-window -t t; $TM -S $S split-window -t t; sleep 1
# Une tache qui LAISSE UNE TRACE : un compteur qui s'incremente dans un fichier.
# Si la mise a jour relancait le programme, le compteur repartirait de zero et
# le PID changerait - c'est ce qui distingue "herite" de "relance".
cat > $V/tache.sh <<'TACHE'
#!/bin/bash
i=0
while true; do i=$((i+1)); echo "$i $$" > "$1"; sleep 0.2; done
TACHE
chmod +x $V/tache.sh
$TM -S $S send-keys -t t.0 "$V/tache.sh $V/compteur.txt" Enter
sleep 2
# Le serveur se renomme "tmux: server" puis "tmuxv-neuf:" apres l'exec, donc ni
# pgrep -x ni un motif fixe ne le trouvent. On balaie /proc en cherchant la
# socket dans la ligne de commande, en excluant les shells (dont le mien, qui
# contient aussi le motif - piege deja rencontre trois fois).
srvpid(){
  local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    # Le CLIENT porte la meme ligne de commande que le serveur : c'est le nom
    # que tmux se donne qui les separe ("tmux: server" / "tmux: client"), et
    # apres l'exec le serveur devient "tmuxv-neuf:".
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in
      *"$S"*) echo $p; return;;
    esac
  done
}
SRV_AVANT=$(srvpid)
PIDS_AVANT=$($TM -S $S list-panes -a -F '#{pane_pid}' | sort -n | tr '\n' ' ')
LAY_AVANT=$($TM -S $S list-windows -F '#{window_layout}')
# Un reglage fait A CHAUD, jamais ecrit dans la configuration.
$TM -S $S set -g @marqueur-chaud oui
$TM -S $S set -g status-interval 7
$TM -S $S claude-rename -t %0 "essai agent" 2>/dev/null   # adresse de bus en memoire
NB_AVANT=$($TM -S $S display-message -p '#{window_panes}')
TACHE_PID_AVANT=$(awk '{print $2}' $V/compteur.txt 2>/dev/null)
CPT_AVANT=$(awk '{print $1}' $V/compteur.txt 2>/dev/null)
echo "  serveur pid=$SRV_AVANT ; panneaux pid : $PIDS_AVANT"
echo "  tache pid=$TACHE_PID_AVANT ; compteur=$CPT_AVANT"
import -window root $V/work/u0.png

echo "--- mise a jour vers l'autre binaire ---"
rm -f $V/run-fait
timeout 15 $TM -S $S upgrade-server $V/tmuxv-neuf 2>&1 | head -2
sleep 4
ok "un serveur a bien ete trouve avant la mise a jour" "$([ -n "$SRV_AVANT" ] && echo oui || echo NON)" "oui"
ok "le serveur a garde son pid (exec, pas relance)" "$(srvpid)" "$SRV_AVANT"
ok "il execute le NOUVEAU binaire" "$(readlink /proc/$SRV_AVANT/exe 2>/dev/null | xargs basename 2>/dev/null)" "tmuxv-neuf"
ok "toujours joignable" "$(timeout 4 $TM -S $S display-message -p ok 2>/dev/null)" "ok"
PIDS_APRES=$($TM -S $S list-panes -a -F '#{pane_pid}' 2>/dev/null | sort -n | tr '\n' ' ')
ok "les processus des panneaux sont les MEMES" "$PIDS_APRES" "$PIDS_AVANT"
ok "nombre de panneaux" "$(fmt '#{window_panes}')" "$NB_AVANT"
# Les IDENTIFIANTS de panneau sont renumerotes par la mise a jour (le nouveau
# serveur recree les panneaux autour des memes descripteurs) : ce qui doit etre
# conserve, c'est la GEOMETRIE. On compare donc la disposition sans la somme de
# controle ni les identifiants.
geo(){ echo "$1" | sed -E 's/^[0-9a-f]+,//; s/([0-9]+x[0-9]+,[0-9]+,[0-9]+),[0-9]+/\1/g'; }
ok "disposition conservee" "$(geo "$($TM -S $S list-windows -F '#{window_layout}')")" "$(geo "$LAY_AVANT")"
# La tache elle-meme : meme PID, et compteur qui a CONTINUE (jamais reparti).
TACHE_PID_APRES=$(awk '{print $2}' $V/compteur.txt 2>/dev/null)
CPT_APRES=$(awk '{print $1}' $V/compteur.txt 2>/dev/null)
# Le nom du programme doit revenir : sans lui, la barre de statut et le
# renommage automatique affichent du vide apres une mise a jour.
# L'adresse de bus d'une conversation renommee ne vit qu'en memoire : sans elle,
# le panneau revient a l'ecoute de son ancien nom et son courrier n'arrive plus.
ok "l'adresse de bus d'une conversation renommee survit" \
   "$($TM -S $S display-message -t %0 -p '#{pane_agent}' 2>/dev/null)" "essai-agent"
ok "option de la configuration conservee" "$($TM -S $S show -gv @marqueur-conf 2>/dev/null)" "oui"
ok "le run de la configuration est rejoue (client attache)" "$([ -f $V/run-fait ] && echo oui || echo NON)" "oui"
ok "valeur non par defaut conservee (history-limit)" "$($TM -S $S show -gv history-limit 2>/dev/null)" "4242"
ok "reglage fait a chaud conserve (@option)" "$($TM -S $S show -gv @marqueur-chaud 2>/dev/null)" "oui"
ok "reglage fait a chaud conserve (status-interval)" "$($TM -S $S show -gv status-interval 2>/dev/null)" "7"
ok "l'ajout -ga n'est pas double" "$($TM -S $S show -g terminal-overrides 2>/dev/null | grep -c essai)" "1"
ok "le nom du programme du panneau est connu" \
   "$([ -n "$($TM -S $S display-message -p '#{pane_current_command}')" ] && echo oui || echo NON)" "oui"
# Les cgroups ne doivent pas s'empiler d'une mise a jour a l'autre.
NCG=$(grep -o tmuxv-server /proc/$SRV_AVANT/cgroup 2>/dev/null | wc -l)
ok "pas d'empilement de cgroups (niveaux : $NCG)" \
   "$([ "$NCG" -le 1 ] && echo oui || echo NON)" "oui"
# Les panneaux ouverts AVANT la mise a jour doivent garder leur groupe memoire,
# sinon plus personne ne saurait les pousser en swap.
CG=$(sed 's/^0:://' /proc/$SRV_AVANT/cgroup 2>/dev/null)
case "$CG" in
  */tmuxv-server)
	SC=/sys/fs/cgroup${CG%/tmuxv-server}
	NP=$(ls -d $SC/pane-* 2>/dev/null | wc -l)
	NQ=$(cat $SC/pane-*/cgroup.procs 2>/dev/null | wc -l)
	ok "les panneaux gardent leur groupe memoire ($NP groupes, $NQ processus)" \
	   "$([ "$NP" -ge 1 ] && [ "$NQ" -ge 1 ] && echo oui || echo NON)" "oui" ;;
  *) echo "  (pas de portee cgroup ici : verification sautee)" ;;
esac
ok "la tache a garde son pid (pas relancee)" "$TACHE_PID_APRES" "$TACHE_PID_AVANT"
ok "son compteur a continue (pas reparti de zero)" \
   "$([ "${CPT_APRES:-0}" -gt "${CPT_AVANT:-0}" ] && echo oui || echo "NON ($CPT_AVANT -> $CPT_APRES)")" "oui"
sleep 1
ok "et il avance toujours" \
   "$([ "$(awk '{print $1}' $V/compteur.txt)" -gt "${CPT_APRES:-0}" ] && echo oui || echo NON)" "oui"
echo "--- les panneaux repondent-ils encore ? ---"
$TM -S $S send-keys -t t.1 "echo VIVANT-APRES-MAJ" Enter; sleep 1.5
ok "un shell repond" "$($TM -S $S capture-pane -p -t t.1 | grep -c VIVANT-APRES-MAJ)" "2"
import -window root $V/work/u1.png
echo "captures dans $V/work"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 140x36" 2>/dev/null
