#!/bin/bash
# FENETRE DES PARAMETRES : recherche, modification, ajout d'une option @,
# suppression, reinitialisation d'une option integree, element de tableau,
# valeur refusee - et un enregistrement qui FUSIONNE dans ~/.tmux.conf (une
# seconde modification ne doit pas effacer la premiere).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/params; rm -rf $V; mkdir -p $V; cd $V
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $HOME/.tmux.conf
S=/tmp/pa_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S new-session -s t" >/dev/null 2>&1 </dev/null &
XP=$!; sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
k(){ xdotool mousemove 30 100; xdotool key "$@"; sleep 0.25; }
ty(){ xdotool mousemove 30 100; xdotool type --delay 20 "$1"; sleep 0.4; }
efface(){ k End; for i in $(seq 1 ${1:-12}); do xdotool key BackSpace; done; sleep 0.2; }
ouvre(){ $TM -S $S display-form -c "$CL"; sleep 0.7; }
shot(){ import -window root $V/$1.png; convert $V/$1.png -crop 968x604+20+30 +repage $V/$1-dlg.png; }
diffn(){ compare -metric AE $V/$1.png $V/$2.png null: 2>&1 | awk '{print int($1)}'; }
BLOC(){ sed -n '/>>> tmuxv parametres/,/<<< tmuxv parametres/p' $HOME/.tmux.conf; }

shot base
echo "--- 1. recherche puis modification ---"
ouvre; shot p0
ty history-limit; shot p1
k Down; efface; ty 7777; k Return; sleep 0.5
ok "history-limit applique a chaud" "$($TM -S $S show -gv history-limit)" "7777"
ok "une ligne dans le bloc enregistre" "$(BLOC | grep -c 'history-limit')" "1"

echo "--- 2. une seconde modification ne fait pas oublier la premiere ---"
ouvre; ty status-interval; k Down; efface 6; ty 9; k Return; sleep 0.5
ok "status-interval applique" "$($TM -S $S show -gv status-interval)" "9"
ok "history-limit toujours enregistre (fusion)" "$(BLOC | grep -c 'history-limit')" "1"
ok "status-interval enregistre" "$(BLOC | grep -c 'status-interval')" "1"
ok "la configuration d'origine est intacte" "$(grep -c '^set -g mouse on' $HOME/.tmux.conf)" "1"

echo "--- 3. ajout d'une option utilisateur ---"
ouvre; k Insert; ty essai; k Tab; ty bonjour; shot p3
k Return; sleep 0.3; shot p3b; k Return; sleep 0.5
ok "@essai creee" "$($TM -S $S show -gv @essai 2>/dev/null)" "bonjour"
ok "@essai enregistree" "$(BLOC | grep -c 'set -g @essai "bonjour"')" "1"

echo "--- 4. suppression de l'option utilisateur ---"
ouvre; ty @essai; k Down; k ctrl+Delete; shot p4; k Return; sleep 0.5
ok "@essai n'existe plus" "$($TM -S $S show -gv @essai 2>/dev/null)" ""
ok "suppression enregistree" "$(BLOC | grep -c 'set -u -g @essai')" "1"
ok "plus de ligne qui la recree" "$(BLOC | grep -c 'set -g @essai')" "0"

echo "--- 5. reinitialisation d'une option integree ---"
ouvre; ty history-limit; k Down; k ctrl+Delete; shot p5; k Return; sleep 0.5
ok "history-limit revenu a sa valeur par defaut" "$($TM -S $S show -gv history-limit)" "2000"
ok "reinitialisation enregistree" "$(BLOC | grep -c 'set -u -g history-limit')" "1"

echo "--- 6. element de tableau ---"
ouvre; k Insert; k BackSpace; ty "terminal-overrides[7]"; k Tab; ty ",essai:RGB"; k Return; sleep 0.3; k Return; sleep 0.5
ok "terminal-overrides[7] ajoute" "$($TM -S $S show -gv 'terminal-overrides[7]' 2>/dev/null)" ",essai:RGB"
ok "element enregistre" "$(BLOC | grep -c 'terminal-overrides\[7\]')" "1"

echo "--- 7. valeur refusee : la fenetre reste ouverte et le dit ---"
AV=$($TM -S $S show -gv display-panes-colour)
ouvre; ty display-panes-colour; k Down; efface 20; ty pasunecouleur; k Return; sleep 0.5; shot p7
k Escape; sleep 0.5; shot p7b
ok "l'option n'a pas change" "$($TM -S $S show -gv display-panes-colour)" "$AV"
ok "la fenetre etait restee ouverte" "$([ "$(diffn p7 p7b)" -gt 5000 ] && echo oui || echo NON)" "oui"

echo "--- 8. Echap : vide d'abord la recherche, puis ferme ---"
ouvre; ty zzz; k Escape; sleep 0.4; shot p8a; k Escape; sleep 0.6; shot p8b
ok "premier Echap : toujours ouverte" "$([ "$(diffn base p8a)" -gt 5000 ] && echo oui || echo NON)" "oui"
ok "second Echap : fermee" "$([ "$(diffn base p8b)" -lt 1500 ] && echo oui || echo NON)" "oui"
ok "le serveur est vivant" "$($TM -S $S display-message -p ok)" "ok"
echo "--- bloc enregistre ---"; BLOC | sed 's/^/    /'
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
