#!/bin/bash
# Lance tmuxv AVEC LA CONFIGURATION DE L'UTILISATEUR (clone), sur une socket
# isolee, pour reproduire fidelement ce qu'il voit : plugins TPM, theme tmux2k,
# ses raccourcis. C'est le point de depart pour tout defaut qu'il signale.
#
#   bash tests/userconf.sh                  # ouvre la fenetre et une capture
#   bash tests/userconf.sh "commande tmux"  # ... puis joue une commande
#
# La socket est jetable ; la session « default » de l'utilisateur n'est jamais
# touchee, et rien n'est ecrit dans son ~/.tmux.
export DISPLAY=${DISPLAY:-:99}
HERE=$(cd "$(dirname "$0")" && pwd)
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
WORK=${TMUXV_WORK:-/tmp/tmuxv-tests}
CONF=$HERE/user.conf
GEO=${GEO:-160x44}

[ -r "$CONF" ] || bash "$HERE/clone-user-conf.sh"
V=$WORK/userconf; rm -rf "$V"; mkdir -p "$V/resurrect"; cd "$V"
S=/tmp/uc_$$
# Les redirections evitent que l'xterm garde le tuyau du script ouvert.
xterm -geometry "$GEO" -fa Monospace -fs 10 -e "$TM -S $S -f $CONF new-session -s t" \
    >/dev/null 2>&1 </dev/null &
XP=$!
sleep 4      # TPM installe/charge les plugins au demarrage

echo "socket   : $S"
echo "config   : $CONF"
echo "capture  : $V/user0.png"
import -window root "$V/user0.png" 2>/dev/null
$TM -S $S display-message -p \
  "session #{session_name} | @desktop=#{@desktop} @menu-bar=#{@menu-bar} | #{window_width}x#{window_height}" 2>&1

[ -n "$1" ] && { echo "commande : $1"; $TM -S $S $1; sleep 1; import -window root "$V/user1.png"; }

echo "(la fenetre reste ouverte : $TM -S $S ... ; pour fermer : $TM -S $S kill-server)"
