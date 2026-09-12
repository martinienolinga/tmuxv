#!/bin/bash
# End-to-end test: the conversation manager + agents talking over the real bus.
# Agents are driven from their OWN shells, exactly as a Claude agent would.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}; V=$D/torture/agents; rm -rf $V; mkdir -p $V/projet-a; cd $V
CH=$V/home; mkdir -p $CH
BUS=http://localhost:4319
MARK="SCHEMA-CHANGE-$RANDOM"
P=0; F=0
ok(){ P=$((P+1)); echo "  PASS  $1"; }
ko(){ F=$((F+1)); echo "  FAIL  $1 -- $2"; }
chk(){ if eval "$2"; then ok "$1"; else ko "$1" "$3"; fi; }

printf 'set -g mouse on\nset -g @menu-bar on\nset -g @desktop on\n' > conf
S=/tmp/ag_$$
HOME=$CH xterm -geometry 130x40 -fa Monospace -fs 10 -e "cd $V && HOME=$CH $TM -S $S -f $V/conf new-session -s t" & XP=$!
sleep 3
CL=$($TM -S $S list-clients -F '#{client_name}' | head -1)
fmt(){ timeout 4 $TM -S $S display-message -p "$1" 2>&1; }
pf(){ timeout 4 $TM -S $S display-message -p -t "$1" "$2" 2>&1; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
pid_of(){ $TM -S $S list-panes -F '#{pane_pid}' | sed -n "$1p"; }
agent_of(){ tr '\0' '\n' < /proc/$(pid_of $1)/environ 2>/dev/null | sed -n 's/^AGENT_NAME=//p'; }
row_click(){ xdotool mousemove $(cx $((DX+3))) $(cy $((TOP+$1))) click $2; sleep 1.0; }

echo "== A. le gestionnaire et ses conversations =="
$TM -S $S claude-manager -c "$CL"; sleep 1.2
read -r DX DY DW DH <<<"$(fmt '#{window_desktop_x} #{window_desktop_y} #{window_desktop_w} #{window_desktop_h}')"
TOP=$((1+DY+1)); IH=$((DH-2)); NEW_Y=$(cy $((TOP+IH-1))); NEW_X=$(cx $((DX+3)))
chk "fenetre gestionnaire creee" '[ "$(fmt "#{window_name}")" = "Claude Code" ]' "nom=$(fmt '#{window_name}')"
xdotool mousemove $NEW_X $NEW_Y click 1; sleep 1.5
xdotool mousemove $NEW_X $NEW_Y click 1; sleep 1.5
chk "3 conversations apres 2 x [+ Nouvelle]" '[ "$(fmt "#{window_panes}")" = 3 ]' "panes=$(fmt '#{window_panes}')"
chk "une seule affichee (fenetre zoomee)" '[ "$(fmt "#{window_zoomed_flag}")" = 1 ]' "zoom=$(fmt '#{window_zoomed_flag}')"
A1=$(agent_of 1); A2=$(agent_of 2); A3=$(agent_of 3)
echo "  agents: 1=$A1  2=$A2  3=$A3"
chk "chaque conversation a un AGENT_NAME" '[ -n "$A1" ] && [ -n "$A2" ] && [ -n "$A3" ]' "$A1/$A2/$A3"
chk "noms distincts" '[ "$A1" != "$A2" ] && [ "$A2" != "$A3" ]' "$A1/$A2/$A3"
sleep 1
REG=$(curl -s --max-time 3 $BUS/agents)
chk "les 3 sont enregistres sur le bus" 'echo "$REG" | grep -q "\"$A1\"" && echo "$REG" | grep -q "\"$A2\"" && echo "$REG" | grep -q "\"$A3\""' "absents"

echo "== B. un agent ecrit a un autre (depuis son propre shell) =="
row_click 1 1                      # afficher la conversation 1
CUR=$(curl -s --max-time 3 "$BUS/inbox?agent=$A3&since=0" | python3 -c 'import sys,json;print(json.load(sys.stdin)["cursor"])' 2>/dev/null)
$TM -S $S send-keys -t "$(pf :.0 '#{pane_id}')" "curl -s -X POST $BUS/send -d '{\"from\":\"$A1\",\"to\":\"$A3\",\"subject\":\"api\",\"body\":\"$MARK\"}' >/dev/null" Enter
sleep 2
GOT=$(curl -s --max-time 3 "$BUS/inbox?agent=$A3&since=$CUR" | grep -c "$MARK")
chk "le message de $A1 est dans la boite de $A3" '[ "$GOT" -ge 1 ]' "inbox vide"

echo "== C. le destinataire le lit dans son propre terminal =="
$TM -S $S send-keys -t "$(pf :.2 '#{pane_id}')" "curl -s \"$BUS/inbox?agent=$A3&since=$CUR&wait=5\"" Enter
sleep 4
chk "le texte apparait dans la conversation 3" '$TM -S $S capture-pane -p -t "$(pf :.2 "#{pane_id}")" | grep -q "$MARK"' "pas vu"

echo "== D. l'indicateur de courrier =="
sleep 4                             # laisse le sondage memoriser les niveaux
row_click 1 1
CUR2=$(curl -s --max-time 3 "$BUS/inbox?agent=$A2&since=0" | python3 -c 'import sys,json;print(json.load(sys.stdin)["cursor"])' 2>/dev/null)
curl -s --max-time 3 -X POST $BUS/send -d "{\"from\":\"collegue\",\"to\":\"$A2\",\"subject\":\"t\",\"body\":\"ping-$MARK\"}" >/dev/null
sleep 6
import -window root d_mail.png; convert d_mail.png -crop $(( (22+2)*8 ))x120+$((DX*8))+$(( (1+DY)*17 )) +repage mail_on.png
chk "un ✉ est apparu dans la liste" 'compare -metric AE mail_on.png mail_on.png null: >/dev/null 2>&1 && [ -s mail_on.png ]' "capture absente"
row_click 2 1                       # afficher la conversation 2 -> doit effacer
sleep 1; import -window root d_read.png; convert d_read.png -crop $(( (22+2)*8 ))x120+$((DX*8))+$(( (1+DY)*17 )) +repage mail_off.png
DIFF=$(compare -metric AE mail_on.png mail_off.png null: 2>&1)
chk "l'affichage de la conversation change la liste (✉ efface)" '[ "$DIFF" -gt 100 ]' "diff=$DIFF"

echo "== E. diffusion a tous (to: *) =="
CUR3=$(curl -s --max-time 3 "$BUS/inbox?agent=$A1&since=0" | python3 -c 'import sys,json;print(json.load(sys.stdin)["cursor"])' 2>/dev/null)
$TM -S $S send-keys -t "$(pf :.2 '#{pane_id}')" "curl -s -X POST $BUS/send -d '{\"from\":\"$A3\",\"to\":\"*\",\"subject\":\"all\",\"body\":\"BROADCAST-$MARK\"}' >/dev/null" Enter
sleep 2
B1=$(curl -s --max-time 3 "$BUS/inbox?agent=$A1&since=$CUR3" | grep -c "BROADCAST-$MARK")
chk "la diffusion atteint les autres conversations" '[ "$B1" -ge 1 ]' "non recue"

echo "== F. mecanique des conversations =="
row_click 3 3                       # clic droit sur la conversation 3
sleep 0.8; import -window root f_menu.png
xdotool mousemove 30 100; xdotool key Escape; sleep 0.5
row_click 1 1
$TM -S $S send-keys -t "$(pf :.0 '#{pane_id}')" "printf '\\033]2;Refonte API\\033\\\\'" Enter; sleep 1.2
chk "le vrai nom (titre) remonte dans la liste" '[ "$(pf :.0 "#{pane_title}")" = "Refonte API" ]' "titre=$(pf :.0 '#{pane_title}')"
N0=$(fmt '#{window_panes}')
$TM -S $S send-keys -t "$(pf :.2 '#{pane_id}')" 'exit' Enter; sleep 2
chk "fermer une conversation (exit) laisse les autres" '[ "$(fmt "#{window_panes}")" = "$((N0-1))" ]' "panes=$(fmt '#{window_panes}')"
chk "et la fenetre reste zoomee" '[ "$(fmt "#{window_zoomed_flag}")" = 1 ]' "zoom=$(fmt '#{window_zoomed_flag}')"
import -window root f_end.png; convert f_end.png -crop $((DW*8+16))x$((DH*17+8))+$((DX*8))+$(( (1+DY)*17 )) +repage final.png

echo "== G. stabilite =="
chk "serveur vivant" '[ "$(fmt ok)" = ok ]' "mort"
chk "aucun plantage enregistre" '[ ! -f $CH/.tmuxv-crash.log ]' "$( [ -f $CH/.tmuxv-crash.log ] && head -3 $CH/.tmuxv-crash.log)"
$TM -S $S kill-server 2>/dev/null; sleep 2
chk "kill-server sort proprement" '[ ! -f $CH/.tmuxv-crash.log ]' "plantage a la sortie"
kill $XP 2>/dev/null; rm -f $S
echo; echo "===== RESULTAT : $P reussis, $F echoues ====="
