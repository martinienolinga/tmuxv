#!/bin/bash
# VERSION : le binaire porte la version du paquet (packaging/src/control),
# que la boite "A propos" et le format #{tmuxv_version} affichent ; et le
# paquet refuse un binaire qui ne la porte pas.
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
SRC=/home/martinien/tmux
V=$D/torture/version; rm -rf $V; mkdir -p $V; cd $V
export HOME=$V/home; mkdir -p "$HOME"
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
WANT=$(sed -n 's/^Version: //p' $SRC/packaging/src/control)
S=/tmp/ver_$$
$TM -S $S -f /dev/null new-session -d -s v
ok "#{tmuxv_version} donne la version du paquet" "$($TM -S $S display -p '#{tmuxv_version}')" "$WANT"
ok "le binaire porte le repere de version" "$(grep -acF "@(#)tmuxv $WANT @" $TM)" "1"
ok "-V inchange : tpm et les plugins y lisent 3.4" "$($TM -V)" "tmux 3.4 (tmuxv)"
# Un binaire d'une autre version : build.sh doit refuser de l'empaqueter.
sed "s/@(#)tmuxv $WANT @/@(#)tmuxv 0.0+autre @/" $TM > $V/faux 2>/dev/null
ok "le faux binaire ne porte plus la version" "$(grep -acF "@(#)tmuxv $WANT @" $V/faux)" "0"
$TM -S $S kill-server 2>/dev/null; true
