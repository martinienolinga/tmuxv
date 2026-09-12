#!/bin/bash
# 1. Plus d'annonce automatique : un agent qui demarre ne recoit rien.
# 2. L'annonce est declenchee par l'utilisateur, depuis le menu de la
#    conversation, avec un texte MODIFIABLE dans une boite de dialogue.
export DISPLAY=:99
D=${TMUXV_WORK:-/tmp/tmuxv-tests}
TM=${TMUXV:-/home/martinien/tmux/build/tmux}
V=$D/torture/announce; rm -rf $V; mkdir -p $V/work $V/bin; cd $V/work
# Serveur de test : son propre HOME, pour ne jamais lire ni effacer le
# fichier d'etat (~/.tmuxv-state) du tmuxv de l'utilisateur.
export HOME=$V/home; mkdir -p "$HOME"
printf 'set -g mouse on\n' > $V/conf
# Faux agent : il se comporte comme claude (ecran alterne + invite ❯) et
# ecrit tout ce qu'on lui tape dans un fichier.
# Le faux agent active le COLLAGE ENTRE CROCHETS (comme Claude Code) et
# enregistre les octets BRUTS : c'est le seul moyen de verifier qu'un message
# multi-lignes arrive en UN seul morceau et non en trois.
cat > $V/bin/claude <<'EOF'
#!/bin/bash
printf '\033[?1049h\033[?2004h\033[H'
echo "faux agent pret"; echo "❯ "
stdbuf -o0 cat >> "$RECU"
EOF
chmod +x $V/bin/claude
S=/tmp/an_$$
xterm -geometry 140x36 -fa Monospace -fs 10 \
  -e "cd $V/work && RECU=$V/recu.txt PATH=$V/bin:\$PATH $TM -S $S -f $V/conf new-session -s t" \
  >/dev/null 2>&1 </dev/null &
sleep 3
fmt(){ $TM -S $S display-message -p "$1" 2>&1; }
ok(){ if [ "$2" = "$3" ]; then echo "  OK    $1 ($2)"; else echo "  ECHEC $1 : $2 au lieu de $3"; fi; }
cx(){ echo $(( $1*8+4 )); }; cy(){ echo $(( $1*17+11 )); }
$TM -S $S claude-manager; sleep 2
# exec -a : le faux agent doit se PRESENTER comme "claude", sinon tmuxv refuse
# d injecter (garde volontaire : ne jamais taper dans un shell).
$TM -S $S send-keys -t t "RECU=$V/recu.txt exec -a claude bash $V/bin/claude" Enter; sleep 3
echo "--- 1. aucune annonce automatique ---"
ok "agent detecte" "$(fmt '#{pane_agent}' | grep -c .)" "1"
sleep 6      # le minuteur du bus tourne toutes les 3 s : deux tours
ok "rien n'a ete injecte" "$([ -s $V/recu.txt ] && echo "OUI: $(cat $V/recu.txt)" || echo non)" "non"
echo "--- 2. annonce declenchee par l'utilisateur ---"
RX=$(fmt '#{window_desktop_x}'); RY=$(fmt '#{window_desktop_y}')
MB=0; [ "$(fmt '#{@menu-bar}')" = "on" ] && MB=1
import -window root avant.png
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+2 ))) click 3; sleep 1.2
import -window root a0.png
ok "le menu s'ouvre" "$([ "$(compare -metric AE avant.png a0.png null: 2>&1)" -gt 0 ] && echo oui || echo NON)" "oui"
xdotool key a; sleep 1.5           # mnemonique "Annoncer..."
import -window root a1.png
# Comparer deux captures PLEIN ECRAN ne prouve rien : l'horloge de la barre de
# statut les fait differer de toute facon. On regarde la REGION DE LA BOITE
# (centree, 64x14 cellules), ou rien d'autre ne bouge : elle doit changer
# massivement quand la boite s'ouvre.
CW=$(fmt '#{client_width}'); CH=$(fmt '#{client_height}')
BX=$(( ((CW - 64) / 2) * 8 )); BY=$(( ((CH - 14) / 2) * 17 ))
boite(){ convert "$1" -crop $(( 64*8 ))x$(( 14*17 ))+$BX+$BY +repage "$2"; }
boite avant.png z0.png; boite a1.png z1.png
DZ=$(compare -metric AE z0.png z1.png null: 2>&1)
ok "la boite de saisie s'ouvre ($DZ pixels dans sa zone)" \
   "$([ "$DZ" -gt 5000 ] && echo oui || echo NON)" "oui"
# Zone MULTI-LIGNES : on efface le texte propose (BackSpace en rafale), puis on
# tape trois lignes separees par Entree - Entree fait une nouvelle ligne ici.
for i in $(seq 1 260); do xdotool key BackSpace; done; sleep 0.5
xdotool type --delay 8 "MON ANNONCE A MOI"; xdotool key Return
xdotool type --delay 8 "deuxieme ligne"; xdotool key Return
xdotool type --delay 8 "troisieme ligne"; sleep 0.5
import -window root a2.png
# Tab amene sur OK, Entree valide (Entree dans le texte = nouvelle ligne).
xdotool key Tab; sleep 0.4; xdotool key Return; sleep 3
import -window root a3.png            # etat APRES validation (message d'erreur ?)
echo "  ecran du panneau : [$($TM -S $S capture-pane -p | grep -v '^$' | tail -2 | tr '\n' '|')]"
echo "  recu par l'agent : [$(cat $V/recu.txt 2>/dev/null | tr '\n' '|')]"
RAW=$(cat -v $V/recu.txt 2>/dev/null)
ok "les 3 lignes sont arrivees" \
   "$(grep -c 'MON ANNONCE A MOI' $V/recu.txt)$(grep -c 'deuxieme ligne' $V/recu.txt)$(grep -c 'troisieme ligne' $V/recu.txt)" "111"
# ESC[200~ ... ESC[201~ : un seul message pour Claude Code, pas trois.
ok "envoye en collage entre crochets (debut)" "$(echo "$RAW" | grep -c '\^\[\[200~')" "1"
ok "... et fin"                              "$(echo "$RAW" | grep -c '\^\[\[201~')" "1"
echo "--- 3. Renommer : meme piege (%1 du modele), corrige ---"
xdotool mousemove $(cx $((RX+4))) $(cy $(( MB+RY+2 ))) click 3; sleep 1.2
xdotool key r; sleep 1.5                      # mnemonique "Renommer"
xdotool key ctrl+u; sleep 0.3
xdotool type --delay 12 "conversation-renommee"; sleep 0.4
xdotool key Return; sleep 2
import -window root a4.png
ok "le titre a change" "$(fmt '#{pane_title}')" "conversation-renommee"
ok "et la liste l affiche" "$($TM -S $S display-message -p '#{window_claude_manager}')" "1"
echo "captures dans $V/work"
$TM -S $S kill-server 2>/dev/null; pkill -f "^xterm -geometr[y] 140x36" 2>/dev/null
