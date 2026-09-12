#!/bin/bash
# AUDIT : chaque touche du prefixe est envoyee au gestionnaire Claude ; on
# verifie ensuite ses invariants (fenetre toujours gestionnaire, toujours
# zoomee sur UNE conversation, liste intacte, nombre de conversations stable).
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/mgraudit; rm -rf $V; mkdir -p $V; cd $V
printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/au_$$
xterm -geometry 125x38 -fa Monospace -fs 10 -e "$TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 2.5
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
setup(){ # gestionnaire neuf avec 3 conversations, fenetre courante = lui
  $TM -S $S kill-window -t 1 2>/dev/null; sleep 0.3
  $TM -S $S claude-manager; sleep 1.2
  for i in 1 2; do
    $TM -S $S select-layout -t 1 tiled 2>/dev/null
    $TM -S $S split-window -h -t 1 2>/dev/null
    $TM -S $S select-layout -t 1 tiled 2>/dev/null; sleep 0.4
  done; sleep 0.6
}
setup; import -window root base.png
BP=$(fmt '#{window_panes}'); echo "reference : $BP conversations, zoom=$(fmt '#{window_zoomed_flag}')"
echo
printf "%-6s %-5s %-6s %-6s %-8s %s\n" TOUCHE mgr zoom panneaux fenetre VERDICT
for k in space z exclam quotedbl percent o semicolon C-o braceleft braceright \
         Up Down Left Right w s f i t n p l ampersand x q asciitilde question \
         equal numbersign m e M-1 M-5 C-Up; do
  xdotool mousemove 400 300; xdotool key ctrl+b; sleep 0.15; xdotool key "$k" 2>/dev/null; sleep 0.8
  xdotool key Escape; sleep 0.5      # refermer un eventuel dialogue/confirmation
  mgr=$(fmt '#{window_claude_manager}'); zoom=$(fmt '#{window_zoomed_flag}')
  pn=$(fmt '#{window_panes}'); wi=$(fmt '#{window_index}')
  v="ok"
  # n/p/l changent de fenetre : c'est leur role, pas une anomalie.
  case $k in n|p|l) [ "$wi" != "1" ] && v="ok (change de fenetre, normal)";; esac
  [ "$wi" != "1" ] && [ "$v" = "ok" ] && v="fenetre changee"
  [ "$mgr" != "1" ] && [ "$v" = "ok" ] && v="plus le gestionnaire"
  [ "$wi" = "1" ] && [ "$zoom" != "1" ] && v="DEZOOME (conversations cote a cote)"
  [ "$wi" = "1" ] && [ "$pn" != "$BP" ] && v="conversations $BP -> $pn"
  printf "%-6s %-5s %-6s %-6s %-8s %s\n" "$k" "$mgr" "$zoom" "$pn" "$wi" "$v"
  if [ "$v" != "ok" ]; then import -window root "audit_$k.png"; setup; fi
done
echo; echo "captures des anomalies dans $V"
$TM -S $S kill-server 2>/dev/null; kill $XP 2>/dev/null
