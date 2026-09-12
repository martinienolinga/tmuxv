#!/bin/bash
# ARRET PAR LE SYSTEME : a la deconnexion (linger desactive), systemd arrete la
# scope de l'utilisateur et envoie SIGTERM au serveur. Ce n'est PAS l'utilisateur
# qui quitte : l'etat doit rester sur le disque et le demarrage suivant doit
# tout restaurer. kill-server, lui, est voulu : l'etat est efface.
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/sigterm; rm -rf $V; mkdir -p $V/work/a $V/work/b; cd $V/work
printf 'set -g @restore-interval 2\n' > $V/conf
export HOME=$V
S=/tmp/st_$$
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
etat(){ [ -s $V/.tmuxv-state ] && echo present || echo absent; }

echo "--- mise en place : 2 sessions, 3 fenetres ---"
$TM -S $S -f $V/conf new-session -d -s travail -c $V/work/a -x 120 -y 30
$TM -S $S new-window -d -t travail -c $V/work/b -n seconde
$TM -S $S new-session -d -s perso -c $V/work/b -x 120 -y 30
sleep 6
ok "l'etat est ecrit" "$(etat)" "present"
PID=$($TM -S $S display-message -p '#{pid}')

echo "--- SIGTERM venu de l'exterieur (systemd a la deconnexion) ---"
kill -TERM $PID; sleep 1.5
ok "le serveur s'est arrete" "$(kill -0 $PID 2>/dev/null && echo vivant || echo arrete)" "arrete"
ok "l'etat est garde" "$(etat)" "present"

echo "--- demarrage suivant ---"
$TM -S $S -f $V/conf start-server 2>&1 | head -2; sleep 3
ok "les 2 sessions sont revenues" \
   "$($TM -S $S list-sessions -F '#{session_name}' 2>/dev/null | sort | tr '\n' ' ')" "perso travail "
ok "les 3 fenetres sont revenues" "$($TM -S $S list-windows -a 2>/dev/null | wc -l)" "3"
ok "la fenetre 'seconde' a garde son nom" \
   "$($TM -S $S list-windows -a -F '#{window_name}' 2>/dev/null | grep -c '^seconde$')" "1"

echo "--- kill-server : voulu par l'utilisateur ---"
sleep 3
ok "le nouveau serveur a ecrit son etat" "$(etat)" "present"
$TM -S $S kill-server; sleep 1.5
ok "kill-server efface l'etat" "$(etat)" "absent"
$TM -S $S -f $V/conf new-session -d -s vide 2>&1 | head -2; sleep 1
ok "le demarrage suivant ne restaure rien" \
   "$($TM -S $S list-sessions -F '#{session_name}' 2>/dev/null | tr '\n' ' ')" "vide "

echo "--- Ctrl+C (SIGINT) sur le serveur : traite comme un arret exterieur ---"
sleep 3
PID=$($TM -S $S display-message -p '#{pid}')
kill -INT $PID; sleep 1.5
ok "l'etat est garde apres SIGINT" "$(etat)" "present"
$TM -S $S -f $V/conf start-server 2>&1 | head -2; sleep 2
ok "et restaure" "$($TM -S $S list-sessions -F '#{session_name}' 2>/dev/null | tr '\n' ' ')" "vide "
$TM -S $S kill-server 2>/dev/null; true
