#!/bin/bash
# GESTIONNAIRE CLAUDE : le clic droit sur une conversation sauvegardee ouvre son
# menu contextuel (Reprendre / Copier le titre / Supprimer definitivement). La
# suppression demande confirmation et efface REELLEMENT le fichier .jsonl.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/sessdel; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\n' > conf
FAKEHOME=$V/home; mkdir -p "$FAKEHOME" "$V/work"
mk(){ printf '{"type":"user","cwd":"%s"}\n' "$WORKDIR" > "$PROJ/$1.jsonl"
      printf '{"type":"ai-title","aiTitle":"%s"}\n' "$2" >> "$PROJ/$1.jsonl"
      touch -d "@$(( $(date +%s) - $3 ))" "$PROJ/$1.jsonl"; }
mkdir -p $V/bin
cat > $V/bin/claude <<'EOF'
#!/bin/bash
echo "CLAUDE-LANCE args=[$*]"
exec sleep 600
EOF
chmod +x $V/bin/claude
S=/tmp/sd_$$
HOME=$FAKEHOME PATH=$V/bin:$PATH xterm -geometry 150x40 -fa Monospace -fs 10 \
  -e "cd $V/work && HOME=$FAKEHOME PATH=$V/bin:$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
cd $V/work && $TM -S $S claude-manager; sleep 2
WORKDIR=$(fmt '#{pane_current_path}')
PROJ="$FAKEHOME/.claude/projects/$(echo "$WORKDIR" | tr '/' '-')"
mkdir -p "$PROJ"
A=aaaaaaaa-1111-1111-1111-111111111111
B=bbbbbbbb-2222-2222-2222-222222222222
C=cccccccc-3333-3333-3333-333333333333
mk $A "Refonte de l API"     300
mk $B "Correction du parseur" 60
mk $C "Notes d architecture" 900
sleep 6; $TM -S $S refresh-client; sleep 1
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
NCONV=$(fmt '#{window_panes}')
ROW1=$(( MB + RY + 1 + NCONV + 2 ))   # 1re ligne "Reprendre" (la plus recente : B)
echo "depot : $PROJ ; 1re reprise ligne $ROW1"
ok "les 3 fichiers sont la" "$(ls $PROJ/*.jsonl | wc -l)" "3"
import -window root d0.png
echo "--- clic droit sur une reprise : le menu s'ouvre ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW1) click 3; sleep 1
import -window root d1.png
ok "le menu contextuel est affiche" \
   "$([ "$(compare -metric AE d0.png d1.png null: 2>&1)" -gt 2000 ] && echo oui || echo NON)" "oui"
echo "--- 'Copier le titre' remplit le presse-papiers ---"
xdotool key c; sleep 1
ok "le titre est copie" "$($TM -S $S show-buffer 2>&1)" "Correction du parseur"
echo "--- 'Supprimer definitivement' demande confirmation ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW1) click 3; sleep 1
xdotool key x; sleep 1
import -window root d2.png
# La demande de confirmation est une boite : on la voit a l'ecran, et le texte
# est lu sur la capture.
ok "une confirmation est affichee" \
   "$([ "$(compare -metric AE d0.png d2.png null: 2>&1)" -gt 2000 ] && echo oui || echo NON)" "oui"
echo "  refus : n"
xdotool key n; sleep 1
ok "un refus ne supprime rien" "$(ls $PROJ/*.jsonl | wc -l)" "3"
ok "la conversation est toujours listee" "$([ -f $PROJ/$B.jsonl ] && echo oui || echo NON)" "oui"
echo "--- confirmation : y ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW1) click 3; sleep 1
xdotool key x; sleep 1; xdotool key y; sleep 1.5
ok "le fichier est efface" "$([ -f $PROJ/$B.jsonl ] && echo present || echo efface)" "efface"
ok "les autres sont intacts" "$(ls $PROJ/*.jsonl | wc -l)" "2"
import -window root d3.png
ok "la ligne a disparu de la liste" \
   "$([ "$(compare -metric AE d0.png d3.png null: 2>&1)" -gt 500 ] && echo oui || echo NON)" "oui"
ok "aucune conversation ouverte au passage" "$(fmt '#{window_panes}')" "$NCONV"
echo "--- la liste s'est refermee sur les 2 restantes : 'Reprendre' relance la 1re ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW1) click 3; sleep 1
xdotool key r; sleep 3
ok "une conversation de plus" "$(fmt '#{window_panes}')" "$(( NCONV + 1 ))"
LANCE=$($TM -S $S capture-pane -p -t t | grep CLAUDE-LANCE | head -1)
echo "  lance : $LANCE"
ok "c'est bien la plus recente restante (A)" "$(echo "$LANCE" | grep -c -- "--resume $A")" "1"
echo "--- la commande refuse un uuid inconnu ---"
ok "uuid inconnu rejete" \
   "$($TM -S $S claude-session -d 99999999-9999-9999-9999-999999999999 2>&1 | grep -c introuvable)" "1"
ok "rien n'a ete efface" "$(ls $PROJ/*.jsonl | wc -l)" "2"
ok "le serveur est vivant" "$(fmt ok)" "ok"
import -window root d4.png
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 150x40 -fa Monospace -fs 10 -e cd $V" 2>/dev/null
