#!/bin/bash
# GESTIONNAIRE CLAUDE : selection multiple dans la liste. Ctrl+clic ajoute ou
# retire une conversation, Alt+clic (ou Maj+clic quand le terminal le laisse
# passer) selectionne une plage, un clic simple vide la selection. Le clic
# droit sur une ligne d'une selection de plusieurs ouvre le menu des actions
# communes (copier, fermer / reprendre, supprimer). Idem pour "Reprendre".
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/multisel; rm -rf $V; mkdir -p $V; cd $V
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
S=/tmp/ms_$$
# xterm garde Ctrl+clic pour ses menus : on les lui retire, comme le fait un
# terminal qui laisse passer Ctrl+clic (Terminator, Konsole...).
HOME=$FAKEHOME PATH=$V/bin:$PATH xterm -xrm 'XTerm*omitTranslation: popup-menu' \
  -geometry 150x40 -fa Monospace -fs 10 \
  -e "cd $V/work && HOME=$FAKEHOME PATH=$V/bin:$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null & XP=$!
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
sel(){ $TM -S $S list-panes -F '#{pane_index} #{pane_claude_selected}' | awk '$2==1{printf "%s%s", s, $1; s=" "}'; }
geo(){ RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
       MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
       NCONV=$(fmt '#{window_panes}'); }
row(){ echo $(( MB + RY + 1 + $1 )); }                # conversation n (1..)
srow(){ echo $(( MB + RY + 1 + NCONV + 1 + $1 )); }   # reprise n (1..)
click(){ # $1 = ligne ecran, $2 = modificateur ou "", $3 = bouton
  xdotool mousemove $(cx $((RX+4))) $(cy $1); sleep 0.2
  [ -n "$2" ] && xdotool keydown $2
  xdotool click ${3:-1}; sleep 0.3
  [ -n "$2" ] && xdotool keyup $2
  sleep 0.5; }
cd $V/work && $TM -S $S claude-manager; sleep 2
for i in 1 2 3; do
  $TM -S $S select-layout -t t tiled 2>/dev/null
  $TM -S $S split-window -h -t t 2>/dev/null; sleep 0.6
  $TM -S $S select-layout -t t tiled 2>/dev/null; sleep 0.6
done
geo
ok "4 conversations" "$NCONV" "4"

echo "--- conversations ---"
click $(row 1) ""
ok "clic simple : aucune selection" "$(sel)" ""
ok "clic simple : la 1re est affichee" "$(fmt '#{pane_index}')" "0"
click $(row 3) ctrl
ok "Ctrl+clic : la 3e rejoint celle affichee" "$(sel)" "0 2"
ok "Ctrl+clic ne change pas la conversation affichee" "$(fmt '#{pane_index}')" "0"
click $(row 3) ctrl
ok "Ctrl+clic a nouveau : la 3e est retiree" "$(sel)" "0"
click $(row 2) ctrl
click $(row 4) alt
ok "Alt+clic : plage depuis la derniere cliquee" "$(sel)" "1 2 3"
import -window root m1.png
click $(row 2) ""
ok "clic simple : la selection est videe" "$(sel)" ""
ok "clic simple : la 2e est affichee" "$(fmt '#{pane_index}')" "1"

echo "--- par le menu (terminaux qui gardent Ctrl/Maj+clic) ---"
click $(row 1) "" 3; xdotool key s; sleep 0.6
ok "menu 'Ajouter a la selection'" "$(sel)" "0"
click $(row 3) "" 3; xdotool key s; sleep 0.6
ok "une 2e par le menu" "$(sel)" "0 2"
click $(row 4) "" 3; xdotool key j; sleep 0.6
ok "menu 'Selectionner jusqu'ici'" "$(sel)" "2 3"
click $(row 1) "" 3; xdotool key t; sleep 0.6
ok "menu 'Tout selectionner'" "$(sel)" "0 1 2 3"

echo "--- menu des actions communes ---"
$TM -S $S claude-mark -u; sleep 0.3
click $(row 1) ""; click $(row 2) ctrl; click $(row 3) ctrl
ok "3 conversations selectionnees" "$(sel)" "0 1 2"
click $(row 2) "" 3; sleep 0.4
import -window root m2.png
xdotool key c; sleep 0.8
ok "'Copier les chemins' : une ligne par conversation" \
   "$($TM -S $S show-buffer 2>&1 | wc -l | tr -d ' ')" "2"
