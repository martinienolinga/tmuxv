#!/bin/bash
# RESTAURATION APRES PLANTAGE : le serveur est tue par SIGKILL (donc aucune
# chance de faire quoi que ce soit en partant) ; au demarrage suivant, les
# sessions, fenetres, dispositions et repertoires doivent revenir.
# Les programmes ne sont PAS relances : les panneaux reviennent en shells.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/crashrestore; rm -rf $V; mkdir -p $V/work/a $V/work/b; cd $V/work
printf 'set -g mouse on\nset -g @restore-interval 2\n' > $V/conf
export HOME=$V                       # l'etat va dans $HOME/.tmuxv-state
S=/tmp/cr_$$
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
srvpid(){
  local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done
}

echo "--- mise en place : 2 fenetres, 3 panneaux, 2 repertoires ---"
$TM -S $S -f $V/conf new-session -d -s travail -c $V/work/a -x 120 -y 30
$TM -S $S split-window -d -t travail -c $V/work/b
$TM -S $S new-window -d -t travail -c $V/work/a -n seconde
# Une conversation renommee : son adresse de bus ne vit qu'en memoire, elle doit
# repasser par le fichier d'etat pour que son courrier arrive encore apres.
PREM=$($TM -S $S list-panes -a -F '#{pane_id}' | head -1)
$TM -S $S claude-rename -t $PREM "essai agent" 2>/dev/null
sleep 6                              # laisser le minuteur ecrire l'etat
# Les identifiants de panneaux changent forcement (ils sont recrees) : on
# compare la GEOMETRIE, pas la chaine de disposition brute.
geo(){ $TM -S $S list-panes -a -F '#{window_index}:#{pane_width}x#{pane_height}+#{pane_left},#{pane_top}' 2>/dev/null | sort | tr '\n' '|'; }
LAY=$(geo)
NW=$($TM -S $S list-windows -t travail | wc -l)
NP=$($TM -S $S list-panes -a | wc -l)
echo "  $NW fenetres, $NP panneaux"
ok "l'etat a bien ete ecrit" "$([ -s $V/.tmuxv-state ] && echo oui || echo NON)" "oui"

echo "--- SIGKILL : le serveur n'a aucune chance de se preparer ---"
SRV=$(srvpid); kill -9 $SRV; sleep 2
ok "serveur bien mort" "$(timeout 3 $TM -S $S display-message -p ok 2>/dev/null | grep -c ok)" "0"
ok "l'etat est reste sur le disque" "$([ -s $V/.tmuxv-state ] && echo oui || echo NON)" "oui"

echo "--- demarrage suivant : restauration ---"
$TM -S $S -f $V/conf start-server 2>&1 | head -2; sleep 3
ok "la session est revenue"          "$($TM -S $S list-sessions -F '#{session_name}' 2>/dev/null | tr '\n' ' ')" "travail "
ok "les 2 fenetres sont la"          "$($TM -S $S list-windows -t travail 2>/dev/null | wc -l)" "$NW"
ok "les 3 panneaux sont la"          "$($TM -S $S list-panes -a 2>/dev/null | wc -l)" "$NP"
ok "la 2e fenetre a garde son nom"   "$($TM -S $S list-windows -t travail -F '#{window_name}' 2>/dev/null | tr '\n' ' ' | grep -c seconde)" "1"
ok "les geometries sont identiques" "$(geo)" "$LAY"
ok "l'adresse de bus de la conversation renommee est revenue" \
   "$($TM -S $S list-panes -a -F '#{pane_agent}' 2>/dev/null | grep -c '^essai-agent$')" "1"
ok "un panneau a retrouve son repertoire b" "$($TM -S $S list-panes -a -F '#{pane_current_path}' 2>/dev/null | grep -c "work/b")" "1"
# Le fichier est relu puis SUPPRIME ; le nouveau serveur en ecrit aussitot un
# autre (c'est son travail). Ce qui compte : pas de restauration en double.
ok "aucune session en double"        "$($TM -S $S list-sessions 2>/dev/null | wc -l)" "1"
$TM -S $S send-keys -t travail "echo APRES-RESTAURATION" Enter; sleep 1.5
ok "les panneaux sont des shells vivants" "$($TM -S $S capture-pane -p -t travail | grep -c APRES-RESTAURATION)" "2"
# Ce que la restauration ne fait PAS, et qui doit rester vrai : les taches ne
# sont jamais relancees (un "sudo mkfs" qui repartirait tout seul serait pire
# que le plantage). Les panneaux reviennent en shells, rien d'autre.
ok "aucune tache relancee : ce sont des shells" \
   "$($TM -S $S list-panes -a -F '#{pane_current_command}' 2>/dev/null | sort -u | tr '\n' ' ')" "bash "
echo "--- arret PROPRE : l'etat ne doit pas rester ---"
$TM -S $S kill-server 2>/dev/null; sleep 2
ok "pas d'etat apres un arret propre" "$([ -e $V/.tmuxv-state ] && echo reste || echo efface)" "efface"
