#!/bin/bash
# Gestionnaire Claude : les conversations SAUVEGARDEES s'affichent sous
# "Reprendre" et un DOUBLE CLIC en relance une (claude --resume <uuid>).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/resume; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\n' > conf
FAKEHOME=$V/home; mkdir -p "$FAKEHOME" "$V/work"
# Le depot factice est cree plus bas, dans le repertoire que le GESTIONNAIRE
# utilise reellement (il herite de celui du client qui lance la commande).
mk(){ # $1 = uuid, $2 = titre, $3 = age en secondes
  printf '{"type":"user","cwd":"%s"}\n' "$WORKDIR" > "$PROJ/$1.jsonl"
  printf '{"type":"ai-title","aiTitle":"%s"}\n' "$2" >> "$PROJ/$1.jsonl"
  touch -d "@$(( $(date +%s) - $3 ))" "$PROJ/$1.jsonl"
}
# Un faux "claude" qui prouve ce qu'il a recu.
mkdir -p $V/bin
cat > $V/bin/claude <<'EOF'
#!/bin/bash
echo "CLAUDE-LANCE args=[$*] cwd=$PWD agent=$AGENT_NAME"
exec sleep 600
EOF
chmod +x $V/bin/claude
S=/tmp/rs_$$
HOME=$FAKEHOME PATH=$V/bin:$PATH xterm -geometry 150x40 -fa Monospace -fs 10 \
  -e "cd $V/work && HOME=$FAKEHOME PATH=$V/bin:$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
cd $V/work && $TM -S $S claude-manager; sleep 2
# Repertoire reel du gestionnaire -> nom du depot Claude (chaque / devient -).
WORKDIR=$(fmt '#{pane_current_path}')
PROJ="$FAKEHOME/.claude/projects/$(echo "$WORKDIR" | tr '/' '-')"
mkdir -p "$PROJ"
echo "repertoire du gestionnaire : $WORKDIR"
mk 11111111-1111-1111-1111-111111111111 "Refonte de l API"        300
mk 22222222-2222-2222-2222-222222222222 "Correction du parseur"    60
mk 33333333-3333-3333-3333-333333333333 "Notes d architecture"    900
sleep 6            # le balayage est limite a une fois toutes les 5 s
$TM -S S refresh-client 2>/dev/null; $TM -S $S refresh-client; sleep 1
import -window root r0.png
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
NCONV=$(fmt '#{window_panes}')
# lignes : 0 en-tete, 1..n conversations, n+1 "Reprendre", n+2.. sessions
ROW_SESS1=$(( MB + RY + 1 + NCONV + 2 ))
echo "conversations=$NCONV ; 1re session sur la ligne ecran $ROW_SESS1"
echo "--- clic simple : selection seulement ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW_SESS1) click 1; sleep 1
ok "aucune conversation creee" "$(fmt '#{window_panes}')" "$NCONV"
import -window root r1.png
# L'horloge de la barre de statut change entre deux captures : on ne compare
# que la colonne de la liste.
# On compare EXACTEMENT la ligne cliquee (une ligne de haut), pas un bloc :
# ailleurs, l'horloge de la barre de statut fausse la comparaison.
ligne(){ convert "$1" -crop $(( 30*8 ))x17+$(( (RX+1)*8 ))+$(( ROW_SESS1*17 )) +repage "$2"; }
ligne r0.png k0.png; ligne r1.png k1.png
DIFF=$(compare -metric AE k0.png k1.png null: 2>&1)
ok "la ligne cliquee est mise en evidence ($DIFF pixels changes)" \
   "$([ "$DIFF" -gt 0 ] && echo oui || echo NON)" "oui"
echo "--- double clic : la conversation revient ---"
xdotool mousemove $(cx $((RX+4))) $(cy $ROW_SESS1) click --repeat 2 --delay 150 1; sleep 3
ok "une conversation de plus" "$(fmt '#{window_panes}')" "$(( NCONV + 1 ))"
CONTENU=$($TM -S $S capture-pane -p -t t | grep CLAUDE-LANCE | head -1)
echo "  lance : $CONTENU"
ok "claude --resume avec le bon uuid" "$(echo "$CONTENU" | grep -c -- '--resume 22222222-2222-2222-2222-222222222222')" "1"
# capture-pane tronque a la largeur du panneau : on interroge le panneau.
ok "dans le bon repertoire" "$(fmt '#{pane_current_path}')" "$V/work"
import -window root r2.png
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 150x40" 2>/dev/null
