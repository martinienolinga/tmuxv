#!/bin/bash
# La liste du gestionnaire deborde -> ascenseur vertical : molette, fleches
# maintenues (repetition), glissement du curseur. Et surtout : ce qui est
# DESSINE a une ligne est bien ce que la SOURIS y atteint, meme defile.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/listscroll; rm -rf $V; mkdir -p $V/work; cd $V
printf 'set -g mouse on\n' > conf
FAKEHOME=$V/home; mkdir -p "$FAKEHOME"
mkdir -p $V/bin
cat > $V/bin/claude <<'EOF'
#!/bin/bash
echo "CLAUDE args=[$*]"; exec sleep 600
EOF
chmod +x $V/bin/claude
S=/tmp/lsc_$$
# Fenetre volontairement PETITE pour saturer la liste.
xterm -geometry 110x24 -fa Monospace -fs 10 \
  -e "cd $V/work && HOME=$FAKEHOME PATH=$V/bin:$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
cd $V/work && $TM -S $S claude-manager; sleep 2
WORKDIR=$(fmt '#{pane_current_path}')
PROJ="$FAKEHOME/.claude/projects/$(echo "$WORKDIR" | tr '/' '-')"; mkdir -p "$PROJ"
import -window root avant.png     # liste courte : pas d'ascenseur
for i in $(seq 1 14); do
  printf '{"type":"ai-title","aiTitle":"Session numero %02d"}\n' $i > "$PROJ/aaaaaaaa-0000-0000-0000-0000000000$(printf %02d $i).jsonl"
  touch -d "@$(( $(date +%s) - i*60 ))" "$PROJ/aaaaaaaa-0000-0000-0000-0000000000$(printf %02d $i).jsonl"
done
sleep 6; $TM -S $S refresh-client; sleep 1
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}'); RW=$(fmt '#{window_desktop_w}'); RH=$(fmt '#{window_desktop_h}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
LW=$(fmt '#{window_claude_listw}' 2>/dev/null)
echo "fenetre ${RW}x${RH} en $RX,$RY"
import -window root s0.png
echo "--- l'ascenseur apparait quand ca deborde ---"
# On compare la DERNIERE COLONNE de la liste avant/apres saturation : elle est
# vide quand tout tient, elle porte la barre quand ca deborde.
LW=$(( $(fmt '#{window_desktop_w}') ))   # largeur fenetre, la liste est a gauche
barcol(){ convert "$1" -crop 8x$(( (RH-2)*17 ))+$(( (RX+1+$2)*8 ))+$(( (MB+RY+1)*17 )) +repage "$3"; }
# largeur de la liste = position de la bande "|" ; on la deduit du dessin :
# la barre est la derniere colonne AVANT la bande. On teste la zone entiere.
convert avant.png -crop $(( 30*8 ))x$(( (RH-2)*17 ))+$(( (RX+1)*8 ))+$(( (MB+RY+1)*17 )) +repage a1.png
convert s0.png    -crop $(( 30*8 ))x$(( (RH-2)*17 ))+$(( (RX+1)*8 ))+$(( (MB+RY+1)*17 )) +repage a2.png
ok "la liste change quand elle deborde" "$([ "$(compare -metric AE a1.png a2.png null: 2>&1)" -gt 0 ] && echo oui || echo NON)" "oui"
echo "--- molette vers le bas ---"
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+4 ))) click 5 click 5; sleep 1
import -window root s1.png
ok "l'affichage a change" "$([ "$(compare -metric AE s0.png s1.png null: 2>&1)" -gt 0 ] && echo oui || echo NON)" "oui"
echo "--- molette vers le haut : retour au depart ---"
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+4 ))) click 4 click 4 click 4 click 4; sleep 1
import -window root s2.png
# L'horloge de la barre de statut change : on ne compare que la colonne liste.
crop(){ convert "$1" -crop $(( (RX+1)*8 ))x$(( RH*17 ))+$(( RX*8 ))+$(( (MB+RY)*17 )) +repage "$2"; }
crop s0.png c0.png; crop s2.png c2.png
ok "retour a l'etat initial" "$(compare -metric AE c0.png c2.png null: 2>&1)" "0"
echo "--- double clic sur une session APRES defilement : la bonne est reprise ---"
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+4 ))) click 5 click 5 click 5; sleep 1
import -window root s3.png
NP=$(fmt '#{window_panes}')
# 1re ligne defilante = ligne ecran MB+RY+2
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+2 ))) click --repeat 2 --delay 150 1; sleep 3
ok "une conversation de plus" "$(fmt '#{window_panes}')" "$(( NP + 1 ))"
# La ligne affichee est tronquee a la largeur du panneau : on interroge tmux.
LANCE=$($TM -S $S list-panes -t t -F '#{pane_start_command}' | grep resume | head -1)
echo "  commande du nouveau panneau : $LANCE"
# scroll=4 (total 16, vue 12) -> la 1re ligne defilante montre sessions[2] = 03
ok "c'est bien la session AFFICHEE sur cette ligne" \
   "$(echo "$LANCE" | grep -c -- '--resume aaaaaaaa-0000-0000-0000-000000000003')" "1"
import -window root s4.png
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 110x24" 2>/dev/null
