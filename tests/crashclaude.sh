#!/bin/bash
# APRES PLANTAGE : une conversation Claude doit revenir SUR ELLE-MEME
# (claude --resume <uuid>), pas en shell vide - et surtout sur la BONNE
# conversation quand plusieurs coexistent dans le meme repertoire.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/crashclaude; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
export HOME=$V
printf 'set -g mouse on\nset -g @restore-interval 2\n' > $V/conf
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
srvpid(){ local p comm
  for p in $(ls /proc | grep -E '^[0-9]+$'); do
    comm=$(cat /proc/$p/comm 2>/dev/null) || continue
    case "$comm" in bash|sh|dash|grep|xterm) continue;; esac
    case "$comm" in *client*) continue;; esac
    case "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$S"*) echo $p; return;; esac
  done; }
# Faux claude AUTONOME : quand tmuxv le relance lui-meme, aucune variable
# d'environnement ne lui est passee - il doit donc deduire ses chemins de sa
# propre position. (Premiere version dependante de $TRACE : elle mourait a la
# restauration et faisait croire a un echec du produit.)
cat > $V/bin/claude <<'EOF'
#!/bin/bash
BASE=$(cd "$(dirname "$0")/.." && pwd)
echo "CLAUDE demarre args=[$*]" >> "$BASE/trace.txt"
J="$BASE/.claude/projects/$(echo $BASE/work | tr '/' '-')/bbbbbbbb-2222-2222-2222-222222222222.jsonl"
while true; do echo x >> "$J"; sleep 1; done
EOF
chmod +x $V/bin/claude
PROJ="$V/.claude/projects/$(echo $V/work | tr '/' '-')"; mkdir -p "$PROJ"
# Deux conversations dans le MEME repertoire : une ancienne, une vivante.
OLD=aaaaaaaa-1111-1111-1111-111111111111
LIVE=bbbbbbbb-2222-2222-2222-222222222222
echo '{"type":"ai-title","aiTitle":"ancienne"}' > "$PROJ/$OLD.jsonl"
touch -d "@$(( $(date +%s) - 7200 ))" "$PROJ/$OLD.jsonl"   # vieille de 2 h
echo '{"type":"ai-title","aiTitle":"vivante"}' > "$PROJ/$LIVE.jsonl"

S=/tmp/cc_$$
$TM -S $S -f $V/conf new-session -d -s t -c $V/work -x 100 -y 28
# La conversation : le faux claude alimente le transcript "vivant".
# exec -a claude : sinon un script shell est vu comme "bash" par tmuxv, qui
# refuse alors d'y reconnaitre un agent (garde volontaire).
$TM -S $S send-keys -t t "exec -a claude bash $V/bin/claude" Enter
sleep 4
ok "tmuxv voit un agent claude" "$($TM -S $S display-message -p '#{pane_current_command}')" "claude"
sleep 3                                     # laisser l'instantane s'ecrire
ok "l'instantane retient l'uuid vivant" "$(grep -c "$LIVE" $V/.tmuxv-state 2>/dev/null)" "1"
ok "et PAS l'ancienne conversation"     "$(grep -c "$OLD" $V/.tmuxv-state 2>/dev/null)" "0"

echo "--- SIGKILL ---"
kill -9 $(srvpid); sleep 2
rm -f $V/trace.txt
echo "--- redemarrage ---"
PATH=$V/bin:$PATH $TM -S $S -f $V/conf start-server 2>&1 | head -2; sleep 4
ok "la session est revenue" "$($TM -S $S list-sessions -F '#{session_name}' 2>/dev/null)" "t"
# Apres restauration, tmuxv lance "claude --resume ..." : le processus est un
# script, donc vu comme bash - ce qui compte est qu'il ait ete lance ET avec le
# bon identifiant, ce que prouve sa trace.
ok "la fenetre porte le nom de la commande relancee" \
   "$($TM -S $S list-windows -a -F '#{window_name}' 2>/dev/null)" "claude"
echo "  trace : $(cat $V/trace.txt 2>/dev/null | head -1)"
ok "repris sur la BONNE conversation" "$(grep -c -- "--resume $LIVE" $V/trace.txt 2>/dev/null)" "1"
$TM -S $S kill-server 2>/dev/null
