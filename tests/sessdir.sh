#!/bin/bash
# GESTIONNAIRE CLAUDE : la section "Reprendre" suit le repertoire OU SE TROUVE
# la console (un `cd` est pris en compte tout de suite) et n'y montre que les
# conversations REPRENABLES : celles qui tournent deja sont masquees.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/sessdir; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\n' > conf
FAKEHOME=$V/home; mkdir -p "$FAKEHOME" $V/work1 $V/work2 $V/work3 $V/bin
# Faux claude : le nom du processus doit etre "claude" (c'est ce que tmuxv
# regarde) et sa ligne de commande doit garder le --resume qu'on lui passe.
cat > $V/bin/stay <<'EOF'
#!/bin/bash
while :; do sleep 5; done
EOF
cat > $V/bin/claude <<EOF
#!/bin/bash
echo "CLAUDE-LANCE args=[\$*]"
exec -a claude /bin/bash $V/bin/stay "\$@"
EOF
chmod +x $V/bin/stay $V/bin/claude
proj(){ echo "$FAKEHOME/.claude/projects/$(echo "$1" | tr '/' '-')"; }
mk(){ # $1 = repertoire de travail, $2 = uuid, $3 = titre, $4 = age (s)
  local p; p=$(proj "$1"); mkdir -p "$p"
  printf '{"type":"user","cwd":"%s"}\n'          "$1" >  "$p/$2.jsonl"
  printf '{"type":"ai-title","aiTitle":"%s"}\n'  "$3" >> "$p/$2.jsonl"
  touch -d "@$(( $(date +%s) - $4 ))" "$p/$2.jsonl"; }
S=/tmp/sdir_$$
HOME=$FAKEHOME PATH=$V/bin:$PATH xterm -geometry 150x40 -fa Monospace -fs 10 \
  -e "cd $V/work1 && HOME=$FAKEHOME PATH=$V/bin:$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
# Titre de la n-ieme reprise, lu SANS rien ouvrir : menu contextuel -> "Copier".
titre(){
  local rx ry n row
  rx=$(fmt '#{window_desktop_x}'); ry=$(fmt '#{window_desktop_y}')
  n=$(fmt '#{window_panes}'); row=$(( MB + ry + 1 + n + 1 + $1 ))
  $TM -S $S set-buffer -- "AUCUNE"
  xdotool mousemove $(cx $((rx+4))) $(cy $row) click 3; sleep 0.8
  xdotool key c; sleep 0.8
  xdotool key ctrl+u; sleep 0.2
  $TM -S $S show-buffer 2>&1
}
A=aaaaaaaa-1111-1111-1111-111111111111
B=bbbbbbbb-2222-2222-2222-222222222222
C=cccccccc-3333-3333-3333-333333333333
DD=dddddddd-4444-4444-4444-444444444444
mk $V/work1 $A "Alpha work1"  30
mk $V/work1 $B "Beta work1"  300
mk $V/work2 $C "Gamma work2" 100
mk $V/work3 $DD "Delta work3"  50
cd $V/work1 && $TM -S $S claude-manager; sleep 2
$TM -S $S refresh-client; sleep 5
echo "--- au depart : les reprises de work1, la plus recente en tete ---"
ok "1re reprise" "$(titre 1)" "Alpha work1"
ok "2e reprise"  "$(titre 2)" "Beta work1"
ok "pas de 3e"   "$(titre 3)" "AUCUNE"
echo "--- la console change de repertoire : la liste suit tout de suite ---"
$TM -S $S send-keys "cd $V/work2" Enter; sleep 2
ok "1re reprise apres le cd" "$(titre 1)" "Gamma work2"
ok "plus rien de work1"      "$(titre 2)" "AUCUNE"
echo "--- retour dans work1 ---"
$TM -S $S send-keys "cd $V/work1" Enter; sleep 2
ok "on retrouve work1" "$(titre 1)" "Alpha work1"
echo "--- Alpha est reprise : elle sort de la liste tant qu'elle tourne ---"
# Par le menu contextuel, comme un utilisateur (la commande hors client n'a
# pas de session a qui parler).
reprendre(){
  local rx ry n row
  rx=$(fmt '#{window_desktop_x}'); ry=$(fmt '#{window_desktop_y}')
  n=$(fmt '#{window_panes}'); row=$(( MB + ry + 1 + n + 1 + $1 ))
  xdotool mousemove $(cx $((rx+4))) $(cy $row) click 3; sleep 0.8
  xdotool key r; sleep 3
}
reprendre 1; sleep 2
ok "la conversation est ouverte" "$($TM -S $S capture-pane -p -t t | grep -c -- "--resume $A")" "1"
sleep 6
ok "Alpha n'est plus proposee" "$(titre 1)" "Beta work1"
ok "et rien derriere"          "$(titre 2)" "AUCUNE"
echo "--- la conversation se termine : Alpha redevient reprenable ---"
$TM -S $S kill-pane -t "$($TM -S $S list-panes -F '#{pane_id} #{pane_current_command}' | awk '$2=="claude"{print $1; exit}')" 2>/dev/null
sleep 7
ok "Alpha revient en tete" "$(titre 1)" "Alpha work1"
echo "--- une conversation NEUVE (sans --resume) masque aussi la sienne ---"
$TM -S $S send-keys "cd $V/work3" Enter; sleep 2
ok "work3 propose Delta" "$(titre 1)" "Delta work3"
$TM -S $S split-window -h -c $V/work3 claude; sleep 4
touch $(proj $V/work3)/$DD.jsonl      # la conversation ecrit dans son journal
sleep 7
ok "Delta n'est plus proposee" "$(titre 1)" "AUCUNE"
ok "le serveur est vivant" "$(fmt ok)" "ok"
import -window root fin.png
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 150x40 -fa Monospace -fs 10 -e cd $V" 2>/dev/null
