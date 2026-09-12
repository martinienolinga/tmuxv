#!/bin/bash
# Lance toute la batterie, chaque test borne dans le temps : une commande tmux
# sans timeout peut attendre indefiniment si un serveur de test est mal reveille,
# et bloquer la batterie entiere (deja vu).
export DISPLAY=:99
cd "$(dirname "$0")"
# Un test interrompu entre un "keydown ctrl" et son "keyup" laisse la touche
# ENFONCEE dans Xvfb : tous les clics des tests suivants deviennent des
# Ctrl+clics, sans effet (deja vu : midpaste a echoue ainsi, tmuxv hors de
# cause). On relache tout avant chaque test.
relache(){
  xdotool keyup Control_L Control_R Alt_L Alt_R Shift_L Shift_R Super_L Meta_L 2>/dev/null
  for b in 1 2 3; do xdotool mouseup $b 2>/dev/null; done
}
relache
echo "### torture"
timeout 1800 bash torture.sh 2>&1 | grep -E "^T[0-9]+ (FAIL|CRASH)|TOTAL"
for t in upgrade upgradefail crashrestore crashclaude swapidle swapauto buspoll guisweep memory announce resume listscroll newconv emptydesk lastwin lastwin2 mgrguard zoomdbl sessdel sessdir buscold businternal busmulti busrestore multisel midpaste sigterm buspieces version mgrhidden; do
  relache
  t0=$(date +%s)
  out=$(timeout 300 bash $t.sh 2>&1)
  dt=$(( $(date +%s) - t0 ))
  # La sortie complete de chaque test est gardee : un compte d'OK qui baisse
  # sans ECHEC ne s'explique pas sans elle.
  printf '%s\n' "$out" > /tmp/tmuxv-tests/sortie-$t.txt
  printf "### %-13s %s OK" "$t" "$(echo "$out" | grep -cE '^  OK')"
  e=$(echo "$out" | grep -c ECHEC); [ "$e" != 0 ] && printf "  %s ECHEC" "$e"
  # Aucune sortie : le test n'a rien verifie (bloque, arrete) - ce n'est pas
  # un succes, meme sans ECHEC.
  [ -z "$out" ] && printf "  SANS SORTIE"
  printf "  (%ss)\n" "$dt"
  echo "$out" | grep ECHEC | sed 's/^/      /'
done
echo "(fin)"
