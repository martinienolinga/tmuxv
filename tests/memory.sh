#!/bin/bash
# MEMOIRE : 1. les processus des panneaux sont les victimes preferees de l'OOM
# killer (oom_score_adj = @oom-score) ; 2. chaque conversation affiche ce
# qu'elle pese ; 3. alerte quand la machine manque de memoire (@memory-warn).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/memory; rm -rf $V; mkdir -p $V/work; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
OOMBASE=$(cat /proc/self/oom_score_adj); OOMWANT=$(( OOMBASE < 700 ? 700 : 1000 ))
printf 'set -g mouse on\nset -g @oom-score %s\n' "$OOMWANT" > $V/conf
S=/tmp/mem_$$
xterm -geometry 140x36 -fa Monospace -fs 10 \
  -e "cd $V/work && $TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
echo "--- 1. score OOM des processus de panneau ---"
PID=$(fmt '#{pane_pid}')
# Un processus herite du score de celui qui l'a lance, et un utilisateur ne
# peut que le RELEVER. Lance depuis un panneau tmuxv (deja a 500), le test voyait
# donc un serveur a 500 sans que tmuxv y soit pour rien. On compare au score
# HERITE, et le panneau doit porter le score demande (@oom-score, fixe au-dessus
# de l'herite pour que la verification ait un sens).
ok "shell du panneau : oom_score_adj" "$(cat /proc/$PID/oom_score_adj)" "$OOMWANT"
# Le serveur s'appelle "tmux" ou "tmuxv" selon le binaire teste, et se renomme
# "...: server" : on le retrouve par sa socket dans /proc, pas par son nom.
srvpid(){ local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done; }
ok "le serveur, lui, garde son score herite" "$(cat /proc/$(srvpid)/oom_score_adj 2>/dev/null)" "$OOMBASE"
echo "--- 2. memoire par conversation ---"
$TM -S $S claude-manager; sleep 2
import -window root m0.png              # liste AVANT : aucun chiffre
# un programme qui pese ~150 Mo, en avant-plan dans la conversation
$TM -S $S send-keys -t t "python3 -c 'import time; b = bytearray(150*1024*1024); time.sleep(600)'" Enter
sleep 5          # l'echantillonnage se fait sur le tic de 3 s
MB=$(fmt '#{pane_memory}')
ok "pane_memory releve un poids credible (>= 140 Mo)" "$([ "$MB" -ge 140 ] && echo oui || echo "NON ($MB)")" "oui"
ok "et pas delirant (< 400 Mo)"                       "$([ "$MB" -lt 400 ] && echo oui || echo "NON ($MB)")" "oui"
ok "machine pas en manque de memoire"                 "$(fmt '#{window_claude_lowmem}')" "0"
import -window root m1.png
# Le chiffre doit etre DESSINE, pas seulement disponible dans un format : on
# compare la colonne de la liste avant/apres (rien d'autre n'y bouge).
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}'); RH=$(fmt '#{window_desktop_h}')
MB1=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB1=1
liste(){ convert "$1" -crop $(( 24*8 ))x$(( (RH-2)*17 ))+$(( (RX+1)*8 ))+$(( (MB1+RY+1)*17 )) +repage "$2"; }
liste m0.png l0.png; liste m1.png l1.png
DL=$(compare -metric AE l0.png l1.png null: 2>&1)
ok "le chiffre est dessine dans la liste ($DL pixels)" "$([ "$DL" -gt 200 ] && echo oui || echo NON)" "oui"
echo "--- 3. alerte de memoire basse (seuil place a 100 Go) ---"
# En mode bureau, un message s'affiche dans une BOITE Turbo Vision centree
# (message_dialog), pas dans la ligne de statut : on regarde le centre.
CH=$(fmt '#{client_height}'); CW=$(fmt '#{client_width}')
centre(){ convert "$1" -crop $(( (CW/2)*8 ))x$(( (CH/3)*17 ))+$(( (CW/4)*8 ))+$(( (CH/3)*17 )) +repage "$2"; }
centre m1.png s0.png
$TM -S $S set -g @memory-warn 100000; sleep 4
ok "drapeau lowmem leve" "$(fmt '#{window_claude_lowmem}')" "1"
import -window root m2.png
centre m2.png s1.png
DS=$(compare -metric AE s0.png s1.png null: 2>&1)
ok "l'alerte est visible (boite au centre, $DS pixels)" "$([ "$DS" -gt 5000 ] && echo oui || echo NON)" "oui"
ok "elle est journalisee" "$($TM -S $S show-messages | grep -c 'Memoire faible')" "1"
$TM -S $S send-keys -t t Escape; sleep 0.5      # une touche la referme
$TM -S $S set -g @memory-warn 512; sleep 4
ok "drapeau retombe avec le seuil normal" "$(fmt '#{window_claude_lowmem}')" "0"
echo "captures dans $V/work"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 140x36" 2>/dev/null
