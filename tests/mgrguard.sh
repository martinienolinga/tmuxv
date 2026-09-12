#!/bin/bash
# 1. prefixe+B / prefixe+F refuses sur le gestionnaire Claude.
# 2. ... mais toujours actifs sur une fenetre ordinaire.
# 3. fenetre maximisee : plus de bande de bureau residuelle apres bascule.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgrguard; rm -rf $V; mkdir -p $V; cd $V
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/mg_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
pfx(){ xdotool mousemove 400 300; xdotool key ctrl+b; sleep 0.2; xdotool key "$1"; sleep 0.9; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
echo "--- fenetre ORDINAIRE (bash) : les bascules doivent marcher ---"
pfx B; ok "prefixe+B eteint la barre" "$(fmt '#{@menu-bar}')" "off"
pfx B; ok "prefixe+B la rallume"      "$(fmt '#{@menu-bar}')" "on"
pfx F; ok "prefixe+F eteint le bureau" "$(fmt '#{@desktop}')" "off"
pfx F; ok "prefixe+F le rallume"       "$(fmt '#{@desktop}')" "on"
$TM -S $S claude-manager; sleep 1.5
$TM -S $S select-layout -t t tiled 2>/dev/null; $TM -S $S split-window -h -t t 2>/dev/null
$TM -S $S select-layout -t t tiled 2>/dev/null; sleep 1
ok "fenetre active = gestionnaire" "$(fmt '#{window_claude_manager}')" "1"
echo "--- GESTIONNAIRE : les bascules doivent etre refusees ---"
pfx B; ok "prefixe+B laisse la barre" "$(fmt '#{@menu-bar}')" "on"
pfx F; ok "prefixe+F laisse le bureau" "$(fmt '#{@desktop}')" "on"
ok "conversations toujours la" "$(fmt '#{window_panes}')" "2"
echo "--- maximisee + bascule depuis une fenetre ordinaire ---"
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}'); RW=$(fmt '#{window_desktop_w}')
xdotool mousemove $(( (RX+RW-4)*8+4 )) $(( (1+RY)*17+11 )) click 1; sleep 1
echo "  maximisee : $(fmt '#{window_desktop_w}')x$(fmt '#{window_desktop_h}') (zone $(fmt '#{client_width}')x?)"
import -window root g0.png
$TM -S $S select-window -t 0; sleep 0.5     # aller sur la fenetre bash
pfx B; sleep 0.5
$TM -S $S select-window -t 1; sleep 0.8     # revenir au gestionnaire maximise
echo "  barre eteinte, maximisee : $(fmt '#{window_desktop_w}')x$(fmt '#{window_desktop_h}')"
import -window root g1.png
$TM -S $S select-window -t 0; sleep 0.3; pfx B; $TM -S $S select-window -t 1; sleep 0.8
import -window root g2.png
echo "  retour a l'etat initial : diff=$(compare -metric AE g0.png g2.png null: 2>&1)"
echo "captures dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
