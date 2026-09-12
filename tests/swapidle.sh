#!/bin/bash
# PANNEAUX INACTIFS EN SWAP : sous le seuil @memory-warn, les panneaux que
# personne ne regarde sont sortis en swap. Celui qui est AFFICHE ne l'est
# jamais. Sans cgroup delegue, la fonction s'eteint sans rien casser.
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/swapidle; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
printf 'set -g mouse on\n' > $V/conf
cat > $V/gonfle.py <<'EOF'
import sys, time
b = bytearray(250*1024*1024)
for i in range(0, len(b), 4096): b[i] = 1
sys.stdout.write("pret\n"); sys.stdout.flush()
time.sleep(600)
EOF

cat > $V/dedans.sh <<'INNER'
#!/bin/bash
TM=$1; V=$2
export DISPLAY=${DISPLAY:-:99}
S=/tmp/swp_$$
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
# tmuxv nous a deplaces dans sa feuille : la racine de la portee est au-dessus.
MY=/sys/fs/cgroup$(cat /proc/self/cgroup | cut -d: -f3)
SCOPE=$(dirname $MY)
# UN CLIENT ATTACHE est indispensable : sans lui, aucun panneau n'est
# "affiche" et tous sont candidats au swap - ce qui est correct, mais ne teste
# pas ce qu'on veut.
xterm -geometry 100x30 -fa Monospace -fs 10 \
  -e "$TM -S $S -f $V/conf new-session -s t" >/dev/null 2>&1 </dev/null &
sleep 3
$TM -S $S send-keys -t t:0 "python3 $V/gonfle.py" Enter
$TM -S $S new-window -t t: -d
$TM -S $S send-keys -t t:1 "python3 $V/gonfle.py" Enter
$TM -S $S select-window -t t:1        # la 1 est A L'ECRAN, la 0 est cachee
sleep 8
P0=$($TM -S $S list-panes -t t:0 -F '#{pane_pid}'); P1=$($TM -S $S list-panes -t t:1 -F '#{pane_pid}')
enfant(){ head -1 /proc/$1/task/$1/children 2>/dev/null | awk '{print $1}'; }
C0=$(enfant $P0); C1=$(enfant $P1)
rss(){ awk '/VmRSS/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
swp(){ awk '/VmSwap/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }
# On lit le cgroup DU PANNEAU plutot que de reconstruire le chemin : tmuxv
# deplace aussi ses voisins pour satisfaire la regle cgroup, donc notre propre
# position n'est pas un repere fiable.
ok "chaque panneau a son cgroup" \
   "$(cat /proc/$P0/cgroup | grep -c 'pane-')$(cat /proc/$P1/cgroup | grep -c 'pane-')" "11"
ok "un client est bien attache" "$($TM -S $S list-clients | wc -l)" "1"
echo "  avant : cachee RSS $(rss $C0) Mo | affichee RSS $(rss $C1) Mo"
ok "les deux ont bien alloue" "$([ "$(rss $C0)" -gt 200 ] && [ "$(rss $C1)" -gt 200 ] && echo oui || echo NON)" "oui"
$TM -S $S set -g @memory-warn 100000   # pression simulee : seuil inatteignable
sleep 12
echo "  apres : cachee RSS $(rss $C0) Mo swap $(swp $C0) Mo | affichee RSS $(rss $C1) Mo swap $(swp $C1) Mo"
ok "la fenetre CACHEE est partie en swap" "$([ "$(swp $C0)" -gt 100 ] && echo oui || echo NON)" "oui"
ok "la fenetre AFFICHEE est epargnee"     "$([ "$(swp $C1)" -lt 20 ] && echo oui || echo NON)" "oui"
ok "le processus cache est toujours vivant" "$(kill -0 $C0 2>/dev/null && echo oui || echo NON)" "oui"
$TM -S $S select-window -t t:0; sleep 2
ok "il repond encore apres retour" "$($TM -S $S list-panes -t t:0 -F '#{pane_dead}')" "0"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 100x30" 2>/dev/null
INNER
chmod +x $V/dedans.sh
echo "=== 1. dans une portee DELEGUEE ==="
systemd-run --user --scope -p Delegate=yes --quiet $V/dedans.sh "$TM" "$V" 2>&1 | grep -v "^Running scope"
echo "=== 2. sans delegation (portee de session) : rien ne doit casser ==="
S=/tmp/swpnd_$$
$TM -S $S -f $V/conf new-session -d -s t -x 80 -y 24; sleep 2
$TM -S $S set -g @memory-warn 100000; sleep 7
ok "le serveur va bien sans cgroup" "$(timeout 4 $TM -S $S display-message -p ok 2>/dev/null)" "ok"
ok "et les panneaux aussi" "$($TM -S $S list-panes -F '#{pane_dead}')" "0"
$TM -S $S kill-server 2>/dev/null