ok "la selection reste apres une copie" "$(sel)" "0 1 2"
click $(row 3) "" 3; xdotool key x; sleep 0.8
import -window root m3.png
xdotool key n; sleep 0.8
ok "refus : rien n'est ferme" "$(fmt '#{window_panes}')" "4"
click $(row 3) "" 3; xdotool key x; sleep 0.8; xdotool key y; sleep 1.5
ok "'Fermer la selection' ferme les 3" "$(fmt '#{window_panes}')" "1"
ok "plus de selection" "$(sel)" ""
ok "sans selection, l'action est refusee" \
   "$($TM -S $S claude-marked -k 2>&1 | grep -c 'aucune')" "1"

echo "--- message du bus a toute la selection ---"
for a in ms-alpha ms-beta; do
  $TM -S $S select-layout -t t tiled 2>/dev/null
  $TM -S $S split-window -h -t t -e AGENT_NAME=$a 2>/dev/null; sleep 0.6
done
$TM -S $S select-layout -t t tiled 2>/dev/null; sleep 0.5
$TM -S $S claude-mark -a
R=$($TM -S $S claude-marked -m "l'equipe \"selection\" au rapport" 2>&1)
echo "  reponse : $R"
ok "envoye aux 2 agents, la conversation sans agent ignoree" \
   "$(echo "$R" | grep -c 'a 2 conversations (sans agent')" "1"
sleep 1.5
P=$(fmt '#{bus_port}')
for a in ms-alpha ms-beta; do
  ok "$a a recu le texte intact" \
     "$(curl -s "http://127.0.0.1:$P/inbox?agent=$a&since=0" | grep -c "l'equipe \\\\\"selection\\\\\" au rapport")" "1"
done
$TM -S $S claude-mark -u
for p in $($TM -S $S list-panes -F '#{pane_id} #{pane_agent}' | awk '$2 ~ /^ms-/{print $1}'); do
  $TM -S $S kill-pane -t $p
done
sleep 0.5

echo "--- reprises ---"
WORKDIR=$(fmt '#{pane_current_path}')
PROJ="$FAKEHOME/.claude/projects/$(echo "$WORKDIR" | tr '/' '-')"
mkdir -p "$PROJ"
A=aaaaaaaa-1111-1111-1111-111111111111
B=bbbbbbbb-2222-2222-2222-222222222222
C=cccccccc-3333-3333-3333-333333333333
E=eeeeeeee-4444-4444-4444-444444444444
mk $B "Correction du parseur" 60     # 1re ligne
mk $A "Refonte de l API"     300    # 2e
mk $C "Notes d architecture" 900    # 3e
mk $E "Essai de charge"      1200   # 4e
sleep 6; $TM -S $S refresh-client; sleep 1
geo
click $(srow 1) ""
click $(srow 3) ctrl
$TM -S $S claude-marked -T
ok "Ctrl+clic : 1re et 3e" "$($TM -S $S show-buffer 2>&1 | tr '\n' '|')" \
   "Correction du parseur|Notes d architecture"
click $(srow 2) alt
$TM -S $S claude-marked -T
ok "Alt+clic : plage 2..3 depuis la derniere cliquee" \
   "$($TM -S $S show-buffer 2>&1 | tr '\n' '|')" "Refonte de l API|Notes d architecture"
import -window root m4.png
click $(srow 2) "" 3; sleep 0.4
import -window root m5.png
xdotool key x; sleep 0.8; xdotool key y; sleep 1.5
ok "'Supprimer definitivement la selection' : 2 fichiers effaces" \
   "$(ls $PROJ/*.jsonl | wc -l | tr -d ' ')" "2"
ok "ce sont bien A et C" "$([ ! -f $PROJ/$A.jsonl ] && [ ! -f $PROJ/$C.jsonl ] && echo oui || echo NON)" "oui"
sleep 1; geo
click $(srow 1) ""; click $(srow 2) ctrl
click $(srow 2) "" 3; sleep 0.4; xdotool key r; sleep 3
ok "'Reprendre la selection' : 2 conversations de plus" "$(fmt '#{window_panes}')" "3"
L=$($TM -S $S list-panes -F '#{pane_id}' | while read p; do $TM -S $S capture-pane -p -t $p; done | grep -o -- '--resume [a-f0-9-]*' | sort | tr '\n' '|')
ok "avec les bons uuid" "$L" "--resume $B|--resume $E|"
ok "le serveur est vivant" "$(fmt ok)" "ok"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null; true
